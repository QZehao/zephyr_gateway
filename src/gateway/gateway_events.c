/**
 * @file gateway_events.c
 * @brief 工业网关事件 + 数据通道辅助实现
 *
 * 负责：
 * - data_bus 通道创建（sensor / can_raw / modbus_raw），SYS_INIT 阶段一次性建立
 * - 三类高频数据的 publish helper，封装 data_bus_publish() 与错误码归一
 * - sensor / anomaly 数据的 JSON 序列化与类型名映射
 */

#include "gateway_events.h"
#include "gateway_config.h"

#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

LOG_MODULE_REGISTER(gateway_events, CONFIG_SYS_LOG_LEVEL);

/* slab 256B 上限约束（见 spec §5） */
_Static_assert(sizeof(gateway_sensor_data_t) <= 256,
               "gateway_sensor_data_t exceeds DATA_BUS_SLAB_256");
_Static_assert(sizeof(gateway_can_frame_t) <= 256,
               "gateway_can_frame_t exceeds DATA_BUS_SLAB_256");
_Static_assert(sizeof(gateway_modbus_data_t) <= 256,
               "gateway_modbus_data_t exceeds DATA_BUS_SLAB_256");

/* =============================================================================
 * 全局通道句柄
 * ============================================================================= */

/* 不变量：以下指针由 gateway_channels_init() 在 SYS_INIT(APPLICATION,68) 单次写入，
 * 之后只被读取，永不再赋值。读者一律在 module_manager_start_all() 之后（即各模块
 * _start() 内）使用，因此不需要锁或原子操作。如需在运行时重建通道，必须重新设计
 * 同步策略（atomic 交换 或 写锁），并审计所有 publish helper。*/
data_bus_channel_t* g_sensor_channel     = NULL;
data_bus_channel_t* g_can_raw_channel    = NULL;
data_bus_channel_t* g_modbus_raw_channel = NULL;

/* =============================================================================
 * 通道创建（SYS_INIT APPLICATION, GATEWAY_INIT_PRIO_CHANNELS）
 * ============================================================================= */

static int gateway_channels_init(void)
{
    int ret;

    ret = data_bus_channel_create("sensor", &g_sensor_channel);
    if (ret != 0) {
        LOG_ERR("创建 sensor 通道失败: %d", ret);
        g_sensor_channel = NULL;
    } else {
        LOG_INF("data_bus 通道 'sensor' 已创建");
    }

    ret = data_bus_channel_create("can_raw", &g_can_raw_channel);
    if (ret != 0) {
        LOG_ERR("创建 can_raw 通道失败: %d", ret);
        g_can_raw_channel = NULL;
    } else {
        LOG_INF("data_bus 通道 'can_raw' 已创建");
    }

    ret = data_bus_channel_create("modbus_raw", &g_modbus_raw_channel);
    if (ret != 0) {
        LOG_ERR("创建 modbus_raw 通道失败: %d", ret);
        g_modbus_raw_channel = NULL;
    } else {
        LOG_INF("data_bus 通道 'modbus_raw' 已创建");
    }

    return 0;  /* 单通道失败不阻塞系统启动；helper 会返回 -ENODEV */
}

SYS_INIT(gateway_channels_init, APPLICATION, GATEWAY_INIT_PRIO_CHANNELS);

/* =============================================================================
 * Publish Helper
 * ============================================================================= */

int gateway_sensor_publish(const gateway_sensor_data_t* data)
{
    if (data == NULL) {
        return -EINVAL;
    }
    if (g_sensor_channel == NULL) {
        return -ENODEV;
    }
    return data_bus_publish(g_sensor_channel, data, sizeof(*data));
}

int gateway_can_raw_publish(const gateway_can_frame_t* frame)
{
    if (frame == NULL) {
        return -EINVAL;
    }
    if (g_can_raw_channel == NULL) {
        return -ENODEV;
    }
    return data_bus_publish(g_can_raw_channel, frame, sizeof(*frame));
}

int gateway_modbus_raw_publish(const gateway_modbus_data_t* mb)
{
    if (mb == NULL) {
        return -EINVAL;
    }
    if (g_modbus_raw_channel == NULL) {
        return -ENODEV;
    }
    return data_bus_publish(g_modbus_raw_channel, mb, sizeof(*mb));
}

/* =============================================================================
 * JSON 序列化与类型名（保留原有实现）
 * ============================================================================= */

int gateway_sensor_to_json(const gateway_sensor_data_t* data, char* buf, size_t buf_len)
{
    if (data == NULL || buf == NULL || buf_len == 0) {
        return -1;
    }

    const char* type_str = gateway_sensor_type_str(data->sensor_type);

    return snprintf(buf, buf_len,
                    "{\"ts\":%lu,\"ch\":%u,\"type\":\"%s\",\"val\":%.2f,\"raw\":%u}",
                    (unsigned long)data->timestamp,
                    (unsigned)data->channel_id,
                    type_str,
                    (double)data->value,
                    (unsigned)data->raw_u16);
}

int gateway_anomaly_to_json(const gateway_anomaly_event_t* evt, char* buf, size_t buf_len)
{
    if (evt == NULL || buf == NULL || buf_len == 0) {
        return -1;
    }

    const char* level_str = gateway_anomaly_level_str(evt->level);
    const char* type_str  = gateway_sensor_type_str(evt->sensor_type);

    return snprintf(buf, buf_len,
                    "{\"ts\":%lu,\"sensor\":\"%s\",\"lvl\":\"%s\","
                    "\"val\":%.2f,\"mean\":%.2f,\"std\":%.2f,\"sigma\":%.1f}",
                    (unsigned long)evt->timestamp,
                    type_str,
                    level_str,
                    (double)evt->current_value,
                    (double)evt->baseline_mean,
                    (double)evt->baseline_stddev,
                    (double)evt->threshold_sigma);
}

const char* gateway_sensor_type_str(uint8_t sensor_type)
{
    switch (sensor_type) {
    case SENSOR_TYPE_CURRENT:     return "current";
    case SENSOR_TYPE_TEMPERATURE: return "temperature";
    case SENSOR_TYPE_VOLTAGE:     return "voltage";
    case SENSOR_TYPE_PRESSURE:    return "pressure";
    case SENSOR_TYPE_HUMIDITY:    return "humidity";
    default:                      return "unknown";
    }
}

const char* gateway_anomaly_level_str(uint8_t level)
{
    switch (level) {
    case 0: return "warning";
    case 1: return "critical";
    case 2: return "emergency";
    default: return "unknown";
    }
}
