/**
 * @file cloud_tencent.c
 * @brief 腾讯云 IoT Hub Provider 实现
 *
 * TODO: 实现设备密钥签名和腾讯云特定 Topic 构造。
 * 当前为框架实现，publish 直接走 protocol_eth（Topic 和 Payload 需后续适配）。
 */

#include "cloud_tencent.h"
#include "protocol_eth.h"
#include "gateway_config.h"
#include "app_config.h"

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <string.h>

LOG_MODULE_REGISTER(cloud_tencent, CONFIG_SYS_LOG_LEVEL);

/* =============================================================================
 * Provider 回调实现
 * ============================================================================= */

static int cloud_tencent_publish(cloud_msg_type_t type, const char* json_payload)
{
    /* TODO: 使用腾讯云 Topic 格式：${productID}/${deviceName}/event */
    ARG_UNUSED(type);
    ARG_UNUSED(json_payload);
    LOG_WRN("腾讯云 Provider publish 暂未实现");
    return -ENOTSUP;
}

static bool cloud_tencent_is_connected(void)
{
    return protocol_eth_is_connected();
}

static void cloud_tencent_print_status(const struct shell* sh)
{
    shell_print(sh, "  [腾讯云 IoT] %s",
                cloud_tencent_is_connected() ? "已连接" : "未连接");
    shell_print(sh, "    说明：当前为框架实现，认证与 Topic 映射待填充");
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
 * 模块接口
 * ============================================================================= */

static int cloud_tencent_init(void* config)
{
    ARG_UNUSED(config);
    LOG_INF("初始化腾讯云 Provider...");
    cloud_provider_register(cloud_tencent_get_provider());
    LOG_INF("腾讯云 Provider 初始化完成（框架）");
    return 0;
}

static int cloud_tencent_start(void)
{
    LOG_INF("腾讯云 Provider 已启动");
    return 0;
}

static int cloud_tencent_stop(void)
{
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

static const char* const cloud_tencent_deps[] = {"protocol_eth", NULL};

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
