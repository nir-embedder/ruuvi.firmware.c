cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ruuvi_gatt_rx_test)

target_sources(app PRIVATE main.c ../../src/gatt.c)
target_include_directories(app PRIVATE
    ../../src
    ../../../nrf52_oldsdk/src/ruuvi.endpoints.c/src)
if(RUUVI_TEST_HISTORY)
    target_compile_definitions(app PRIVATE RUUVI_HISTORY_ENABLED=1)
else()
    target_compile_definitions(app PRIVATE RUUVI_HISTORY_ENABLED=0)
endif()
zephyr_ld_options(-Wl,--wrap=bt_conn_ref -Wl,--wrap=bt_conn_unref
                  -Wl,--wrap=bt_conn_get_info -Wl,--wrap=bt_gatt_notify_cb
                  -Wl,--wrap=bt_gatt_is_subscribed)
