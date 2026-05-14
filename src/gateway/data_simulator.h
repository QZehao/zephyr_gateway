/**
 * @file data_simulator.h
 * @brief 数据模拟生成采集模块头文件
 *
 * 用于功能测试：在没有真实 CAN/Modbus 硬件时，
 * 按配置的基线和波动范围生成随机传感器数据。
 */

#ifndef DATA_SIMULATOR_H
#define DATA_SIMULATOR_H

#include <stdbool.h>
#include <stdint.h>
#include "gateway_events.h"
#include "module_base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * 配置默认值
 * ============================================================================= */

#define SIM_DEFAULT_INTERVAL_MS    1000
#define SIM_DEFAULT_BASELINE_CURR  50.0f
#define SIM_DEFAULT_BASELINE_TEMP  25.0f
#define SIM_DEFAULT_BASELINE_VOLT  220.0f
#define SIM_DEFAULT_BASELINE_PRES  100.0f
#define SIM_DEFAULT_BASELINE_HUMI  60.0f
#define SIM_DEFAULT_RANGE_CURR     5.0f
#define SIM_DEFAULT_RANGE_TEMP     3.0f
#define SIM_DEFAULT_RANGE_VOLT     10.0f
#define SIM_DEFAULT_RANGE_PRES     8.0f
#define SIM_DEFAULT_RANGE_HUMI     5.0f

/* =============================================================================
 * 控制命令枚举
 * ============================================================================= */

#define SIM_CMD_SET_BASELINE   1
#define SIM_CMD_SET_RANGE      2
#define SIM_CMD_SET_INTERVAL   3
#define SIM_CMD_GET_STATS      4
#define SIM_CMD_RESET_STATS    5
#define SIM_CMD_INJECT         6

/* =============================================================================
 * 数据结构
 * ============================================================================= */

/** 模拟通道配置 */
typedef struct {
    float    baseline;
    float    range;
    uint8_t  channel_id;
    bool     enabled;
} sim_channel_config_t;

/** 异常注入参数 */
typedef struct {
    uint8_t  sensor_type;
    float    value;
} sim_inject_param_t;

/** 模块统计信息 */
typedef struct {
    uint32_t sample_count;
    uint32_t inject_count;
} sim_stats_t;

/* =============================================================================
 * 模块接口声明
 * ============================================================================= */

int  data_simulator_init(void* config);
int  data_simulator_start(void);
int  data_simulator_stop(void);
int  data_simulator_shutdown(void);
void data_simulator_on_event(const event_t* event, void* user_data);
module_status_t data_simulator_get_status(void);
int  data_simulator_control(int cmd, void* arg);

const module_interface_t* data_simulator_get_interface(void);

/* =============================================================================
 * 模块特定 API
 * ============================================================================= */

/** 获取模块统计信息 */
void data_simulator_get_stats(sim_stats_t* stats);

/** 重置统计信息 */
void data_simulator_reset_stats(void);

#ifdef __cplusplus
}
#endif

#endif /* DATA_SIMULATOR_H */
