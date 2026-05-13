/**
 * @file gateway_init.c
 * @brief 工业网关业务入口
 *
 * 通过 SYS_INIT 在应用阶段初始化网关专用事件订阅关系。
 * 所有模块已通过各自的 SYS_INIT 自动注册到模块管理器。
 */

#include "gateway_events.h"
#include "gateway_config.h"
#include "protocol_can.h"
#include "protocol_modbus.h"
#include "protocol_eth.h"
#include "anomaly_detection.h"
#include "cloud_upload.h"
#include "offline_cache.h"
#include "webshell.h"
#include "app_config.h"

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/logging/log.h>

#include "event_system.h"
#include "module_manager.h"

LOG_MODULE_REGISTER(gateway_init, CONFIG_SYS_LOG_LEVEL);

/* =============================================================================
 * 事件订阅关系表
 * ============================================================================= */

typedef struct
{
	const char *module_name;
	event_type_t event_type;
} gateway_subscription_t;

static const gateway_subscription_t g_subscriptions[] = {
	/* anomaly_detection 订阅 sensor 数据 */
	{"anomaly_detection", EVENT_TYPE_SENSOR_DATA},

	/* cloud_upload 订阅 sensor 和 anomaly 数据 */
	{"cloud_upload", EVENT_TYPE_SENSOR_DATA},
	{"cloud_upload", EVENT_TYPE_ANOMALY_WARNING},
	{"cloud_upload", EVENT_TYPE_ANOMALY_CRITICAL},
	{"cloud_upload", EVENT_TYPE_ANOMALY_EMERGENCY},

	/* offline_cache 订阅 cloud_upload 事件和网络状态 */
	{"offline_cache", EVENT_TYPE_CLOUD_UPLOAD},
	{"offline_cache", EVENT_TYPE_CLOUD_CONNECTED},
	{"offline_cache", EVENT_TYPE_CLOUD_DISCONNECTED},
};

#define SUBSCRIPTION_COUNT (sizeof(g_subscriptions) / sizeof(g_subscriptions[0]))

/* =============================================================================
 * 前向声明
 * ============================================================================= */

static int gateway_setup_subscriptions(void);
static void gateway_print_banner(void);

/* =============================================================================
 * 初始化入口
 * ============================================================================= */

static int gateway_init(void)
{
	LOG_INF("========================================");
	LOG_INF("  Zephyr 工业边缘网关 — 业务初始化");
	LOG_INF("  Version: %d.%d.%d", GATEWAY_VERSION_MAJOR,
			GATEWAY_VERSION_MINOR, GATEWAY_VERSION_PATCH);
	LOG_INF("========================================");

	/* 注册网关事件类型（未被各模块 init 自动注册的部分） */
	event_register_type(EVENT_TYPE_SENSOR_DATA, "sensor_data");

	/* 设置模块间事件订阅 */
	int ret = gateway_setup_subscriptions();
	if (ret != 0)
	{
		LOG_WRN("部分事件订阅设置失败: %d", ret);
	}

	gateway_print_banner();

	return 0;
}

SYS_INIT(gateway_init, APPLICATION, GATEWAY_INIT_PRIO_ENTRY);

/* =============================================================================
 * 内部函数
 * ============================================================================= */

static int gateway_setup_subscriptions(void)
{
	int errors = 0;

	for (size_t i = 0; i < SUBSCRIPTION_COUNT; i++)
	{
		const gateway_subscription_t *sub = &g_subscriptions[i];

		/* 查找模块 ID */
		uint32_t module_id = module_manager_get_id_by_name(sub->module_name);
		if (module_id == 0)
		{
			LOG_WRN("模块 '%s' 未注册，跳过事件订阅", sub->module_name);
			continue;
		}

		/* 订阅事件 */
		int ret = module_manager_subscribe(module_id, sub->event_type);
		if (ret != 0)
		{
			LOG_WRN("模块 '%s' 订阅事件 %u 失败: %d",
					sub->module_name, sub->event_type, ret);
			errors++;
		}
		else
		{
			LOG_DBG("模块 '%s' 已订阅事件 %u", sub->module_name, sub->event_type);
		}
	}

	return errors;
}

static void gateway_print_banner(void)
{
	printk("\n");
	printk("=================================================\n");
	printk("  Zephyr 工业边缘网关\n");
	printk("  Version: %d.%d.%d\n", GATEWAY_VERSION_MAJOR,
		   GATEWAY_VERSION_MINOR, GATEWAY_VERSION_PATCH);
	printk("=================================================\n");
	printk("  协议层:\n");
#if GATEWAY_CAN_ENABLE
	printk("    [+] CAN 数据采集\n");
#else
	printk("    [-] CAN 数据采集 (禁用)\n");
#endif
#if GATEWAY_MODBUS_ENABLE
	printk("    [+] Modbus RTU Master\n");
#else
	printk("    [-] Modbus RTU Master (禁用)\n");
#endif
#if GATEWAY_MQTT_ENABLE
	printk("    [+] MQTT 上云\n");
#else
	printk("    [-] MQTT 上云 (禁用)\n");
#endif
	printk("  业务层:\n");
#if GATEWAY_ANOMALY_ENABLE
	printk("    [+] 自适应阈值异常检测\n");
#else
	printk("    [-] 异常检测 (禁用)\n");
#endif
#if GATEWAY_CLOUD_UPLOAD_ENABLE
	printk("    [+] 数据上云\n");
#else
	printk("    [-] 数据上云 (禁用)\n");
#endif
#if GATEWAY_OFFLINE_CACHE_ENABLE
	printk("    [+] 断网续传缓存\n");
#else
	printk("    [-] 断网续传缓存 (禁用)\n");
#endif
#if GATEWAY_WEBSHELL_ENABLE
	printk("    [+] Shell 命令扩展\n");
#else
	printk("    [-] Shell 命令扩展 (禁用)\n");
#endif
	printk("=================================================\n");
	printk("\n");
}
