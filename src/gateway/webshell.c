/**
 * @file webshell.c
 * @brief 远程 Web Shell 基础版模块实现
 *
 * 扩展 framework Shell 服务，注册工业网关专用命令。
 * 通过 MQTT 订阅命令主题，执行后返回结果。
 */

#include "webshell.h"
#include "gateway_events.h"
#include "gateway_config.h"
#include "app_config.h"

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <stdlib.h>
#include <string.h>

#include "event_system.h"
#include "module_manager.h"

/* 条件 include：模块禁用时避免引用未编译的符号 */
#if GATEWAY_CAN_ENABLE
#include "protocol_can.h"
#endif
#if GATEWAY_MODBUS_ENABLE
#include "protocol_modbus.h"
#endif
#if GATEWAY_MQTT_ENABLE
#include "protocol_eth.h"
#include "cloud_upload.h"
#endif
#if GATEWAY_ANOMALY_ENABLE
#include "anomaly_detection.h"
#endif
#if GATEWAY_OFFLINE_CACHE_ENABLE
#include "offline_cache.h"
#endif

LOG_MODULE_REGISTER(webshell, CONFIG_SYS_LOG_LEVEL);

/* =============================================================================
 * 内部数据结构
 * ============================================================================= */

typedef struct {
    module_status_t status;
} webshell_cb_t;

static webshell_cb_t g_ws;

/* =============================================================================
 * 辅助函数
 * ============================================================================= */

static void print_module_status(const struct shell* sh, const char* label, const char* module_name)
{
    uint32_t module_id = module_manager_get_id_by_name(module_name);
    const char* status_str = "N/A (未注册)";

    if (module_id != 0) {
        module_info_t info;
        if (module_manager_get_module_info(module_id, &info) == 0) {
            switch (info.status) {
            case MODULE_STATUS_RUNNING:
                status_str = "RUNNING";
                break;
            case MODULE_STATUS_INITIALIZED:
                status_str = "INITIALIZED";
                break;
            case MODULE_STATUS_STOPPED:
                status_str = "STOPPED";
                break;
            case MODULE_STATUS_ERROR:
                status_str = "ERROR";
                break;
            default:
                status_str = "UNKNOWN";
                break;
            }
        }
    }

    shell_print(sh, "%-10s %s", label, status_str);
}

/* =============================================================================
 * Shell 命令实现
 * ============================================================================= */

static int cmd_gateway_status(const struct shell* sh, size_t argc, char** argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    shell_print(sh, "=== 工业网关状态 ===");
    shell_print(sh, "  协议层:");
    print_module_status(sh, "CAN:", "protocol_can");
    print_module_status(sh, "Modbus:", "protocol_modbus");
    print_module_status(sh, "Ethernet:", "protocol_eth");
    shell_print(sh, "  业务层:");
    print_module_status(sh, "Anomaly:", "anomaly_detection");
    print_module_status(sh, "Cloud:", "cloud_upload");
    print_module_status(sh, "Cache:", "offline_cache");
    print_module_status(sh, "Simulator:", "data_simulator");

    return 0;
}

#if GATEWAY_CAN_ENABLE
static int cmd_can_stats(const struct shell* sh, size_t argc, char** argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    uint32_t rx, tx, err;
    protocol_can_get_stats(&rx, &tx, &err);

    shell_print(sh, "=== CAN 统计 ===");
    shell_print(sh, "RX:   %lu", (unsigned long)rx);
    shell_print(sh, "TX:   %lu", (unsigned long)tx);
    shell_print(sh, "ERR:  %lu", (unsigned long)err);

    return 0;
}
#endif

#if GATEWAY_MODBUS_ENABLE
static int cmd_modbus_read(const struct shell* sh, size_t argc, char** argv)
{
    if (argc < 3) {
        shell_print(sh, "用法: gateway modbus read <addr> <count>");
        return -1;
    }

    unsigned long addr_raw = shell_strtoul(argv[1], 0, NULL);
    unsigned long count_raw = shell_strtoul(argv[2], 0, NULL);

    if (addr_raw > 0xFFFF) {
        shell_print(sh, "addr 超出 16 位范围");
        return -1;
    }
    if (count_raw == 0 || count_raw > 16) {
        shell_print(sh, "count 范围: 1-16");
        return -1;
    }

    uint16_t addr = (uint16_t)addr_raw;
    uint16_t count = (uint16_t)count_raw;

    uint16_t values[16];
    int ret = protocol_modbus_read_holding_regs(CONFIG_GATEWAY_MODBUS_SLAVE_ID, addr, count, values);

    if (ret == 0) {
        shell_print(sh, "Modbus 读取成功 [%u, %u]:", addr, addr + count - 1);
        for (uint16_t i = 0; i < count; i++) {
            shell_print(sh, "  [%u] = %u (0x%04X)", addr + i, values[i], values[i]);
        }
    } else {
        shell_print(sh, "Modbus 读取失败: %d", ret);
    }

    return ret;
}
#endif

#if GATEWAY_ANOMALY_ENABLE
static int cmd_anomaly_config(const struct shell* sh, size_t argc, char** argv)
{
    if (argc < 4) {
        shell_print(sh, "用法: gateway anomaly config <sensor_type> <warning> <critical> [emergency]");
        shell_print(sh, "  sensor_type: 0=current 1=temp 2=voltage 3=pressure 4=humidity");
        shell_print(sh, "  warning/critical/emergency: sigma 阈值");
        return -1;
    }

    int err;
    unsigned long sensor_type_ul = shell_strtoul(argv[1], 0, &err);
    if (err != 0 || sensor_type_ul >= 5) {
        shell_print(sh, "sensor_type 范围: 0-4");
        return -1;
    }
    uint8_t sensor_type = (uint8_t)sensor_type_ul;

    char* endptr;
    float w = strtof(argv[2], &endptr);
    if (endptr == argv[2] || w <= 0.0f) {
        shell_print(sh, "warning 阈值必须是正数");
        return -1;
    }
    float c = strtof(argv[3], &endptr);
    if (endptr == argv[3] || c <= 0.0f) {
        shell_print(sh, "critical 阈值必须是正数");
        return -1;
    }
    float e = c * 1.5f;
    if (argc > 4) {
        e = strtof(argv[4], &endptr);
        if (endptr == argv[4] || e <= 0.0f) {
            shell_print(sh, "emergency 阈值必须是正数");
            return -1;
        }
    }

    int ret = anomaly_detection_set_threshold(sensor_type, w, c, e);
    if (ret == 0) {
        shell_print(sh, "传感器 %u 阈值已更新: W=%.1f C=%.1f E=%.1f", sensor_type, (double)w, (double)c, (double)e);
    } else {
        shell_print(sh, "设置失败");
    }

    return ret;
}
#endif

#if GATEWAY_CLOUD_UPLOAD_ENABLE
#include "cloud_provider.h"

static int cmd_cloud_status(const struct shell* sh, size_t argc, char** argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    uint32_t s, f, cached;
    cloud_upload_get_stats(&s, &f, &cached);

    shell_print(sh, "=== 云端状态 ===");
    shell_print(sh, "已注册 Provider 数: %u", cloud_provider_get_count());

    uint8_t count = cloud_provider_get_count();
    for (uint8_t i = 0; i < count; i++) {
        const cloud_provider_t* p = cloud_provider_get_by_index(i);
        if (p != NULL && p->print_status != NULL) {
            p->print_status(sh);
        }
    }

    shell_print(sh, "上传成功:  %lu", (unsigned long)s);
    shell_print(sh, "上传失败:  %lu", (unsigned long)f);
    shell_print(sh, "缓存数据:  %lu", (unsigned long)cached);

    return 0;
}
#endif

#if GATEWAY_OFFLINE_CACHE_ENABLE
static int cmd_cache_info(const struct shell* sh, size_t argc, char** argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    uint32_t count, max, overflow;
    offline_cache_get_info(&count, &max, &overflow);

    shell_print(sh, "=== 离线缓存 ===");
    shell_print(sh, "条目数:   %lu / %lu", (unsigned long)count, (unsigned long)max);
    shell_print(sh, "溢出次数: %lu", (unsigned long)overflow);

    return 0;
}

static int cmd_cache_clear(const struct shell* sh, size_t argc, char** argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    int ret = -1;
    uint32_t module_id = module_manager_get_id_by_name("offline_cache");
    if (module_id != 0) {
        module_info_t info;
        if (module_manager_get_module_info(module_id, &info) == 0 && info.interface != NULL && info.interface->control) {
            ret = info.interface->control(CACHE_CMD_CLEAR, NULL);
        }
    }
    if (ret == 0) {
        shell_print(sh, "离线缓存已清空");
    } else {
        shell_print(sh, "离线缓存清空失败: %d", ret);
    }
    return ret;
}
#endif

/* =============================================================================
 * Shell 命令注册（条件编译：模块禁用时不注册对应命令）
 * ============================================================================= */

SHELL_STATIC_SUBCMD_SET_CREATE(
    gateway_cmds,
    SHELL_CMD(status, NULL, "显示网关模块状态", cmd_gateway_status),
    SHELL_SUBCMD_SET_END);

#if GATEWAY_CAN_ENABLE
SHELL_STATIC_SUBCMD_SET_CREATE(
    can_cmds,
    SHELL_CMD(stats, NULL, "显示 CAN 统计", cmd_can_stats),
    SHELL_SUBCMD_SET_END);
#endif

#if GATEWAY_MODBUS_ENABLE
SHELL_STATIC_SUBCMD_SET_CREATE(
    modbus_cmds,
    SHELL_CMD(read, NULL, "读取 Modbus 保持寄存器: read <addr> <count>", cmd_modbus_read),
    SHELL_SUBCMD_SET_END);
#endif

#if GATEWAY_ANOMALY_ENABLE
SHELL_STATIC_SUBCMD_SET_CREATE(
    anomaly_cmds,
    SHELL_CMD(config, NULL, "配置异常检测阈值: config <sensor> <w> <c> [e]", cmd_anomaly_config),
    SHELL_SUBCMD_SET_END);
#endif

#if GATEWAY_CLOUD_UPLOAD_ENABLE
SHELL_STATIC_SUBCMD_SET_CREATE(
    cloud_cmds,
    SHELL_CMD(status, NULL, "显示云端连接状态", cmd_cloud_status),
    SHELL_SUBCMD_SET_END);
#endif

#if GATEWAY_OFFLINE_CACHE_ENABLE
SHELL_STATIC_SUBCMD_SET_CREATE(
    cache_cmds,
    SHELL_CMD(info, NULL, "显示离线缓存信息", cmd_cache_info),
    SHELL_CMD(clear, NULL, "清空离线缓存", cmd_cache_clear),
    SHELL_SUBCMD_SET_END);
#endif

SHELL_CMD_REGISTER(gateway, &gateway_cmds, "工业网关命令", NULL);
#if GATEWAY_CAN_ENABLE
SHELL_CMD_REGISTER(can, &can_cmds, "CAN 总线命令", NULL);
#endif
#if GATEWAY_MODBUS_ENABLE
SHELL_CMD_REGISTER(modbus, &modbus_cmds, "Modbus 命令", NULL);
#endif
#if GATEWAY_ANOMALY_ENABLE
SHELL_CMD_REGISTER(anomaly, &anomaly_cmds, "异常检测命令", NULL);
#endif
#if GATEWAY_CLOUD_UPLOAD_ENABLE
SHELL_CMD_REGISTER(cloud, &cloud_cmds, "云端连接命令", NULL);
#endif
#if GATEWAY_OFFLINE_CACHE_ENABLE
SHELL_CMD_REGISTER(cache, &cache_cmds, "离线缓存命令", NULL);
#endif

/* =============================================================================
 * 模块接口实现
 * ============================================================================= */

int webshell_init(void* config)
{
    ARG_UNUSED(config);
    LOG_INF("初始化 WebShell 模块...");

    memset(&g_ws, 0, sizeof(g_ws));
    g_ws.status = MODULE_STATUS_INITIALIZED;

    LOG_INF("WebShell 模块初始化完成");
    return 0;
}

int webshell_start(void)
{
    if (g_ws.status != MODULE_STATUS_INITIALIZED &&
        g_ws.status != MODULE_STATUS_STOPPED) {
        return -1;
    }
    g_ws.status = MODULE_STATUS_RUNNING;
    LOG_INF("WebShell 模块已启动");
    return 0;
}

int webshell_stop(void)
{
    if (g_ws.status != MODULE_STATUS_RUNNING) {
        return 0;
    }
    g_ws.status = MODULE_STATUS_STOPPED;
    LOG_INF("WebShell 模块已停止");
    return 0;
}

int webshell_shutdown(void)
{
    webshell_stop();
    g_ws.status = MODULE_STATUS_UNINITIALIZED;
    return 0;
}

void webshell_on_event(const event_t* event, void* user_data)
{
    ARG_UNUSED(user_data);
    ARG_UNUSED(event);
}

module_status_t webshell_get_status(void)
{
    return g_ws.status;
}

int webshell_control(int cmd, void* arg)
{
    ARG_UNUSED(cmd);
    ARG_UNUSED(arg);
    return -1;
}

/* =============================================================================
 * 模块接口声明与自动注册
 * ============================================================================= */

static const char* const webshell_deps[] = {NULL};

DECLARE_MODULE_INTERFACE_WITH_DEPS(webshell, webshell_deps);

const module_interface_t* webshell_get_interface(void)
{
    return &webshell_interface;
}

static int webshell_auto_register(void)
{
    uint32_t module_id;
    if (module_manager_register(webshell_get_interface(), NULL, &module_id) != 0) {
        LOG_ERR("WebShell 模块注册失败");
        return -EIO;
    }
    LOG_INF("WebShell 模块已注册 (id=%u)", module_id);
    return 0;
}

SYS_INIT(webshell_auto_register, POST_KERNEL, GATEWAY_INIT_PRIO_WEBSHELL);
