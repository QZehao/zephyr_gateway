/**
 * @file protocol_mqtt.h
 * @brief MQTT 客户端模块接口
 *
 * 管理 MQTT 连接、认证、发布，订阅 network_manager 的网络事件。
 * 支持运行时设置 clientId 和可选 TLS（CONFIG_MQTT_LIB_TLS）。
 */

#ifndef PROTOCOL_MQTT_H
#define PROTOCOL_MQTT_H

#include <zeplod/module_base.h>
#include <zephyr/net/tls_credentials.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * 控制命令
 * ============================================================================= */

/** 发布 MQTT 消息（arg 为 const char*[2]: {topic, payload}） */
#define MQTT_CMD_PUBLISH       1

/** 获取连接状态（arg 为 bool* 输出连接状态） */
#define MQTT_CMD_GET_STATUS    2

/* =============================================================================
 * 模块 API
 * ============================================================================= */

/** 获取模块接口指针 */
const module_interface_t* protocol_mqtt_get_interface(void);

/** 检查 MQTT 是否已连接（处于 SUBSCRIBED 状态） */
bool protocol_mqtt_is_connected(void);

/** 发布 MQTT 消息（云上传模块调用） */
int protocol_mqtt_publish(const char* topic, const char* payload, uint16_t payload_len);

/**
 * @brief 设置 MQTT 认证参数（username/password；触发重连）
 *
 * @param username 用户名字符串（NULL 表示与 password 一起清除认证）；容量契约见
 *                 protocol_mqtt.c 中 mqtt_username[192] / mqtt_password[128]
 * @param password 密码字符串
 * @return 0 成功；-EINVAL 参数超出缓冲容量（不再静默截断）
 */
int protocol_mqtt_set_auth(const char* username, const char* password);

/**
 * @brief 设置 MQTT Broker 地址和端口（触发重连）
 *
 * @param addr broker 地址字符串，长度需小于 BROKER_ADDR_MAX_LEN(64)
 * @param port broker 端口
 * @return 0 成功；-EINVAL 参数无效或地址超出缓冲容量（不再静默截断）
 */
int protocol_mqtt_set_broker(const char* addr, uint16_t port);

/**
 * @brief 设置 MQTT clientId（运行时下发，触发重连）
 *
 * @param id 新的 clientId 字符串（不能为空，长度需小于 192）
 * @return 0 成功；-EINVAL 参数无效或超出缓冲容量（不再静默截断）
 *
 * @note 持 client_mutex 写入，置 pending_disconnect 触发重连。
 *       由云 provider（阿里云/腾讯/AWS）在 setup_auth 时调用。
 */
int protocol_mqtt_set_client_id(const char* id);

/**
 * @brief 配置 TLS 传输参数（需 CONFIG_MQTT_LIB_TLS=y）
 *
 * @param tags      证书 sec_tag 数组（最多 3 项）
 * @param count     数组长度
 * @param hostname  SNI 主机名（NULL 表示不设置）
 * @return 0 成功；-EINVAL 参数无效；-ENOTSUP TLS 未启用
 *
 * @note 仅在 CONFIG_MQTT_LIB_TLS 启用时有效，否则返回 -ENOTSUP。
 *       证书实体需在调用本函数前通过 tls_credential_add() 预先注入（部署期操作）。
 */
int protocol_mqtt_set_tls(const sec_tag_t* tags, size_t count, const char* hostname);

/**
 * @brief 清除 TLS 配置，切换回明文连接（需 CONFIG_MQTT_LIB_TLS=y）
 *
 * @note 未启用 TLS 时为空操作。
 */
void protocol_mqtt_clear_tls(void);

/** 获取连接统计 */
void protocol_mqtt_get_stats(uint32_t* connect_count, uint32_t* disconnect_count, uint32_t* msg_count);

#ifdef __cplusplus
}
#endif

#endif /* PROTOCOL_MQTT_H */
