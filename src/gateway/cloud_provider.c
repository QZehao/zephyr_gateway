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

int cloud_provider_init(void)
{
    g_provider_count = 0;
    k_mutex_init(&g_provider_lock);
    return 0;
}

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

int cloud_provider_publish_all(cloud_msg_type_t type, const char* json_payload,
                                uint8_t* out_success_count, uint8_t* out_fail_count)
{
    if (json_payload == NULL) {
        return -EINVAL;
    }

    uint8_t success = 0;
    uint8_t fail = 0;

    k_mutex_lock(&g_provider_lock, K_FOREVER);

    for (uint8_t i = 0; i < g_provider_count; i++) {
        const cloud_provider_t* p = g_providers[i];
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

    k_mutex_unlock(&g_provider_lock);

    if (out_success_count != NULL) {
        *out_success_count = success;
    }
    if (out_fail_count != NULL) {
        *out_fail_count = fail;
    }

    return (fail > 0) ? -EIO : 0;
}

uint8_t cloud_provider_get_count(void)
{
    return g_provider_count;
}

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

const cloud_provider_t* cloud_provider_get_by_index(uint8_t idx)
{
    if (idx >= g_provider_count) {
        return NULL;
    }
    return g_providers[idx];
}

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
