/**
 * @file cloud_aliyun.c
 * @brief 阿里云 IoT 平台 Provider 实现
 *
 * 对接阿里云物联网平台，支持一机一密认证、Alink JSON 格式、物模型 Topic。
 *
 * MQTT 连接参数（阿里云一机一密）：
 *   - Broker:  ${productKey}.iot-as-mqtt.${region}.aliyuncs.com:1883
 *   - ClientId: ${clientId}|securemode=3,signmethod=hmacsha1|
 *   - Username: ${deviceName}&${productKey}
 *   - Password: HMAC-SHA1(DeviceSecret, content) → Hex（需 crypto 库）
 *
 * TODO: 当前密码使用 DeviceSecret 直接填充，生产环境需集成 mbedtls
 *       计算 HMAC-SHA1 签名。
 * 参考文档:
 *   - [阿里云 IoT MQTT 一机一密接入](https://help.aliyun.com/zh/iot/developer-reference/device-authentication)
 */

#include "cloud_aliyun.h"
#include "cloud_crypto.h"
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

LOG_MODULE_REGISTER(cloud_aliyun, CONFIG_SYS_LOG_LEVEL);

/* =============================================================================
 * 配置（从 Kconfig 获取）
 * ============================================================================= */

#define ALIYUN_PRODUCT_KEY  CONFIG_GATEWAY_ALIYUN_PRODUCT_KEY
#define ALIYUN_DEVICE_NAME  CONFIG_GATEWAY_ALIYUN_DEVICE_NAME
#define ALIYUN_DEVICE_SECRET CONFIG_GATEWAY_ALIYUN_DEVICE_SECRET
#define ALIYUN_REGION       CONFIG_GATEWAY_ALIYUN_REGION

/* =============================================================================
 * Topic 与 Payload 构造
 * ============================================================================= */

/* 物模型属性上报 Topic */
#define ALIYUN_TOPIC_PROPERTY_POST  "/sys/%s/%s/thing/event/property/post"

/* Alink JSON 格式：物模型属性上报 */
static int aliyun_build_property_json(const char* json_in, char* buf, size_t buf_len)
{
    /* 将轻量 JSON 包装为 Alink 格式 */
    /* 示例: {"id":"123","version":"1.0","params":{"Current":1.23},"method":"thing.event.property.post"} */
    return snprintf(buf, buf_len,
                    "{\"id\":\"%lu\",\"version\":\"1.0\","
                    "\"params\":%s,"
                    "\"method\":\"thing.event.property.post\"}",
                    (unsigned long)k_uptime_get(), json_in);
}

/* =============================================================================
 * Provider 回调实现
 * ============================================================================= */

static int cloud_aliyun_publish(cloud_msg_type_t type, const char* json_payload)
{
    (void)type;

    char topic[128];
    int  topic_len = snprintf(topic, sizeof(topic), ALIYUN_TOPIC_PROPERTY_POST,
                               ALIYUN_PRODUCT_KEY, ALIYUN_DEVICE_NAME);
    if (topic_len <= 0 || (size_t)topic_len >= sizeof(topic)) {
        LOG_ERR("阿里云 Topic 拼装失败或被截断 (len=%d)", topic_len);
        return -ENOMEM;
    }

    char payload[256];
    int len = aliyun_build_property_json(json_payload, payload, sizeof(payload));
    if (len <= 0 || (size_t)len >= sizeof(payload)) {
        return -ENOMEM;
    }

    return protocol_mqtt_publish(topic, payload, (uint16_t)strlen(payload));
}

static bool cloud_aliyun_is_connected(void)
{
    return protocol_mqtt_is_connected();
}

static void cloud_aliyun_print_status(const struct shell* sh)
{
    shell_print(sh, "  [阿里云 IoT] %s",
                cloud_aliyun_is_connected() ? "已连接" : "未连接");
    shell_print(sh, "    Product:  %s", ALIYUN_PRODUCT_KEY);
    shell_print(sh, "    Device:   %s", ALIYUN_DEVICE_NAME);
    shell_print(sh, "    Region:   %s", ALIYUN_REGION);
#if defined(CONFIG_MBEDTLS)
    shell_print(sh, "    认证：HMAC-SHA1 一机一密（已启用）");
#else
    shell_print(sh, "    认证：DeviceSecret 占位（CONFIG_MBEDTLS 未启用）");
#endif
}

/* =============================================================================
 * Provider 接口实例
 * ============================================================================= */

static const cloud_provider_t s_cloud_aliyun_provider = {
    .name         = "aliyun",
    .init         = NULL,
    .start        = NULL,
    .stop         = NULL,
    .shutdown     = NULL,
    .is_connected = cloud_aliyun_is_connected,
    .publish      = cloud_aliyun_publish,
    .print_status = cloud_aliyun_print_status,
};

const cloud_provider_t* cloud_aliyun_get_provider(void)
{
    return &s_cloud_aliyun_provider;
}

/* =============================================================================
 * 内部辅助函数
 * ============================================================================= */

/**
 * @brief 配置阿里云 MQTT 一机一密认证参数
 *
 * 按阿里云规范构造：
 *   - clientId = ${rawClientId}|securemode=3,signmethod=hmacsha1|
 *   - username = ${deviceName}&${productKey}
 *   - password = HMAC-SHA1(DeviceSecret, content)，content 按字典序拼接
 *   - content  = "clientId${rawClientId}deviceName${deviceName}productKey${productKey}"
 *   - broker   = ${productKey}.iot-as-mqtt.${region}.aliyuncs.com:1883
 *
 * 若 HMAC 不可用（CONFIG_MBEDTLS 未启用），回退到 DeviceSecret 占位密码并 LOG_WRN。
 *
 * @return 0 成功；snprintf 拼装被截断/失败返回 -ENOMEM；下发 MQTT 层参数失败
 *         返回 protocol_mqtt_set_* 对应的负错误码。
 */
static int aliyun_setup_auth(void)
{
    /* rawClientId：用于签名的原始 client id（不带后缀），Broker 端校验签名时用此值 */
    const char* raw_client_id = CONFIG_GATEWAY_MQTT_CLIENT_ID;
    int len;

    /* 构造 MQTT clientId（含鉴权参数后缀） */
    char client_id_full[192];
    len = snprintf(client_id_full, sizeof(client_id_full),
                    "%s|securemode=3,signmethod=hmacsha1|", raw_client_id);
    if (len <= 0 || (size_t)len >= sizeof(client_id_full)) {
        LOG_ERR("阿里云 clientId 拼装失败或被截断 (len=%d)", len);
        return -ENOMEM;
    }

    /* username = deviceName&productKey */
    char username[96];
    len = snprintf(username, sizeof(username), "%s&%s",
                    ALIYUN_DEVICE_NAME, ALIYUN_PRODUCT_KEY);
    if (len <= 0 || (size_t)len >= sizeof(username)) {
        LOG_ERR("阿里云 username 拼装失败或被截断 (len=%d)", len);
        return -ENOMEM;
    }

    /* 构造签名内容（按字典序：clientId/deviceName/productKey） */
    char content[256];
    len = snprintf(content, sizeof(content),
                    "clientId%sdeviceName%sproductKey%s",
                    raw_client_id, ALIYUN_DEVICE_NAME, ALIYUN_PRODUCT_KEY);
    if (len <= 0 || (size_t)len >= sizeof(content)) {
        LOG_ERR("阿里云签名内容拼装失败或被截断 (len=%d)", len);
        return -ENOMEM;
    }

    /* HMAC-SHA1 签名，输出 40 字符小写 hex */
    char password[64] = {0};
    int ret = gateway_hmac_sha1_hex(
        (const uint8_t*)ALIYUN_DEVICE_SECRET, strlen(ALIYUN_DEVICE_SECRET),
        (const uint8_t*)content, strlen(content),
        password, sizeof(password));

    if (ret != 0) {
        /* HMAC 不可用（mbedtls 未启用或计算失败），回退到 DeviceSecret 占位 */
        LOG_WRN("HMAC-SHA1 签名失败 (%d)，回退到 DeviceSecret 占位密码（仅测试用途）", ret);
        strncpy(password, ALIYUN_DEVICE_SECRET, sizeof(password) - 1);
        password[sizeof(password) - 1] = '\0';
    }

    /* 构造阿里云 broker 地址 */
    char broker[128];
    len = snprintf(broker, sizeof(broker),
                    "%s.iot-as-mqtt.%s.aliyuncs.com", ALIYUN_PRODUCT_KEY, ALIYUN_REGION);
    if (len <= 0 || (size_t)len >= sizeof(broker)) {
        LOG_ERR("阿里云 broker 地址拼装失败或被截断 (len=%d)", len);
        return -ENOMEM;
    }

    /* 下发到 MQTT 层，逐一检查返回值 */
    ret = protocol_mqtt_set_client_id(client_id_full);
    if (ret != 0) {
        LOG_ERR("设置 MQTT clientId 失败: %d", ret);
        return ret;
    }
    ret = protocol_mqtt_set_auth(username, password);
    if (ret != 0) {
        LOG_ERR("设置 MQTT 认证失败: %d", ret);
        return ret;
    }
    ret = protocol_mqtt_set_broker(broker, 1883);
    if (ret != 0) {
        LOG_ERR("设置 MQTT broker 失败: %d", ret);
        return ret;
    }

    LOG_INF("阿里云 MQTT 参数: clientId=%s user=%s broker=%s",
            client_id_full, username, broker);
    return 0;
}

/* =============================================================================
 * 模块接口
 * ============================================================================= */

static int cloud_aliyun_init(void* config)
{
    ARG_UNUSED(config);
    LOG_INF("初始化阿里云 Provider...");

    int ret = cloud_provider_register(cloud_aliyun_get_provider());
    if (ret != 0) {
        LOG_ERR("阿里云 Provider 注册失败: %d", ret);
        return ret;
    }

    LOG_INF("阿里云 Provider 初始化完成");
    return 0;
}

static int cloud_aliyun_start(void)
{
    int ret = aliyun_setup_auth();
    if (ret != 0) {
        LOG_ERR("阿里云 Provider 启动失败: %d", ret);
        return ret;
    }
    LOG_INF("阿里云 Provider 已启动");
    return 0;
}

static int cloud_aliyun_stop(void)
{
    protocol_mqtt_set_auth(NULL, NULL);
    LOG_INF("阿里云 Provider 已停止");
    return 0;
}

static int cloud_aliyun_shutdown(void)
{
    return 0;
}

static module_status_t cloud_aliyun_get_status(void)
{
    return MODULE_STATUS_RUNNING;
}

static int cloud_aliyun_control(int cmd, void* arg)
{
    ARG_UNUSED(cmd);
    ARG_UNUSED(arg);
    return -1;
}

static void cloud_aliyun_on_event(const event_t* event, void* user_data)
{
    ARG_UNUSED(event);
    ARG_UNUSED(user_data);
}

/* =============================================================================
 * 模块接口声明与自动注册
 * ============================================================================= */

static const char* const cloud_aliyun_deps[] = {"protocol_mqtt", NULL};

DECLARE_MODULE_INTERFACE_WITH_DEPS(cloud_aliyun, cloud_aliyun_deps);

const module_interface_t* cloud_aliyun_get_interface(void)
{
    return &cloud_aliyun_interface;
}

static int cloud_aliyun_auto_register(void)
{
    uint32_t module_id;
    if (module_manager_register(cloud_aliyun_get_interface(), NULL, &module_id) != 0) {
        LOG_ERR("阿里云 Provider 注册失败");
        return -EIO;
    }
    LOG_INF("阿里云 Provider 已注册 (id=%u)", module_id);
    return 0;
}

SYS_INIT(cloud_aliyun_auto_register, POST_KERNEL, GATEWAY_INIT_PRIO_CLOUD_ALIYUN);
