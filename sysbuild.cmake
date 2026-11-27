# SPDX-FileCopyrightText: © 2026 Tenstorrent USA, Inc.
# SPDX-License-Identifier: Apache-2.0

# Sysbuild includes this file after its Kconfig has run and before the images
# are configured. The images need the signing key to exist when they are
# configured.
#
# Kconfig.sysbuild defaults the key to mcuboot-generated-<type>.pem in the
# build directory, where <type> is an imgtool key type. Generate that key if
# it is the one in use and it doesn't exist. A key the user names is theirs
# to provide.
set(key_file "${SB_CONFIG_BOOT_SIGNATURE_KEY_FILE}")
cmake_path(IS_PREFIX CMAKE_BINARY_DIR "${key_file}" NORMALIZE key_in_build_dir)

if(SB_CONFIG_BOOTLOADER_MCUBOOT AND key_in_build_dir AND
   key_file MATCHES "/mcuboot-generated-([a-z0-9-]+)\\.pem$")
  if(NOT EXISTS "${key_file}")
    execute_process(
      COMMAND ${PYTHON_EXECUTABLE} ${ZEPHYR_MCUBOOT_MODULE_DIR}/scripts/imgtool.py
        keygen -t ${CMAKE_MATCH_1} -k "${key_file}"
      COMMAND_ERROR_IS_FATAL ANY
    )
    message(STATUS "Generated MCUboot signing key: ${key_file}")
  endif()
  message(NOTICE
    "MCUboot is using a signing key generated for this build directory. "
    "Images from this build only boot with the bootloader from this build. "
    "Set SB_CONFIG_BOOT_SIGNATURE_KEY_FILE to use your own key.")
endif()

# Run MCUboot with the I and D caches on, as the application does (see
# CACHE_MANAGEMENT in Kconfig). BOOT_DISABLE_CACHES defaults to y, so they
# are flushed and disabled before jumping to the application.
if(SB_CONFIG_BOOTLOADER_MCUBOOT AND SB_CONFIG_SOC_SERIES_STM32F7X)
  set_config_bool(mcuboot CONFIG_CACHE_MANAGEMENT y)
endif()
