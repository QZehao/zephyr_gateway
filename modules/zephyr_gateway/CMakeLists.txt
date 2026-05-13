zephyr_library()

zephyr_library_sources_ifdef(
  CONFIG_ZEPHYR_GATEWAY_BUSINESS
  ${CMAKE_CURRENT_LIST_DIR}/../../src/gateway/gateway_init.c)
