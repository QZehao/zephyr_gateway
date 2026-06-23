/**
 * @file protocol_mqtt.h
 * @brief MQTT 客户端模块接口
 *
 * 管理 MQTT 连接、认证、发布，订阅 network_manager 的网络事件。
 */

#ifndef PROTOCOL_MQTT_H
#define PROTOCOL_MQTT_H

#include <zeplod/module_base.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * 控制命令
 * ============================================================================= */

#define MQTT_CMD_PUBLISH       1  /* 发布 MQTT 消息 */
#define MQTT_CMD_GET_STATUS    2  /* 获取连接状态 */

/* =============================================================================
 * 模块 API
 * ============================================================================= */

const module_interface_t* protocol_mqtt_get_interface(void);

/** 检查 MQTT 是否已连接 */
bool protocol_mqtt_is_connected(void);

/** 发布 MQTT 消息（云上传模块调用） */
int protocol_mqtt_publish(const char* topic, const char* payload, uint16_t payload_len);

/** 设置 MQTT 认证参数（在连接前调用，触发重连） */
int protocol_mqtt_set_auth(const char* username, const char* password);

/** 设置 MQTT Broker 地址和端口（触发重连） */
int protocol_mqtt_set_broker(const char* addr, uint16_t port);

/** 获取连接统计 */
void protocol_mqtt_get_stats(uint32_t* connect_count, uint32_t* disconnect_count, uint32_t* msg_count);

#ifdef __cplusplus
}
#endif

#endif /* PROTOCOL_MQTT_H */
