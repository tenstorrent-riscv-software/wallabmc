# SPDX-FileCopyrightText: © 2025-2026 Tenstorrent USA, Inc.
# SPDX-License-Identifier: Apache-2.0

list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(wallabmc)

if(BOARD STREQUAL "qemu_cortex_m3")
	if(SYSBUILD)
		message(FATAL_ERROR "qemu_cortex_m3 must NOT be built with sysbuild (e.g., no --sysbuild argument)")
	endif()
else()
	if(NOT SYSBUILD)
		message(FATAL_ERROR "Hardware boards must be built with sysbuild (e.g., --sysbuild argument)")
	endif()
endif()

target_include_directories(app PRIVATE src)

zephyr_linker_sources(SECTIONS sections-rom.ld)
zephyr_linker_section_ifdef(CONFIG_HTTP_SERVER
  NAME http_resource_desc_web_https_service
  KVMA RAM_REGION GROUP RODATA_REGION
)
zephyr_linker_section_ifdef(CONFIG_HTTP_SERVER
  NAME http_resource_desc_web_http_service
  KVMA RAM_REGION GROUP RODATA_REGION
)

# Used for webserver resources
set(gen_dir ${ZEPHYR_BINARY_DIR}/include/generated/)

# Generate git SHA header at build time so it stays current across commits
find_program(BASH_EXECUTABLE bash)
set(git_sha_header ${gen_dir}/git_sha.h)
add_custom_target(app_git_sha ALL
	COMMAND ${CMAKE_COMMAND}
		-DBASH_EXECUTABLE=${BASH_EXECUTABLE}
		-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
		-DOUTPUT_FILE=${git_sha_header}
		-P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_git_sha.cmake
	BYPRODUCTS ${git_sha_header}
	COMMENT "Generating git SHA header"
)
add_dependencies(app app_git_sha)

target_sources(app PRIVATE
	src/config.c
	src/dhcp.c
	src/main.c
	src/net.c
	src/power.c
	src/synch.c
	src/vpd.c
)

target_sources_ifdef(CONFIG_RTC app PRIVATE
	src/rtc.c
)

target_sources_ifdef(CONFIG_APP_SENSORS app PRIVATE
	src/sensors.c
)

target_sources_ifdef(CONFIG_APP_NTP app PRIVATE
	src/ntp.c
)

target_sources_ifdef(CONFIG_HTTP_SERVER app PRIVATE
	src/http.c
)

target_sources_ifdef(CONFIG_APP_WEB app PRIVATE
	src/web.c
)

target_sources_ifdef(CONFIG_JTAG app PRIVATE
	src/jtag.c
)

target_sources_ifdef(CONFIG_APP_CONSOLE_LOGGER app PRIVATE
	src/console_logger.c
)

target_sources_ifdef(CONFIG_CONSOLE_BRIDGE app PRIVATE
	src/console_bridge.c
)

target_sources_ifdef(CONFIG_APP_WEB_TERMINAL_HOST_SERIAL app PRIVATE
	src/console_bridge_ws.c
)

target_sources_ifdef(CONFIG_PERSISTENT_STORAGE app PRIVATE
	src/button.c
	src/fs.c
)

target_sources_ifdef(CONFIG_FAN_CONTROL app PRIVATE
	src/fan.c
)

target_sources_ifdef(CONFIG_POWER_MONITOR app PRIVATE
	src/power_monitor.c
)

target_sources_ifdef(CONFIG_REDFISH app PRIVATE
	src/redfish.c
)

if(CONFIG_REDFISH)
  foreach(redfish_resource
    redfish_metadata.xml
  )
    generate_inc_file_for_target(app
      static_web_resources/${redfish_resource}
      ${gen_dir}/${redfish_resource}.gz.inc
      --gzip
    )
  endforeach()
endif()

if(CONFIG_APP_WEB)
  foreach(web_resource
    index.html
  )
    generate_inc_file_for_target(app
      static_web_resources/${web_resource}
      ${gen_dir}/${web_resource}.gz.inc
      --gzip
    )
  endforeach()

  foreach(web_resource
    logo.png
    favicon.png
  )
    generate_inc_file_for_target(app
      static_web_resources/${web_resource}
      ${gen_dir}/${web_resource}.inc
    )
  endforeach()
endif()

if(CONFIG_APP_CRYPTO)
  find_program(OPENSSL_EXECUTABLE openssl)
  if(NOT OPENSSL_EXECUTABLE)
    message(FATAL_ERROR "CONFIG_APP_CRYPTO requires openssl in PATH")
  endif()
  find_program(BASH_EXECUTABLE bash)
  if(NOT BASH_EXECUTABLE)
    message(FATAL_ERROR "Unable to find bash, needed to generate certificates")
  endif()

  set(generated_certs_dir ${CMAKE_CURRENT_BINARY_DIR}/generated_certs)
  set(generated_server_cert_der ${generated_certs_dir}/server_cert.der)
  set(generated_server_privkey_der ${generated_certs_dir}/server_privkey.der)

  add_custom_command(
    OUTPUT
      ${generated_server_cert_der}
      ${generated_server_privkey_der}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${generated_certs_dir}
    COMMAND ${BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/src/certs/gen_ca_cert.sh ${OPENSSL_EXECUTABLE}
    COMMAND ${BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/src/certs/gen_server_cert.sh ${OPENSSL_EXECUTABLE}
    WORKING_DIRECTORY ${generated_certs_dir}
    DEPENDS
      ${CMAKE_CURRENT_SOURCE_DIR}/src/certs/gen_ca_cert.sh
      ${CMAKE_CURRENT_SOURCE_DIR}/src/certs/gen_server_cert.sh
    BYPRODUCTS
      ${generated_certs_dir}/ca_cert.pem
      ${generated_certs_dir}/ca_privkey.pem
      ${generated_certs_dir}/ca.srl
      ${generated_certs_dir}/server_cert.pem
      ${generated_certs_dir}/server_privkey.pem
      ${generated_certs_dir}/server_csr.pem
      ${generated_certs_dir}/server_csr.ext
    COMMENT "Generating TLS certificates for APP_CRYPTO"
    VERBATIM
  )

  add_custom_target(app_generate_certs
    DEPENDS
      ${generated_server_cert_der}
      ${generated_server_privkey_der}
  )
  add_dependencies(app app_generate_certs)

  foreach(inc_file
    server_cert.der
    server_privkey.der
  )
    generate_inc_file_for_target(app
      ${generated_certs_dir}/${inc_file}
      ${gen_dir}/${inc_file}.inc
    )
  endforeach()
endif()

if(CONFIG_PERSISTENT_STORAGE)
  if(CONFIG_CODE_DATA_RELOCATION_SRAM AND CONFIG_SOC_FLASH_STM32)
    include(${ZEPHYR_BASE}/cmake/modules/extensions.cmake)
    FILE(GLOB flash_driver ${ZEPHYR_BASE}/drivers/flash/flash_stm32*.c)
    zephyr_code_relocate(FILES ${flash_driver} LOCATION RAM)
  else()
    message(FATAL_ERROR "flash storage handling is not defined for this platform")
  endif()
endif()
