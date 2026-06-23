/**
 * @file offline_cache.c
 * @brief 断网续传本地缓存模块实现
 *
 * 订阅 EVENT_TYPE_CLOUD_UPLOAD，网络断开时缓存到 NVS，
 * 网络恢复后批量读取并重新发布。
 */

#include "offline_cache.h"
#include "gateway_events.h"
#include "gateway_config.h"
#include <zeplod/app_config.h>

#include <zephyr/drivers/flash.h>
#include <zephyr/fs/nvs.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/atomic.h>
#include <string.h>

#include <zeplod/event_system.h>
#include <zeplod/module_manager.h>

LOG_MODULE_REGISTER(offline_cache, CONFIG_SYS_LOG_LEVEL);

/* =============================================================================
 * 配置
 * ============================================================================= */

#define CACHE_MAX_ENTRIES    GATEWAY_CACHE_MAX_ENTRIES
#define CACHE_ENTRY_SIZE     GATEWAY_CACHE_ENTRY_SIZE
#define CACHE_NVS_ID_START   64
/* 原子状态 NVS ID：count/head/tail 合并为单一原子写入，防止掉电导致不一致 */
#define CACHE_STATE_ID       61

/* =============================================================================
 * 原子状态结构（单次 NVS 写入保证一致性）
 * ============================================================================= */

typedef struct {
    uint16_t count;
    uint16_t head;
    uint16_t tail;
    uint16_t crc;   /* CRC-16/MODBUS of count+head+tail */
} cache_state_t;

_Static_assert(sizeof(cache_state_t) == 8, "cache_state_t must be 8 bytes");

/* 计算 cache_state_t 前 4 字节的 CRC-16/MODBUS（排除自身 crc 字段） */
static uint16_t cache_state_crc(uint16_t count, uint16_t head, uint16_t tail)
{
    uint16_t crc = 0xFFFF;
    uint8_t data[6] = {
        (uint8_t)(count >> 8), (uint8_t)(count & 0xFF),
        (uint8_t)(head >> 8),  (uint8_t)(head & 0xFF),
        (uint8_t)(tail >> 8),  (uint8_t)(tail & 0xFF),
    };
    for (size_t i = 0; i < sizeof(data); i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc & 1) ? ((crc >> 1) ^ 0xA001) : (crc >> 1);
        }
    }
    return crc;
}

/* =============================================================================
 * 内部数据结构
 * ============================================================================= */

typedef struct {
    uint32_t timestamp;
    uint8_t  data_type;  /* 0=传感器, 1=异常, 2=心跳 */
    char     data[CACHE_ENTRY_SIZE - sizeof(uint32_t) - sizeof(uint8_t)];
} cache_entry_t;

/* 编译时检查大小 */
BUILD_ASSERT(sizeof(cache_entry_t) <= CACHE_ENTRY_SIZE);

typedef struct {
    module_status_t status;
    struct nvs_fs   fs;
    /* 环形缓冲区指针 */
    uint16_t        head;          /* 写入位置 */
    uint16_t        tail;          /* 读取位置 */
    uint16_t        count;         /* 当前条目数 */
    /* 状态：atomic_t 保证事件线程写、workqueue 读无竞争 */
    atomic_t        net_connected;
    /* 统计 */
    uint32_t        write_count;
    uint32_t        read_count;
    uint32_t        overflow_count;
    /* 同步 */
    struct k_mutex  lock;
    /* 批量上报工作项：网络恢复后持续排空缓存 */
    struct k_work_delayable upload_work;
} offline_cache_cb_t;

/* =============================================================================
 * 静态变量
 * ============================================================================= */

static offline_cache_cb_t g_cache;

/* =============================================================================
 * 前向声明
 * ============================================================================= */

static void cache_on_cloud_upload(const gateway_cloud_data_t* data);
static void cache_on_net_state(bool connected);
static void cache_upload_work_handler(struct k_work* work);
static int  cache_write_entry(const char* json_data, uint8_t data_type);
static int  cache_read_entry(char* out_data, size_t out_size, uint8_t* out_data_type);
static int  cache_nvs_init(void);
static void cache_save_state(void);
static void cache_load_state(void);

/* =============================================================================
 * 模块接口实现
 * ============================================================================= */

int offline_cache_init(void* config)
{
    ARG_UNUSED(config);
    LOG_INF("初始化离线缓存模块...");

    memset(&g_cache, 0, sizeof(g_cache));
    g_cache.status = MODULE_STATUS_INITIALIZED;
    k_mutex_init(&g_cache.lock);
    k_work_init_delayable(&g_cache.upload_work, cache_upload_work_handler);

    /* 注册回放事件类型：重播时由本模块发布、cloud_upload 订阅直发，与 CLOUD_UPLOAD 缓存语义区分 */
    event_register_type(EVENT_TYPE_CLOUD_REPLAY, "cloud_replay");

    int ret = cache_nvs_init();
    if (ret != 0) {
        LOG_ERR("NVS 初始化失败 (%d)，离线缓存模块无法启动", ret);
        return ret;
    }
    cache_load_state();
    LOG_INF("NVS 离线缓存已恢复: head=%u tail=%u count=%u",
            g_cache.head, g_cache.tail, g_cache.count);

    LOG_INF("离线缓存模块初始化完成");
    return 0;
}

int offline_cache_start(void)
{
    if (g_cache.status != MODULE_STATUS_INITIALIZED &&
        g_cache.status != MODULE_STATUS_STOPPED) {
        return -1;
    }
    g_cache.status = MODULE_STATUS_RUNNING;
    atomic_set(&g_cache.net_connected, 0);
    LOG_INF("离线缓存模块已启动");
    return 0;
}

int offline_cache_stop(void)
{
    if (g_cache.status != MODULE_STATUS_RUNNING) {
        return 0;
    }
    /* 取消挂起的批量上报，避免模块停止后工作项仍执行 */
    k_work_cancel_delayable(&g_cache.upload_work);
    cache_save_state();
    g_cache.status = MODULE_STATUS_STOPPED;
    LOG_INF("离线缓存模块已停止");
    return 0;
}

int offline_cache_shutdown(void)
{
    offline_cache_stop();
    g_cache.status = MODULE_STATUS_UNINITIALIZED;
    return 0;
}

void offline_cache_on_event(const event_t* event, void* user_data)
{
    ARG_UNUSED(user_data);
    if (event == NULL || g_cache.status != MODULE_STATUS_RUNNING) {
        return;
    }

    switch (event->type) {
    case EVENT_TYPE_CLOUD_UPLOAD:
        if (event->data_len == sizeof(gateway_cloud_data_t)) {
            cache_on_cloud_upload((const gateway_cloud_data_t*)gateway_event_data(event));
        }
        break;

    case EVENT_TYPE_CLOUD_CONNECTED:
        cache_on_net_state(true);
        break;

    case EVENT_TYPE_CLOUD_DISCONNECTED:
        cache_on_net_state(false);
        break;
    }
}

module_status_t offline_cache_get_status(void)
{
    return g_cache.status;
}

int offline_cache_control(int cmd, void* arg)
{
    switch (cmd) {
    case CACHE_CMD_GET_INFO:
        if (arg != NULL) {
            uint32_t* info = (uint32_t*)arg;
            offline_cache_get_info(&info[0], &info[1], &info[2]);
        }
        return 0;
    case CACHE_CMD_CLEAR:
        k_mutex_lock(&g_cache.lock, K_FOREVER);
        g_cache.head = 0;
        g_cache.tail = 0;
        g_cache.count = 0;
        cache_save_state();
        k_mutex_unlock(&g_cache.lock);
        LOG_INF("离线缓存已清空");
        return 0;
    default:
        return -1;
    }
}

/* =============================================================================
 * 模块特定 API
 * ============================================================================= */

void offline_cache_get_info(uint32_t* entry_count, uint32_t* max_entries, uint32_t* overflow_count)
{
    k_mutex_lock(&g_cache.lock, K_FOREVER);
    if (entry_count != NULL) *entry_count = g_cache.count;
    if (max_entries != NULL) *max_entries = CACHE_MAX_ENTRIES;
    if (overflow_count != NULL) *overflow_count = g_cache.overflow_count;
    k_mutex_unlock(&g_cache.lock);
}

/* =============================================================================
 * 内部函数
 * ============================================================================= */

static void cache_on_cloud_upload(const gateway_cloud_data_t* data)
{
    if (data == NULL) return;

    /* 如果网络已连接，不缓存（cloud_upload 会直接发送） */
    if (atomic_get(&g_cache.net_connected) != 0) {
        return;
    }

    k_mutex_lock(&g_cache.lock, K_FOREVER);

    int ret = cache_write_entry(data->json_payload, data->data_type);
    if (ret == 0) {
        g_cache.write_count++;
        /* M3：写入成功后立即持久化状态，消除"条目已落 NVS 但状态未更新"的掉电窗口 */
        cache_save_state();
        LOG_DBG("离线缓存写入成功: %s", data->json_payload);
    } else {
        g_cache.overflow_count++;
        LOG_WRN("离线缓存写入失败/溢出");
    }

    k_mutex_unlock(&g_cache.lock);
}

static void cache_upload_work_handler(struct k_work* work)
{
    ARG_UNUSED(work);

    if (g_cache.status != MODULE_STATUS_RUNNING || atomic_get(&g_cache.net_connected) == 0) {
        return;
    }

    k_mutex_lock(&g_cache.lock, K_FOREVER);

    uint16_t batch_count = 0;
    char json[CACHE_ENTRY_SIZE];

    while (g_cache.count > 0) {
        uint8_t data_type = 0;
        int ret = cache_read_entry(json, sizeof(json), &data_type);
        if (ret != 0) break;

        gateway_cloud_data_t data = {
            .timestamp = k_uptime_get_32(),
            .data_type = data_type,
        };
        strncpy(data.json_payload, json, sizeof(data.json_payload) - 1);
        data.json_payload[sizeof(data.json_payload) - 1] = '\0';

        /* 重播发布回放事件（REPLAY），仅 cloud_upload 订阅直发；不再发 CLOUD_UPLOAD，
         * 避免被 offline_cache 自身的缓存路径重新捕获造成回环 */
        event_publish_copy(EVENT_TYPE_CLOUD_REPLAY, EVENT_PRIORITY_NORMAL,
                           &data, sizeof(data));

        g_cache.read_count++;
        batch_count++;

        if (batch_count >= 10) {
            break;
        }
    }

    cache_save_state();
    k_mutex_unlock(&g_cache.lock);

    if (batch_count > 0) {
        LOG_INF("批量上报: %u 条", batch_count);
    }

    /* 如果还有剩余数据，100ms 后继续上报 */
    if (g_cache.count > 0 && atomic_get(&g_cache.net_connected) != 0) {
        k_work_reschedule(&g_cache.upload_work, K_MSEC(100));
    }
}

static void cache_on_net_state(bool connected)
{
    atomic_val_t was_connected = atomic_set(&g_cache.net_connected, connected ? 1 : 0);

    if (connected && (was_connected == 0)) {
        /* 网络恢复，启动批量上报工作项 */
        LOG_INF("网络恢复，开始批量上报离线数据...");
        k_work_reschedule(&g_cache.upload_work, K_NO_WAIT);
    }
}

static int cache_write_entry(const char* json_data, uint8_t data_type)
{
    if (g_cache.fs.flash_device == NULL) {
        return -ENODEV;
    }

    uint16_t nvs_id = CACHE_NVS_ID_START + g_cache.head;
    cache_entry_t entry;
    entry.timestamp = k_uptime_get_32();
    entry.data_type = data_type;
    strncpy(entry.data, json_data, sizeof(entry.data) - 1);
    entry.data[sizeof(entry.data) - 1] = '\0';

    int ret = nvs_write(&g_cache.fs, nvs_id, &entry, sizeof(entry));
    if (ret < 0) {
        return ret;
    }
    if ((size_t)ret < sizeof(entry)) {
        /* NVS 空间不足，部分写入视为失败 */
        return -ENOSPC;
    }

    if (g_cache.count >= CACHE_MAX_ENTRIES) {
        /* 覆盖最旧数据 */
        g_cache.tail = (g_cache.tail + 1) % CACHE_MAX_ENTRIES;
    } else {
        g_cache.count++;
    }
    g_cache.head = (g_cache.head + 1) % CACHE_MAX_ENTRIES;

    return 0;
}

static int cache_read_entry(char* out_data, size_t out_size, uint8_t* out_data_type)
{
    if (g_cache.count == 0) {
        return -1;
    }

    if (g_cache.fs.flash_device == NULL) {
        return -ENODEV;
    }

    uint16_t nvs_id = CACHE_NVS_ID_START + g_cache.tail;
    cache_entry_t entry;
    size_t len = sizeof(entry);

    int ret = nvs_read(&g_cache.fs, nvs_id, &entry, len);
    if (ret < 0) {
        return ret;
    }

    if (out_data_type != NULL) {
        *out_data_type = entry.data_type;
    }
    strncpy(out_data, entry.data, out_size - 1);
    out_data[out_size - 1] = '\0';

    g_cache.tail = (g_cache.tail + 1) % CACHE_MAX_ENTRIES;
    g_cache.count--;

    return 0;
}

static int cache_nvs_init(void)
{
    const struct flash_area* fa;
    int ret = flash_area_open(FIXED_PARTITION_ID(storage_partition), &fa);
    if (ret != 0) {
        return ret;
    }

    g_cache.fs.flash_device = fa->fa_dev;
    g_cache.fs.offset = fa->fa_off;

    /* 查询 flash 实际 sector (page) 大小 */
    struct flash_pages_info page_info;
    ret = flash_get_page_info_by_offs(fa->fa_dev, fa->fa_off, &page_info);
    if (ret != 0) {
        flash_area_close(fa);
        return ret;
    }
    g_cache.fs.sector_size = page_info.size;
    g_cache.fs.sector_count = fa->fa_size / page_info.size;

    if (g_cache.fs.sector_count == 0 || (fa->fa_size % page_info.size) != 0) {
        LOG_ERR("storage_partition 大小 (%u) 不是 sector_size (%u) 的整数倍",
                (unsigned)fa->fa_size, (unsigned)page_info.size);
        flash_area_close(fa);
        return -EINVAL;
    }

    ret = nvs_mount(&g_cache.fs);
    if (ret != 0) {
        g_cache.fs.flash_device = NULL;
        flash_area_close(fa);
        return ret;
    }

    flash_area_close(fa);
    return 0;
}

static void cache_save_state(void)
{
    if (g_cache.fs.flash_device == NULL) return;

    cache_state_t state = {
        .count = g_cache.count,
        .head  = g_cache.head,
        .tail  = g_cache.tail,
        .crc   = cache_state_crc(g_cache.count, g_cache.head, g_cache.tail),
    };

    int ret = nvs_write(&g_cache.fs, CACHE_STATE_ID, &state, sizeof(state));
    if (ret < 0) {
        LOG_WRN("保存缓存状态失败: %d", ret);
    }
}

static void cache_load_state(void)
{
    if (g_cache.fs.flash_device == NULL) return;

    cache_state_t state;
    int ret = nvs_read(&g_cache.fs, CACHE_STATE_ID, &state, sizeof(state));
    if (ret < 0) {
        LOG_WRN("加载缓存状态失败: %d，重置为默认值", ret);
        g_cache.count = 0;
        g_cache.head  = 0;
        g_cache.tail  = 0;
        return;
    }

    /* CRC 校验：若数据损坏则丢弃，使用默认值重置 */
    if (state.crc != cache_state_crc(state.count, state.head, state.tail)) {
        LOG_WRN("缓存状态 CRC 校验失败，已丢弃损坏数据");
        g_cache.count = 0;
        g_cache.head  = 0;
        g_cache.tail  = 0;
        return;
    }

    /* 范围校验 */
    if (state.count > CACHE_MAX_ENTRIES ||
        state.head  >= CACHE_MAX_ENTRIES ||
        state.tail  >= CACHE_MAX_ENTRIES) {
        LOG_WRN("缓存状态数据越界，重置为默认值");
        g_cache.count = 0;
        g_cache.head  = 0;
        g_cache.tail  = 0;
        return;
    }

    g_cache.count = state.count;
    g_cache.head  = state.head;
    g_cache.tail  = state.tail;
}

/* =============================================================================
 * 模块接口声明与自动注册
 * ============================================================================= */

static const char* const offline_cache_deps[] = {NULL};

DECLARE_MODULE_INTERFACE_WITH_DEPS(offline_cache, offline_cache_deps);

const module_interface_t* offline_cache_get_interface(void)
{
    return &offline_cache_interface;
}

static int offline_cache_auto_register(void)
{
    uint32_t module_id;
    if (module_manager_register(offline_cache_get_interface(), NULL, &module_id) != 0) {
        LOG_ERR("离线缓存模块注册失败");
        return -EIO;
    }
    LOG_INF("离线缓存模块已注册 (id=%u)", module_id);
    return 0;
}

SYS_INIT(offline_cache_auto_register, POST_KERNEL, GATEWAY_INIT_PRIO_OFFLINE_CACHE);
