/**
 * @file offline_cache.h
 * @brief 断网续传本地缓存模块
 */

#ifndef OFFLINE_CACHE_H
#define OFFLINE_CACHE_H

#include <zeplod/module_base.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CACHE_CMD_GET_INFO    1
#define CACHE_CMD_CLEAR       2

const module_interface_t* offline_cache_get_interface(void);

/** 获取缓存信息 */
void offline_cache_get_info(uint32_t* entry_count, uint32_t* max_entries, uint32_t* overflow_count);

#ifdef __cplusplus
}
#endif

#endif /* OFFLINE_CACHE_H */
