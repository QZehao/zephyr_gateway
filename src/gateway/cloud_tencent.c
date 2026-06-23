/**
 * @file cloud_tencent.c
 * @brief 腾讯云 IoT Hub Provider 实现
 *
 * 对接腾讯云物联网通信平台，支持密钥认证、腾讯云 Topic 格式。
 *
 * MQTT 连接参数（腾讯云 IoT Hub）：
 *   - Broker:   ${productId}.iotcloud.tencentdevices.com:1883
 *   - ClientId: ${productId}${deviceName}
 *   - Username: ${productId}${deviceName};${sdkappid};${connid};${expiry}
 *   - Password: HMAC-SHA256 签名（需 crypto 库）
 *
 * Topic 格式：
 *   - 事件上报：${productId}/${deviceName}/event
 *   - 数据通道：${productId}/${deviceName}/data
 *
 * TODO: 当前密码使用 DeviceSecret 直接填充，生产环境需集成 crypto 库
 *       计算 HMAC-SHA256 签名。
 * 参考文档:
 *   - [腾讯云 IoT Hub MQTT 接入](https://cloud.tencent.com/document/product/634/32546)
 */

#include "cloud_tencent.h"
#include "protocol_mqtt.h"
#include "gateway_config.h"
#include <zeplod/app_config.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>
#include <stdio.h>
#include <string.h>

#include <zeplod/module_manager.h>

LOG_MODULE_REGISTER(cloud_tencent, CONFIG_SYS_LOG_LEVEL);

/* =============================================================================
 * 配置（从 Kconfig 获取）
 * ============================================================================= */

#define TENCENT_PRODUCT_ID   CONFIG_GATEWAY_TENCENT_PRODUCT_ID
#define TENCENT_DEVICE_NAME  CONFIG_GATEWAY_TENCENT_DEVICE_NAME
#define TENCENT_DEVICE_SECRET CONFIG_GATEWAY_TENCENT_DEVICE_SECRET

/* =============================================================================
 * Provider 回调实现
 * ============================================================================= */

static int cloud_tencent_publish(cloud_msg_type_t type, const char* json_payload)
{
    char topic[128];

    switch (type) {
    case CLOUD_MSG_TELEMETRY:
        snprintf(topic, sizeof(topic), "%s/%s/event",
                 TENCENT_PRODUCT_ID, TENCENT_DEVICE_NAME);
        break;
    case CLOUD_MSG_ANOMALY:
        snprintf(topic, sizeof(topic), "%s/%s/event",
                 TENCENT_PRODUCT_ID, TENCENT_DEVICE_NAME);
        break;
    default:
        return -EINVAL;
    }

    return protocol_mqtt_publish(topic, json_payload,
                                 (uint16_t)strlen(json_payload));
}

static bool cloud_tencent_is_connected(void)
{
    return protocol_mqtt_is_connected();
}

static void cloud_tencent_print_status(const struct shell* sh)
{
    shell_print(sh, "  [腾讯云 IoT Hub] %s",
                cloud_tencent_is_connected() ? "已连接" : "未连接");
    shell_print(sh, "    Product:  %s", TENCENT_PRODUCT_ID);
    shell_print(sh, "    Device:   %s", TENCENT_DEVICE_NAME);
    shell_print(sh, "    说明：Password 当前为 DeviceSecret 占位，"
                    "生产环境需 HMAC-SHA256 签名");
}

/* =============================================================================
 * Provider 接口实例
 * ============================================================================= */

static const cloud_provider_t s_cloud_tencent_provider = {
    .name         = "tencent",
    .init         = NULL,
    .start        = NULL,
    .stop         = NULL,
    .shutdown     = NULL,
    .is_connected = cloud_tencent_is_connected,
    .publish      = cloud_tencent_publish,
    .print_status = cloud_tencent_print_status,
};

const cloud_provider_t* cloud_tencent_get_provider(void)
{
    return &s_cloud_tencent_provider;
}

/* =============================================================================
 * 内部辅助函数
 * ============================================================================= */

static void tencent_setup_auth(void)
{
    /* 构造腾讯云 MQTT 连接参数 */
    char username[128];
    char client_id[128];

    /* ClientId = productId + deviceName */
    snprintf(client_id, sizeof(client_id), "%s%s",
             TENCENT_PRODUCT_ID, TENCENT_DEVICE_NAME);

    /* Username = productId + deviceName; sdkappid; connid; expiry
     * 简化：sdkappid/connid/expiry 先留空或固定值 */
    snprintf(username, sizeof(username), "%s%s;;;;",
             TENCENT_PRODUCT_ID, TENCENT_DEVICE_NAME);

    /* TODO: Password 应为 HMAC-SHA256 签名，当前暂用 DeviceSecret 占位 */
    const char* password = TENCENT_DEVICE_SECRET;

    protocol_mqtt_set_auth(username, password);

    LOG_INF("腾讯云 MQTT 参数: clientId=%s user=%s", client_id, username);
}

/* =============================================================================
 * 模块接口
 * ============================================================================= */

static int cloud_tencent_init(void* config)
{
    ARG_UNUSED(config);
    LOG_INF("初始化腾讯云 Provider...");
    cloud_provider_register(cloud_tencent_get_provider());
    LOG_INF("腾讯云 Provider 初始化完成");
    return 0;
}

static int cloud_tencent_start(void)
{
    tencent_setup_auth();
    LOG_INF("腾讯云 Provider 已启动");
    return 0;
}

static int cloud_tencent_stop(void)
{
    protocol_mqtt_set_auth(NULL, NULL);
    LOG_INF("腾讯云 Provider 已停止");
    return 0;
}

static int cloud_tencent_shutdown(void)
{
    return 0;
}

static module_status_t cloud_tencent_get_status(void)
{
    return MODULE_STATUS_RUNNING;
}

static int cloud_tencent_control(int cmd, void* arg)
{
    ARG_UNUSED(cmd);
    ARG_UNUSED(arg);
    return -1;
}

static void cloud_tencent_on_event(const event_t* event, void* user_data)
{
    ARG_UNUSED(event);
    ARG_UNUSED(user_data);
}

/* =============================================================================
 * 模块接口声明与自动注册
 * ============================================================================= */

static const char* const cloud_tencent_deps[] = {"protocol_mqtt", NULL};

DECLARE_MODULE_INTERFACE_WITH_DEPS(cloud_tencent, cloud_tencent_deps);

const module_interface_t* cloud_tencent_get_interface(void)
{
    return &cloud_tencent_interface;
}

static int cloud_tencent_auto_register(void)
{
    uint32_t module_id;
    if (module_manager_register(cloud_tencent_get_interface(), NULL, &module_id) != 0) {
        LOG_ERR("腾讯云 Provider 注册失败");
        return -EIO;
    }
    LOG_INF("腾讯云 Provider 已注册 (id=%u)", module_id);
    return 0;
}

SYS_INIT(cloud_tencent_auto_register, POST_KERNEL, GATEWAY_INIT_PRIO_CLOUD_TENCENT);
