/**
 * @file protocol_eth.h
 * @brief 以太网/MQTT 连接管理模块
 */

#ifndef PROTOCOL_ETH_H
#define PROTOCOL_ETH_H

#include "module_base.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * 控制命令
 * ============================================================================= */

#define ETH_CMD_PUBLISH       1  /* 发布 MQTT 消息 */
#define ETH_CMD_GET_STATUS    2  /* 获取连接状态 */

/* =============================================================================
 * 模块 API
 * ============================================================================= */

const module_interface_t* protocol_eth_get_interface(void);

/** 检查 MQTT 是否已连接 */
bool protocol_eth_is_connected(void);

/** 发布 MQTT 消息（云上传模块调用） */
int protocol_eth_mqtt_publish(const char* topic, const char* payload, uint16_t payload_len);

/** 获取连接统计 */
void protocol_eth_get_stats(uint32_t* connect_count, uint32_t* disconnect_count, uint32_t* msg_count);

#ifdef __cplusplus
}
#endif

#endif /* PROTOCOL_ETH_H */
