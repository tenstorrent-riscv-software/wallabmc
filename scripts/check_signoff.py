#!/usr/bin/env python3
# SPDX-FileCopyrightText: © 2026 Tenstorrent USA, Inc.
# SPDX-License-Identifier: Apache-2.0

"""Check commits carry a Signed-off-by trailer.

Two modes:

  check_signoff.py <commit-msg-file>
      commit-msg hook mode. Requires a sign-off matching the local git
      user.name/user.email. WIP, fixup and amend commits are allowed through.

  check_signoff.py --range <base>..<head>
      CI mode. Requires every non-merge commit in the range to carry a
      sign-off from its author. No commits are exempt.
"""

import argparse
import os
import subprocess
import sys


def git(*args):
    return subprocess.check_output(['git', *args], text=True)


def signoff_line(name, email):
    return f'Signed-off-by: {name} <{email}>'


def has_signoff(msg, name, email):
    expected = signoff_line(name, email)
    return any(line.strip() == expected for line in msg.splitlines())


def error(msg):
    if os.environ.get('GITHUB_ACTIONS') == 'true':
        print(f'::error::{msg}')
    else:
        print(f'ERROR: {msg}', file=sys.stderr)


def check_msg_file(msg_file):
    with open(msg_file) as f:
        msg = f.read()

    if msg.lower().startswith(('wip', 'fixup', 'amend')):
        return 0

    name = git('config', 'user.name').strip()
    email = git('config', 'user.email').strip()

    if not has_signoff(msg, name, email):
        error(f'Sign-off missing or incorrect. Expected: {signoff_line(name, email)}')
        return 1

    return 0


def check_range(rev_range):
    commits = git('rev-list', '--no-merges', '--reverse', rev_range).split()
    failed = 0

    for sha in commits:
        name, email, subject, msg = \
            git('log', '-1', '--format=%an%x00%ae%x00%s%x00%B', sha).split('\0', 3)

        if not has_signoff(msg, name, email):
            error(f'{sha[:12]} ("{subject}"): missing {signoff_line(name, email)}')
            failed += 1

    print(f'Checked {len(commits)} commit(s), {failed} missing author sign-off.')
    return 1 if failed else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument('msg_file', nargs='?', help='commit message file (commit-msg hook)')
    group.add_argument('--range', dest='rev_range', metavar='BASE..HEAD',
                       help='check every non-merge commit in a revision range')
    args = parser.parse_args()

    try:
        if args.rev_range:
            return check_range(args.rev_range)
        return check_msg_file(args.msg_file)
    except subprocess.CalledProcessError as e:
        # git has already printed its own error to stderr.
        error(f'"{" ".join(e.cmd)}" failed with exit status {e.returncode}')
        return 1


if __name__ == '__main__':
    sys.exit(main())
