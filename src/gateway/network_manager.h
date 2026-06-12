/**
 * @file network_manager.h
 * @brief 网络链路管理模块
 *
 * 管理网络接口状态，发布网络上线/下线事件。
 * MQTT 相关逻辑已迁移到 protocol_mqtt。
 */

#ifndef NETWORK_MANAGER_H
#define NETWORK_MANAGER_H

#include "module_base.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * 模块 API
 * ============================================================================= */

const module_interface_t* network_manager_get_interface(void);

/** 检查网络接口是否已上线 */
bool network_is_up(void);

#ifdef __cplusplus
}
#endif

#endif /* NETWORK_MANAGER_H */
