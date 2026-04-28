#!/bin/bash
# SPDX-FileCopyrightText: © 2025-2026 Tenstorrent USA, Inc.
# SPDX-License-Identifier: Apache-2.0
#
# Get the git SHA1 of the project with optional -dirty suffix
# Usage: get_project_git_sha.sh [project_directory]
# Outputs: git SHA1 (short) with -dirty suffix if working directory has uncommitted changes

if [ -n "$CI_COMMIT_SHA" ]; then
	echo "${CI_COMMIT_SHA:0:7}"
	exit 0
fi

PROJECT_DIR="${1:-${CI_PROJECT_DIR:-.}}"

PROJECT_GIT_SHA=$(git -C "$PROJECT_DIR" describe --long --dirty --always 2>/dev/null) || exit 1

echo "$PROJECT_GIT_SHA"
