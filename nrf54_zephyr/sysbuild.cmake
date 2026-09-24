# A history-enabled TAG image must never be paired with the stock-slot MCUboot.
set(_ruuvi_tag_history_conf "${APP_DIR}/nrf54l15tag_history.conf")
set(_ruuvi_tag_history_overlay "${APP_DIR}/nrf54l15tag_history.overlay")

if(_ruuvi_tag_history_overlay IN_LIST EXTRA_DTC_OVERLAY_FILE OR
   _ruuvi_tag_history_conf IN_LIST EXTRA_CONF_FILE)
  if(NOT BOARD MATCHES "^nrf54l15tag(/|$)")
    message(FATAL_ERROR "The TAG history layout is only qualified for nrf54l15tag")
  endif()
  if(NOT _ruuvi_tag_history_overlay IN_LIST EXTRA_DTC_OVERLAY_FILE OR
     NOT _ruuvi_tag_history_conf IN_LIST EXTRA_CONF_FILE)
    message(FATAL_ERROR "TAG history needs both its config fragment and overlay")
  endif()
  if(NOT SB_CONFIG_BOOTLOADER_MCUBOOT OR SB_CONFIG_PARTITION_MANAGER)
    message(FATAL_ERROR "TAG history sysbuild needs MCUboot and fixed devicetree partitions")
  endif()
  if(DEFINED mcuboot_EXTRA_DTC_OVERLAY_FILE AND
     NOT "${mcuboot_EXTRA_DTC_OVERLAY_FILE}" STREQUAL "" AND
     NOT "${mcuboot_EXTRA_DTC_OVERLAY_FILE}" STREQUAL "${_ruuvi_tag_history_overlay}")
    message(FATAL_ERROR "MCUboot overlay conflicts with the TAG history slot layout")
  endif()

  set(mcuboot_EXTRA_DTC_OVERLAY_FILE "${_ruuvi_tag_history_overlay}"
      CACHE STRING "Match MCUboot slots to the Ruuvi TAG history partition layout" FORCE)
  message(STATUS "Using one TAG history slot layout for MCUboot and the main application")
elseif(DEFINED mcuboot_EXTRA_DTC_OVERLAY_FILE AND
       "${mcuboot_EXTRA_DTC_OVERLAY_FILE}" STREQUAL "${_ruuvi_tag_history_overlay}")
  message(FATAL_ERROR "TAG history MCUboot overlay has no matching app layout; use a fresh build")
endif()
