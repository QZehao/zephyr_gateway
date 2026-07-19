/**
 * @file cloud_aws.c
 * @brief AWS IoT Core Provider 实现
 *
 * 对接 AWS IoT Core，支持 X.509 证书认证（双向 TLS）、Device Shadow Topic。
 *
 * MQTT 连接参数（AWS IoT Core）：
 *   - Broker:   ${endpoint}-ats.iot.${region}.amazonaws.com
 *   - 端口:     8883（TLS，CONFIG_GATEWAY_AWS_TLS=y）或 1883（明文测试，不推荐）
 *   - 认证:     X.509 双向 TLS（不需要 username/password）
 *   - ClientId: ${thingName}
 *
 * TLS 钩子（CONFIG_GATEWAY_AWS_TLS=y）：
 *   - 通过 protocol_mqtt_set_tls() 传入证书 sec_tag，SNI 设置为 endpoint
 *   - 证书实体（AmazonRootCA1 + 设备证书 + 私钥）须在部署期通过
 *     tls_credential_add() 注入（不硬编码到固件）
 *   - sec_tag 值由 CONFIG_GATEWAY_AWS_TLS_SEC_TAG 配置（默认 100）
 *
 * Topic 格式：
 *   - Shadow 更新:  $aws/things/${thingName}/shadow/update
 *   - Shadow delta: $aws/things/${thingName}/shadow/update/delta
 *
 * 参考文档:
 *   - https://docs.aws.amazon.com/iot/latest/developerguide/protocols.html
 */

#include "cloud_aws.h"
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
    int  len = snprintf(topic, sizeof(topic),
                         "$aws/things/%s/shadow/update", AWS_THING_NAME);
    if (len <= 0 || (size_t)len >= sizeof(topic)) {
        LOG_ERR("AWS Topic 拼装失败或被截断 (len=%d)", len);
        return -ENOMEM;
    }

    /* AWS IoT 要求 TLS (8883)，当前项目未启用 TLS，publish 会失败。
     * 先尝试用明文端口发送（仅用于测试），生产必须启用 TLS。 */
    return protocol_mqtt_publish(topic, json_payload,
                                 (uint16_t)strlen(json_payload));
}

static bool cloud_aws_is_connected(void)
{
    return protocol_mqtt_is_connected();
}

static void cloud_aws_print_status(const struct shell* sh)
{
    shell_print(sh, "  [AWS IoT Core] %s",
                cloud_aws_is_connected() ? "已连接" : "未连接");
    shell_print(sh, "    Endpoint: %s", AWS_ENDPOINT);
    shell_print(sh, "    Thing:    %s", AWS_THING_NAME);
    shell_print(sh, "    Region:   %s", AWS_REGION);
#if defined(CONFIG_GATEWAY_AWS_TLS)
    shell_print(sh, "    TLS：已启用（sec_tag=%d，证书须部署期注入）",
                CONFIG_GATEWAY_AWS_TLS_SEC_TAG);
#else
    shell_print(sh, "    TLS：未启用（明文 1883，仅测试用途）");
#endif
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

/**
 * @brief 配置 AWS IoT Core 连接参数（含 TLS 钩子链路）
 *
 * - clientId = thingName
 * - 清除 username/password（AWS 使用证书认证，不需要）
 * - broker = ${endpoint}-ats.iot.${region}.amazonaws.com
 *   端口：CONFIG_GATEWAY_AWS_TLS 启用时 8883，否则 1883（仅测试）
 * - TLS 配置（CONFIG_GATEWAY_AWS_TLS 守护）：
 *   通过 protocol_mqtt_set_tls() 传入 sec_tag 和 SNI；
 *   证书实体须部署期通过 tls_credential_add() 注入（此处不加载证书文件）。
 *   TODO（部署期）：在 main 或 app_init 中调用 tls_credential_add() 加载：
 *     TLS_CREDENTIAL_CA_CERTIFICATE   → AmazonRootCA1.pem
 *     TLS_CREDENTIAL_SERVER_CERTIFICATE → device-cert.pem（若双向 TLS 要求）
 *     TLS_CREDENTIAL_PRIVATE_KEY      → private-key.pem
 *
 * @return 0 成功；snprintf 拼装被截断/失败返回 -ENOMEM；下发 MQTT 层参数失败
 *         返回 protocol_mqtt_set_* 对应的负错误码。
 */
static int aws_setup_connection(void)
{
    int ret;

    /* clientId = thingName */
    ret = protocol_mqtt_set_client_id(AWS_THING_NAME);
    if (ret != 0) {
        LOG_ERR("设置 MQTT clientId 失败: %d", ret);
        return ret;
    }

    /* AWS 使用证书认证，不设置 username/password */
    ret = protocol_mqtt_set_auth(NULL, NULL);
    if (ret != 0) {
        LOG_ERR("清除 MQTT 认证失败: %d", ret);
        return ret;
    }

    char broker[128];
    int  len = snprintf(broker, sizeof(broker), "%s", AWS_ENDPOINT);
    if (len <= 0 || (size_t)len >= sizeof(broker)) {
        LOG_ERR("AWS broker 地址拼装失败或被截断 (len=%d)", len);
        return -ENOMEM;
    }

#if defined(CONFIG_GATEWAY_AWS_TLS)
    /* TLS 已启用：设置 sec_tag 和 SNI，证书实体须部署期注入 */
    sec_tag_t tls_sec_tags[] = { CONFIG_GATEWAY_AWS_TLS_SEC_TAG };
    ret = protocol_mqtt_set_tls(tls_sec_tags, ARRAY_SIZE(tls_sec_tags), AWS_ENDPOINT);
    if (ret != 0) {
        LOG_WRN("TLS 配置失败 (%d)，回退到明文连接", ret);
        protocol_mqtt_clear_tls();
    }

    /* broker:8883（TLS 端口） */
    ret = protocol_mqtt_set_broker(broker, 8883);
    if (ret != 0) {
        LOG_ERR("设置 MQTT broker 失败: %d", ret);
        return ret;
    }

    LOG_INF("AWS IoT Core 连接参数: endpoint=%s thing=%s port=8883 (TLS)",
            AWS_ENDPOINT, AWS_THING_NAME);
    LOG_WRN("证书实体须在部署期通过 tls_credential_add() 注入 sec_tag=%d",
            CONFIG_GATEWAY_AWS_TLS_SEC_TAG);
#else
    /* TLS 未启用：使用明文 1883（仅测试，AWS 生产必须启用 TLS） */
    protocol_mqtt_clear_tls();
    ret = protocol_mqtt_set_broker(broker, 1883);
    if (ret != 0) {
        LOG_ERR("设置 MQTT broker 失败: %d", ret);
        return ret;
    }

    LOG_INF("AWS IoT Core 连接参数: endpoint=%s thing=%s port=1883 (明文)",
            AWS_ENDPOINT, AWS_THING_NAME);
    LOG_WRN("AWS TLS 未启用（CONFIG_GATEWAY_AWS_TLS=n），生产环境必须配置 X.509 双向 TLS");
#endif /* CONFIG_GATEWAY_AWS_TLS */

    return 0;
}

/* =============================================================================
 * 模块接口
 * ============================================================================= */

static int cloud_aws_init(void* config)
{
    ARG_UNUSED(config);
    LOG_INF("初始化 AWS Provider...");

    int ret = cloud_provider_register(cloud_aws_get_provider());
    if (ret != 0) {
        LOG_ERR("AWS Provider 注册失败: %d", ret);
        return ret;
    }

    LOG_INF("AWS Provider 初始化完成");
    return 0;
}

static int cloud_aws_start(void)
{
    int ret = aws_setup_connection();
    if (ret != 0) {
        LOG_ERR("AWS Provider 启动失败: %d", ret);
        return ret;
    }
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

static const char* const cloud_aws_deps[] = {"protocol_mqtt", NULL};

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
