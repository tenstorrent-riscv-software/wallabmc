# SPDX-FileCopyrightText: © 2025-2026 Tenstorrent USA, Inc.
# SPDX-License-Identifier: Apache-2.0

get_filename_component(CURRENT_DIR ${CMAKE_CURRENT_LIST_DIR} ABSOLUTE)
get_filename_component(PARENT_DIR ${CURRENT_DIR} DIRECTORY)
list(APPEND BOARD_ROOT ${PARENT_DIR})

# MCUboot's overlays include the application's, so every image needs the
# application's devicetree bindings (dts/bindings)
set(DTS_ROOT ${PARENT_DIR} CACHE PATH "Devicetree bindings for all images")

# Find the Sysbuild package
find_package(Sysbuild REQUIRED HINTS $ENV{ZEPHYR_BASE})

# Project name
project(sysbuild LANGUAGES)
