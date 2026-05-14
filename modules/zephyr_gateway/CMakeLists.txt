zephyr_library()

zephyr_include_directories(${CMAKE_CURRENT_LIST_DIR}/../../src/gateway)
zephyr_include_directories(${CMAKE_CURRENT_LIST_DIR}/../../framework/src/modules)
zephyr_include_directories(${CMAKE_CURRENT_LIST_DIR}/../../framework/src/core)
zephyr_include_directories(${CMAKE_CURRENT_LIST_DIR}/../../framework/src/services)
zephyr_include_directories(${CMAKE_CURRENT_LIST_DIR}/../../framework/src/app)

zephyr_library_sources_ifdef(
  CONFIG_ZEPHYR_GATEWAY_BUSINESS
  ${CMAKE_CURRENT_LIST_DIR}/../../src/gateway/gateway_init.c
  ${CMAKE_CURRENT_LIST_DIR}/../../src/gateway/gateway_events.c
)

zephyr_library_sources_ifdef(
  CONFIG_GATEWAY_CAN_ENABLE
  ${CMAKE_CURRENT_LIST_DIR}/../../src/gateway/protocol_can.c
)

zephyr_library_sources_ifdef(
  CONFIG_GATEWAY_MODBUS_ENABLE
  ${CMAKE_CURRENT_LIST_DIR}/../../src/gateway/protocol_modbus.c
)

zephyr_library_sources_ifdef(
  CONFIG_GATEWAY_MQTT_ENABLE
  ${CMAKE_CURRENT_LIST_DIR}/../../src/gateway/protocol_eth.c
)

zephyr_library_sources_ifdef(
  CONFIG_GATEWAY_ANOMALY_ENABLE
  ${CMAKE_CURRENT_LIST_DIR}/../../src/gateway/anomaly_detection.c
)

zephyr_library_sources_ifdef(
  CONFIG_GATEWAY_CLOUD_UPLOAD_ENABLE
  ${CMAKE_CURRENT_LIST_DIR}/../../src/gateway/cloud_provider.c
  ${CMAKE_CURRENT_LIST_DIR}/../../src/gateway/cloud_upload.c
)

zephyr_library_sources_ifdef(
  CONFIG_GATEWAY_CLOUD_PROVIDER_PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/../../src/gateway/cloud_private.c
)

zephyr_library_sources_ifdef(
  CONFIG_GATEWAY_CLOUD_PROVIDER_ALIYUN
  ${CMAKE_CURRENT_LIST_DIR}/../../src/gateway/cloud_aliyun.c
)

zephyr_library_sources_ifdef(
  CONFIG_GATEWAY_CLOUD_PROVIDER_TENCENT
  ${CMAKE_CURRENT_LIST_DIR}/../../src/gateway/cloud_tencent.c
)

zephyr_library_sources_ifdef(
  CONFIG_GATEWAY_CLOUD_PROVIDER_AWS
  ${CMAKE_CURRENT_LIST_DIR}/../../src/gateway/cloud_aws.c
)

zephyr_library_sources_ifdef(
  CONFIG_GATEWAY_OFFLINE_CACHE_ENABLE
  ${CMAKE_CURRENT_LIST_DIR}/../../src/gateway/offline_cache.c
)

zephyr_library_sources_ifdef(
  CONFIG_GATEWAY_WEBSHELL_ENABLE
  ${CMAKE_CURRENT_LIST_DIR}/../../src/gateway/webshell.c
)
