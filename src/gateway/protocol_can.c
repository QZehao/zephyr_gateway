/**
 * @file protocol_can.c
 * @brief CAN 总线数据采集模块实现
 *
 * 通过 Zephyr CAN API 接收工业传感器数据帧，解析为标准传感器格式，
 * 通过 `gateway_sensor_publish()` 发布到 data_bus 通道 "sensor"，
 * 原始帧另通过 `gateway_can_raw_publish()` 发布到 "can_raw"。
 */

#include "protocol_can.h"
#include "gateway_events.h"
#include "gateway_config.h"
#include <zeplod/app_config.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/can.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include <zeplod/event_system.h>
#include <zeplod/module_manager.h>

LOG_MODULE_REGISTER(protocol_can, CONFIG_SYS_LOG_LEVEL);

/* =============================================================================
 * 配置
 * ============================================================================= */

#define CAN_THREAD_PRIORITY   GATEWAY_THREAD_PRIORITY_DEFAULT
#define CAN_THREAD_STACK_SIZE GATEWAY_THREAD_STACK_SIZE_DEFAULT
#define CAN_RX_RING_SIZE      GATEWAY_CAN_RX_BUF_SIZE

/* =============================================================================
 * 内部数据结构
 * ============================================================================= */

typedef struct {
    module_status_t         status;
    struct k_thread         rx_thread;
    K_KERNEL_STACK_MEMBER(rx_stack, CAN_THREAD_STACK_SIZE);
    const struct device*    dev;
    struct can_filter       filter;
    int                     filter_id;
    /* stats */
    uint32_t                rx_count;
    uint32_t                tx_count;
    uint32_t                err_count;
} protocol_can_cb_t;

/* =============================================================================
 * 静态变量
 * ============================================================================= */

static protocol_can_cb_t g_can;

/* =============================================================================
 * 前向声明
 * ============================================================================= */

static void can_rx_thread(void* p1, void* p2, void* p3);
static void can_state_callback(const struct device* dev, enum can_state state,
                                struct can_bus_err_cnt err_cnt, void* user_data);

/* =============================================================================
 * 模块接口实现
 * ============================================================================= */

int protocol_can_init(void* config)
{
    ARG_UNUSED(config);
    LOG_INF("初始化 CAN 模块...");

    memset(&g_can, 0, sizeof(g_can));
    g_can.status = MODULE_STATUS_INITIALIZED;
    g_can.filter_id = -1;

    LOG_INF("CAN 模块初始化完成");
    return 0;
}

int protocol_can_start(void)
{
    if (g_can.status != MODULE_STATUS_INITIALIZED && g_can.status != MODULE_STATUS_STOPPED) {
        return -1;
    }

    /* 获取 CAN 设备 */
    g_can.dev = DEVICE_DT_GET_OR_NULL(DT_ALIAS(can0));
    if (g_can.dev == NULL) {
        LOG_WRN("未找到 can0 alias，尝试 device_get_binding");
        g_can.dev = device_get_binding(CONFIG_GATEWAY_CAN_DEVICE);
    }
    if (g_can.dev == NULL || !device_is_ready(g_can.dev)) {
        LOG_ERR("CAN 设备未就绪");
        g_can.status = MODULE_STATUS_ERROR;
        return -1;
    }

    /* 设置 CAN 模式 */
    if (can_set_mode(g_can.dev, CAN_MODE_NORMAL) != 0) {
        LOG_ERR("设置 CAN 模式失败");
        g_can.status = MODULE_STATUS_ERROR;
        return -1;
    }

    /* 启动 CAN */
    if (can_start(g_can.dev) != 0) {
        LOG_ERR("启动 CAN 失败");
        g_can.status = MODULE_STATUS_ERROR;
        return -1;
    }

    /* 配置 RX 过滤器参数（线程启动后使用） */
    g_can.filter.flags = CAN_FILTER_IDE;
    g_can.filter.id = CONFIG_GATEWAY_CAN_FILTER_ID;
    g_can.filter.mask = CONFIG_GATEWAY_CAN_FILTER_MASK;
    g_can.filter_id = -1;

    /* 设置状态回调 */
    can_set_state_change_callback(g_can.dev, can_state_callback, NULL);

    g_can.status = MODULE_STATUS_RUNNING;

    /* 创建接收线程（线程内自行添加 msgq 过滤器） */
    k_thread_create(&g_can.rx_thread, g_can.rx_stack,
                    K_THREAD_STACK_SIZEOF(g_can.rx_stack),
                    can_rx_thread, NULL, NULL, NULL,
                    CAN_THREAD_PRIORITY, 0, K_FOREVER);
    k_thread_name_set(&g_can.rx_thread, "proto_can");
    k_thread_start(&g_can.rx_thread);

    LOG_INF("CAN 模块已启动: %s", g_can.dev->name);
    return 0;
}

int protocol_can_stop(void)
{
    if (g_can.status != MODULE_STATUS_RUNNING) {
        return 0;
    }

    /* 置位 STOPPED：rx_thread 的 k_msgq_get 带 100ms 超时，
     * 超时后检查 status 退出，线程退出路径会执行 can_remove_rx_filter 清理 */
    g_can.status = MODULE_STATUS_STOPPED;
    k_thread_join(&g_can.rx_thread, K_FOREVER);

    /* 停止 CAN 硬件（在 rx_thread 退出后执行，确保无并发访问） */
    if (g_can.dev != NULL) {
        can_stop(g_can.dev);
    }

    LOG_INF("CAN 模块已停止");
    return 0;
}

int protocol_can_shutdown(void)
{
    protocol_can_stop();
    g_can.status = MODULE_STATUS_UNINITIALIZED;
    return 0;
}

void protocol_can_on_event(const event_t* event, void* user_data)
{
    ARG_UNUSED(user_data);
    if (event == NULL) {
        return;
    }
    /* CAN 模块当前不订阅外部事件 */
}

module_status_t protocol_can_get_status(void)
{
    return g_can.status;
}

int protocol_can_control(int cmd, void* arg)
{
    switch (cmd) {
    case 0: /* GET_STATS */
        if (arg != NULL) {
            uint32_t* stats = (uint32_t*)arg;
            protocol_can_get_stats(&stats[0], &stats[1], &stats[2]);
        }
        return 0;
    default:
        return -1;
    }
}

/* =============================================================================
 * 模块特定 API
 * ============================================================================= */

int protocol_can_send(uint32_t id, const uint8_t* data, uint8_t dlc, bool ext_id)
{
    if (g_can.status != MODULE_STATUS_RUNNING || g_can.dev == NULL || data == NULL || dlc > 8) {
        return -1;
    }

    struct can_frame frame = {
        .id = id,
        .dlc = dlc,
        .flags = ext_id ? CAN_FRAME_IDE : 0,
    };
    memcpy(frame.data, data, dlc);

    int ret = can_send(g_can.dev, &frame, K_MSEC(100), NULL, NULL);
    if (ret == 0) {
        g_can.tx_count++;
    } else {
        g_can.err_count++;
    }
    return ret;
}

void protocol_can_get_stats(uint32_t* rx_count, uint32_t* tx_count, uint32_t* err_count)
{
    if (rx_count != NULL) *rx_count = g_can.rx_count;
    if (tx_count != NULL) *tx_count = g_can.tx_count;
    if (err_count != NULL) *err_count = g_can.err_count;
}

/* =============================================================================
 * 内部函数
 * ============================================================================= */

static void can_state_callback(const struct device* dev, enum can_state state,
                                struct can_bus_err_cnt err_cnt, void* user_data)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(err_cnt);
    ARG_UNUSED(user_data);

    const char* state_str = "unknown";
    switch (state) {
    case CAN_STATE_ERROR_ACTIVE:
        state_str = "error_active";
        break;
    case CAN_STATE_ERROR_WARNING:
        state_str = "error_warning";
        break;
    case CAN_STATE_ERROR_PASSIVE:
        state_str = "error_passive";
        break;
    case CAN_STATE_BUS_OFF:
        state_str = "bus_off";
        break;
    case CAN_STATE_STOPPED:
        state_str = "stopped";
        break;
    }
    LOG_INF("CAN 状态变化: %s", state_str);
}

static void can_rx_thread(void* p1, void* p2, void* p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    LOG_INF("CAN 接收线程已启动");

    /* 使用 msgq 方式接收（Zephyr CAN 驱动标准方式） */
    struct can_frame frame;
    struct k_msgq can_msgq;
    char msgq_buffer[CAN_RX_RING_SIZE * sizeof(struct can_frame)];
    k_msgq_init(&can_msgq, msgq_buffer, sizeof(struct can_frame), CAN_RX_RING_SIZE);

    /* 添加过滤器，绑定到本地 msgq */
    g_can.filter_id = can_add_rx_filter_msgq(g_can.dev, &can_msgq, &g_can.filter);
    if (g_can.filter_id < 0) {
        LOG_ERR("CAN RX 过滤器添加失败: %d", g_can.filter_id);
        return;
    }

    while (g_can.status == MODULE_STATUS_RUNNING) {
        if (k_msgq_get(&can_msgq, &frame, K_MSEC(100)) == 0) {
            g_can.rx_count++;

            /* 解析 CAN 帧为传感器数据 */
            gateway_sensor_data_t sensor = {
                .timestamp = k_uptime_get_32(),
                .channel_id = (uint8_t)(frame.id & GATEWAY_CAN_CHANNEL_ID_MASK),
                .sensor_type = SENSOR_TYPE_CURRENT,
                .value = 0.0f,
                .raw_u16 = 0,
            };

            if (frame.dlc >= 2) {
                sensor.raw_u16 = (uint16_t)(frame.data[0] | (frame.data[1] << 8));
                sensor.value = (float)sensor.raw_u16 * 0.01f;
            }

            (void)gateway_sensor_publish(&sensor);

            /* 原始 CAN 帧（无消费者，预留 trace） */
            gateway_can_frame_t can_evt = {
                .timestamp = sensor.timestamp,
                .id = frame.id,
                .dlc = frame.dlc,
                .rtr = (frame.flags & CAN_FRAME_RTR) != 0,
                .ext_id = (frame.flags & CAN_FRAME_IDE) != 0,
            };
            memcpy(can_evt.data, frame.data, frame.dlc);
            (void)gateway_can_raw_publish(&can_evt);

            LOG_DBG("CAN RX: id=0x%x dlc=%u val=%.2f", frame.id, frame.dlc,
                    (double)sensor.value);
        }
    }

    /* 清理 */
    if (g_can.dev != NULL && g_can.filter_id >= 0) {
        can_remove_rx_filter(g_can.dev, g_can.filter_id);
        g_can.filter_id = -1;
    }

    LOG_INF("CAN 接收线程已退出");
}

/* =============================================================================
 * 模块接口声明与自动注册
 * ============================================================================= */

static const char* const protocol_can_deps[] = {NULL};

DECLARE_MODULE_INTERFACE_WITH_DEPS(protocol_can, protocol_can_deps);

const module_interface_t* protocol_can_get_interface(void)
{
    return &protocol_can_interface;
}

static int protocol_can_auto_register(void)
{
    uint32_t module_id;
    if (module_manager_register(protocol_can_get_interface(), NULL, &module_id) != 0) {
        LOG_ERR("CAN 模块注册失败");
        return -EIO;
    }
    LOG_INF("CAN 模块已注册 (id=%u)", module_id);
    return 0;
}

SYS_INIT(protocol_can_auto_register, POST_KERNEL, GATEWAY_INIT_PRIO_PROTOCOL_CAN);
