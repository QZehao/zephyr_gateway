/**
 * @file cloud_upload.h
 * @brief MQTT 数据上云模块
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
