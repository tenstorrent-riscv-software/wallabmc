# SPDX-FileCopyrightText: © 2025-2026 Tenstorrent USA, Inc.
# SPDX-License-Identifier: Apache-2.0
#
# Generates git_sha.h at build time so the SHA reflects the current commit.
# Only writes the file when the SHA changes to avoid unnecessary recompilation.
# Invoked via add_custom_target with -DBASH_EXECUTABLE, -DSOURCE_DIR, -DOUTPUT_FILE.

if(BASH_EXECUTABLE)
	execute_process(
		COMMAND ${BASH_EXECUTABLE} ${SOURCE_DIR}/scripts/get_project_git_sha.sh ${SOURCE_DIR}
		OUTPUT_VARIABLE sha
		OUTPUT_STRIP_TRAILING_WHITESPACE
		ERROR_QUIET
	)
endif()

if(NOT sha OR sha STREQUAL "")
	set(sha "unknown")
endif()

set(content "#pragma once\n#define PROJECT_GIT_SHA \"${sha}\"\n")
if(EXISTS "${OUTPUT_FILE}")
	file(READ "${OUTPUT_FILE}" existing)
	if(existing STREQUAL content)
		return()
	endif()
endif()
file(WRITE "${OUTPUT_FILE}" "${content}")
