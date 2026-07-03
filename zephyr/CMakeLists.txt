# Copyright (c) 2026, CogniPilot Foundation
# SPDX-License-Identifier: Apache-2.0

if(CONFIG_CSYN)

include(FetchContent)

# csyn owns the synapse_fbs schema contract: this pinned release fixes topic
# ids, keyexprs, payload layouts, and the generated topic printer for every
# csyn consumer.
FetchContent_Declare(synapse_fbs_c
  URL https://github.com/CogniPilot/synapse_fbs/releases/download/v0.3.0/synapse_fbs-c.tar.gz
  URL_HASH SHA256=4b00b8ef83c9f849059eb662d3f5acb93201a6c1fa17e6ecec2c9886bd1d5944
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(synapse_fbs_c)

zephyr_library_named(csyn)

set(CSYN_DIR ${CMAKE_CURRENT_LIST_DIR})

zephyr_include_directories(${CSYN_DIR}/include)
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
