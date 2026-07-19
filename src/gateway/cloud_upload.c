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
#include <zeplod/app_config.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include <zeplod/event_system.h>
#include <zeplod/module_manager.h>
#include <zeplod/data_bus.h>

LOG_MODULE_REGISTER(cloud_upload, CONFIG_SYS_LOG_LEVEL);

/* =============================================================================
 * 配置
 * ============================================================================= */

#define CLOUD_JSON_BUF_SIZE 256

/* 专用上传线程：回调只做拷贝入队，JSON 序列化与 cloud_provider_publish_all
 * 均在此线程完成，避免在 data_bus/事件分发线程上下文中做阻塞 MQTT 发送。 */
#define CLOUD_UPLOAD_THREAD_PRIORITY   GATEWAY_THREAD_PRIORITY_DEFAULT
#define CLOUD_UPLOAD_THREAD_STACK_SIZE GATEWAY_THREAD_STACK_SIZE_DEFAULT
#define CLOUD_UPLOAD_QUEUE_DEPTH       4
#define CLOUD_UPLOAD_THREAD_POLL_MS    100  /* k_msgq_get 超时，保证 stop 后快速 join */

/* 零 Provider 场景下 LOG_WRN 限频周期，避免上传间隔内刷屏 */
#define CLOUD_NO_PROVIDER_WARN_INTERVAL_MS 30000

/* =============================================================================
 * 内部数据结构
 * ============================================================================= */

/** 上传队列条目类型 */
typedef enum
{
    CLOUD_UPLOAD_ITEM_SENSOR = 0,  /* 传感器遥测原始样本 */
    CLOUD_UPLOAD_ITEM_ANOMALY,     /* 异常告警原始信息 */
} cloud_upload_item_type_t;

/** 上传队列条目：回调只拷贝原始样本入队，JSON 序列化延后到上传线程完成 */
typedef struct
{
    cloud_upload_item_type_t type;
    union
    {
        gateway_sensor_data_t   sensor;
        gateway_anomaly_event_t anomaly;
    } data;
} cloud_upload_item_t;

typedef struct
{
    module_status_t status;
    struct k_mutex  lock;         /* 保护全局状态，防止 Shell 与上传线程竞争 */
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

    /* 专用上传线程与队列：回调仅 K_NO_WAIT 入队，序列化/发布均在该线程完成 */
    struct k_thread upload_thread;
    K_KERNEL_STACK_MEMBER(upload_stack, CLOUD_UPLOAD_THREAD_STACK_SIZE);
    struct k_msgq   upload_msgq;
    __aligned(sizeof(void*)) uint8_t
        upload_msgq_buf[CLOUD_UPLOAD_QUEUE_DEPTH * sizeof(cloud_upload_item_t)];
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

static int  cloud_on_sensor_data_internal(const gateway_sensor_data_t *sensor, bool force);
static void cloud_on_sensor_data(const gateway_sensor_data_t *sensor);
static void cloud_on_anomaly(const gateway_anomaly_event_t *evt);
static void cloud_handle_offline_unlocked(uint8_t data_type, const char *json);
static void cloud_sensor_data_cb(data_bus_channel_t* ch,
                                  data_bus_block_t* block,
                                  void* user_data);
static void cloud_enqueue_or_fallback(const cloud_upload_item_t *item);
static void cloud_process_upload_item(const cloud_upload_item_t *item);
static void cloud_report_no_upload_unlocked(uint8_t fail_count, uint8_t data_type,
                                             const char *json);
static void cloud_upload_worker_thread(void *p1, void *p2, void *p3);

/* =============================================================================
 * 模块接口实现
 * ============================================================================= */

int cloud_upload_init(void *config)
{
    ARG_UNUSED(config);
    LOG_INF("初始化云上传模块...");

    memset(&g_cloud, 0, sizeof(g_cloud));
    k_mutex_init(&g_cloud.lock);
    k_msgq_init(&g_cloud.upload_msgq, g_cloud.upload_msgq_buf,
                sizeof(cloud_upload_item_t), CLOUD_UPLOAD_QUEUE_DEPTH);
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

    /* 注册到 data_bus sensor 通道：注册失败视为启动失败。此时上传线程尚未创建
     * （线程创建在本函数末尾，晚于此处），直接 return 不会造成线程泄漏，无需
     * 额外的线程回收逻辑——这是改动最小且不引入资源泄漏的方案。 */
    if (g_sensor_channel != NULL && g_cloud_sensor_consumer == NULL) {
        data_bus_consumer_cfg_t cfg = {
            .name      = "cloud_upload",
            .callback  = cloud_sensor_data_cb,
        };
        int ret = data_bus_consumer_register(g_sensor_channel, &cfg,
                                              &g_cloud_sensor_consumer);
        if (ret != 0) {
            LOG_ERR("注册 sensor consumer 失败: %d，云上传模块启动失败", ret);
            g_cloud_sensor_consumer = NULL;
            return ret;
        }
        LOG_INF("cloud_upload 已订阅 data_bus 'sensor'");
    }

    g_cloud.status = MODULE_STATUS_RUNNING;
    g_cloud.last_upload_ms = k_uptime_get_32();

    /* 启动专用上传线程：JSON 序列化与 cloud_provider_publish_all 均在该线程完成，
     * 可重入：stop() 中已 join 旧线程后才会再次进入 INITIALIZED/STOPPED 分支 */
    k_thread_create(
        &g_cloud.upload_thread, g_cloud.upload_stack,
        K_THREAD_STACK_SIZEOF(g_cloud.upload_stack),
        cloud_upload_worker_thread, NULL, NULL, NULL,
        CLOUD_UPLOAD_THREAD_PRIORITY, 0, K_FOREVER);
    k_thread_name_set(&g_cloud.upload_thread, "cloud_upload");
    k_thread_start(&g_cloud.upload_thread);

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

    /* 置位 STOPPED：上传线程在 k_msgq_get 超时（至多 CLOUD_UPLOAD_THREAD_POLL_MS）后
     * 检查状态自然退出，stop 后不再处理已入队数据（符合 LIFECYCLE_CONTRACTS 停后不再发事件的约定） */
    g_cloud.status = MODULE_STATUS_STOPPED;
    k_thread_join(&g_cloud.upload_thread, K_FOREVER);

    /* 清空遗留队列，避免下次 start 后处理陈旧数据 */
    k_msgq_purge(&g_cloud.upload_msgq);

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

    case EVENT_TYPE_CLOUD_REPLAY:
        /* H1 断网续传回放：offline_cache 重播时发布 EVENT_TYPE_CLOUD_REPLAY（回放专用类型），
         * 由此分支直接转发到 cloud_provider_publish_all，绕过上传间隔限制，不再写回离线缓存。
         * 用独立类型与 CLOUD_UPLOAD（缓存语义）分流：断网时 cloud_handle_offline_unlocked
         * 发的是 CLOUD_UPLOAD，本分支不会被触发，杜绝无效重试与误导告警。
         *
         * 若回放途中网络再抖动导致直发失败，或压根没有已注册 Provider（success==0 且
         * fail==0），均允许丢弃（条目已出缓存），LOG_WRN 记录，并区分“全部 Provider
         * 失败”与“无 Provider”两种措辞。本分支刻意保持同步直发（不走上传线程/队列），
         * 仅在事件分发线程上做一次轻量重试，不与常规遥测/异常路径共用队列，避免回放
         * 堆积占用有限队列深度。
         *
         * 锁结构与 cloud_process_upload_item 一致：cloud_provider_publish_all 在
         * g_cloud.lock 之外调用（其内部有自己的 g_provider_lock 保护注册表，且会阻塞
         * 做 MQTT 发送），发布结束后才短暂持锁更新统计，避免事件分发线程被 broker
         * 响应延迟卡死。 */
        if (event->data_len == sizeof(gateway_cloud_data_t))
        {
            const gateway_cloud_data_t *cd =
                (const gateway_cloud_data_t *)gateway_event_data(event);
            if (cd != NULL)
            {
                cloud_msg_type_t msg_type =
                    (cd->data_type == 1) ? CLOUD_MSG_ANOMALY : CLOUD_MSG_TELEMETRY;

                uint8_t ok = 0, fail = 0;
                (void)cloud_provider_publish_all(msg_type, cd->json_payload, &ok, &fail);

                k_mutex_lock(&g_cloud.lock, K_FOREVER);
                if (ok > 0) {
                    g_cloud.success_count++;
                    LOG_DBG("离线回放上传成功: %s", cd->json_payload);
                } else {
                    g_cloud.fail_count++;
                    if (fail > 0) {
                        LOG_WRN("离线回放直发失败，本条丢弃（已出缓存）");
                    } else {
                        LOG_WRN("离线回放时无云 Provider 已注册，本条丢弃（已出缓存）");
                    }
                }
                k_mutex_unlock(&g_cloud.lock);
            }
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
            /* 已持有 g_cloud.lock，stats 读取线程安全 */
            s[0] = g_cloud.success_count;
            s[1] = g_cloud.fail_count;
            s[2] = g_cloud.cached_count;
        }
        k_mutex_unlock(&g_cloud.lock);
        return 0;
    }
    case CLOUD_CMD_FORCE_UPLOAD: {
        /* 强制上传最后一条数据：需持有锁防止与 cloud_on_sensor_data 竞争。
         * force=true 跳过 upload_interval_ms 节流判断，避免复用带节流的
         * cloud_on_sensor_data() 导致本次强制上传被静默吞掉；没有可上传的
         * pending 样本时如实返回 -ENODATA，不再谎报成功。 */
        if (g_cloud.has_pending_sensor)
        {
            gateway_sensor_data_t sensor_copy = g_cloud.last_sensor;
            g_cloud.has_pending_sensor = false;
            k_mutex_unlock(&g_cloud.lock);
            return cloud_on_sensor_data_internal(&sensor_copy, true);
        }
        k_mutex_unlock(&g_cloud.lock);
        return -ENODATA;
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
    /* 与 control 路径（CLOUD_CMD_GET_STATS）对齐，加锁防止读到撕裂的统计值 */
    k_mutex_lock(&g_cloud.lock, K_FOREVER);
    if (success_count != NULL)
        *success_count = g_cloud.success_count;
    if (fail_count != NULL)
        *fail_count = g_cloud.fail_count;
    if (cached_count != NULL)
        *cached_count = g_cloud.cached_count;
    k_mutex_unlock(&g_cloud.lock);
}

/* =============================================================================
 * 内部函数
 * ============================================================================= */

/**
 * @brief 传感器采样处理内部实现：仅做间隔判定与拷贝入队，不做 JSON 序列化/网络发送
 *
 * 在 data_bus 分发线程上下文中被调用，必须保持非阻塞：
 *   - 保存最新样本、按 upload_interval_ms 节流，均为纯内存操作（持锁很短）；
 *   - 真正的 JSON 序列化与 cloud_provider_publish_all 发布延后到专用上传线程完成。
 *
 * @param sensor 传感器样本
 * @param force  true：跳过 upload_interval_ms 节流判断，无条件走入队路径（用于
 *               CLOUD_CMD_FORCE_UPLOAD——调用方已持锁读出 last_sensor 副本并清
 *               空 has_pending_sensor，这里不再重复写 last_sensor/has_pending_sensor，
 *               避免覆盖节流路径下的正常状态）；false 为常规采样回调路径。
 * @return 恒为 0（保留返回值用于未来扩展；当前失败路径均为静默丢弃/退化为离线缓存）
 */
static int cloud_on_sensor_data_internal(const gateway_sensor_data_t *sensor, bool force)
{
    if (sensor == NULL)
        return -EINVAL;

    if (!force) {
        k_mutex_lock(&g_cloud.lock, K_FOREVER);

        /* 保存最新传感器数据 */
        g_cloud.last_sensor = *sensor;
        g_cloud.has_pending_sensor = true;

        /* 检查上传间隔 */
        uint32_t now = k_uptime_get_32();
        if ((now - g_cloud.last_upload_ms) < g_cloud.upload_interval_ms)
        {
            k_mutex_unlock(&g_cloud.lock);
            return 0;
        }
        g_cloud.last_upload_ms = now;
        g_cloud.has_pending_sensor = false;
        k_mutex_unlock(&g_cloud.lock);
    }

    cloud_upload_item_t item = { .type = CLOUD_UPLOAD_ITEM_SENSOR };
    item.data.sensor = *sensor;
    cloud_enqueue_or_fallback(&item);
    return 0;
}

static void cloud_on_sensor_data(const gateway_sensor_data_t *sensor)
{
    (void)cloud_on_sensor_data_internal(sensor, false);
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

/**
 * @brief 异常事件回调：仅拷贝入队，不做 JSON 序列化/网络发送
 *
 * 与 cloud_on_sensor_data 一样，在事件分发线程上下文中被调用，异常数据不受
 * 上传间隔限制，直接入队交给上传线程尽快处理。
 */
static void cloud_on_anomaly(const gateway_anomaly_event_t *evt)
{
    if (evt == NULL)
        return;

    cloud_upload_item_t item = { .type = CLOUD_UPLOAD_ITEM_ANOMALY };
    item.data.anomaly = *evt;
    cloud_enqueue_or_fallback(&item);
}

/**
 * @brief 将采集到的原始数据拷贝入队，交由专用上传线程序列化与发布
 *
 * 仅执行内存拷贝（K_NO_WAIT），不含任何网络 I/O，保证调用者（data_bus/事件
 * 分发线程）不被阻塞。队满时退化为离线缓存直存：本地 JSON 序列化与
 * event_publish_copy() 均为非阻塞操作，因此可以安全地在回调上下文中完成。
 *
 * @param item 待上传条目（按值拷贝入队，调用后调用者的副本可继续复用）
 */
static void cloud_enqueue_or_fallback(const cloud_upload_item_t *item)
{
    int ret = k_msgq_put(&g_cloud.upload_msgq, item, K_NO_WAIT);
    if (ret == 0) {
        return;
    }

    LOG_WRN("上传队列已满 (err=%d)，退化为离线缓存直存", ret);

    char json[CLOUD_JSON_BUF_SIZE];
    int len;
    uint8_t data_type;

    if (item->type == CLOUD_UPLOAD_ITEM_SENSOR) {
        len = gateway_sensor_to_json(&item->data.sensor, json, sizeof(json));
        data_type = 0;
    } else {
        len = gateway_anomaly_to_json(&item->data.anomaly, json, sizeof(json));
        data_type = 1;
    }
    if (len <= 0 || (size_t)len >= sizeof(json)) {
        return;
    }

    k_mutex_lock(&g_cloud.lock, K_FOREVER);
    cloud_handle_offline_unlocked(data_type, json);
    k_mutex_unlock(&g_cloud.lock);
}

/**
 * @brief 上报“本次未能上云”的统一收尾：区分零 Provider 与全部 Provider 失败两种场景
 *
 * @note 调用者必须持有 g_cloud.lock（与 cloud_handle_offline_unlocked 约定一致）。
 *       success_count==0 时，无论 fail_count 是否为 0（即“无 Provider 已注册”与
 *       “全部 Provider 发布失败”两种场景）均视为本次上传失败，统一转入离线缓存，
 *       避免零 Provider 组合被误判为“已成功上传”。
 * @param fail_count 本次 cloud_provider_publish_all 返回的失败 Provider 数
 * @param data_type  离线缓存数据类型（0=传感器, 1=异常）
 * @param json       待缓存的 JSON 载荷
 */
static void cloud_report_no_upload_unlocked(uint8_t fail_count, uint8_t data_type,
                                             const char *json)
{
    if (fail_count == 0) {
        /* success_count==0 且 fail_count==0：没有任何已注册 Provider，
         * 限频提示，避免在上传间隔内刷屏 */
        static uint32_t s_last_warn_ms;
        uint32_t now = k_uptime_get_32();
        if ((now - s_last_warn_ms) >= CLOUD_NO_PROVIDER_WARN_INTERVAL_MS) {
            LOG_WRN("无云 Provider 已注册，数据转入离线缓存");
            s_last_warn_ms = now;
        }
    }
    cloud_handle_offline_unlocked(data_type, json);
}

/**
 * @brief 处理一条上传队列条目：JSON 序列化 + 向所有 Provider 发布
 *
 * 仅在专用上传线程中调用。JSON 序列化与 cloud_provider_publish_all 均在
 * g_cloud.lock 之外执行（前者本就无需持锁，后者内部有自己的 g_provider_lock
 * 保护注册表，且会阻塞做 MQTT 发送）；g_cloud.lock 只在发布结束后短暂持有，
 * 仅用于保护统计计数与离线缓存转存（cloud_report_no_upload_unlocked /
 * cloud_handle_offline_unlocked 要求持锁的契约不变）。这样可避免
 * cloud_on_sensor_data（data_bus 分发线程）在 broker 响应慢时被同一把锁卡死。
 */
static void cloud_process_upload_item(const cloud_upload_item_t *item)
{
    char json[CLOUD_JSON_BUF_SIZE];
    int len;
    cloud_msg_type_t msg_type;
    uint8_t data_type;

    if (item->type == CLOUD_UPLOAD_ITEM_SENSOR) {
        len = gateway_sensor_to_json(&item->data.sensor, json, sizeof(json));
        msg_type = CLOUD_MSG_TELEMETRY;
        data_type = 0;
    } else {
        len = gateway_anomaly_to_json(&item->data.anomaly, json, sizeof(json));
        msg_type = CLOUD_MSG_ANOMALY;
        data_type = 1;
    }
    if (len <= 0 || (size_t)len >= sizeof(json)) {
        return;
    }

    uint8_t success_count = 0;
    uint8_t fail_count = 0;
    (void)cloud_provider_publish_all(msg_type, json, &success_count, &fail_count);

    k_mutex_lock(&g_cloud.lock, K_FOREVER);
    if (success_count == 0) {
        /* 全部 Provider 失败，或压根没有已注册 Provider：均按失败处理 */
        cloud_report_no_upload_unlocked(fail_count, data_type, json);
    } else {
        g_cloud.success_count++;
        LOG_DBG("云上传成功: %s", json);
    }
    k_mutex_unlock(&g_cloud.lock);
}

/**
 * @brief 云上传专用工作线程
 *
 * 阻塞等待队列条目（超时 CLOUD_UPLOAD_THREAD_POLL_MS 以便及时响应 stop），
 * 取出后完成 JSON 序列化与 cloud_provider_publish_all 发布。stop() 置位
 * MODULE_STATUS_STOPPED 后，本线程不再处理已取出但未处理的条目，直接退出。
 */
static void cloud_upload_worker_thread(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    LOG_INF("云上传工作线程已启动");

    cloud_upload_item_t item;
    while (g_cloud.status == MODULE_STATUS_RUNNING) {
        int ret = k_msgq_get(&g_cloud.upload_msgq, &item, K_MSEC(CLOUD_UPLOAD_THREAD_POLL_MS));
        if (ret != 0) {
            continue; /* 超时：重新检查停止标志 */
        }
        if (g_cloud.status != MODULE_STATUS_RUNNING) {
            break; /* stop 期间不再处理已入队数据 */
        }
        cloud_process_upload_item(&item);
    }

    LOG_INF("云上传工作线程已退出");
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
