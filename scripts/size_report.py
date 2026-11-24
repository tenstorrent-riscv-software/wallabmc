#!/usr/bin/env python3
# SPDX-FileCopyrightText: © 2026 Tenstorrent USA, Inc.
# SPDX-License-Identifier: Apache-2.0

"""Collect and compare binary size numbers and Kconfig from west builds.

collect: read the linker memory usage tables from a west build log, the
         output file sizes, the rom_report/ram_report JSON if present, and
         the final .config, and write them to one JSON file per board.

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

# Output files worth tracking, in order of preference for the report.
FILES = ["zephyr.signed.bin", "zephyr.bin"]

# Regions that are linker bookkeeping rather than memory.
#
# On the ESP32-C6, irom0_0_seg and drom0_0_seg are the same address window
# onto FLASH, for code and for read only data. What they hold is already
# counted in FLASH, and the data follows the code in the window, so the
# figure for drom0_0_seg includes the code as well.
SKIP_REGIONS = {"IDT_LIST", "irom0_0_seg", "drom0_0_seg"}

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


def flatten(node, prefix, out):
    """Flatten a size_report tree into {path: size} for the leaf symbols."""
    name = node.get("name", "?")
    path = f"{prefix.rstrip('/')}/{name}" if prefix else name
    children = node.get("children")
    if children:
        for child in children:
            flatten(child, path, out)
    else:
        # Static symbols can share a path, so add them up.
        out[path] = out.get(path, 0) + int(node.get("size", 0))


def load_symbols(path):
    with open(path) as f:
        data = json.load(f)
    out = {}
    for child in data["symbols"].get("children", []):
        flatten(child, "", out)
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
        image = {"regions": {}, "files": {}, "symbols": {}}

        for table_dir, regions in tables:
            if os.path.realpath(table_dir) == real:
                # A rebuild can link more than once; the last one wins.
                # Unused regions are kept, so that a region going to or
                # from zero shows up as a change.
                image["regions"] = {
                    k: v for k, v in regions.items() if k not in SKIP_REGIONS
                }

        for filename in FILES:
            path = os.path.join(directory, "zephyr", filename)
            if os.path.exists(path):
                image["files"][filename] = os.path.getsize(path)

        for kind in ("rom", "ram"):
            path = os.path.join(directory, f"{kind}.json")
            if os.path.exists(path):
                image["symbols"][kind] = load_symbols(path)

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


def clean(text, limit=160):
    """Make an untrusted string safe to put in a markdown code span."""
    text = re.sub(r"[^A-Za-z0-9_./:()<>+,*&~ \"'=@%#\[\]{}-]", "?", str(text))
    return text[:limit]


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


def rows_for(board, head, base):
    """Table rows for one board, and whether anything changed."""
    rows = []
    changed = False

    def add(label, h, b, size=None):
        nonlocal changed
        h = as_int(h)
        b = as_int(b) if b is not None else None
        if not h and not b:
            # Unused in both builds, or unused with nothing to compare to.
            return
        if b is not None and b != h:
            changed = True
        free = ""
        if size:
            free = f"{num(size - h)} ({(size - h) / size:.1%})"
        rows.append((label, num(h), delta(b, h), free))

    for image in sorted(head, key=lambda n: (n != "wallabmc", n)):
        prefix = "" if image == "wallabmc" else f"{clean(image, 32)} "
        regions = head[image].get("regions", {})
        files = head[image].get("files", {})
        base_regions = base.get(image, {}).get("regions", {})
        image_rows = len(rows)
        image_changed = changed
        changed = False

        # Regions of this build, then any that only the baseline has.
        for region in [*regions, *(r for r in base_regions if r not in regions)]:
            v = regions.get(region, {})
            b = base_regions.get(region)
            add(
                prefix + clean(region, 32),
                v.get("used"),
                b.get("used") if isinstance(b, dict) else None,
                as_int(v.get("size")),
            )
        for filename in FILES:
            if filename in files:
                add(
                    prefix + filename,
                    files[filename],
                    get(base, image, "files", filename),
                )
                break

        # The bootloader rarely changes; only show it when it does, or
        # when there is no baseline and the sizes are all there is to show.
        if image != "wallabmc" and image in base and not changed:
            del rows[image_rows:]
        changed = changed or image_changed

    return rows, changed


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
    if head:
        out += [
            "",
            "| Board | Item | Used | Change | Free |",
            "|---|---|--:|--:|--:|",
        ]
    for board in sorted(head):
        board_base = base.get(board, {})
        rows, changed = rows_for(board, head[board], board_base)
        for i, (label, used, change, free) in enumerate(rows):
            name = f"`{board}`" if i == 0 else ""
            if i == 0 and base and board not in base:
                name += " (new)"
            out.append(f"| {name} | `{label}` | {used} | {change} | {free} |")

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
                section.append(f"{d:+8,}  {clean(name)}")
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

    out += config_report(head, base)

    if details:
        out += ["", *details]

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
    p.add_argument("-o", "--output", default="-")
    p.set_defaults(func=compare)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
