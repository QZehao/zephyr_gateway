/**
 * @file protocol_can.h
 * @brief CAN 总线数据采集模块
 */

#ifndef PROTOCOL_CAN_H
#define PROTOCOL_CAN_H

#include "module_base.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * 模块 API
 * ============================================================================= */

/** 获取模块接口指针（用于手动注册） */
const module_interface_t* protocol_can_get_interface(void);

/** 发送 CAN 帧（测试/调试用途） */
int protocol_can_send(uint32_t id, const uint8_t* data, uint8_t dlc, bool ext_id);

/** 获取接收统计 */
void protocol_can_get_stats(uint32_t* rx_count, uint32_t* tx_count, uint32_t* err_count);

#ifdef __cplusplus
}
#endif

#endif /* PROTOCOL_CAN_H */
