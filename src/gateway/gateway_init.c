/*
 * 业务入口占位：通过 Zephyr SYS_INIT 在应用阶段执行，无需改动 framework 内 app_main。
 * 后续可在此注册模块、订阅事件总线等（按 zephyr_framework 文档扩展）。
 */
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

static int gateway_init(void)
{
	printk("gateway: business init (src/gateway)\n");
	return 0;
}

SYS_INIT(gateway_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
