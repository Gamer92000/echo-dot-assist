if(CONFIG_HM_XIAO_BOOTLOADER)
  zephyr_library()
  zephyr_library_sources(bootloader.c fatal.c hfxo.c ackie.c ackcount.c)
  # ackcount.c: the radio driver's ack and receive callouts passed through it
  zephyr_link_libraries(-Wl,--wrap=nrf_802154_tx_ack_started -Wl,--wrap=nrf_802154_received_timestamp_raw)
endif()
