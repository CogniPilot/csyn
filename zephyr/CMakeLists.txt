# Copyright (c) 2026, CogniPilot Foundation
# SPDX-License-Identifier: Apache-2.0

if(CONFIG_CSYN)

include(FetchContent)

set(CSYN_DIR ${CMAKE_CURRENT_LIST_DIR})

# csyn owns the synapse_fbs schema contract: this pinned release fixes topic
# ids, keyexprs, payload layouts, and the generated topic printer for every
# csyn consumer.
set(CSYN_SYNAPSE_FBS_C_URL
  "https://github.com/CogniPilot/synapse_fbs/releases/download/v0.6.0/synapse_fbs-c.tar.gz"
  CACHE STRING "URL for the generated synapse_fbs C archive"
)
set(CSYN_SYNAPSE_FBS_C_URL_HASH
  "SHA256=b9a25c3b4b8105934023c86597d925272aca2c14b07ebf710760fa4928780991"
  CACHE STRING "CMake URL_HASH for the generated synapse_fbs C archive"
)
string(REGEX MATCH "/v([0-9]+\\.[0-9]+\\.[0-9]+)/" _csyn_synapse_fbs_match
  "${CSYN_SYNAPSE_FBS_C_URL}"
)
if(CMAKE_MATCH_1)
  set(CSYN_SYNAPSE_FBS_VERSION "${CMAKE_MATCH_1}")
else()
  set(CSYN_SYNAPSE_FBS_VERSION "unknown")
endif()

configure_file(
  ${CSYN_DIR}/include/csyn/csyn_version.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/include/csyn/csyn_version.h
  @ONLY
)

FetchContent_Declare(synapse_fbs_c
  URL ${CSYN_SYNAPSE_FBS_C_URL}
  URL_HASH ${CSYN_SYNAPSE_FBS_C_URL_HASH}
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(synapse_fbs_c)

zephyr_library_named(csyn)

zephyr_linker_sources(DATA_SECTIONS ${CSYN_DIR}/csyn_sections.ld)

zephyr_include_directories(${CSYN_DIR}/include)
zephyr_include_directories(${CMAKE_CURRENT_BINARY_DIR}/include)
zephyr_system_include_directories(${synapse_fbs_c_SOURCE_DIR}/include)

zephyr_library_sources(
  ${CSYN_DIR}/src/csyn.c
  ${CSYN_DIR}/src/csyn_codec.c
)

zephyr_library_sources_ifdef(CONFIG_CSYN_ZROS_BRIDGE
  ${CSYN_DIR}/src/csyn_zros_bridge.c
)

if(CONFIG_CSYN_SHELL)
  zephyr_library_sources(
    ${CSYN_DIR}/src/csyn_shell.c
    ${synapse_fbs_c_SOURCE_DIR}/src/topic_print.c
  )
endif()

zephyr_library_sources_ifdef(CONFIG_CSYN_NATIVE_UDP
  ${CSYN_DIR}/src/csyn_native_udp.c
)

if(CONFIG_CSYN_ZENOH)
  zephyr_library_sources(${CSYN_DIR}/src/csyn_zenoh.c)
  zephyr_library_include_directories(
    ${ZEPHYR_ZENOH_PICO_MODULE_DIR}/src/system/zephyr
  )
endif()

endif()
