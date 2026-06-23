/**
 * @file anomaly_detection.c
 * @brief 自适应阈值异常检测模块实现
 *
 * 滑动窗口统计（均值 + 标准差），三级阈值检测，多维度联动。
 * 通过 data_bus 通道 "sensor" 消费传感器数据，发布 EVENT_TYPE_ANOMALY_* 到 event_system。
 */

#include "anomaly_detection.h"
#include "gateway_events.h"
#include "gateway_config.h"
#include <zeplod/app_config.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <float.h>
#include <math.h>
#include <string.h>

#include <zeplod/event_system.h>
#include <zeplod/module_manager.h>
#include <zeplod/data_bus.h>

LOG_MODULE_REGISTER(anomaly_detection, CONFIG_SYS_LOG_LEVEL);

/* =============================================================================
 * 配置
 * ============================================================================= */

#define ANOMALY_WINDOW_SIZE  GATEWAY_ANOMALY_WINDOW_SIZE
#define MAX_SENSOR_TYPES     GATEWAY_ANOMALY_MAX_SENSOR_TYPES

BUILD_ASSERT(MAX_SENSOR_TYPES >= SENSOR_TYPE_COUNT,
             "MAX_SENSOR_TYPES 必须大于等于 SENSOR_TYPE_COUNT");

/* 异常级别 */
#define ANOMALY_LEVEL_NONE      0
#define ANOMALY_LEVEL_WARNING   1
#define ANOMALY_LEVEL_CRITICAL  2
#define ANOMALY_LEVEL_EMERGENCY 3
#define ANOMALY_STDDEV_MIN_VALID 0.001f

/* =============================================================================
 * 内部数据结构
 * ============================================================================= */

typedef struct {
    float values[ANOMALY_WINDOW_SIZE];
    uint16_t head;       /* 写入位置 */
    uint16_t count;      /* 当前有效数据点数 */
    float sum;           /* 窗口内数据之和（增量维护，O(1) 更新） */
    float sum_sq;        /* 窗口内数据平方之和（增量维护） */
    float mean;
    float stddev;
    bool valid;          /* 窗口是否已满（基线有效） */
} sensor_window_t;

typedef struct {
    module_status_t status;
    struct k_mutex  lock;         /* 保护全局状态，防止 Shell 与 data_bus 回调竞争 */
    sensor_window_t windows[MAX_SENSOR_TYPES];
    /* 阈值（按传感器类型独立配置） */
    float warning_sigma[MAX_SENSOR_TYPES];
    float critical_sigma[MAX_SENSOR_TYPES];
    float emergency_sigma[MAX_SENSOR_TYPES];
    float abs_max_value[MAX_SENSOR_TYPES];  /* 绝对上下限 */
    /* 联动规则 */
    bool multi_dim_enable;   /* 启用多维度联动 */
    /* 统计 */
    uint32_t warning_count;
    uint32_t critical_count;
    uint32_t emergency_count;
    /* 速率限制 */
    uint32_t last_alert_ms[MAX_SENSOR_TYPES];
    uint16_t alert_interval_ms;  /* 同一传感器最小告警间隔 */
    /* 联动状态 */
    uint32_t last_multi_dim_alert_ms;
    /* 最近检测状态（用于多维度联动，基线不包含当前值） */
    float   last_value[MAX_SENSOR_TYPES];
    uint8_t last_level[MAX_SENSOR_TYPES];
    float   last_sigma[MAX_SENSOR_TYPES];
} anomaly_detection_cb_t;

/* =============================================================================
 * 静态变量
 * ============================================================================= */

static anomaly_detection_cb_t g_ad;

/* data_bus consumer 句柄（_start 注册，_stop 注销） */
static data_bus_consumer_t* g_anomaly_sensor_consumer = NULL;

/* =============================================================================
 * 前向声明
 * ============================================================================= */

static void anomaly_on_sensor_data(const gateway_sensor_data_t* sensor);
static void anomaly_update_window(sensor_window_t* win, float value);
static uint8_t anomaly_detect(sensor_window_t* win, float value,
                               uint8_t sensor_type, float* out_sigma);
static void anomaly_publish_event(uint8_t level, const gateway_sensor_data_t* sensor,
                                   float mean, float stddev, float sigma);
static void anomaly_check_multi_dim(void);
static void anomaly_sensor_data_cb(data_bus_channel_t* ch,
                                    data_bus_block_t* block,
                                    void* user_data);
static void anomaly_detection_get_stats_unlocked(uint32_t* warning_count, uint32_t* critical_count,
                                                  uint32_t* emergency_count);

/* =============================================================================
 * 模块接口实现
 * ============================================================================= */

int anomaly_detection_init(void* config)
{
    ARG_UNUSED(config);
    LOG_INF("初始化异常检测模块...");

    memset(&g_ad, 0, sizeof(g_ad));
    k_mutex_init(&g_ad.lock);
    g_ad.status = MODULE_STATUS_INITIALIZED;
    for (int i = 0; i < MAX_SENSOR_TYPES; i++) {
        g_ad.warning_sigma[i]  = (float)CONFIG_GATEWAY_ANOMALY_WARNING_SIGMA / 10.0f;
        g_ad.critical_sigma[i] = (float)CONFIG_GATEWAY_ANOMALY_CRITICAL_SIGMA / 10.0f;
        g_ad.emergency_sigma[i]= (float)CONFIG_GATEWAY_ANOMALY_EMERGENCY_SIGMA / 10.0f;
        g_ad.abs_max_value[i]  = FLT_MAX;  /* 默认禁用绝对上限 */
    }
    /* 为特定传感器设置合理的物理上限（可选） */
    g_ad.abs_max_value[SENSOR_TYPE_TEMPERATURE] = 200.0f;   /* °C */
    g_ad.abs_max_value[SENSOR_TYPE_VOLTAGE]     = 500.0f;   /* V */
    g_ad.abs_max_value[SENSOR_TYPE_CURRENT]     = 100.0f;   /* A */
    g_ad.abs_max_value[SENSOR_TYPE_PRESSURE]    = 10000.0f; /* kPa */
    g_ad.abs_max_value[SENSOR_TYPE_HUMIDITY]    = 100.0f;   /* % */
    g_ad.multi_dim_enable = true;
    g_ad.alert_interval_ms = 5000;  /* 5s 内同一传感器只告警一次 */

    event_register_type(EVENT_TYPE_ANOMALY_WARNING, "anomaly_warning");
    event_register_type(EVENT_TYPE_ANOMALY_CRITICAL, "anomaly_critical");
    event_register_type(EVENT_TYPE_ANOMALY_EMERGENCY, "anomaly_emergency");

    LOG_INF("异常检测模块初始化完成");
    return 0;
}

int anomaly_detection_start(void)
{
    if (g_ad.status != MODULE_STATUS_INITIALIZED && g_ad.status != MODULE_STATUS_STOPPED) {
        return -1;
    }

    /* 注册到 data_bus sensor 通道 */
    if (g_sensor_channel != NULL && g_anomaly_sensor_consumer == NULL) {
        data_bus_consumer_cfg_t cfg = {
            .name       = "anomaly_detection",
            .callback   = anomaly_sensor_data_cb,
            /* manual_release = false：回调返回后框架自动 release */
        };
        int ret = data_bus_consumer_register(g_sensor_channel, &cfg,
                                              &g_anomaly_sensor_consumer);
        if (ret != 0) {
            LOG_ERR("注册 sensor consumer 失败: %d", ret);
            g_anomaly_sensor_consumer = NULL;
            /* 继续启动：on_event 路径仍可用作 fallback */
        } else {
            LOG_INF("anomaly_detection 已订阅 data_bus 'sensor'");
        }
    }

    g_ad.status = MODULE_STATUS_RUNNING;
    LOG_INF("异常检测模块已启动");
    return 0;
}

int anomaly_detection_stop(void)
{
    if (g_ad.status != MODULE_STATUS_RUNNING) {
        return 0;
    }

    /* 注销 bus consumer */
    if (g_anomaly_sensor_consumer != NULL) {
        data_bus_consumer_unregister(g_anomaly_sensor_consumer);
        g_anomaly_sensor_consumer = NULL;
        LOG_INF("anomaly_detection 已注销 data_bus consumer");
    }

    g_ad.status = MODULE_STATUS_STOPPED;
    LOG_INF("异常检测模块已停止");
    return 0;
}

int anomaly_detection_shutdown(void)
{
    anomaly_detection_stop();
    g_ad.status = MODULE_STATUS_UNINITIALIZED;
    return 0;
}

void anomaly_detection_on_event(const event_t* event, void* user_data)
{
    ARG_UNUSED(user_data);
    ARG_UNUSED(event);
    /* sensor 数据已迁移到 data_bus；当前模块不订阅任何 event_system 事件 */
}

module_status_t anomaly_detection_get_status(void)
{
    return g_ad.status;
}

int anomaly_detection_control(int cmd, void* arg)
{
    k_mutex_lock(&g_ad.lock, K_FOREVER);

    switch (cmd) {
    case ANOMALY_CMD_GET_STATS:
        if (arg != NULL) {
            uint32_t* s = (uint32_t*)arg;
            anomaly_detection_get_stats_unlocked(&s[0], &s[1], &s[2]);
        }
        k_mutex_unlock(&g_ad.lock);
        return 0;
    case ANOMALY_CMD_SET_THRESHOLD:
        if (arg != NULL) {
            anomaly_threshold_cmd_t* tcmd = (anomaly_threshold_cmd_t*)arg;
            g_ad.warning_sigma[tcmd->sensor_type]  = tcmd->warning_sigma;
            g_ad.critical_sigma[tcmd->sensor_type] = tcmd->critical_sigma;
            g_ad.emergency_sigma[tcmd->sensor_type]= tcmd->emergency_sigma;
            LOG_INF("传感器 %u 阈值更新: W=%.1f C=%.1f E=%.1f sigma",
                    tcmd->sensor_type, (double)tcmd->warning_sigma,
                    (double)tcmd->critical_sigma, (double)tcmd->emergency_sigma);
        }
        k_mutex_unlock(&g_ad.lock);
        return 0;
    case ANOMALY_CMD_RESET_WINDOW:
        if (arg != NULL) {
            uint8_t sensor_type = *(uint8_t*)arg;
            if (sensor_type < MAX_SENSOR_TYPES) {
                memset(&g_ad.windows[sensor_type], 0, sizeof(sensor_window_t));
            }
        }
        k_mutex_unlock(&g_ad.lock);
        return 0;
    default:
        k_mutex_unlock(&g_ad.lock);
        return -1;
    }
}

/* =============================================================================
 * 模块特定 API
 * ============================================================================= */

void anomaly_detection_get_stats(uint32_t* warning_count, uint32_t* critical_count,
                                  uint32_t* emergency_count)
{
    k_mutex_lock(&g_ad.lock, K_FOREVER);
    anomaly_detection_get_stats_unlocked(warning_count, critical_count, emergency_count);
    k_mutex_unlock(&g_ad.lock);
}

/* 无锁版本：调用者必须持有 g_ad.lock */
static void anomaly_detection_get_stats_unlocked(uint32_t* warning_count, uint32_t* critical_count,
                                                  uint32_t* emergency_count)
{
    if (warning_count != NULL) *warning_count = g_ad.warning_count;
    if (critical_count != NULL) *critical_count = g_ad.critical_count;
    if (emergency_count != NULL) *emergency_count = g_ad.emergency_count;
}

int anomaly_detection_set_threshold(uint8_t sensor_type,
                                     float warning_sigma,
                                     float critical_sigma,
                                     float emergency_sigma)
{
    if (sensor_type >= MAX_SENSOR_TYPES) {
        return -1;
    }
    k_mutex_lock(&g_ad.lock, K_FOREVER);
    g_ad.warning_sigma[sensor_type]  = warning_sigma;
    g_ad.critical_sigma[sensor_type] = critical_sigma;
    g_ad.emergency_sigma[sensor_type]= emergency_sigma;
    k_mutex_unlock(&g_ad.lock);
    LOG_INF("传感器 %u 阈值更新: W=%.1f C=%.1f E=%.1f sigma",
            sensor_type, (double)warning_sigma, (double)critical_sigma, (double)emergency_sigma);
    return 0;
}

/* =============================================================================
 * 内部函数
 * ============================================================================= */

static void anomaly_on_sensor_data(const gateway_sensor_data_t* sensor)
{
    if (sensor == NULL || sensor->sensor_type >= MAX_SENSOR_TYPES) {
        return;
    }

    k_mutex_lock(&g_ad.lock, K_FOREVER);

    sensor_window_t* win = &g_ad.windows[sensor->sensor_type];

    /* 检测（窗口更新前，基线不包含当前值） */
    float sigma = 0.0f;
    uint8_t level = anomaly_detect(win, sensor->value, sensor->sensor_type, &sigma);

    /* 保存最近检测状态 */
    g_ad.last_value[sensor->sensor_type] = sensor->value;
    g_ad.last_level[sensor->sensor_type] = level;
    g_ad.last_sigma[sensor->sensor_type] = sigma;

    /* 更新窗口 */
    anomaly_update_window(win, sensor->value);

    /* 发布异常事件 */
    if (level != ANOMALY_LEVEL_NONE) {
        anomaly_publish_event(level, sensor, win->mean, win->stddev, sigma);
    }

    /* 多维度联动检测（仅在相关传感器数据到达时触发） */
    if (g_ad.multi_dim_enable &&
        (sensor->sensor_type == SENSOR_TYPE_CURRENT ||
         sensor->sensor_type == SENSOR_TYPE_TEMPERATURE)) {
        anomaly_check_multi_dim();
    }

    k_mutex_unlock(&g_ad.lock);
}

static void anomaly_update_window(sensor_window_t* win, float value)
{
    /* 窗口已满时，先移出即将被覆盖的最旧值 */
    if (win->count >= ANOMALY_WINDOW_SIZE) {
        float old = win->values[win->head];
        win->sum -= old;
        win->sum_sq -= old * old;
    }

    /* 写入新值 */
    win->values[win->head] = value;
    win->sum += value;
    win->sum_sq += value * value;
    win->head = (win->head + 1) % ANOMALY_WINDOW_SIZE;
    if (win->count < ANOMALY_WINDOW_SIZE) {
        win->count++;
    }

    /* 计算均值（O(1)） */
    win->mean = win->sum / win->count;

    /* 计算标准差（O(1)） */
    if (win->count >= 2) {
        float mean_sq = win->mean * win->mean;
        float avg_sq = win->sum_sq / win->count;
        float variance = avg_sq - mean_sq;
        if (variance < 0.0f) {
            variance = 0.0f;  /* 浮点精度可能导致极小负数 */
        }
        /* 转换为样本方差，与原始实现一致 */
        variance = variance * win->count / (win->count - 1);
        win->stddev = sqrtf(variance);
    } else {
        win->stddev = 0.0f;
    }

    if (win->count >= ANOMALY_WINDOW_SIZE) {
        win->valid = true;
    }
}

static uint8_t anomaly_detect(sensor_window_t* win, float value,
                               uint8_t sensor_type, float* out_sigma)
{
    /* 窗口未满时不检测 */
    if (!win->valid || win->stddev < ANOMALY_STDDEV_MIN_VALID) {
        *out_sigma = 0.0f;
        return ANOMALY_LEVEL_NONE;
    }

    float deviation = fabsf(value - win->mean);
    float sigma = deviation / win->stddev;
    *out_sigma = sigma;

    /* 绝对上下限检查 */
    float abs_max = g_ad.abs_max_value[sensor_type];
    if (fabsf(value) > abs_max) {
        return ANOMALY_LEVEL_EMERGENCY;
    }

    /* sigma 阈值检查 */
    if (sigma >= g_ad.emergency_sigma[sensor_type]) {
        return ANOMALY_LEVEL_EMERGENCY;
    }
    if (sigma >= g_ad.critical_sigma[sensor_type]) {
        return ANOMALY_LEVEL_CRITICAL;
    }
    if (sigma >= g_ad.warning_sigma[sensor_type]) {
        return ANOMALY_LEVEL_WARNING;
    }

    return ANOMALY_LEVEL_NONE;
}

static void anomaly_publish_event(uint8_t level, const gateway_sensor_data_t* sensor,
                                   float mean, float stddev, float sigma)
{
    /* 速率限制 */
    uint32_t now = k_uptime_get_32();
    if (sensor->sensor_type < MAX_SENSOR_TYPES) {
        if ((now - g_ad.last_alert_ms[sensor->sensor_type]) < g_ad.alert_interval_ms) {
            return;  /* 忽略，间隔太短 */
        }
        g_ad.last_alert_ms[sensor->sensor_type] = now;
    }

    gateway_anomaly_event_t evt = {
        .timestamp = now,
        .sensor_type = sensor->sensor_type,
        .level = level - 1,  /* 转换为 0=warning, 1=critical, 2=emergency */
        .current_value = sensor->value,
        .baseline_mean = mean,
        .baseline_stddev = stddev,
        .threshold_sigma = sigma,
    };

    event_type_t event_type;
    switch (level) {
    case ANOMALY_LEVEL_WARNING:
        event_type = EVENT_TYPE_ANOMALY_WARNING;
        g_ad.warning_count++;
        LOG_WRN("异常 WARNING: sensor=%s val=%.2f mean=%.2f std=%.2f sigma=%.1f",
                gateway_sensor_type_str(sensor->sensor_type),
                (double)sensor->value, (double)mean, (double)stddev, (double)sigma);
        break;
    case ANOMALY_LEVEL_CRITICAL:
        event_type = EVENT_TYPE_ANOMALY_CRITICAL;
        g_ad.critical_count++;
        LOG_ERR("异常 CRITICAL: sensor=%s val=%.2f mean=%.2f std=%.2f sigma=%.1f",
                gateway_sensor_type_str(sensor->sensor_type),
                (double)sensor->value, (double)mean, (double)stddev, (double)sigma);
        break;
    case ANOMALY_LEVEL_EMERGENCY:
        event_type = EVENT_TYPE_ANOMALY_EMERGENCY;
        g_ad.emergency_count++;
        LOG_ERR("异常 EMERGENCY: sensor=%s val=%.2f mean=%.2f std=%.2f sigma=%.1f",
                gateway_sensor_type_str(sensor->sensor_type),
                (double)sensor->value, (double)mean, (double)stddev, (double)sigma);
        break;
    default:
        return;
    }

    event_publish_copy(event_type, EVENT_PRIORITY_HIGH, &evt, sizeof(evt));

    /* 开源层：屏幕打印告警 */
    printk("\n*** ANOMALY [%s]: sensor=%s value=%.2f ***\n\n",
           gateway_anomaly_level_str(evt.level),
           gateway_sensor_type_str(sensor->sensor_type),
           (double)sensor->value);

#ifdef CONFIG_USE_EVENT_SYSTEM_PRO
    /* 商业层：事件路由 + 速率限制 + 持久化 */
    /* event_system_pro_route_with_qos(&evt, QOS_GUARANTEED); */
    /* event_system_pro_persist_event(&evt); */
#endif
}

static void anomaly_check_multi_dim(void)
{
    /* 联动规则：电流 + 温度同时超过 critical 阈值 -> 触发联动告警 */
    if (MAX_SENSOR_TYPES < 2) return;

    /* 使用保存的检测状态（基线不包含当前值） */
    uint8_t cur_level  = g_ad.last_level[SENSOR_TYPE_CURRENT];
    uint8_t temp_level = g_ad.last_level[SENSOR_TYPE_TEMPERATURE];

    /* 两者均 Critical 或以上时触发联动 */
    if (cur_level >= ANOMALY_LEVEL_CRITICAL && temp_level >= ANOMALY_LEVEL_CRITICAL) {
        uint32_t now = k_uptime_get_32();
        if ((now - g_ad.last_multi_dim_alert_ms) >= g_ad.alert_interval_ms) {
            g_ad.last_multi_dim_alert_ms = now;
            LOG_ERR("多维度联动告警: 电流=%.2f(sigma=%.1f) + 温度=%.2f(sigma=%.1f)",
                    (double)g_ad.last_value[SENSOR_TYPE_CURRENT],
                    (double)g_ad.last_sigma[SENSOR_TYPE_CURRENT],
                    (double)g_ad.last_value[SENSOR_TYPE_TEMPERATURE],
                    (double)g_ad.last_sigma[SENSOR_TYPE_TEMPERATURE]);
            /* 可在此发布联动告警事件，如需要 */
        }
    }
}

/* =============================================================================
 * data_bus 消费者回调
 * ============================================================================= */

static void anomaly_sensor_data_cb(data_bus_channel_t* ch,
                                    data_bus_block_t* block,
                                    void* user_data)
{
    ARG_UNUSED(ch);
    ARG_UNUSED(user_data);

    /* NOTE: data_bus_consumer_unregister() does not wait for in-flight
     * dispatches (see framework/src/data_bus/data_bus_consumer.c).
     * This status check is the only barrier preventing post-stop work
     * from executing; do not remove. */
    if (g_ad.status != MODULE_STATUS_RUNNING) {
        return;
    }
    void* payload = data_bus_block_ptr(block);
    if (payload == NULL ||
        data_bus_block_len(block) != sizeof(gateway_sensor_data_t)) {
        return;
    }

    anomaly_on_sensor_data((const gateway_sensor_data_t*)payload);
}

/* =============================================================================
 * 模块接口声明与自动注册
 * ============================================================================= */

static const char* const anomaly_detection_deps[] = {NULL};

DECLARE_MODULE_INTERFACE_WITH_DEPS(anomaly_detection, anomaly_detection_deps);

const module_interface_t* anomaly_detection_get_interface(void)
{
    return &anomaly_detection_interface;
}

static int anomaly_detection_auto_register(void)
{
    uint32_t module_id;
    if (module_manager_register(anomaly_detection_get_interface(), NULL, &module_id) != 0) {
        LOG_ERR("异常检测模块注册失败");
        return -EIO;
    }
    LOG_INF("异常检测模块已注册 (id=%u)", module_id);
    return 0;
}

SYS_INIT(anomaly_detection_auto_register, POST_KERNEL, GATEWAY_INIT_PRIO_ANOMALY);
