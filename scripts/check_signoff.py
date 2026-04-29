#!/usr/bin/env python3
# SPDX-FileCopyrightText: © 2026 Tenstorrent USA, Inc.
# SPDX-License-Identifier: Apache-2.0

import re
import subprocess
import sys


def main():
    msg_file = sys.argv[1]
    with open(msg_file) as f:
        msg = f.read()

    if re.match(r'^(wip|fixup)', msg, re.IGNORECASE):
        sys.exit(0)

    name = subprocess.check_output(['git', 'config', 'user.name']).decode().strip()
    email = subprocess.check_output(['git', 'config', 'user.email']).decode().strip()
    expected = f'Signed-off-by: {name} <{email}>'

    if expected not in msg:
        print(f'ERROR: Sign-off missing or incorrect. Expected: {expected}')
        sys.exit(1)


if __name__ == '__main__':
    main()
