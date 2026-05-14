/**
 * @file data_simulator.c
 * @brief 数据模拟生成采集模块实现
 *
 * 按配置的基线和波动范围生成随机传感器数据，
 * 通过事件系统发布 EVENT_TYPE_SENSOR_DATA，用于功能测试。
 */

#include "data_simulator.h"
#include "gateway_config.h"
#include "app_config.h"

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zephyr/shell/shell.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "event_system.h"
#include "module_manager.h"

LOG_MODULE_REGISTER(data_simulator, CONFIG_SYS_LOG_LEVEL);

/* =============================================================================
 * 内部配置
 * ============================================================================= */

#define SIM_THREAD_PRIORITY   GATEWAY_THREAD_PRIORITY_DEFAULT
#define SIM_THREAD_STACK_SIZE GATEWAY_THREAD_STACK_SIZE_DEFAULT

/* =============================================================================
 * 内部数据结构
 * ============================================================================= */

typedef struct {
    module_status_t      status;
    sim_channel_config_t channels[SENSOR_TYPE_COUNT];
    uint32_t             sample_interval_ms;
    uint32_t             sample_count;
    uint32_t             inject_count;
    struct k_thread      thread;
    K_KERNEL_STACK_MEMBER(stack, SIM_THREAD_STACK_SIZE);
} data_simulator_cb_t;

/* =============================================================================
 * 静态变量
 * ============================================================================= */

static data_simulator_cb_t g_sim;

/* =============================================================================
 * 前向声明
 * ============================================================================= */

static void data_sim_thread_func(void* p1, void* p2, void* p3);
static float generate_noise(float baseline, float range);
static void publish_sensor_data(uint8_t sensor_type, float value);

/* =============================================================================
 * Shell 命令前向声明
 * ============================================================================= */

#if defined(CONFIG_SHELL)
static int cmd_sim_status(const struct shell* sh, size_t argc, char** argv);
static int cmd_sim_start(const struct shell* sh, size_t argc, char** argv);
static int cmd_sim_stop(const struct shell* sh, size_t argc, char** argv);
static int cmd_sim_interval(const struct shell* sh, size_t argc, char** argv);
static int cmd_sim_config(const struct shell* sh, size_t argc, char** argv);
static int cmd_sim_inject(const struct shell* sh, size_t argc, char** argv);
#endif

/* =============================================================================
 * 模块接口实现
 * ============================================================================= */

int data_simulator_init(void* config)
{
    ARG_UNUSED(config);
    LOG_INF("初始化数据模拟模块...");

    memset(&g_sim, 0, sizeof(g_sim));

    g_sim.sample_interval_ms = SIM_DEFAULT_INTERVAL_MS;

    /* 初始化各通道默认值 */
    g_sim.channels[SENSOR_TYPE_CURRENT] = (sim_channel_config_t){
        .baseline = SIM_DEFAULT_BASELINE_CURR,
        .range = SIM_DEFAULT_RANGE_CURR,
        .channel_id = 0,
        .enabled = true,
    };
    g_sim.channels[SENSOR_TYPE_TEMPERATURE] = (sim_channel_config_t){
        .baseline = SIM_DEFAULT_BASELINE_TEMP,
        .range = SIM_DEFAULT_RANGE_TEMP,
        .channel_id = 1,
        .enabled = true,
    };
    g_sim.channels[SENSOR_TYPE_VOLTAGE] = (sim_channel_config_t){
        .baseline = SIM_DEFAULT_BASELINE_VOLT,
        .range = SIM_DEFAULT_RANGE_VOLT,
        .channel_id = 2,
        .enabled = true,
    };
    g_sim.channels[SENSOR_TYPE_PRESSURE] = (sim_channel_config_t){
        .baseline = SIM_DEFAULT_BASELINE_PRES,
        .range = SIM_DEFAULT_RANGE_PRES,
        .channel_id = 3,
        .enabled = true,
    };
    g_sim.channels[SENSOR_TYPE_HUMIDITY] = (sim_channel_config_t){
        .baseline = SIM_DEFAULT_BASELINE_HUMI,
        .range = SIM_DEFAULT_RANGE_HUMI,
        .channel_id = 4,
        .enabled = true,
    };

    g_sim.status = MODULE_STATUS_INITIALIZED;
    LOG_INF("数据模拟模块初始化完成");
    return 0;
}

int data_simulator_start(void)
{
    if (g_sim.status != MODULE_STATUS_INITIALIZED && g_sim.status != MODULE_STATUS_STOPPED) {
        return -EALREADY;
    }

    g_sim.status = MODULE_STATUS_RUNNING;

    k_thread_create(&g_sim.thread, g_sim.stack, K_THREAD_STACK_SIZEOF(g_sim.stack),
                    data_sim_thread_func, NULL, NULL, NULL,
                    SIM_THREAD_PRIORITY, 0, K_FOREVER);
    k_thread_name_set(&g_sim.thread, "data_sim");
    k_thread_start(&g_sim.thread);

    LOG_INF("数据模拟模块已启动 (interval=%ums)", g_sim.sample_interval_ms);
    return 0;
}

int data_simulator_stop(void)
{
    if (g_sim.status != MODULE_STATUS_RUNNING) {
        return 0;
    }

    g_sim.status = MODULE_STATUS_STOPPED;
    k_msleep(50);

    LOG_INF("数据模拟模块已停止");
    return 0;
}

int data_simulator_shutdown(void)
{
    data_simulator_stop();
    g_sim.status = MODULE_STATUS_UNINITIALIZED;
    return 0;
}

void data_simulator_on_event(const event_t* event, void* user_data)
{
    ARG_UNUSED(user_data);
    ARG_UNUSED(event);
}

module_status_t data_simulator_get_status(void)
{
    return g_sim.status;
}

int data_simulator_control(int cmd, void* arg)
{
    if (arg == NULL && cmd != SIM_CMD_GET_STATS && cmd != SIM_CMD_RESET_STATS) {
        return -EINVAL;
    }

    switch (cmd) {
    case SIM_CMD_SET_BASELINE: {
        sim_channel_config_t* ch = (sim_channel_config_t*)arg;
        if (ch == NULL || ch->channel_id >= SENSOR_TYPE_COUNT) {
            return -EINVAL;
        }
        g_sim.channels[ch->channel_id].baseline = ch->baseline;
        LOG_INF("传感器 %u 基线设为 %.2f", ch->channel_id, (double)ch->baseline);
        return 0;
    }
    case SIM_CMD_SET_RANGE: {
        sim_channel_config_t* ch = (sim_channel_config_t*)arg;
        if (ch == NULL || ch->channel_id >= SENSOR_TYPE_COUNT || ch->range < 0.0f) {
            return -EINVAL;
        }
        g_sim.channels[ch->channel_id].range = ch->range;
        LOG_INF("传感器 %u 范围设为 %.2f", ch->channel_id, (double)ch->range);
        return 0;
    }
    case SIM_CMD_SET_INTERVAL: {
        uint32_t* interval = (uint32_t*)arg;
        if (interval == NULL || *interval == 0) {
            return -EINVAL;
        }
        g_sim.sample_interval_ms = *interval;
        LOG_INF("采样周期设为 %u ms", *interval);
        return 0;
    }
    case SIM_CMD_GET_STATS: {
        sim_stats_t* stats = (sim_stats_t*)arg;
        if (stats != NULL) {
            stats->sample_count = g_sim.sample_count;
            stats->inject_count = g_sim.inject_count;
        }
        return 0;
    }
    case SIM_CMD_RESET_STATS: {
        g_sim.sample_count = 0;
        g_sim.inject_count = 0;
        return 0;
    }
    case SIM_CMD_INJECT: {
        sim_inject_param_t* param = (sim_inject_param_t*)arg;
        if (param == NULL || param->sensor_type >= SENSOR_TYPE_COUNT) {
            return -EINVAL;
        }
        publish_sensor_data(param->sensor_type, param->value);
        g_sim.inject_count++;
        LOG_INF("注入传感器 %u 值 %.2f", param->sensor_type, (double)param->value);
        return 0;
    }
    default:
        return -ENOTSUP;
    }
}

/* =============================================================================
 * 模块特定 API
 * ============================================================================= */

void data_simulator_get_stats(sim_stats_t* stats)
{
    if (stats != NULL) {
        stats->sample_count = g_sim.sample_count;
        stats->inject_count = g_sim.inject_count;
    }
}

void data_simulator_reset_stats(void)
{
    g_sim.sample_count = 0;
    g_sim.inject_count = 0;
}

/* =============================================================================
 * 内部函数
 * ============================================================================= */

static void data_sim_thread_func(void* p1, void* p2, void* p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    uint32_t last_sample_time = k_uptime_get_32();

    LOG_INF("数据模拟线程启动");

    while (g_sim.status == MODULE_STATUS_RUNNING) {
        uint32_t now = k_uptime_get_32();
        uint32_t elapsed = now - last_sample_time;

        if (elapsed >= g_sim.sample_interval_ms) {
            for (int type = 0; type < SENSOR_TYPE_COUNT; type++) {
                sim_channel_config_t* ch = &g_sim.channels[type];
                if (!ch->enabled) {
                    continue;
                }

                float value = generate_noise(ch->baseline, ch->range);
                publish_sensor_data((uint8_t)type, value);
                g_sim.sample_count++;
            }
            last_sample_time = now;
        }

        k_msleep(10);
    }

    LOG_INF("数据模拟线程停止");
}

static float generate_noise(float baseline, float range)
{
    uint32_t r = sys_rand32_get();
    float normalized = (float)r / (float)UINT32_MAX;
    float offset = (normalized * 2.0f - 1.0f) * range;
    return baseline + offset;
}

static void publish_sensor_data(uint8_t sensor_type, float value)
{
    gateway_sensor_data_t data = {
        .timestamp = k_uptime_get_32(),
        .channel_id = g_sim.channels[sensor_type].channel_id,
        .sensor_type = sensor_type,
        .value = value,
        .raw_u16 = 0,
    };

    event_status_t ret = event_publish_copy(EVENT_TYPE_SENSOR_DATA, EVENT_PRIORITY_NORMAL,
                                             &data, sizeof(data));
    if (ret != EVENT_OK) {
        LOG_WRN("发布模拟数据失败: %d", ret);
    }
}

/* =============================================================================
 * Shell 命令实现
 * ============================================================================= */

#if defined(CONFIG_SHELL)

static int cmd_sim_status(const struct shell* sh, size_t argc, char** argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    const char* status_str = "UNKNOWN";
    switch (g_sim.status) {
    case MODULE_STATUS_RUNNING:
        status_str = "RUNNING";
        break;
    case MODULE_STATUS_INITIALIZED:
        status_str = "INITIALIZED";
        break;
    case MODULE_STATUS_STOPPED:
        status_str = "STOPPED";
        break;
    default:
        break;
    }

    shell_print(sh, "=== 数据模拟模块 ===");
    shell_print(sh, "状态:       %s", status_str);
    shell_print(sh, "采样周期:   %u ms", g_sim.sample_interval_ms);
    shell_print(sh, "样本总数:   %lu", (unsigned long)g_sim.sample_count);
    shell_print(sh, "注入次数:   %lu", (unsigned long)g_sim.inject_count);
    shell_print(sh, "通道配置:");
    for (int i = 0; i < SENSOR_TYPE_COUNT; i++) {
        sim_channel_config_t* ch = &g_sim.channels[i];
        shell_print(sh, "  [%u] %s: baseline=%.2f range=%.2f %s",
                    ch->channel_id,
                    gateway_sensor_type_str((uint8_t)i),
                    (double)ch->baseline, (double)ch->range,
                    ch->enabled ? "" : "(disabled)");
    }

    return 0;
}

static int cmd_sim_start(const struct shell* sh, size_t argc, char** argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    uint32_t module_id = module_manager_get_id_by_name("data_simulator");
    if (module_id == 0) {
        shell_print(sh, "模块未注册");
        return -1;
    }

    int ret = module_manager_start_module(module_id);
    if (ret == 0) {
        shell_print(sh, "数据模拟模块已启动");
    } else {
        shell_print(sh, "启动失败: %d", ret);
    }
    return ret;
}

static int cmd_sim_stop(const struct shell* sh, size_t argc, char** argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    uint32_t module_id = module_manager_get_id_by_name("data_simulator");
    if (module_id == 0) {
        shell_print(sh, "模块未注册");
        return -1;
    }

    int ret = module_manager_stop_module(module_id);
    if (ret == 0) {
        shell_print(sh, "数据模拟模块已停止");
    } else {
        shell_print(sh, "停止失败: %d", ret);
    }
    return ret;
}

static int cmd_sim_interval(const struct shell* sh, size_t argc, char** argv)
{
    if (argc < 2) {
        shell_print(sh, "用法: sim interval <ms>");
        return -1;
    }

    int err;
    unsigned long ms = shell_strtoul(argv[1], 0, &err);
    if (err != 0 || ms == 0 || ms > 60000) {
        shell_print(sh, "采样周期范围: 1-60000 ms");
        return -1;
    }

    uint32_t interval = (uint32_t)ms;
    data_simulator_control(SIM_CMD_SET_INTERVAL, &interval);
    shell_print(sh, "采样周期设为 %u ms", interval);
    return 0;
}

static int cmd_sim_config(const struct shell* sh, size_t argc, char** argv)
{
    if (argc < 4) {
        shell_print(sh, "用法: sim config <type> baseline|range <value>");
        shell_print(sh, "  type: 0=current 1=temp 2=voltage 3=pressure 4=humidity");
        return -1;
    }

    int err;
    unsigned long type = shell_strtoul(argv[1], 0, &err);
    if (err != 0 || type >= SENSOR_TYPE_COUNT) {
        shell_print(sh, "type 范围: 0-%u", SENSOR_TYPE_COUNT - 1);
        return -1;
    }

    char* endptr;
    float value = strtof(argv[3], &endptr);
    if (endptr == argv[3]) {
        shell_print(sh, "无效数值");
        return -1;
    }

    sim_channel_config_t ch = {
        .channel_id = (uint8_t)type,
    };

    if (strcmp(argv[2], "baseline") == 0) {
        ch.baseline = value;
        data_simulator_control(SIM_CMD_SET_BASELINE, &ch);
        shell_print(sh, "传感器 %u 基线设为 %.2f", (unsigned)type, (double)value);
    } else if (strcmp(argv[2], "range") == 0) {
        if (value < 0.0f) {
            shell_print(sh, "range 必须 >= 0");
            return -1;
        }
        ch.range = value;
        data_simulator_control(SIM_CMD_SET_RANGE, &ch);
        shell_print(sh, "传感器 %u 范围设为 %.2f", (unsigned)type, (double)value);
    } else {
        shell_print(sh, "未知配置项: %s (可用: baseline, range)", argv[2]);
        return -1;
    }

    return 0;
}

static int cmd_sim_inject(const struct shell* sh, size_t argc, char** argv)
{
    if (argc < 3) {
        shell_print(sh, "用法: sim inject <type> <value>");
        shell_print(sh, "  type: 0=current 1=temp 2=voltage 3=pressure 4=humidity");
        return -1;
    }

    int err;
    unsigned long type = shell_strtoul(argv[1], 0, &err);
    if (err != 0 || type >= SENSOR_TYPE_COUNT) {
        shell_print(sh, "type 范围: 0-%u", SENSOR_TYPE_COUNT - 1);
        return -1;
    }

    char* endptr;
    float value = strtof(argv[2], &endptr);
    if (endptr == argv[2]) {
        shell_print(sh, "无效数值");
        return -1;
    }

    sim_inject_param_t param = {
        .sensor_type = (uint8_t)type,
        .value = value,
    };

    int ret = data_simulator_control(SIM_CMD_INJECT, &param);
    if (ret == 0) {
        shell_print(sh, "已注入 %s = %.2f", gateway_sensor_type_str((uint8_t)type), (double)value);
    } else {
        shell_print(sh, "注入失败: %d", ret);
    }
    return ret;
}

/* =============================================================================
 * Shell 命令注册
 * ============================================================================= */

SHELL_STATIC_SUBCMD_SET_CREATE(
    sim_cmds,
    SHELL_CMD(status,    NULL, "显示模拟模块状态",                  cmd_sim_status),
    SHELL_CMD(start,     NULL, "启动模拟数据采集",                  cmd_sim_start),
    SHELL_CMD(stop,      NULL, "停止模拟数据采集",                  cmd_sim_stop),
    SHELL_CMD(interval,  NULL, "设置采样周期: interval <ms>",       cmd_sim_interval),
    SHELL_CMD(config,    NULL, "配置通道: config <type> baseline|range <val>", cmd_sim_config),
    SHELL_CMD(inject,    NULL, "注入异常值: inject <type> <value>", cmd_sim_inject),
    SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(sim, &sim_cmds, "数据模拟器命令", NULL);

#endif /* CONFIG_SHELL */

/* =============================================================================
 * 模块接口声明与自动注册
 * ============================================================================= */

static const char* const data_simulator_deps[] = {NULL};

DECLARE_MODULE_INTERFACE_WITH_DEPS(data_simulator, data_simulator_deps);

const module_interface_t* data_simulator_get_interface(void)
{
    return &data_simulator_interface;
}

static int data_simulator_auto_register(void)
{
    uint32_t module_id;
    if (module_manager_register(data_simulator_get_interface(), NULL, &module_id) != 0) {
        LOG_ERR("数据模拟模块注册失败");
        return -EIO;
    }
    LOG_INF("数据模拟模块已注册 (id=%u)", module_id);
    return 0;
}

SYS_INIT(data_simulator_auto_register, POST_KERNEL, GATEWAY_INIT_PRIO_SIMULATOR);
