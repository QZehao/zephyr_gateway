/**
 * @file cloud_private.c
 * @brief 私有 MQTT Broker Provider 实现
 *
 * 基于 protocol_eth 的通用 MQTT 能力，对接私有/自部署 MQTT Broker。
 * 行为与重构前的 cloud_upload 直接发 MQTT 完全一致。
 */

#include "cloud_private.h"
#include "protocol_eth.h"
#include "gateway_config.h"
#include "app_config.h"

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <string.h>

#include "module_manager.h"

LOG_MODULE_REGISTER(cloud_private, CONFIG_SYS_LOG_LEVEL);

/* =============================================================================
 * Provider 回调实现
 * ============================================================================= */

static int cloud_private_publish(cloud_msg_type_t type, const char* json_payload)
{
    const char* topic;

    switch (type) {
    case CLOUD_MSG_TELEMETRY:
        topic = CONFIG_GATEWAY_MQTT_TOPIC_TELEMETRY;
        break;
    case CLOUD_MSG_ANOMALY:
        topic = CONFIG_GATEWAY_MQTT_TOPIC_ANOMALY;
        break;
    default:
        return -EINVAL;
    }

    return protocol_eth_mqtt_publish(topic, json_payload,
                                     (uint16_t)strlen(json_payload));
}

static bool cloud_private_is_connected(void)
{
    return protocol_eth_is_connected();
}

static void cloud_private_print_status(const struct shell* sh)
{
    shell_print(sh, "  [Private MQTT] %s",
                cloud_private_is_connected() ? "已连接" : "未连接");
    shell_print(sh, "    Broker: %s:%d",
                CONFIG_GATEWAY_MQTT_BROKER_ADDR,
                CONFIG_GATEWAY_MQTT_BROKER_PORT);
    shell_print(sh, "    Client: %s", CONFIG_GATEWAY_MQTT_CLIENT_ID);
}

/* =============================================================================
 * Provider 接口实例
 * ============================================================================= */

static const cloud_provider_t s_cloud_private_provider = {
    .name         = "private",
    .init         = NULL,
    .start        = NULL,
    .stop         = NULL,
    .shutdown     = NULL,
    .is_connected = cloud_private_is_connected,
    .publish      = cloud_private_publish,
    .print_status = cloud_private_print_status,
};

const cloud_provider_t* cloud_private_get_provider(void)
{
    return &s_cloud_private_provider;
}

/* =============================================================================
 * 模块接口（遵循 framework 模块生命周期）
 * ============================================================================= */

static int cloud_private_init(void* config)
{
    ARG_UNUSED(config);
    LOG_INF("初始化私有云 Provider...");

    cloud_provider_register(cloud_private_get_provider());

    LOG_INF("私有云 Provider 初始化完成");
    return 0;
}

static int cloud_private_start(void)
{
    LOG_INF("私有云 Provider 已启动");
    return 0;
}

static int cloud_private_stop(void)
{
    LOG_INF("私有云 Provider 已停止");
    return 0;
}

static int cloud_private_shutdown(void)
{
    return 0;
}

static module_status_t cloud_private_get_status(void)
{
    return MODULE_STATUS_RUNNING;
}

static int cloud_private_control(int cmd, void* arg)
{
    ARG_UNUSED(cmd);
    ARG_UNUSED(arg);
    return -1;
}

static void cloud_private_on_event(const event_t* event, void* user_data)
{
    ARG_UNUSED(event);
    ARG_UNUSED(user_data);
}

/* =============================================================================
 * 模块接口声明与自动注册
 * ============================================================================= */

static const char* const cloud_private_deps[] = {"protocol_eth", NULL};

DECLARE_MODULE_INTERFACE_WITH_DEPS(cloud_private, cloud_private_deps);

const module_interface_t* cloud_private_get_interface(void)
{
    return &cloud_private_interface;
}

static int cloud_private_auto_register(void)
{
    uint32_t module_id;
    if (module_manager_register(cloud_private_get_interface(), NULL, &module_id) != 0) {
        LOG_ERR("私有云 Provider 注册失败");
        return -EIO;
    }
    LOG_INF("私有云 Provider 已注册 (id=%u)", module_id);
    return 0;
}

SYS_INIT(cloud_private_auto_register, POST_KERNEL, GATEWAY_INIT_PRIO_CLOUD_PRIVATE);
