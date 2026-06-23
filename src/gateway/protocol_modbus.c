/**
 * @file protocol_modbus.c
 * @brief Modbus RTU Master 模块实现
 *
 * 通过 UART/RS-485 周期性轮询 Modbus 从站，读取保持寄存器，解析为传感器数据格式，
 * 通过 `gateway_sensor_publish()` 发布到 data_bus 通道 "sensor"，
 * 原始寄存器块另通过 `gateway_modbus_raw_publish()` 发布到 "modbus_raw"。
 */

#include "protocol_modbus.h"
#include "gateway_events.h"
#include "gateway_config.h"
#include <zeplod/app_config.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include <zeplod/event_system.h>
#include <zeplod/module_manager.h>

LOG_MODULE_REGISTER(protocol_modbus, CONFIG_SYS_LOG_LEVEL);

/* =============================================================================
 * 配置
 * ============================================================================= */

#define MODBUS_THREAD_PRIORITY   GATEWAY_THREAD_PRIORITY_DEFAULT
#define MODBUS_THREAD_STACK_SIZE GATEWAY_THREAD_STACK_SIZE_DEFAULT
#define MODBUS_RX_BUF_SIZE       256
#define MODBUS_TX_BUF_SIZE       32

/* 按字节计算的发送时间：10bit（1起始+8数据+1停止）/ 波特率，微秒 */
#define MODBUS_BYTE_TIME_US(baud)  ((10U * 1000000U + (baud) / 2) / (baud))

/* =============================================================================
 * 内部数据结构
 * ============================================================================= */

typedef struct {
    module_status_t      status;
    struct k_thread      worker_thread;
    K_KERNEL_STACK_MEMBER(worker_stack, MODBUS_THREAD_STACK_SIZE);
    const struct device* dev;
    /* RS-485 DE GPIO */
    const struct gpio_dt_spec* de_gpio;
    bool                 has_de_gpio;
    /* 缓冲区 */
    uint8_t              rx_buf[MODBUS_RX_BUF_SIZE];
    uint8_t              tx_buf[MODBUS_TX_BUF_SIZE];
    size_t               rx_len;
    /* 配置 */
    uint8_t              slave_id;
    uint16_t             poll_start_addr;
    uint16_t             poll_reg_count;
    uint32_t             poll_interval_ms;
    uint32_t             baudrate;          /**< 运行时波特率（用于帧时延计算） */
    /* 统计 */
    uint32_t             tx_count;
    uint32_t             rx_count;
    uint32_t             err_count;
    /* 同步：bus_mutex 贯穿整笔 Modbus 事务（清空→发送→接收→解析），防止并发 */
    struct k_mutex       bus_mutex;
} protocol_modbus_cb_t;

/* =============================================================================
 * 静态变量
 * ============================================================================= */

static protocol_modbus_cb_t g_modbus;

#if DT_HAS_ALIAS(rs485_de)
static const struct gpio_dt_spec g_de_gpio = GPIO_DT_SPEC_GET(DT_ALIAS(rs485_de), gpios);
#endif

/* =============================================================================
 * 前向声明
 * ============================================================================= */

static void modbus_worker_thread(void* p1, void* p2, void* p3);
static void modbus_uart_irq_cb(const struct device* dev, void* user_data);
static void modbus_send_frame(const uint8_t* data, size_t len);
static int  modbus_receive_response(uint8_t* buf, size_t buf_len,
                                     size_t* out_len, k_timeout_t timeout);
static uint16_t crc16_modbus(const uint8_t* data, size_t len);

/* =============================================================================
 * 模块接口实现
 * ============================================================================= */

int protocol_modbus_init(void* config)
{
    ARG_UNUSED(config);
    LOG_INF("初始化 Modbus 模块...");

    memset(&g_modbus, 0, sizeof(g_modbus));
    g_modbus.status = MODULE_STATUS_INITIALIZED;
    g_modbus.slave_id = CONFIG_GATEWAY_MODBUS_SLAVE_ID;
    g_modbus.poll_start_addr = GATEWAY_MODBUS_REG_START_DEFAULT;
    g_modbus.poll_reg_count = GATEWAY_MODBUS_REG_COUNT_DEFAULT;
    g_modbus.poll_interval_ms = CONFIG_GATEWAY_MODBUS_POLL_INTERVAL_MS;
    /* 从 Kconfig 取运行时波特率，用于 send_frame 帧时延计算 */
    g_modbus.baudrate = CONFIG_GATEWAY_MODBUS_BAUDRATE;

    /* bus_mutex：贯穿整笔事务（清空 rx → 发送 → 接收 → 解析），替代原 tx_mutex + rx_sem */
    k_mutex_init(&g_modbus.bus_mutex);

    LOG_INF("Modbus 模块初始化完成");
    return 0;
}

int protocol_modbus_start(void)
{
    if (g_modbus.status != MODULE_STATUS_INITIALIZED &&
        g_modbus.status != MODULE_STATUS_STOPPED) {
        return -1;
    }

    /* 获取 UART 设备 */
    g_modbus.dev = DEVICE_DT_GET_OR_NULL(DT_ALIAS(rs485_uart));
    if (g_modbus.dev == NULL) {
        g_modbus.dev = device_get_binding(CONFIG_GATEWAY_MODBUS_DEVICE);
    }
    if (g_modbus.dev == NULL || !device_is_ready(g_modbus.dev)) {
        LOG_ERR("Modbus UART 设备未就绪");
        g_modbus.status = MODULE_STATUS_ERROR;
        return -1;
    }

    /* 配置 RS-485 DE GPIO */
#if DT_HAS_ALIAS(rs485_de)
    if (device_is_ready(g_de_gpio.port)) {
        gpio_pin_configure_dt(&g_de_gpio, GPIO_OUTPUT_INACTIVE);
        g_modbus.de_gpio = &g_de_gpio;
        g_modbus.has_de_gpio = true;
        LOG_INF("RS-485 DE GPIO 已配置");
    } else {
        g_modbus.has_de_gpio = false;
    }
#else
    g_modbus.has_de_gpio = false;
#endif

    /* 注册 UART IRQ 回调 */
    if (uart_irq_callback_user_data_set(g_modbus.dev, modbus_uart_irq_cb, &g_modbus) == 0) {
        uart_irq_rx_enable(g_modbus.dev);
    }

    g_modbus.status = MODULE_STATUS_RUNNING;

    k_thread_create(&g_modbus.worker_thread, g_modbus.worker_stack,
                    K_THREAD_STACK_SIZEOF(g_modbus.worker_stack),
                    modbus_worker_thread, NULL, NULL, NULL,
                    MODBUS_THREAD_PRIORITY, 0, K_FOREVER);
    k_thread_name_set(&g_modbus.worker_thread, "proto_modbus");
    k_thread_start(&g_modbus.worker_thread);

    LOG_INF("Modbus 模块已启动: %s", g_modbus.dev->name);
    return 0;
}

int protocol_modbus_stop(void)
{
    if (g_modbus.status != MODULE_STATUS_RUNNING) {
        return 0;
    }

    /* 置位 STOPPED：工作线程在分片短睡中检查状态后自然退出（不使用 k_thread_abort）。
     * modbus_receive_response 按 1ms 轮询，最长超时 GATEWAY_MODBUS_RX_TIMEOUT_MS(500ms)，
     * 之后 worker 进入分片睡眠检查状态退出，join 在 ~600ms 内完成。 */
    g_modbus.status = MODULE_STATUS_STOPPED;
    k_thread_join(&g_modbus.worker_thread, K_FOREVER);

    /* 停止 UART IRQ（在 worker_thread 退出后，确保无并发访问） */
    if (g_modbus.dev != NULL) {
        uart_irq_rx_disable(g_modbus.dev);
    }

    LOG_INF("Modbus 模块已停止");
    return 0;
}

int protocol_modbus_shutdown(void)
{
    protocol_modbus_stop();
    g_modbus.status = MODULE_STATUS_UNINITIALIZED;
    return 0;
}

void protocol_modbus_on_event(const event_t* event, void* user_data)
{
    ARG_UNUSED(user_data);
    if (event == NULL) return;
}

module_status_t protocol_modbus_get_status(void)
{
    return g_modbus.status;
}

int protocol_modbus_control(int cmd, void* arg)
{
    switch (cmd) {
    case MODBUS_CMD_READ_REGS: {
        if (arg == NULL) return -1;
        modbus_read_regs_arg_t* rra = (modbus_read_regs_arg_t*)arg;
        if (rra->reg_count == 0 || rra->reg_count > 16) return -1;
        return protocol_modbus_read_holding_regs(rra->slave_id, rra->start_addr,
                                                   rra->reg_count, rra->out_values);
    }
    case MODBUS_CMD_GET_STATS:
        if (arg != NULL) {
            uint32_t* s = (uint32_t*)arg;
            protocol_modbus_get_stats(&s[0], &s[1], &s[2]);
        }
        return 0;
    case MODBUS_CMD_SET_SLAVE_ID:
        if (arg == NULL) {
            return -1;
        }
        g_modbus.slave_id = *(uint8_t*)arg;
        return 0;
    case MODBUS_CMD_SET_INTERVAL:
        if (arg == NULL) {
            return -1;
        }
        g_modbus.poll_interval_ms = *(uint32_t*)arg;
        return 0;
    default:
        return -1;
    }
}

/* =============================================================================
 * 模块特定 API
 * ============================================================================= */

int protocol_modbus_read_holding_regs(uint8_t slave_id, uint16_t start_addr,
                                       uint16_t reg_count, uint16_t* out_values)
{
    if (g_modbus.status != MODULE_STATUS_RUNNING || reg_count == 0 || reg_count > 16) {
        return -1;
    }

    /* bus_mutex 贯穿整笔事务：清空 rx → 发送 → 接收 → 解析，防止并发事务交叉 */
    k_mutex_lock(&g_modbus.bus_mutex, K_FOREVER);

    uint8_t req[8];
    req[0] = slave_id;
    req[1] = 0x03; /* Read Holding Registers */
    req[2] = (uint8_t)(start_addr >> 8);
    req[3] = (uint8_t)(start_addr & 0xFF);
    req[4] = (uint8_t)(reg_count >> 8);
    req[5] = (uint8_t)(reg_count & 0xFF);

    uint16_t crc = crc16_modbus(req, 6);
    req[6] = (uint8_t)(crc & 0xFF);
    req[7] = (uint8_t)(crc >> 8);

    /* 清空接收缓冲（在 bus_mutex 保护下，无并发访问） */
    g_modbus.rx_len = 0;

    /* 发送请求（modbus_send_frame 在 bus_mutex 下调用，内部不再单独加锁） */
    modbus_send_frame(req, sizeof(req));
    g_modbus.tx_count++;

    /* 等待响应 */
    size_t resp_len = 0;
    uint8_t resp[MODBUS_RX_BUF_SIZE];
    int ret = modbus_receive_response(resp, sizeof(resp), &resp_len,
                                      K_MSEC(GATEWAY_MODBUS_RX_TIMEOUT_MS));
    if (ret != 0 || resp_len < 5) {
        g_modbus.err_count++;
        k_mutex_unlock(&g_modbus.bus_mutex);
        return ret;
    }

    /* 校验从站 ID 和功能码 */
    if (resp[0] != slave_id || resp[1] != 0x03) {
        g_modbus.err_count++;
        k_mutex_unlock(&g_modbus.bus_mutex);
        return -1;
    }

    uint8_t byte_count = resp[2];
    if (byte_count != reg_count * 2 || resp_len < (size_t)(3 + byte_count + 2)) {
        g_modbus.err_count++;
        k_mutex_unlock(&g_modbus.bus_mutex);
        return -1;
    }

    /* CRC 校验 */
    uint16_t resp_crc = (uint16_t)(resp[3 + byte_count] | (resp[3 + byte_count + 1] << 8));
    if (crc16_modbus(resp, 3 + byte_count) != resp_crc) {
        g_modbus.err_count++;
        k_mutex_unlock(&g_modbus.bus_mutex);
        return -1;
    }

    g_modbus.rx_count++;

    /* 解析寄存器值 */
    if (out_values != NULL) {
        for (uint16_t i = 0; i < reg_count; i++) {
            out_values[i] = (uint16_t)((resp[3 + i * 2] << 8) | resp[3 + i * 2 + 1]);
        }
    }

    k_mutex_unlock(&g_modbus.bus_mutex);
    return 0;
}

void protocol_modbus_get_stats(uint32_t* tx_count, uint32_t* rx_count, uint32_t* err_count)
{
    if (tx_count != NULL) *tx_count = g_modbus.tx_count;
    if (rx_count != NULL) *rx_count = g_modbus.rx_count;
    if (err_count != NULL) *err_count = g_modbus.err_count;
}

/* =============================================================================
 * 内部函数
 * ============================================================================= */

static void modbus_uart_irq_cb(const struct device* dev, void* user_data)
{
    protocol_modbus_cb_t* cb = (protocol_modbus_cb_t*)user_data;

    if (!uart_irq_update(dev)) {
        return;
    }

    while (uart_irq_rx_ready(dev)) {
        uint8_t byte;
        if (uart_fifo_read(dev, &byte, 1) > 0) {
            if (cb->rx_len < MODBUS_RX_BUF_SIZE) {
                cb->rx_buf[cb->rx_len++] = byte;
            } else {
                cb->err_count++;
                LOG_WRN("Modbus RX 缓冲区溢出");
            }
        }
    }
    /* 注意：TX complete 回调未启用（uart_irq_tx_enable 未调用），
     * RS-485 DE 方向切换在 modbus_send_frame() 中通过延时完成。 */
}

/**
 * @brief 发送 Modbus 帧
 *
 * @param data 帧数据指针
 * @param len  帧长度（字节）
 *
 * @note 调用方必须持有 g_modbus.bus_mutex。本函数不再内部加锁，
 *       帧时延按运行时 g_modbus.baudrate 计算，消除硬编码波特率假设。
 */
static void modbus_send_frame(const uint8_t* data, size_t len)
{
    /* 调用方（read_holding_regs）已持有 bus_mutex，无需重复加锁 */
    if (g_modbus.has_de_gpio && g_modbus.de_gpio != NULL) {
        gpio_pin_set_dt(g_modbus.de_gpio, 1);
        k_usleep(10); /* DE 建立时间，典型 2-10us */
    }

    for (size_t i = 0; i < len; i++) {
        uart_poll_out(g_modbus.dev, data[i]);
    }

    /* 等待物理发送完成：按运行时波特率计算帧时间 + 2ms 安全余量 */
    uint32_t baud = (g_modbus.baudrate > 0) ? g_modbus.baudrate : GATEWAY_MODBUS_BAUDRATE_DEFAULT;
    uint32_t frame_time_us = (uint32_t)len * MODBUS_BYTE_TIME_US(baud) + 2000U;
    k_usleep(frame_time_us);

    if (g_modbus.has_de_gpio && g_modbus.de_gpio != NULL) {
        gpio_pin_set_dt(g_modbus.de_gpio, 0);
    }
}

static int modbus_receive_response(uint8_t* buf, size_t buf_len,
                                    size_t* out_len, k_timeout_t timeout)
{
    int64_t end_time = k_uptime_get() + k_ticks_to_ms_ceil64(timeout.ticks);
    size_t  last_rx_len = 0;
    int64_t last_rx_time = 0;
    /* Modbus RTU inter-frame gap = 3.5T; 用 5ms 覆盖 9600bps 及更高速率 */
    const int64_t inter_frame_ms = 5;

    while (k_uptime_get() < end_time) {
        if (g_modbus.rx_len > 0) {
            if (g_modbus.rx_len != last_rx_len) {
                /* 新数据还在到达，更新跟踪 */
                last_rx_len  = g_modbus.rx_len;
                last_rx_time = k_uptime_get();
            } else if ((k_uptime_get() - last_rx_time) >= inter_frame_ms) {
                /* 超过 inter-frame gap，认为帧已完整 */
                size_t copy_len = g_modbus.rx_len;
                if (copy_len > buf_len) {
                    copy_len = buf_len;
                }
                memcpy(buf, g_modbus.rx_buf, copy_len);
                *out_len = copy_len;
                g_modbus.rx_len = 0;
                return 0;
            }
        }
        k_msleep(1);
    }
    return -ETIMEDOUT;
}

static uint16_t crc16_modbus(const uint8_t* data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

static void modbus_worker_thread(void* p1, void* p2, void* p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    LOG_INF("Modbus 工作线程已启动");

    /* 分片睡眠步长：50ms，保证 stop 后 join 在 ~50ms 内完成（在 poll 间隔结束后） */
#define MODBUS_SLICE_MS 50

    while (g_modbus.status == MODULE_STATUS_RUNNING) {
        uint16_t values[16];
        int ret = protocol_modbus_read_holding_regs(
            g_modbus.slave_id, g_modbus.poll_start_addr,
            g_modbus.poll_reg_count, values);

        if (ret == 0) {
            for (uint16_t i = 0; i < g_modbus.poll_reg_count && i < 16; i++) {
                /* TODO: sensor_type 应按寄存器地址配置映射，而非循环取模 */
                gateway_sensor_data_t sensor = {
                    .timestamp = k_uptime_get_32(),
                    .channel_id = g_modbus.slave_id,
                    .sensor_type = (uint8_t)(i % SENSOR_TYPE_COUNT),
                    .value = (float)values[i] * 0.01f,
                    .raw_u16 = values[i],
                };
                (void)gateway_sensor_publish(&sensor);
            }

            gateway_modbus_data_t mb_data = {
                .timestamp = k_uptime_get_32(),
                .slave_id = g_modbus.slave_id,
                .start_addr = g_modbus.poll_start_addr,
                .reg_count = (uint8_t)g_modbus.poll_reg_count,
            };
            for (uint16_t i = 0; i < g_modbus.poll_reg_count && i < 16; i++) {
                mb_data.values[i] = values[i];
            }
            (void)gateway_modbus_raw_publish(&mb_data);
        }

        /* 分片轮询：每 50ms 检查停止标志，使 stop 后快速退出 */
        uint32_t slices = (g_modbus.poll_interval_ms + MODBUS_SLICE_MS - 1) / MODBUS_SLICE_MS;
        for (uint32_t s = 0; s < slices && g_modbus.status == MODULE_STATUS_RUNNING; s++) {
            k_sleep(K_MSEC(MODBUS_SLICE_MS));
        }
    }

    LOG_INF("Modbus 工作线程已退出");
}

/* =============================================================================
 * 模块接口声明与自动注册
 * ============================================================================= */

static const char* const protocol_modbus_deps[] = {NULL};

DECLARE_MODULE_INTERFACE_WITH_DEPS(protocol_modbus, protocol_modbus_deps);

const module_interface_t* protocol_modbus_get_interface(void)
{
    return &protocol_modbus_interface;
}

static int protocol_modbus_auto_register(void)
{
    uint32_t module_id;
    if (module_manager_register(protocol_modbus_get_interface(), NULL, &module_id) != 0) {
        LOG_ERR("Modbus 模块注册失败");
        return -EIO;
    }
    LOG_INF("Modbus 模块已注册 (id=%u)", module_id);
    return 0;
}

SYS_INIT(protocol_modbus_auto_register, POST_KERNEL, GATEWAY_INIT_PRIO_PROTOCOL_MODBUS);
