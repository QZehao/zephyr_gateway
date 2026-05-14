/**
 * @file cloud_aws.c
 * @brief AWS IoT Core Provider 实现
 *
 * 对接 AWS IoT Core，支持 X.509 证书认证、Device Shadow Topic。
 *
 * MQTT 连接参数（AWS IoT Core）：
 *   - Broker:   ${endpoint}-ats.iot.${region}.amazonaws.com:8883
 *   - 端口:     8883（TLS，必须启用 CONFIG_NET_TLS + CONFIG_MBEDTLS）
 *   - 认证:     X.509 客户端证书（双向 TLS）
 *   - ClientId: ${thingName}
 *
 * Topic 格式：
 *   - Shadow 更新: $aws/things/${thingName}/shadow/update
 *   - Shadow delta: $aws/things/${thingName}/shadow/update/delta
 *
 * TODO:
 *   1. 启用 TLS：CONFIG_NET_TLS=y, CONFIG_MBEDTLS=y
 *   2. 加载证书链：AmazonRootCA1.pem + device-cert.pem + private-key.pem
 *   3. 配置 mqtt_client 的 transport 为 TLS
 * 参考文档:
 *   - [AWS IoT Core Protocols](https://docs.aws.amazon.com/iot/latest/developerguide/protocols.html)
 */

#include "cloud_aws.h"
#include "protocol_eth.h"
#include "gateway_config.h"
#include "app_config.h"

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>
#include <stdio.h>
#include <string.h>

#include "module_manager.h"

LOG_MODULE_REGISTER(cloud_aws, CONFIG_SYS_LOG_LEVEL);

/* =============================================================================
 * 配置（从 Kconfig 获取）
 * ============================================================================= */

#define AWS_ENDPOINT  CONFIG_GATEWAY_AWS_ENDPOINT
#define AWS_THING_NAME CONFIG_GATEWAY_AWS_THING_NAME
#define AWS_REGION    CONFIG_GATEWAY_AWS_REGION

/* =============================================================================
 * Provider 回调实现
 * ============================================================================= */

static int cloud_aws_publish(cloud_msg_type_t type, const char* json_payload)
{
    (void)type;

    /* AWS Shadow update topic */
    char topic[128];
    snprintf(topic, sizeof(topic),
             "$aws/things/%s/shadow/update", AWS_THING_NAME);

    /* AWS IoT 要求 TLS (8883)，当前项目未启用 TLS，publish 会失败。
     * 先尝试用明文端口发送（仅用于测试），生产必须启用 TLS。 */
    return protocol_eth_mqtt_publish(topic, json_payload,
                                     (uint16_t)strlen(json_payload));
}

static bool cloud_aws_is_connected(void)
{
    return protocol_eth_is_connected();
}

static void cloud_aws_print_status(const struct shell* sh)
{
    shell_print(sh, "  [AWS IoT Core] %s",
                cloud_aws_is_connected() ? "已连接" : "未连接");
    shell_print(sh, "    Endpoint: %s", AWS_ENDPOINT);
    shell_print(sh, "    Thing:    %s", AWS_THING_NAME);
    shell_print(sh, "    Region:   %s", AWS_REGION);
    shell_print(sh, "    说明：需启用 TLS (CONFIG_NET_TLS + CONFIG_MBEDTLS)"
                    "并加载 X.509 证书");
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
 * 内部辅助函数
 * ============================================================================= */

static void aws_setup_connection(void)
{
    /* TODO: 配置 TLS 传输
     * 1. 设置 Broker 为 AWS_ENDPOINT:8883
     * 2. 加载证书链到 mqtt_client.tls 结构
     * 3. 设置 SNI (Server Name Indication)
     */

    /* 清除认证（AWS 使用证书，不需要用户名密码） */
    protocol_eth_mqtt_set_auth(NULL, NULL);

    LOG_INF("AWS IoT Core 连接参数: endpoint=%s thing=%s",
            AWS_ENDPOINT, AWS_THING_NAME);
    LOG_WRN("AWS 当前未启用 TLS，生产环境必须配置 X.509 证书");
}

/* =============================================================================
 * 模块接口
 * ============================================================================= */

static int cloud_aws_init(void* config)
{
    ARG_UNUSED(config);
    LOG_INF("初始化 AWS Provider...");
    cloud_provider_register(cloud_aws_get_provider());
    LOG_INF("AWS Provider 初始化完成");
    return 0;
}

static int cloud_aws_start(void)
{
    aws_setup_connection();
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
