#!/usr/bin/env python3
# SPDX-FileCopyrightText: © 2026 Tenstorrent USA, Inc.
# SPDX-License-Identifier: Apache-2.0

"""Collect and compare binary size numbers and Kconfig from west builds.

collect: read the linker memory usage tables from a west build log, the
         rom_report/ram_report JSON if present, and the final .config,
         and write them to one JSON file per board.

compare: render a markdown report from two directories of those files.

The compare step is run by CI on files produced by pull request builds, so
it treats every string in them as untrusted.
"""

import argparse
import glob
import html
import json
import os
import re
import sys

MARKER = "<!-- wallabmc-size-report -->"

TABLE_HEADER = re.compile(r"^Memory region\s+Used Size\s+Region Size")
TABLE_ROW = re.compile(
    r"^\s*(\S+):\s+(\d+)\s+(B|KB|MB|GB)\s+(\d+)\s+(B|KB|MB|GB)\s+[\d.]+%"
)
GENERATING = re.compile(r"^Generating files from (.+)/zephyr/zephyr\.elf ")
UNITS = {"B": 1, "KB": 1 << 10, "MB": 1 << 20, "GB": 1 << 30}

CONFIG_SET = re.compile(r"^CONFIG_(\w+)=(.*)$")
CONFIG_UNSET = re.compile(r"^# CONFIG_(\w+) is not set$")

# Kconfig symbols that differ from one build to the next, or that should not
# be published. They are dropped when collecting.
CONFIG_IGNORE = {
    "DEFAULT_ADMIN_PASSWORD",  # random for each build
}

# Regions that are linker bookkeeping rather than memory.
#
# On the ESP32-C6, irom0_0_seg and drom0_0_seg are the same address window
# onto FLASH, for code and for read only data. What they hold is already
# counted in FLASH, and the data follows the code in the window, so the
# figure for drom0_0_seg includes the code as well.
SKIP_REGIONS = {"IDT_LIST", "irom0_0_seg", "drom0_0_seg"}

# Sections that ram_report counts as RAM but that are not, by prefix.
#
# The ESP32-C6 link has NOBITS sections that only place the flash mappings:
# .flash.align_text and .flash.align_rom pad the image, and
# .flash_rodata_dummy covers the addresses of .text. ram_report takes any
# NOBITS section for RAM, so it lists all the code in flash as RAM symbols.
NOT_RAM_SECTIONS = (".flash",)

# The regions every report shows. The others are small and rarely move, so
# the short report only lists them when they change.
PRIMARY_REGIONS = {"FLASH", "RAM", "sram0_0_seg"}

# A board is only called out as changed when something moves by more than
# this many bytes.
NOISE_BYTES = 32

# The board whose memory use is drawn at the top of the short report.
FEATURED_BOARD = "atlantis_evb_mcu"
BAR_WIDTH = 30

# What the featured board's flash is divided into: the name, a short name
# and a square for the bar, and the rom_report paths that belong to it.
# The first match wins. There are only eight coloured squares.
COMPONENTS = [
    (
        "Crypto (mbedTLS)",
        "Crypto",
        "🟦",
        ("WORKSPACE/modules/crypto/", "ZEPHYR_BASE/modules/mbedtls/"),
    ),
    ("Networking", "Networking", "🟪", ("ZEPHYR_BASE/subsys/net/",)),
    ("WallaBMC", "WallaBMC", "🟧", ("WORKSPACE/wallabmc/",)),
    (
        "Drivers and HAL",
        "Drivers",
        "🟩",
        ("ZEPHYR_BASE/drivers/", "WORKSPACE/modules/hal/"),
    ),
    # Symbols with no path come from the C library and the compiler.
    ("libc and libgcc", "libc", "🟨", ("(no paths)/",)),
    (
        "Kernel and arch",
        "Kernel",
        "🟥",
        ("ZEPHYR_BASE/kernel/", "ZEPHYR_BASE/arch/", "ZEPHYR_BASE/soc/"),
    ),
    ("Shell", "Shell", "🟫", ("ZEPHYR_BASE/subsys/shell/",)),
]
OTHER = "Other"
# What rom_report could not name, and what it left out. The bar draws it
# as part of Other.
UNATTRIBUTED = "Unattributed"
OTHER_SQUARE = "⬛"
FREE_SQUARE = "⬜"
COMPONENT_BAR_WIDTH = 12

TOP_SYMBOLS = 10
MAX_CONFIG_LINES = 100
MAX_REPORT = 60000


def parse_log(path):
    """Return a list of (image build dir, {region: {used, size}})."""
    tables = []
    current = None
    with open(path, errors="replace") as f:
        for line in f:
            if TABLE_HEADER.match(line):
                current = {}
                continue
            if current is None:
                continue
            m = TABLE_ROW.match(line)
            if m:
                name, used, used_unit, size, size_unit = m.groups()
                current[name] = {
                    "used": int(used) * UNITS[used_unit],
                    "size": int(size) * UNITS[size_unit],
                }
                continue
            m = GENERATING.match(line)
            if m:
                tables.append((m.group(1), current))
                current = None
    return tables


def flatten(node, prefix, out, skip=()):
    """Flatten a size_report tree into {path: size} for the leaf symbols.

    Symbols in a section whose name starts with one of skip are left out.
    Returns whether there were any.
    """
    name = node.get("name", "?")
    path = f"{prefix.rstrip('/')}/{name}" if prefix else name
    children = node.get("children")
    if children:
        # A list, so that every child is visited.
        return any([flatten(child, path, out, skip) for child in children])
    if skip and str(node.get("section", "")).startswith(skip):
        return True
    # Static symbols can share a path, so add them up.
    out[path] = out.get(path, 0) + int(node.get("size", 0))
    return False


def load_symbols(path, kind):
    with open(path) as f:
        data = json.load(f)
    out = {}
    skip = NOT_RAM_SECTIONS if kind == "ram" else ()
    children = data["symbols"].get("children", [])
    if any([flatten(child, "", out, skip) for child in children]):
        # This is the total less the symbols, and the total counted the
        # sections that were left out. Their sizes are not in the report,
        # so it cannot be corrected.
        out.pop("(hidden)", None)
    return out


def load_config(path):
    """Parse a .config into {symbol: value}, with unset symbols as n."""
    config = {}
    with open(path, errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            m = CONFIG_SET.match(line)
            if m:
                name, value = m.groups()
            else:
                m = CONFIG_UNSET.match(line)
                if not m:
                    continue
                name, value = m.group(1), "n"
            if name not in CONFIG_IGNORE:
                config[name] = value
    return config


def collect(args):
    images = {}
    for spec in args.image:
        name, _, directory = spec.partition("=")
        if not directory:
            sys.exit(f"--image wants NAME=DIR, got {spec!r}")
        images[name] = directory

    tables = parse_log(args.log)
    result = {"board": args.board, "images": {}}

    for name, directory in images.items():
        real = os.path.realpath(directory)
        image = {"regions": {}, "symbols": {}}

        for table_dir, regions in tables:
            if os.path.realpath(table_dir) == real:
                # A rebuild can link more than once; the last one wins.
                # Unused regions are kept, so that a region going to or
                # from zero shows up as a change.
                image["regions"] = {
                    k: v for k, v in regions.items() if k not in SKIP_REGIONS
                }

        for kind in ("rom", "ram"):
            path = os.path.join(directory, f"{kind}.json")
            if os.path.exists(path):
                image["symbols"][kind] = load_symbols(path, kind)

        path = os.path.join(directory, "zephyr", ".config")
        if os.path.exists(path):
            image["config"] = load_config(path)

        if not image["regions"]:
            sys.exit(f"no memory usage table for {name} ({directory}) in {args.log}")
        result["images"][name] = image

    with open(args.output, "w") as f:
        json.dump(result, f)

    for name, image in result["images"].items():
        for region, v in image["regions"].items():
            if v["used"]:
                print(f"{args.board} {name} {region}: {v['used']} / {v['size']}")

    # Draw it as well. There is no baseline here, so no changes are shown.
    rows, _ = rows_for(result["images"], {})
    chart = bars(rows)
    if chart:
        print("", f"{args.board} memory use:", "", *chart, sep="\n")
    parts = breakdown(result["images"], {})
    if parts:
        bar, legend, lines = parts
        print("", bar, legend, "", *lines, sep="\n")


def clean(text, limit=160):
    """Make an untrusted string safe to put in a markdown code span."""
    text = re.sub(r"[^A-Za-z0-9_./:()<>+,*&~ \"'=@%#\[\]{}-]", "?", str(text))
    return text[:limit]


# Prefixes of the symbol paths from rom_report and ram_report, and what to
# print in their place. The first one that matches is used.
PATH_PREFIXES = [
    (re.compile(r"^\(no paths\)/"), ""),
    (re.compile(r"^WORKSPACE/modules/crypto/"), ""),
    (re.compile(r"^WORKSPACE/modules/hal/stm32/stm32cube/"), ""),
    (re.compile(r"^WORKSPACE/modules/(hal/)?"), ""),
    (re.compile(r"^WORKSPACE/(wallabmc/)?"), ""),
    (re.compile(r"^ZEPHYR_BASE/"), "zephyr/"),
    # These paths start with zephyr/, so keep them apart from the sources.
    (re.compile(r"^OUTPUT_DIR/"), "build/"),
    # The toolchain is outside the west workspace, and so is the application
    # in a local build, so they have absolute paths.
    (re.compile(r"^/.*/zephyr-sdk-[^/]+/(gnu/)?"), ""),
    (re.compile(r"^/.*/wallabmc/"), ""),
]


def short_path(name):
    """Shorten a symbol path for display."""
    name = str(name)
    for pattern, replacement in PATH_PREFIXES:
        short, n = pattern.subn(replacement, name, count=1)
        if n:
            return short
    return name


def as_int(value):
    return value if isinstance(value, int) and not isinstance(value, bool) else 0


def load_dir(directory):
    """Return {board: images} from every size-*.json under directory."""
    boards = {}
    if not directory:
        return boards
    pattern = os.path.join(directory, "**", "size-*.json")
    for path in sorted(glob.glob(pattern, recursive=True)):
        try:
            with open(path) as f:
                data = json.load(f)
            boards[clean(data["board"], 64)] = data["images"]
        except (OSError, ValueError, KeyError, TypeError) as e:
            print(f"skipping {path}: {e}", file=sys.stderr)
    return boards


def num(n):
    return f"{n:,}"


def delta(base, head):
    """Format a change, or an empty cell when there is no baseline."""
    if base is None:
        return ""
    d = head - base
    if d == 0:
        return "0"
    pct = f" ({d / base:+.2%})" if base else ""
    return f"**{d:+,}**{pct}"


def get(images, image, section, key):
    try:
        return images[image][section][key]
    except (KeyError, TypeError):
        return None


def row_delta(row):
    return None if row["base"] is None else row["used"] - row["base"]


def row(image, label, kind, region, used, base, size=0):
    return {
        "label": label,
        "kind": kind,
        "image": image,
        "region": region,
        "used": as_int(used),
        "base": as_int(base) if base is not None else None,
        "size": size,
    }


def rows_for(head, base):
    """Rows for one board, and whether it changed by more than the noise."""
    rows = []
    changed = False

    for image in sorted(head, key=lambda n: (n != "wallabmc", n)):
        prefix = "" if image == "wallabmc" else f"{clean(image, 32)} "
        regions = head[image].get("regions", {})
        base_regions = base.get(image, {}).get("regions", {})
        image_rows = []

        # Regions of this build, then any that only the baseline has.
        for region in [*regions, *(r for r in base_regions if r not in regions)]:
            v = regions.get(region, {})
            b = base_regions.get(region)
            if isinstance(b, dict):
                b = b.get("used")
            elif base_regions:
                # The baseline has this image but not the region.
                b = 0
            else:
                b = None
            kind = "primary" if region in PRIMARY_REGIONS else "secondary"
            label = prefix + clean(region, 32)
            size = as_int(v.get("size"))
            image_rows.append(row(image, label, kind, region, v.get("used"), b, size))

        # Leave out what is unused in both builds, or unused with nothing
        # to compare to.
        image_rows = [r for r in image_rows if r["used"] or r["base"]]
        deltas = [row_delta(r) or 0 for r in image_rows]
        # The bootloader rarely changes; only show it when it does, or
        # when there is no baseline and the sizes are all there is to show.
        if image != "wallabmc" and image in base and not any(deltas):
            continue
        rows += image_rows
        if any(abs(d) > NOISE_BYTES for d in deltas):
            changed = True

    return rows, changed


def trim(rows):
    """Drop the rows that the short report leaves out."""
    kept = [r for r in rows if r["kind"] == "primary" or row_delta(r)]
    return kept or rows


def bars(rows):
    """A bar for each main region of the application, as lines of text."""
    rows = [
        r
        for r in rows
        if r["kind"] == "primary" and r["image"] == "wallabmc" and r["size"]
    ]
    if not rows:
        return []
    width = max(len(r["label"]) for r in rows)
    out = []
    for r in rows:
        used = min(max(r["used"] / r["size"], 0), 1)
        full = round(used * BAR_WIDTH)
        line = (
            f"{r['label']:<{width}}  "
            + "\u2588" * full
            + "\u2591" * (BAR_WIDTH - full)
            + f"  {used:.1%} of {num(r['size'])}"
        )
        d = row_delta(r)
        if d:
            line += f" ({d:+,})"
        out.append(line)
    return out


def components(image):
    """Flash use of an image by component, or None without a rom report."""
    symbols = image.get("symbols") if isinstance(image, dict) else None
    symbols = symbols.get("rom") if isinstance(symbols, dict) else None
    if not isinstance(symbols, dict) or not symbols:
        return None

    totals = {name: 0 for name, _, _, _ in COMPONENTS}
    totals[OTHER] = 0
    totals[UNATTRIBUTED] = 0
    for path, size in symbols.items():
        path = str(path)
        where = OTHER
        if path.startswith("(hidden)"):
            where = UNATTRIBUTED
        for name, _, _, prefixes in COMPONENTS:
            if path.startswith(prefixes):
                where = name
                break
        totals[where] += max(as_int(size), 0)

    # Make the parts add up to what the linker says is used.
    flash = get({"i": image}, "i", "regions", "FLASH")
    used = as_int(flash.get("used")) if isinstance(flash, dict) else 0
    totals[UNATTRIBUTED] += max(used - sum(totals.values()), 0)
    return totals


def share_cells(sizes, cells, unit):
    """Divide a number of bar cells between sizes, at least one for each."""
    exact = [min(s / unit, BAR_WIDTH) for s in sizes]
    alloc = [max(int(e), 1) if s else 0 for e, s in zip(exact, sizes)]
    while sum(alloc) < cells:
        i = max(range(len(alloc)), key=lambda i: exact[i] - alloc[i])
        alloc[i] += 1
    while sum(alloc) > cells:
        over = [i for i in range(len(alloc)) if alloc[i] > 1]
        if not over:
            break
        i = min(over, key=lambda i: exact[i] - alloc[i])
        alloc[i] -= 1
    return alloc


def breakdown(head, base):
    """A board's flash by component: the bar, its legend and table lines."""
    totals = components(head.get("wallabmc"))
    flash = get(head, "wallabmc", "regions", "FLASH")
    size = as_int(flash.get("size")) if isinstance(flash, dict) else 0
    if not totals or size <= 0 or not any(totals.values()):
        return None
    before = components(base.get("wallabmc")) or {}
    used = sum(totals.values())

    def change(*names, noise=0):
        if not before:
            return ""
        d = sum(totals[n] - before.get(n, 0) for n in names)
        return f"{d:+,}" if abs(d) > noise else ""

    # The bar: a square for each share of the region, then a legend.
    parts = [(short, sq, (name,)) for name, short, sq, _ in COMPONENTS]
    parts.append((OTHER, OTHER_SQUARE, (OTHER, UNATTRIBUTED)))
    parts = [p for p in parts if any(totals[n] for n in p[2])]
    sizes = [sum(totals[n] for n in names) for _, _, names in parts]
    cells = min(round(used / size * BAR_WIDTH), BAR_WIDTH)
    alloc = share_cells(sizes, cells, size / BAR_WIDTH)
    bar = "".join(sq * n for (_, sq, _), n in zip(parts, alloc))
    bar += FREE_SQUARE * max(BAR_WIDTH - sum(alloc), 0)
    legend = []
    for short, sq, names in parts:
        d = change(*names, noise=NOISE_BYTES)
        legend.append(f"{sq} {short}" + (f" ({d})" if d else ""))
    legend.append(f"{FREE_SQUARE} Free")

    # The table: every component, with what the bar cannot show.
    rows = [(name, totals[name]) for name in totals if totals[name]]
    width = max(len(name) for name, _ in rows)
    largest = max(n for _, n in rows)
    lines = []
    for name, n in rows:
        blocks = "\u2588" * max(round(n / largest * COMPONENT_BAR_WIDTH), 1)
        line = (
            f"{name:<{width}}  {blocks:<{COMPONENT_BAR_WIDTH}}"
            f"  {num(n):>9}  {n / used:>4.0%}"
        )
        d = change(name)
        lines.append(line + (f"  {d}" if d else ""))

    return bar, " · ".join(legend), lines


def table(entries, short):
    """Markdown table for a list of (board, is new, rows)."""
    out = [
        "| Board | Item | Used | Change | Free |",
        "|---|---|--:|--:|--:|",
    ]
    for board, new, rows in entries:
        for i, r in enumerate(trim(rows) if short else rows):
            name = ""
            if i == 0:
                name = f"`{board}`" + " (new)" * new
            free = ""
            if r["size"]:
                free = f"{(r['size'] - r['used']) / r['size']:.1%}"
            change = delta(r["base"], r["used"])
            out.append(
                f"| {name} | `{r['label']}` | {num(r['used'])} | {change} | {free} |"
            )
    return out


def symbol_changes(head, base, kind):
    h = get(head, "wallabmc", "symbols", kind)
    b = get(base, "wallabmc", "symbols", kind)
    if not isinstance(h, dict) or not isinstance(b, dict):
        return []
    changes = []
    for name in set(h) | set(b):
        d = as_int(h.get(name)) - as_int(b.get(name))
        if d:
            changes.append((d, name))
    changes.sort(key=lambda c: (-abs(c[0]), c[1]))
    return changes


def config_changes(head, base):
    """Changes between two configs as (symbol, diffconfig style line)."""
    changes = []
    for name in sorted(set(head) | set(base)):
        h, b = head.get(name), base.get(name)
        if h == b:
            continue
        name = clean(name, 80)
        if b is None:
            changes.append((name, f"+{name} {clean(h)}"))
        elif h is None:
            changes.append((name, f"-{name} {clean(b)}"))
        else:
            changes.append((name, f" {name} {clean(b)} -> {clean(h)}"))
    return changes


def config_report(head, base):
    """Markdown lines for the Kconfig changes of every board."""
    groups = {}  # rendered changes -> boards that have exactly those
    symbols = set()
    compared = 0

    for board in sorted(head):
        lines = []
        seen = False
        for image in sorted(head[board], key=lambda n: (n != "wallabmc", n)):
            h = get(head, board, image, "config")
            b = get(base, board, image, "config")
            if not isinstance(h, dict) or not isinstance(b, dict):
                continue
            seen = True
            changes = config_changes(h, b)
            if not changes:
                continue
            symbols.update((image, name) for name, _ in changes)
            if image != "wallabmc":
                lines.append(f"{clean(image, 32)}:")
            lines += [line for _, line in changes[:MAX_CONFIG_LINES]]
            if len(changes) > MAX_CONFIG_LINES:
                lines.append(f"... and {len(changes) - MAX_CONFIG_LINES} more")
        compared += seen
        if lines:
            groups.setdefault(tuple(lines), []).append(board)

    if not compared:
        return []
    if not groups:
        return ["", "Kconfig: no changes."]

    boards = sum(len(b) for b in groups.values())
    summary = (
        f"Kconfig: {len(symbols)} symbol{'s' * (len(symbols) != 1)} changed"
        f" on {boards} board{'s' * (boards != 1)}."
    )
    out = [
        "",
        summary,
        "",
        "<details><summary>Kconfig changes</summary>",
        "",
    ]
    for lines, names in groups.items():
        if len(names) == compared and compared > 1:
            title = "All boards"
        else:
            title = ", ".join(f"`{n}`" for n in names)
        out += [title, "", "```", *lines, "```", ""]
    out += ["</details>"]
    return out


def compare(args):
    head = load_dir(args.head)
    base = load_dir(args.base)

    out = [MARKER, "### Build report", ""]

    if not head:
        out.append("No size data was produced by this build.")
    elif not base:
        out.append("No baseline from main was found, so only absolute sizes are shown.")
    else:
        sha = re.sub(r"[^0-9a-f]", "", args.base_sha or "")
        wanted = re.sub(r"[^0-9a-f]", "", args.wanted_base_sha or "")
        where = f" at `{sha[:12]}`" if sha else ""
        out.append(f"Change against main{where}. Sizes are in bytes.")
        if wanted and wanted != sha:
            note = (
                f"Note: there was no finished build of `{wanted[:12]}`, which is"
                " what this should be compared with, so the changes shown may"
                " include other commits."
            )
            out += ["", note]

    details = []
    entries = []
    listed = []
    quiet = []
    noise = False
    for board in sorted(head):
        board_base = base.get(board, {})
        rows, changed = rows_for(head[board], board_base)
        new = bool(base) and board not in base
        entry = (board, new, rows)
        entries.append(entry)
        if changed or new or not base:
            listed.append(entry)
        else:
            quiet.append(entry)
            noise = noise or any(row_delta(r) for r in rows)

        if not changed:
            continue
        section = []
        for kind in ("rom", "ram"):
            changes = symbol_changes(head[board], board_base, kind)
            if not changes:
                continue
            plural = "s" * (len(changes) != 1)
            section.append(f"{kind.upper()}, {len(changes)} symbol{plural} changed:")
            for d, name in changes[:TOP_SYMBOLS]:
                section.append(f"{d:+8,}  {clean(short_path(name))}")
            section.append("")
        if section:
            details += [
                "<details><summary>Largest symbol changes: <code>"
                + html.escape(board)
                + "</code></summary>",
                "",
                "```",
                *section[:-1],
                "```",
                "",
                "</details>",
                "",
            ]

    if args.full:
        if entries:
            out += ["", *table(entries, short=False)]
    else:
        for board, _, rows in entries:
            if board == FEATURED_BOARD:
                chart = bars(rows)
                if not chart:
                    continue
                out += ["", f"`{board}` memory use:", "", "```", *chart, "```"]
                parts = breakdown(head[board], base.get(board, {}))
                if parts:
                    bar, legend, lines = parts
                    out += [
                        "",
                        bar,
                        "",
                        legend,
                        "",
                        "<details><summary>Flash by component</summary>",
                        "",
                        "```",
                        *lines,
                        "```",
                        "",
                        "</details>",
                    ]
        if quiet:
            plural = "s" * (len(quiet) != 1)
            small = f" of more than {NOISE_BYTES} bytes" if noise else ""
            out += ["", f"No size change{small} on {len(quiet)} board{plural}."]
        if listed:
            out += ["", *table(listed, short=True)]

    out += config_report(head, base)

    if quiet and not args.full:
        out += [
            "",
            "<details><summary>All boards</summary>",
            "",
            *table(entries, short=True),
            "",
            "</details>",
        ]

    if details:
        out += ["", *details]

    url = args.run_url or ""
    if not args.full and re.fullmatch(r"https://[A-Za-z0-9._/-]+", url):
        out += ["", f"Every region is listed in the [build summary]({url})."]

    text = "\n".join(out).rstrip() + "\n"
    if len(text) > MAX_REPORT:
        text = text[:MAX_REPORT] + "\n\n(truncated)\n"

    if args.output == "-":
        sys.stdout.write(text)
    else:
        with open(args.output, "w") as f:
            f.write(text)


def main():
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("collect", help="gather numbers from a build")
    p.add_argument("--board", required=True, help="name used in the report")
    p.add_argument("--log", required=True, help="west build output")
    p.add_argument(
        "--image",
        action="append",
        required=True,
        metavar="NAME=DIR",
        help="image name and its build directory; repeat for sysbuild",
    )
    p.add_argument("-o", "--output", required=True)
    p.set_defaults(func=collect)

    p = sub.add_parser("compare", help="render a markdown report")
    p.add_argument("--head", required=True, help="directory of size-*.json")
    p.add_argument("--base", help="directory of baseline size-*.json")
    p.add_argument("--base-sha", help="commit the baseline was built from")
    p.add_argument(
        "--wanted-base-sha",
        help="commit the baseline should have been built from, if known",
    )
    p.add_argument(
        "--full",
        action="store_true",
        help="list every row of every board, rather than what changed",
    )
    p.add_argument("--run-url", help="build run to link to from the short report")
    p.add_argument("-o", "--output", default="-")
    p.set_defaults(func=compare)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
