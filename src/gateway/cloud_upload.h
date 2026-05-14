/**
 * @file cloud_upload.h
 * @brief 云数据上传模块
 *
 * 订阅传感器数据和异常事件，格式化为 JSON，通过 cloud_provider 抽象层
 * 向所有已注册的云平台 Provider 分发数据。
 */

#ifndef CLOUD_UPLOAD_H
#define CLOUD_UPLOAD_H

#include "module_base.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 控制命令 */
#define CLOUD_CMD_GET_STATS   1
#define CLOUD_CMD_FORCE_UPLOAD 2

const module_interface_t* cloud_upload_get_interface(void);

/** 获取上传统计 */
void cloud_upload_get_stats(uint32_t* success_count, uint32_t* fail_count, uint32_t* cached_count);

#ifdef __cplusplus
}
#endif

#endif /* CLOUD_UPLOAD_H */
