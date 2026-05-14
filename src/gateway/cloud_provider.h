/**
 * @file cloud_provider.h
 * @brief 云平台 Provider 抽象接口
 *
 * 定义统一的云平台接入接口，支持多云并行。
 * 各云平台模块实现 cloud_provider_t 并注册到 Provider 注册表。
 * cloud_upload 通过 cloud_provider_publish_all() 向所有已注册 Provider 分发数据。
 */

#ifndef CLOUD_PROVIDER_H
#define CLOUD_PROVIDER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 前向声明，避免包含 shell.h */
struct shell;

/* =============================================================================
 * 消息类型枚举
 * ============================================================================= */

typedef enum {
    CLOUD_MSG_TELEMETRY = 0,  /* 传感器遥测数据 */
    CLOUD_MSG_ANOMALY,        /* 异常告警数据 */
} cloud_msg_type_t;

/* =============================================================================
 * Provider 接口结构
 * ============================================================================= */

typedef struct {
    const char* name;   /* Provider 名称（唯一标识） */

    /* 生命周期回调（可选，可为 NULL） */
    int (*init)(void);
    int (*start)(void);
    int (*stop)(void);
    int (*shutdown)(void);

    /* 连接状态 */
    bool (*is_connected)(void);

    /* 数据发布（必须实现） */
    int (*publish)(cloud_msg_type_t type, const char* json_payload);

    /* Shell 状态打印（可选） */
    void (*print_status)(const struct shell* sh);
} cloud_provider_t;

/* =============================================================================
 * Provider 注册表 API
 * ============================================================================= */

/** 初始化 Provider 注册表（内部锁等） */
int cloud_provider_init(void);

/** 注册 Provider */
int cloud_provider_register(const cloud_provider_t* provider);

/** 向所有已注册的 Provider 发布消息。
 *  返回值：0 表示全部成功，非零表示至少一个 Provider 失败 */
int cloud_provider_publish_all(cloud_msg_type_t type, const char* json_payload);

/** 获取已注册 Provider 数量 */
uint8_t cloud_provider_get_count(void);

/** 按名称获取 Provider */
const cloud_provider_t* cloud_provider_get_by_name(const char* name);

/** 按索引获取 Provider（0 ~ count-1） */
const cloud_provider_t* cloud_provider_get_by_index(uint8_t idx);

/** 获取 Provider 连接状态字符串 */
const char* cloud_provider_status_str(const cloud_provider_t* provider);

#ifdef __cplusplus
}
#endif

#endif /* CLOUD_PROVIDER_H */
