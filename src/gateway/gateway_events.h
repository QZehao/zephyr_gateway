/**
 * @file gateway_events.h
 * @brief 工业网关专用事件类型定义
 *
 * 定义网关各模块间通信使用的事件类型和数据载荷格式。
 * 事件类型编号范围 100-149（与 framework 预留的 0-99 不冲突）。
 */

#ifndef GATEWAY_EVENTS_H
#define GATEWAY_EVENTS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * 事件类型枚举 (Event Type Definitions)
 * 范围 100-149: 工业网关专用
 * ============================================================================= */

/* 异常检测输出 */
#define EVENT_TYPE_ANOMALY_WARNING    110  /* |x-μ|/σ ≥ warning_sigma（默认 2.0） */
#define EVENT_TYPE_ANOMALY_CRITICAL   111  /* |x-μ|/σ ≥ critical_sigma（默认 3.0） */
#define EVENT_TYPE_ANOMALY_EMERGENCY  112  /* |x-μ|/σ ≥ emergency_sigma（默认 4.0）或绝对上下限 */

/* 网络状态 */
#define EVENT_TYPE_CLOUD_CONNECTED    120
#define EVENT_TYPE_CLOUD_DISCONNECTED 121

/* 数据上云 */
#define EVENT_TYPE_CLOUD_UPLOAD       130  /* 需要上云的数据（离线缓存用） */

/* =============================================================================
 * 数据载荷结构 (Event Payload Structures)
 * ============================================================================= */

/** 传感器类型枚举 */
typedef enum {
    SENSOR_TYPE_CURRENT = 0,  /* 电流 */
    SENSOR_TYPE_TEMPERATURE,  /* 温度 */
    SENSOR_TYPE_VOLTAGE,      /* 电压 */
    SENSOR_TYPE_PRESSURE,     /* 压力 */
    SENSOR_TYPE_HUMIDITY,     /* 湿度 */
    SENSOR_TYPE_COUNT         /* 类型总数 */
} gateway_sensor_type_t;

/** 传感器数据点（CAN / Modbus 统一格式） */
typedef struct {
    uint32_t timestamp;       /* 采集时间戳 (ms uptime) */
    uint8_t  channel_id;      /* 通道/从站 ID */
    uint8_t  sensor_type;     /* gateway_sensor_type_t */
    float    value;           /* 物理量值 */
    uint16_t raw_u16;         /* 原始寄存器值（用于调试） */
} gateway_sensor_data_t;

/** 异常告警事件 */
typedef struct {
    uint32_t timestamp;
    uint8_t  sensor_type;     /* 触发异常的传感器类型 */
    uint8_t  level;           /* 0=Warning, 1=Critical, 2=Emergency */
    float    current_value;   /* 当前值 */
    float    baseline_mean;   /* 基线均值 */
    float    baseline_stddev; /* 基线标准差 */
    float    threshold_sigma; /* 触发阈值（倍 sigma） */
} gateway_anomaly_event_t;

/** 云端上传数据 */
typedef struct {
    uint32_t timestamp;
    uint8_t  data_type;       /* 0=传感器, 1=异常, 2=心跳 */
    char     json_payload[192]; /* 轻量 JSON（196B > 48B inline，永远走 ptr 分配） */
} gateway_cloud_data_t;

/** CAN 帧原始数据 */
typedef struct {
    uint32_t timestamp;
    uint32_t id;              /* CAN ID */
    uint8_t  dlc;             /* 数据长度 */
    uint8_t  data[8];         /* 原始数据 */
    bool     rtr;             /* 远程帧标志 */
    bool     ext_id;          /* 扩展 ID 标志 */
} gateway_can_frame_t;

/** Modbus 寄存器数据 */
typedef struct {
    uint32_t timestamp;
    uint8_t  slave_id;
    uint16_t start_addr;
    uint8_t  reg_count;
    uint16_t values[16];      /* 最多 16 个寄存器 */
} gateway_modbus_data_t;

/* =============================================================================
 * 事件数据访问辅助
 * ============================================================================= */

#include "event_system.h"

/** 安全获取事件数据指针（自动处理 inline / ptr 模式） */
static inline const void* gateway_event_data(const event_t* evt)
{
    if (evt == NULL) return NULL;
    if (evt->flags & EVENT_FLAG_DATA_INLINE) {
        return (const void*)evt->data.inline_data;
    }
    return evt->data.ptr;
}

/* =============================================================================
 * 辅助函数声明
 * ============================================================================= */

/** 将传感器数据格式化为轻量 JSON 字符串 */
int gateway_sensor_to_json(const gateway_sensor_data_t* data, char* buf, size_t buf_len);

/** 将异常事件格式化为轻量 JSON 字符串 */
int gateway_anomaly_to_json(const gateway_anomaly_event_t* evt, char* buf, size_t buf_len);

/** 将传感器类型转为可读字符串 */
const char* gateway_sensor_type_str(uint8_t sensor_type);

/** 将异常级别转为可读字符串 */
const char* gateway_anomaly_level_str(uint8_t level);

/* =============================================================================
 * Data Bus 通道与 Publish Helper
 * ============================================================================= */

#include "data_bus.h"

/** 全局通道句柄；由 gateway_events.c 在 SYS_INIT(APPLICATION, 68) 阶段创建。
 *  若 data_bus 初始化失败或通道池耗尽，对应句柄保持 NULL，helper 返回 -ENODEV。 */
extern data_bus_channel_t* g_sensor_channel;     /* payload: gateway_sensor_data_t */
extern data_bus_channel_t* g_can_raw_channel;    /* payload: gateway_can_frame_t */
extern data_bus_channel_t* g_modbus_raw_channel; /* payload: gateway_modbus_data_t */

/** 发布传感器数据点（统一格式，data_simulator / protocol_can / protocol_modbus 共用）。
 *  返回 0 成功；-ENODEV 通道未创建；-ENOMEM slab 耗尽；-ENOBUFS 队列满；-EINVAL 参数非法。 */
int gateway_sensor_publish(const gateway_sensor_data_t* data);

/** 发布 CAN 原始帧（当前无消费者，预留 trace/debug 通道）。 */
int gateway_can_raw_publish(const gateway_can_frame_t* frame);

/** 发布 Modbus 原始寄存器数据（当前无消费者，预留 trace/debug 通道）。 */
int gateway_modbus_raw_publish(const gateway_modbus_data_t* mb);

#ifdef __cplusplus
}
#endif

#endif /* GATEWAY_EVENTS_H */
