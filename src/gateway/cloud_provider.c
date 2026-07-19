/**
 * @file cloud_provider.c
 * @brief 云平台 Provider 注册表实现
 */

#include "cloud_provider.h"
#include "gateway_config.h"
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(cloud_provider, CONFIG_SYS_LOG_LEVEL);

/* =============================================================================
 * 配置
 * ============================================================================= */

#define MAX_CLOUD_PROVIDERS 4

/* =============================================================================
 * 静态变量
 * ============================================================================= */

static const cloud_provider_t* g_providers[MAX_CLOUD_PROVIDERS];
static uint8_t                 g_provider_count = 0;
static struct k_mutex          g_provider_lock;

/* =============================================================================
 * API 实现
 * ============================================================================= */

/**
 * @brief 初始化 Provider 注册表
 *
 * 清空已注册计数并初始化内部互斥锁。通过 SYS_INIT 在 POST_KERNEL 阶段自动调用，
 * 优先级早于各 cloud_xxx Provider 的自动注册（GATEWAY_INIT_PRIO_CLOUD_PROVIDER）。
 *
 * @return 恒为 0
 */
int cloud_provider_init(void)
{
    g_provider_count = 0;
    k_mutex_init(&g_provider_lock);
    return 0;
}

/**
 * @brief 注册一个云平台 Provider
 *
 * 按 name 去重（幂等：重复注册同名 Provider 直接返回成功），注册表容量固定为
 * MAX_CLOUD_PROVIDERS，满员时返回 -ENOMEM。
 *
 * @param provider 待注册的 Provider 描述符，name/publish 不可为 NULL
 * @return 0 成功（含幂等命中）；-EINVAL 参数非法；-ENOMEM 注册表已满
 */
int cloud_provider_register(const cloud_provider_t* provider)
{
    if (provider == NULL || provider->name == NULL || provider->publish == NULL) {
        return -EINVAL;
    }

    k_mutex_lock(&g_provider_lock, K_FOREVER);

    if (g_provider_count >= MAX_CLOUD_PROVIDERS) {
        k_mutex_unlock(&g_provider_lock);
        LOG_ERR("Provider 注册失败：已达上限 %u", MAX_CLOUD_PROVIDERS);
        return -ENOMEM;
    }

    /* 检查重复 */
    for (uint8_t i = 0; i < g_provider_count; i++) {
        if (g_providers[i] != NULL &&
            strcmp(g_providers[i]->name, provider->name) == 0) {
            k_mutex_unlock(&g_provider_lock);
            return 0; /* 幂等 */
        }
    }

    g_providers[g_provider_count++] = provider;
    k_mutex_unlock(&g_provider_lock);

    LOG_INF("Cloud provider registered: %s", provider->name);
    return 0;
}

/**
 * @brief 向所有已注册的 Provider 广播发布一条消息
 *
 * 锁内仅将已注册 Provider 的指针拷贝到局部数组快照，解锁后再逐个调用
 * publish()——publish() 内部会阻塞做 MQTT 发送，若持锁横跨该调用，会在
 * CLOUD_REPLAY 等运行在全局唯一事件分发线程上的路径上卡住 g_provider_lock，
 * 拖累全系统事件分发。前提（已用 grep 核查成立）：cloud_provider 未提供
 * unregister API，Provider 描述符均为静态存储期变量，一旦注册后指针永久有效，
 * 因此在锁外持有快照指针调用 publish() 是安全的。单个 Provider 失败不影响其
 * 余 Provider 的发布尝试。调用者可通过 out_success_count/out_fail_count 判断
 * 是否需要转入离线缓存（例如 success_count==0 时，无论是否有 Provider 均视为
 * 失败）。
 *
 * @param type              消息类型（遥测/异常）
 * @param json_payload      JSON 载荷字符串
 * @param[out] out_success_count 成功发布的 Provider 数量（可传 NULL）
 * @param[out] out_fail_count    失败的 Provider 数量（可传 NULL）
 * @return 0 全部成功；-EIO 至少一个 Provider 失败；-EINVAL json_payload 为 NULL
 */
int cloud_provider_publish_all(cloud_msg_type_t type, const char* json_payload,
                                uint8_t* out_success_count, uint8_t* out_fail_count)
{
    if (json_payload == NULL) {
        return -EINVAL;
    }

    const cloud_provider_t* snapshot[MAX_CLOUD_PROVIDERS];
    uint8_t count;

    k_mutex_lock(&g_provider_lock, K_FOREVER);
    count = g_provider_count;
    for (uint8_t i = 0; i < count; i++) {
        snapshot[i] = g_providers[i];
    }
    k_mutex_unlock(&g_provider_lock);

    uint8_t success = 0;
    uint8_t fail = 0;

    for (uint8_t i = 0; i < count; i++) {
        const cloud_provider_t* p = snapshot[i];
        if (p != NULL && p->publish != NULL) {
            int ret = p->publish(type, json_payload);
            if (ret == 0) {
                success++;
            } else {
                LOG_DBG("Provider '%s' publish failed: %d", p->name, ret);
                fail++;
            }
        }
    }

    if (out_success_count != NULL) {
        *out_success_count = success;
    }
    if (out_fail_count != NULL) {
        *out_fail_count = fail;
    }

    return (fail > 0) ? -EIO : 0;
}

/**
 * @brief 获取当前已注册的 Provider 数量
 * @return 已注册数量，范围 [0, MAX_CLOUD_PROVIDERS]
 */
uint8_t cloud_provider_get_count(void)
{
    return g_provider_count;
}

/**
 * @brief 按名称查找已注册 Provider
 * @param name Provider 名称（如 "private"/"aliyun"/"tencent"/"aws"）
 * @return 匹配的 Provider 指针；未找到或 name 为 NULL 返回 NULL
 */
const cloud_provider_t* cloud_provider_get_by_name(const char* name)
{
    if (name == NULL) {
        return NULL;
    }

    for (uint8_t i = 0; i < g_provider_count; i++) {
        if (g_providers[i] != NULL &&
            strcmp(g_providers[i]->name, name) == 0) {
            return g_providers[i];
        }
    }
    return NULL;
}

/**
 * @brief 按索引获取已注册 Provider
 * @param idx 索引，取值范围 [0, cloud_provider_get_count()-1]
 * @return 对应 Provider 指针；越界返回 NULL
 */
const cloud_provider_t* cloud_provider_get_by_index(uint8_t idx)
{
    if (idx >= g_provider_count) {
        return NULL;
    }
    return g_providers[idx];
}

/**
 * @brief 获取 Provider 连接状态的可读字符串
 * @param provider 目标 Provider
 * @return "connected"/"disconnected"（据 is_connected() 结果）；
 *         provider 为 NULL 返回 "invalid"；未实现 is_connected 返回 "unknown"
 */
const char* cloud_provider_status_str(const cloud_provider_t* provider)
{
    if (provider == NULL) {
        return "invalid";
    }
    if (provider->is_connected == NULL) {
        return "unknown";
    }
    return provider->is_connected() ? "connected" : "disconnected";
}

/* =============================================================================
 * 自动初始化
 * ============================================================================= */

SYS_INIT(cloud_provider_init, POST_KERNEL, GATEWAY_INIT_PRIO_CLOUD_PROVIDER);
