/**
 * @file cloud_upload.c
 * @brief MQTT 数据上云模块实现
 *
 * 订阅传感器数据和异常事件，格式化为 JSON，通过 protocol_eth 的 MQTT 发送。
 * 断网时发布 EVENT_TYPE_CLOUD_UPLOAD 供 offline_cache 存储。
 */

#include "cloud_upload.h"
#include "gateway_events.h"
#include "gateway_config.h"
#include "cloud_provider.h"
#include "app_config.h"

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include "event_system.h"
#include "module_manager.h"

LOG_MODULE_REGISTER(cloud_upload, CONFIG_SYS_LOG_LEVEL);

/* =============================================================================
 * 配置
 * ============================================================================= */

#define CLOUD_JSON_BUF_SIZE 256

/* =============================================================================
 * 内部数据结构
 * ============================================================================= */

typedef struct
{
    module_status_t status;
    /* 统计 */
    uint32_t success_count;
    uint32_t fail_count;
    uint32_t cached_count;
    /* 定时器 */
    uint32_t last_upload_ms;
    uint32_t upload_interval_ms;
    /* 缓存 */
    gateway_sensor_data_t last_sensor;
    bool has_pending_sensor;
} cloud_upload_cb_t;

/* =============================================================================
 * 静态变量
 * ============================================================================= */

static cloud_upload_cb_t g_cloud;

/* =============================================================================
 * 前向声明
 * ============================================================================= */

static void cloud_on_sensor_data(const gateway_sensor_data_t *sensor);
static void cloud_on_anomaly(const gateway_anomaly_event_t *evt);
static void cloud_handle_offline(uint8_t data_type, const char *json);

/* =============================================================================
 * 模块接口实现
 * ============================================================================= */

int cloud_upload_init(void *config)
{
    ARG_UNUSED(config);
    LOG_INF("初始化云上传模块...");

    memset(&g_cloud, 0, sizeof(g_cloud));
    g_cloud.status = MODULE_STATUS_INITIALIZED;
    g_cloud.upload_interval_ms = CONFIG_GATEWAY_CLOUD_UPLOAD_INTERVAL_MS;

    event_register_type(EVENT_TYPE_CLOUD_UPLOAD, "cloud_upload");

    LOG_INF("云上传模块初始化完成");
    return 0;
}

int cloud_upload_start(void)
{
    if (g_cloud.status != MODULE_STATUS_INITIALIZED &&
        g_cloud.status != MODULE_STATUS_STOPPED)
    {
        return -1;
    }
    g_cloud.status = MODULE_STATUS_RUNNING;
    g_cloud.last_upload_ms = k_uptime_get_32();
    LOG_INF("云上传模块已启动");
    return 0;
}

int cloud_upload_stop(void)
{
    if (g_cloud.status != MODULE_STATUS_RUNNING)
    {
        return 0;
    }
    g_cloud.status = MODULE_STATUS_STOPPED;
    LOG_INF("云上传模块已停止");
    return 0;
}

int cloud_upload_shutdown(void)
{
    cloud_upload_stop();
    g_cloud.status = MODULE_STATUS_UNINITIALIZED;
    return 0;
}

void cloud_upload_on_event(const event_t *event, void *user_data)
{
    ARG_UNUSED(user_data);
    if (event == NULL || g_cloud.status != MODULE_STATUS_RUNNING)
    {
        return;
    }

    switch (event->type)
    {
    case EVENT_TYPE_SENSOR_DATA:
        if (event->data_len == sizeof(gateway_sensor_data_t))
        {
            cloud_on_sensor_data((const gateway_sensor_data_t *)gateway_event_data(event));
        }
        break;

    case EVENT_TYPE_ANOMALY_WARNING:
    case EVENT_TYPE_ANOMALY_CRITICAL:
    case EVENT_TYPE_ANOMALY_EMERGENCY:
        if (event->data_len == sizeof(gateway_anomaly_event_t))
        {
            cloud_on_anomaly((const gateway_anomaly_event_t *)gateway_event_data(event));
        }
        break;
    }
}

module_status_t cloud_upload_get_status(void)
{
    return g_cloud.status;
}

int cloud_upload_control(int cmd, void *arg)
{
    switch (cmd)
    {
    case CLOUD_CMD_GET_STATS:
        if (arg != NULL)
        {
            uint32_t *s = (uint32_t *)arg;
            cloud_upload_get_stats(&s[0], &s[1], &s[2]);
        }
        return 0;
    case CLOUD_CMD_FORCE_UPLOAD:
        /* 强制上传最后一条数据 */
        if (g_cloud.has_pending_sensor)
        {
            cloud_on_sensor_data(&g_cloud.last_sensor);
        }
        return 0;
    default:
        return -1;
    }
}

/* =============================================================================
 * 模块特定 API
 * ============================================================================= */

void cloud_upload_get_stats(uint32_t *success_count, uint32_t *fail_count, uint32_t *cached_count)
{
    if (success_count != NULL)
        *success_count = g_cloud.success_count;
    if (fail_count != NULL)
        *fail_count = g_cloud.fail_count;
    if (cached_count != NULL)
        *cached_count = g_cloud.cached_count;
}

/* =============================================================================
 * 内部函数
 * ============================================================================= */

static void cloud_on_sensor_data(const gateway_sensor_data_t *sensor)
{
    if (sensor == NULL)
        return;

    /* 保存最新传感器数据 */
    g_cloud.last_sensor = *sensor;
    g_cloud.has_pending_sensor = true;

    /* 检查上传间隔 */
    uint32_t now = k_uptime_get_32();
    if ((now - g_cloud.last_upload_ms) < g_cloud.upload_interval_ms)
    {
        return;
    }
    g_cloud.last_upload_ms = now;

    /* 格式化为 JSON */
    char json[CLOUD_JSON_BUF_SIZE];
    int len = gateway_sensor_to_json(sensor, json, sizeof(json));
    if (len <= 0 || (size_t)len >= sizeof(json))
    {
        return;
    }

    /* 向所有已注册的 Provider 发布 */
    int ret = cloud_provider_publish_all(CLOUD_MSG_TELEMETRY, json);
    if (ret != 0)
    {
        /* 至少一个 Provider 失败：交给离线缓存 */
        cloud_handle_offline(0, json);
    }
    else
    {
        g_cloud.success_count++;
        LOG_DBG("云上传成功: %s", json);
    }

    g_cloud.has_pending_sensor = false;
}

static void cloud_on_anomaly(const gateway_anomaly_event_t *evt)
{
    if (evt == NULL)
        return;

    char json[CLOUD_JSON_BUF_SIZE];
    int len = gateway_anomaly_to_json(evt, json, sizeof(json));
    if (len <= 0 || (size_t)len >= sizeof(json))
    {
        return;
    }

    /* 异常数据立即发送，不受间隔限制 */
    int ret = cloud_provider_publish_all(CLOUD_MSG_ANOMALY, json);
    if (ret != 0)
    {
        cloud_handle_offline(1, json);
    }
    else
    {
        g_cloud.success_count++;
    }
}

static void cloud_handle_offline(uint8_t data_type, const char *json)
{
    gateway_cloud_data_t cache_data = {
        .timestamp = k_uptime_get_32(),
        .data_type = data_type,
    };

    strncpy(cache_data.json_payload, json, sizeof(cache_data.json_payload) - 1);
    cache_data.json_payload[sizeof(cache_data.json_payload) - 1] = '\0';

    event_publish_copy(EVENT_TYPE_CLOUD_UPLOAD, EVENT_PRIORITY_NORMAL,
                       &cache_data, sizeof(cache_data));
    g_cloud.cached_count++;
    LOG_INF("数据已转存离线缓存");
}

/* =============================================================================
 * 模块接口声明与自动注册
 * ============================================================================= */

static const char *const cloud_upload_deps[] = {NULL};

DECLARE_MODULE_INTERFACE_WITH_DEPS(cloud_upload, cloud_upload_deps);

const module_interface_t *cloud_upload_get_interface(void)
{
    return &cloud_upload_interface;
}

static int cloud_upload_auto_register(void)
{
    uint32_t module_id;
    if (module_manager_register(cloud_upload_get_interface(), NULL, &module_id) != 0)
    {
        LOG_ERR("云上传模块注册失败");
        return -EIO;
    }
    LOG_INF("云上传模块已注册 (id=%u)", module_id);
    return 0;
}

SYS_INIT(cloud_upload_auto_register, POST_KERNEL, GATEWAY_INIT_PRIO_CLOUD_UPLOAD);
