/**
 * @file protocol_modbus.h
 * @brief Modbus RTU Master 模块
 */

#ifndef PROTOCOL_MODBUS_H
#define PROTOCOL_MODBUS_H

#include "module_base.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * 控制命令
 * ============================================================================= */

#define MODBUS_CMD_READ_REGS     1  /* 读取保持寄存器 */
#define MODBUS_CMD_GET_STATS     2  /* 获取统计 */
#define MODBUS_CMD_SET_SLAVE_ID  3  /* 设置从站 ID */

/* =============================================================================
 * 控制命令参数结构
 * ============================================================================= */

typedef struct {
    uint8_t  slave_id;      /* 从站 ID */
    uint16_t start_addr;    /* 起始地址 */
    uint16_t reg_count;     /* 寄存器数量 (1-16) */
    uint16_t out_values[16]; /* 输出：读取到的寄存器值 */
} modbus_read_regs_arg_t;

/* =============================================================================
 * 模块 API
 * ============================================================================= */

const module_interface_t* protocol_modbus_get_interface(void);

/** 手动读取保持寄存器 */
int protocol_modbus_read_holding_regs(uint8_t slave_id, uint16_t start_addr,
                                       uint16_t reg_count, uint16_t* out_values);

/** 获取收发统计 */
void protocol_modbus_get_stats(uint32_t* tx_count, uint32_t* rx_count, uint32_t* err_count);

#ifdef __cplusplus
}
#endif

#endif /* PROTOCOL_MODBUS_H */
