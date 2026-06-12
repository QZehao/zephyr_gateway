/**
 * @file cloud_upload.c
 * @brief MQTT 数据上云模块实现
 *
 * 订阅传感器数据和异常事件，格式化为 JSON，通过已注册的 cloud provider 发送。
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
#include "data_bus.h"

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
    struct k_mutex  lock;         /* 保护全局状态，防止 Shell 与 data_bus 回调竞争 */
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

/* data_bus consumer 句柄 */
static data_bus_consumer_t* g_cloud_sensor_consumer = NULL;

/* =============================================================================
 * 前向声明
 * ============================================================================= */

static void cloud_on_sensor_data(const gateway_sensor_data_t *sensor);
static void cloud_on_anomaly(const gateway_anomaly_event_t *evt);
static void cloud_handle_offline_unlocked(uint8_t data_type, const char *json);
static void cloud_sensor_data_cb(data_bus_channel_t* ch,
                                  data_bus_block_t* block,
                                  void* user_data);

/* =============================================================================
 * 模块接口实现
 * ============================================================================= */

int cloud_upload_init(void *config)
{
    ARG_UNUSED(config);
    LOG_INF("初始化云上传模块...");

    memset(&g_cloud, 0, sizeof(g_cloud));
    k_mutex_init(&g_cloud.lock);
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

    /* 注册到 data_bus sensor 通道 */
    if (g_sensor_channel != NULL && g_cloud_sensor_consumer == NULL) {
        data_bus_consumer_cfg_t cfg = {
            .name      = "cloud_upload",
            .callback  = cloud_sensor_data_cb,
        };
        int ret = data_bus_consumer_register(g_sensor_channel, &cfg,
                                              &g_cloud_sensor_consumer);
        if (ret != 0) {
            LOG_ERR("注册 sensor consumer 失败: %d", ret);
            g_cloud_sensor_consumer = NULL;
        } else {
            LOG_INF("cloud_upload 已订阅 data_bus 'sensor'");
        }
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

    if (g_cloud_sensor_consumer != NULL) {
        data_bus_consumer_unregister(g_cloud_sensor_consumer);
        g_cloud_sensor_consumer = NULL;
        LOG_INF("cloud_upload 已注销 data_bus consumer");
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
    k_mutex_lock(&g_cloud.lock, K_FOREVER);

    switch (cmd)
    {
    case CLOUD_CMD_GET_STATS: {
        if (arg != NULL)
        {
            uint32_t *s = (uint32_t *)arg;
            /* stats 读取不持有锁：直接读取 uint32_t 在 ARM 上原子 */
            s[0] = g_cloud.success_count;
            s[1] = g_cloud.fail_count;
            s[2] = g_cloud.cached_count;
        }
        k_mutex_unlock(&g_cloud.lock);
        return 0;
    }
    case CLOUD_CMD_FORCE_UPLOAD: {
        /* 强制上传最后一条数据：需持有锁防止与 cloud_on_sensor_data 竞争 */
        if (g_cloud.has_pending_sensor)
        {
            gateway_sensor_data_t sensor_copy = g_cloud.last_sensor;
            k_mutex_unlock(&g_cloud.lock);
            cloud_on_sensor_data(&sensor_copy);
            return 0;
        }
        k_mutex_unlock(&g_cloud.lock);
        return 0;
    }
    default:
        k_mutex_unlock(&g_cloud.lock);
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

    k_mutex_lock(&g_cloud.lock, K_FOREVER);

    /* 保存最新传感器数据 */
    g_cloud.last_sensor = *sensor;
    g_cloud.has_pending_sensor = true;

    /* 检查上传间隔 */
    uint32_t now = k_uptime_get_32();
    if ((now - g_cloud.last_upload_ms) < g_cloud.upload_interval_ms)
    {
        k_mutex_unlock(&g_cloud.lock);
        return;
    }
    g_cloud.last_upload_ms = now;

    /* 格式化为 JSON */
    char json[CLOUD_JSON_BUF_SIZE];
    int len = gateway_sensor_to_json(sensor, json, sizeof(json));
    if (len <= 0 || (size_t)len >= sizeof(json))
    {
        k_mutex_unlock(&g_cloud.lock);
        return;
    }

    /* 向所有已注册的 Provider 发布 */
    uint8_t success_count = 0;
    uint8_t fail_count = 0;
    (void)cloud_provider_publish_all(CLOUD_MSG_TELEMETRY, json,
                                      &success_count, &fail_count);
    if (success_count == 0 && fail_count > 0) {
        /* 全部 Provider 失败：交给离线缓存 */
        cloud_handle_offline_unlocked(0, json);
    } else {
        g_cloud.success_count++;
        LOG_DBG("云上传成功: %s", json);
    }

    g_cloud.has_pending_sensor = false;
    k_mutex_unlock(&g_cloud.lock);
}

/* 无锁版本：调用者必须持有 g_cloud.lock */
static void cloud_handle_offline_unlocked(uint8_t data_type, const char *json)
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

static void cloud_on_anomaly(const gateway_anomaly_event_t *evt)
{
    if (evt == NULL)
        return;

    k_mutex_lock(&g_cloud.lock, K_FOREVER);

    char json[CLOUD_JSON_BUF_SIZE];
    int len = gateway_anomaly_to_json(evt, json, sizeof(json));
    if (len <= 0 || (size_t)len >= sizeof(json))
    {
        k_mutex_unlock(&g_cloud.lock);
        return;
    }

    /* 异常数据立即发送，不受间隔限制 */
    uint8_t success_count = 0;
    uint8_t fail_count = 0;
    (void)cloud_provider_publish_all(CLOUD_MSG_ANOMALY, json,
                                      &success_count, &fail_count);
    if (success_count == 0 && fail_count > 0) {
        cloud_handle_offline_unlocked(1, json);
    } else {
        g_cloud.success_count++;
    }

    k_mutex_unlock(&g_cloud.lock);
}

/* =============================================================================
 * data_bus 消费者回调
 * ============================================================================= */

static void cloud_sensor_data_cb(data_bus_channel_t* ch,
                                  data_bus_block_t* block,
                                  void* user_data)
{
    ARG_UNUSED(ch);
    ARG_UNUSED(user_data);

    /* NOTE: data_bus_consumer_unregister() does not wait for in-flight
     * dispatches (see framework/src/data_bus/data_bus_consumer.c).
     * This status check is the only barrier preventing post-stop work
     * from executing; do not remove. */
    if (g_cloud.status != MODULE_STATUS_RUNNING) {
        return;
    }
    void* payload = data_bus_block_ptr(block);
    if (payload == NULL ||
        data_bus_block_len(block) != sizeof(gateway_sensor_data_t)) {
        return;
    }

    cloud_on_sensor_data((const gateway_sensor_data_t*)payload);
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
