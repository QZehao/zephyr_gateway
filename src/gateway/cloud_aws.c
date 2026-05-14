/**
 * @file cloud_aws.c
 * @brief AWS IoT Core Provider 实现
 *
 * TODO: 需配置 TLS（CONFIG_NET_TLS、CONFIG_MBEDTLS）和 X.509 证书链加载。
 * 当前为框架实现，publish 返回不支持。
 */

#include "cloud_aws.h"
#include "protocol_eth.h"
#include "gateway_config.h"
#include "app_config.h"

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <string.h>

LOG_MODULE_REGISTER(cloud_aws, CONFIG_SYS_LOG_LEVEL);

/* =============================================================================
 * Provider 回调实现
 * ============================================================================= */

static int cloud_aws_publish(cloud_msg_type_t type, const char* json_payload)
{
    /* TODO: 使用 AWS Shadow Topic：$aws/things/${thingName}/shadow/update */
    /* TODO: 需先启用 TLS 并加载证书 */
    ARG_UNUSED(type);
    ARG_UNUSED(json_payload);
    LOG_WRN("AWS Provider publish 暂未实现（等待 TLS 支持）");
    return -ENOTSUP;
}

static bool cloud_aws_is_connected(void)
{
    return protocol_eth_is_connected();
}

static void cloud_aws_print_status(const struct shell* sh)
{
    shell_print(sh, "  [AWS IoT Core] %s",
                cloud_aws_is_connected() ? "已连接" : "未连接");
    shell_print(sh, "    说明：当前为框架实现，TLS + 证书认证待填充");
}

/* =============================================================================
 * Provider 接口实例
 * ============================================================================= */

static const cloud_provider_t s_cloud_aws_provider = {
    .name         = "aws",
    .init         = NULL,
    .start        = NULL,
    .stop         = NULL,
    .shutdown     = NULL,
    .is_connected = cloud_aws_is_connected,
    .publish      = cloud_aws_publish,
    .print_status = cloud_aws_print_status,
};

const cloud_provider_t* cloud_aws_get_provider(void)
{
    return &s_cloud_aws_provider;
}

/* =============================================================================
 * 模块接口
 * ============================================================================= */

static int cloud_aws_init(void* config)
{
    ARG_UNUSED(config);
    LOG_INF("初始化 AWS Provider...");
    cloud_provider_register(cloud_aws_get_provider());
    LOG_INF("AWS Provider 初始化完成（框架）");
    return 0;
}

static int cloud_aws_start(void)
{
    LOG_INF("AWS Provider 已启动");
    return 0;
}

static int cloud_aws_stop(void)
{
    LOG_INF("AWS Provider 已停止");
    return 0;
}

static int cloud_aws_shutdown(void)
{
    return 0;
}

static module_status_t cloud_aws_get_status(void)
{
    return MODULE_STATUS_RUNNING;
}

static int cloud_aws_control(int cmd, void* arg)
{
    ARG_UNUSED(cmd);
    ARG_UNUSED(arg);
    return -1;
}

static void cloud_aws_on_event(const event_t* event, void* user_data)
{
    ARG_UNUSED(event);
    ARG_UNUSED(user_data);
}

/* =============================================================================
 * 模块接口声明与自动注册
 * ============================================================================= */

static const char* const cloud_aws_deps[] = {"protocol_eth", NULL};

DECLARE_MODULE_INTERFACE_WITH_DEPS(cloud_aws, cloud_aws_deps);

const module_interface_t* cloud_aws_get_interface(void)
{
    return &cloud_aws_interface;
}

static int cloud_aws_auto_register(void)
{
    uint32_t module_id;
    if (module_manager_register(cloud_aws_get_interface(), NULL, &module_id) != 0) {
        LOG_ERR("AWS Provider 注册失败");
        return -EIO;
    }
    LOG_INF("AWS Provider 已注册 (id=%u)", module_id);
    return 0;
}

SYS_INIT(cloud_aws_auto_register, POST_KERNEL, GATEWAY_INIT_PRIO_CLOUD_AWS);
