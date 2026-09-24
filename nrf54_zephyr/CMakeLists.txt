cmake_minimum_required(VERSION 3.20.0)

set(RUUVI_VARIANT "default" CACHE STRING "Legacy application mode")
if(RUUVI_VARIANT STREQUAL "longlife" AND NOT DEFINED CONF_FILE)
    set(CONF_FILE "${CMAKE_CURRENT_SOURCE_DIR}/longlife.conf")
endif()
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ruuvi_nrf54_beacon)

set(RUUVI_ENDPOINTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../nrf52_oldsdk/src/ruuvi.endpoints.c/src")
set(RUUVI_MODE_GATT_ENABLED 1)
set(RUUVI_MODE_HISTORY_ENABLED 1)
if(RUUVI_VARIANT STREQUAL "default")
    set(RUUVI_MODE_ADV_MS 1285)
    set(RUUVI_MODE_REPEATS 2)
    set(RUUVI_MODE_LOG_S 300)
    set(RUUVI_MODE_WDT_MS 360000)
    set(RUUVI_MODE_FORMATS 2)
elseif(RUUVI_VARIANT STREQUAL "debug" OR RUUVI_VARIANT STREQUAL "test")
    set(RUUVI_MODE_ADV_MS 221)
    set(RUUVI_MODE_REPEATS 2)
    set(RUUVI_MODE_LOG_S 1)
    set(RUUVI_MODE_WDT_MS 600000)
    set(RUUVI_MODE_FORMATS 2)
elseif(RUUVI_VARIANT STREQUAL "longmem")
    set(RUUVI_MODE_ADV_MS 1285)
    set(RUUVI_MODE_REPEATS 2)
    set(RUUVI_MODE_LOG_S 1800)
    set(RUUVI_MODE_WDT_MS 360000)
    set(RUUVI_MODE_FORMATS 2)
elseif(RUUVI_VARIANT STREQUAL "longlife")
    set(RUUVI_MODE_GATT_ENABLED 0)
    set(RUUVI_MODE_HISTORY_ENABLED 0)
    set(RUUVI_MODE_ADV_MS 8995)
    set(RUUVI_MODE_REPEATS 1)
    set(RUUVI_MODE_LOG_S 300)
    set(RUUVI_MODE_WDT_MS 360000)
    set(RUUVI_MODE_FORMATS 2)
elseif(RUUVI_VARIANT STREQUAL "uuid")
    set(RUUVI_MODE_ADV_MS 1285)
    set(RUUVI_MODE_REPEATS 7)
    set(RUUVI_MODE_LOG_S 300)
    set(RUUVI_MODE_WDT_MS 360000)
    set(RUUVI_MODE_FORMATS 16)
else()
    message(FATAL_ERROR "Unknown Ruuvi variant: ${RUUVI_VARIANT}")
endif()
if(RUUVI_VARIANT STREQUAL "longlife" AND (CONFIG_BT_PERIPHERAL OR CONFIG_BT_DIS))
    message(FATAL_ERROR "Longlife requires non-connectable longlife.conf; check CONF_FILE")
endif()
set(RUUVI_ENABLED_FORMATS "${RUUVI_MODE_FORMATS}" CACHE STRING "Ruuvi format mask")
math(EXPR RUUVI_CONFIGURED_FORMATS "${RUUVI_ENABLED_FORMATS} & 23")
if(NOT RUUVI_CONFIGURED_FORMATS)
    message(FATAL_ERROR "Enable at least one implemented Ruuvi data format")
endif()
math(EXPR RUUVI_ENCRYPTED_FORMATS "${RUUVI_ENABLED_FORMATS} & 40")
if(RUUVI_ENCRYPTED_FORMATS)
    message(FATAL_ERROR "DF8/FA require provisioned keys and crypto callback wiring before enabling")
endif()

# Shared endpoint codecs do not depend on the nRF5 SDK.
target_sources(app PRIVATE
    src/main.c
    src/formats.c
    src/history.c
    src/history_codec.c
    src/history_flash.c
    src/history_settings.c
    src/log_protocol.c
    src/log_service.c
    src/sensors.c
    src/ui.c
    "${RUUVI_ENDPOINTS_DIR}/ruuvi_endpoint_3.c"
    "${RUUVI_ENDPOINTS_DIR}/ruuvi_endpoint_5.c"
    "${RUUVI_ENDPOINTS_DIR}/ruuvi_endpoint_7.c"
    "${RUUVI_ENDPOINTS_DIR}/ruuvi_endpoint_8.c"
    "${RUUVI_ENDPOINTS_DIR}/ruuvi_endpoint_c5.c"
    "${RUUVI_ENDPOINTS_DIR}/ruuvi_endpoint_fa.c"
    "${RUUVI_ENDPOINTS_DIR}/ruuvi_endpoints.c"
)
if(RUUVI_MODE_GATT_ENABLED)
    target_sources(app PRIVATE src/gatt.c)
endif()
if(CONFIG_NFC_T4T_NRFXLIB)
    target_sources(app PRIVATE src/nfc.c src/nfc_codec.c)
endif()
target_include_directories(app PRIVATE "${RUUVI_ENDPOINTS_DIR}")
target_compile_definitions(app PRIVATE
    RUUVI_ENABLED_FORMATS=${RUUVI_ENABLED_FORMATS}
    RUUVI_BLE_INTERVAL_MS=${RUUVI_MODE_ADV_MS}
    RUUVI_NUM_REPEATS=${RUUVI_MODE_REPEATS}
    RUUVI_LOG_INTERVAL_S=${RUUVI_MODE_LOG_S}
    RUUVI_WDT_TIMEOUT_MS=${RUUVI_MODE_WDT_MS}
    RUUVI_GATT_ENABLED=${RUUVI_MODE_GATT_ENABLED}
    RUUVI_HISTORY_ENABLED=${RUUVI_MODE_HISTORY_ENABLED})
