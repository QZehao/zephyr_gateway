/**
 * @file gateway_config.h
 * @brief 工业网关模块公共配置宏
 */

#ifndef GATEWAY_CONFIG_H
#define GATEWAY_CONFIG_H

#include <zephyr/autoconf.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * 模块使能开关（从 Kconfig 映射）
 * ============================================================================= */

#ifdef CONFIG_GATEWAY_CAN_ENABLE
#define GATEWAY_CAN_ENABLE 1
#else
#define GATEWAY_CAN_ENABLE 0
#endif

#ifdef CONFIG_GATEWAY_MODBUS_ENABLE
#define GATEWAY_MODBUS_ENABLE 1
#else
#define GATEWAY_MODBUS_ENABLE 0
#endif

#ifdef CONFIG_GATEWAY_MQTT_ENABLE
#define GATEWAY_MQTT_ENABLE 1
#else
#define GATEWAY_MQTT_ENABLE 0
#endif

#ifdef CONFIG_GATEWAY_ANOMALY_ENABLE
#define GATEWAY_ANOMALY_ENABLE 1
#else
#define GATEWAY_ANOMALY_ENABLE 0
#endif

#ifdef CONFIG_GATEWAY_CLOUD_UPLOAD_ENABLE
#define GATEWAY_CLOUD_UPLOAD_ENABLE 1
#else
#define GATEWAY_CLOUD_UPLOAD_ENABLE 0
#endif

#ifdef CONFIG_GATEWAY_CLOUD_PROVIDER_PRIVATE
#define GATEWAY_CLOUD_PROVIDER_PRIVATE 1
#else
#define GATEWAY_CLOUD_PROVIDER_PRIVATE 0
#endif

#ifdef CONFIG_GATEWAY_CLOUD_PROVIDER_ALIYUN
#define GATEWAY_CLOUD_PROVIDER_ALIYUN 1
#else
#define GATEWAY_CLOUD_PROVIDER_ALIYUN 0
#endif

#ifdef CONFIG_GATEWAY_CLOUD_PROVIDER_TENCENT
#define GATEWAY_CLOUD_PROVIDER_TENCENT 1
#else
#define GATEWAY_CLOUD_PROVIDER_TENCENT 0
#endif

#ifdef CONFIG_GATEWAY_CLOUD_PROVIDER_AWS
#define GATEWAY_CLOUD_PROVIDER_AWS 1
#else
#define GATEWAY_CLOUD_PROVIDER_AWS 0
#endif

#ifdef CONFIG_GATEWAY_OFFLINE_CACHE_ENABLE
#define GATEWAY_OFFLINE_CACHE_ENABLE 1
#else
#define GATEWAY_OFFLINE_CACHE_ENABLE 0
#endif

#ifdef CONFIG_GATEWAY_WEBSHELL_ENABLE
#define GATEWAY_WEBSHELL_ENABLE 1
#else
#define GATEWAY_WEBSHELL_ENABLE 0
#endif

#ifdef CONFIG_GATEWAY_DATA_SIMULATOR_ENABLE
#define GATEWAY_DATA_SIMULATOR_ENABLE 1
#else
#define GATEWAY_DATA_SIMULATOR_ENABLE 0
#endif

/* =============================================================================
 * 线程配置
 * ============================================================================= */

#define GATEWAY_THREAD_PRIORITY_DEFAULT   5
#define GATEWAY_THREAD_STACK_SIZE_DEFAULT 2048
#define GATEWAY_THREAD_STACK_SIZE_LARGE   4096  /* 网络相关 */

/* =============================================================================
 * 协议模块配置
 * ============================================================================= */

/* CAN */
#define GATEWAY_CAN_FILTER_ID_DEFAULT     0x100
#define GATEWAY_CAN_FILTER_MASK_DEFAULT   0x7F0
#define GATEWAY_CAN_RX_BUF_SIZE           32
#define GATEWAY_CAN_CHANNEL_ID_MASK       0x0F  /* CAN ID 低4位作为 channel_id */

/* Modbus */
#define GATEWAY_MODBUS_BAUDRATE_DEFAULT   9600
#define GATEWAY_MODBUS_POLL_INTERVAL_MS   1000
#define GATEWAY_MODBUS_SLAVE_ID_DEFAULT   1
#define GATEWAY_MODBUS_REG_START_DEFAULT  0
#define GATEWAY_MODBUS_REG_COUNT_DEFAULT  8
#define GATEWAY_MODBUS_FRAME_MAX_LEN      256
#define GATEWAY_MODBUS_RX_TIMEOUT_MS      500

/* MQTT / Ethernet */
#define GATEWAY_MQTT_KEEPALIVE_S          60
#define GATEWAY_MQTT_RECONNECT_MIN_MS     1000
#define GATEWAY_MQTT_RECONNECT_MAX_MS     60000

/* =============================================================================
 * 异常检测配置
 * ============================================================================= */

#define GATEWAY_ANOMALY_WINDOW_SIZE       CONFIG_GATEWAY_ANOMALY_WINDOW_SIZE
#define GATEWAY_ANOMALY_MAX_SENSOR_TYPES  5
#define GATEWAY_ANOMALY_WARNING_SIGMA     CONFIG_GATEWAY_ANOMALY_WARNING_SIGMA
#define GATEWAY_ANOMALY_CRITICAL_SIGMA    CONFIG_GATEWAY_ANOMALY_CRITICAL_SIGMA
#define GATEWAY_ANOMALY_EMERGENCY_SIGMA   CONFIG_GATEWAY_ANOMALY_EMERGENCY_SIGMA

/* =============================================================================
 * 离线缓存配置
 * ============================================================================= */

#define GATEWAY_CACHE_MAX_ENTRIES         CONFIG_GATEWAY_OFFLINE_CACHE_MAX_ENTRIES
#define GATEWAY_CACHE_ENTRY_SIZE          CONFIG_GATEWAY_OFFLINE_CACHE_ENTRY_SIZE

/* =============================================================================
 * 版本号
 * ============================================================================= */

/* 版本号：构建系统提供 PROJECT_VERSION_* 时自动同步，否则回退到 1.0.0 */
#ifdef PROJECT_VERSION_MAJOR
#define GATEWAY_VERSION_MAJOR PROJECT_VERSION_MAJOR
#define GATEWAY_VERSION_MINOR PROJECT_VERSION_MINOR
#define GATEWAY_VERSION_PATCH PROJECT_VERSION_PATCH
#else
#define GATEWAY_VERSION_MAJOR 1
#define GATEWAY_VERSION_MINOR 0
#define GATEWAY_VERSION_PATCH 0
#endif

/* =============================================================================
 * SYS_INIT 优先级（在 framework 已有优先级之后）
 * ============================================================================= */

/* APPLICATION 阶段：通道创建（在所有 POST_KERNEL 模块自动注册之后、gateway_init 之前） */
#define GATEWAY_INIT_PRIO_CHANNELS        68

#define GATEWAY_INIT_PRIO_SIMULATOR       69
#define GATEWAY_INIT_PRIO_PROTOCOL_CAN    70
#define GATEWAY_INIT_PRIO_PROTOCOL_MODBUS 71
#define GATEWAY_INIT_PRIO_NETWORK_MANAGER 72
#define GATEWAY_INIT_PRIO_PROTOCOL_MQTT   73
#define GATEWAY_INIT_PRIO_CLOUD_PROVIDER  74
#define GATEWAY_INIT_PRIO_CLOUD_PRIVATE   75
#define GATEWAY_INIT_PRIO_CLOUD_ALIYUN    76
#define GATEWAY_INIT_PRIO_CLOUD_TENCENT   77
#define GATEWAY_INIT_PRIO_CLOUD_AWS       78
#define GATEWAY_INIT_PRIO_ANOMALY         79
#define GATEWAY_INIT_PRIO_CLOUD_UPLOAD    80
#define GATEWAY_INIT_PRIO_OFFLINE_CACHE   81
#define GATEWAY_INIT_PRIO_WEBSHELL        82
#define GATEWAY_INIT_PRIO_ENTRY           83

#ifdef __cplusplus
}
#endif

#endif /* GATEWAY_CONFIG_H */
