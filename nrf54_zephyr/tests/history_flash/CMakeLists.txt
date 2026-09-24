cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ruuvi_history_flash_build)
set(RUUVI_ENDPOINTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../nrf52_oldsdk/src/ruuvi.endpoints.c/src")
target_sources(app PRIVATE src/main.c ../../src/history.c ../../src/history_codec.c
    ../../src/history_flash.c ../../src/history_settings.c ../../src/log_protocol.c
    ../../src/log_service.c "${RUUVI_ENDPOINTS_DIR}/ruuvi_endpoints.c")
target_include_directories(app PRIVATE ../../src "${RUUVI_ENDPOINTS_DIR}")
