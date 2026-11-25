# SPDX-FileCopyrightText: © 2025-2026 Tenstorrent USA, Inc.
# SPDX-License-Identifier: Apache-2.0

get_filename_component(CURRENT_DIR ${CMAKE_CURRENT_LIST_DIR} ABSOLUTE)
get_filename_component(PARENT_DIR ${CURRENT_DIR} DIRECTORY)
list(APPEND BOARD_ROOT ${PARENT_DIR})

# Find the Sysbuild package
find_package(Sysbuild REQUIRED HINTS $ENV{ZEPHYR_BASE})

# Project name
project(sysbuild LANGUAGES)

# Printed after the images are built, so that it is the last thing on screen
if(BOARD STREQUAL "nucleo_f767zi" AND BOARD_QUALIFIERS MATCHES "/atlantis$")
  add_custom_target(atlantis_settings_warning ALL
    COMMAND ${CMAKE_COMMAND} -E cmake_echo_color --yellow
      "warning: The atlantis variant has a different flash layout to the plain nucleo_f767zi build."
    COMMAND ${CMAKE_COMMAND} -E cmake_echo_color --yellow
      "warning: Settings will be lost when changing builds. See boards/nucleo_f767zi_atlantis.md."
    COMMENT "Built the nucleo_f767zi atlantis variant"
    VERBATIM
  )
  add_dependencies(atlantis_settings_warning ${IMAGES})
endif()
