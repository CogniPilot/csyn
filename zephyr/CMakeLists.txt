# Copyright (c) 2026, CogniPilot Foundation
# SPDX-License-Identifier: Apache-2.0

if(CONFIG_CSYN)

include(FetchContent)

# csyn owns the synapse_fbs schema contract: this pinned release fixes topic
# ids, keyexprs, payload layouts, and the generated topic printer for every
# csyn consumer.
set(CSYN_SYNAPSE_FBS_C_URL
  "https://github.com/CogniPilot/synapse_fbs/releases/download/v0.4.0/synapse_fbs-c.tar.gz"
  CACHE STRING "URL for the generated synapse_fbs C archive"
)
set(CSYN_SYNAPSE_FBS_C_URL_HASH
  "SHA256=06dea200cf3ce02008729e1b230f72cd929f5e69abdc267ded25277a11208d61"
  CACHE STRING "CMake URL_HASH for the generated synapse_fbs C archive"
)
FetchContent_Declare(synapse_fbs_c
  URL ${CSYN_SYNAPSE_FBS_C_URL}
  URL_HASH ${CSYN_SYNAPSE_FBS_C_URL_HASH}
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
