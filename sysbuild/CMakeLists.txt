# SPDX-FileCopyrightText: © 2025-2026 Tenstorrent USA, Inc.
# SPDX-License-Identifier: Apache-2.0

get_filename_component(CURRENT_DIR ${CMAKE_CURRENT_LIST_DIR} ABSOLUTE)
get_filename_component(PARENT_DIR ${CURRENT_DIR} DIRECTORY)
list(APPEND BOARD_ROOT ${PARENT_DIR})

# Find the Sysbuild package
find_package(Sysbuild REQUIRED HINTS $ENV{ZEPHYR_BASE})

# Project name
project(sysbuild LANGUAGES)
