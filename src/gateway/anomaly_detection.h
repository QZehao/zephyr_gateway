/**
 * @file anomaly_detection.h
 * @brief 自适应阈值异常检测模块
 */

#ifndef ANOMALY_DETECTION_H
#define ANOMALY_DETECTION_H

#include <zeplod/module_base.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * 控制命令
 * ============================================================================= */

#define ANOMALY_CMD_GET_STATS     1  /* 获取统计 */
#define ANOMALY_CMD_SET_THRESHOLD 2  /* 设置阈值 */
#define ANOMALY_CMD_RESET_WINDOW  3  /* 重置窗口 */

/* =============================================================================
 * 控制命令参数结构
 * ============================================================================= */

typedef struct {
    uint8_t sensor_type;
    float   warning_sigma;
    float   critical_sigma;
    float   emergency_sigma;
} anomaly_threshold_cmd_t;

/* =============================================================================
 * 模块 API
 * ============================================================================= */

const module_interface_t* anomaly_detection_get_interface(void);

/** 获取检测统计 */
void anomaly_detection_get_stats(uint32_t* warning_count, uint32_t* critical_count,
                                  uint32_t* emergency_count);

/** 设置某传感器类型的阈值参数 */
int anomaly_detection_set_threshold(uint8_t sensor_type,
                                     float warning_sigma,
                                     float critical_sigma,
                                     float emergency_sigma);

#ifdef __cplusplus
}
#endif

#endif /* ANOMALY_DETECTION_H */
