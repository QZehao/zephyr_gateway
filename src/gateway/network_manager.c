/**
 * @file network_manager.c
 * @brief 网络链路管理模块实现
 *
 * 监听默认网络接口状态，发布 EVENT_TYPE_NET_UP / EVENT_TYPE_NET_DOWN。
 * 不处理任何 MQTT 相关逻辑。
 */

#include "network_manager.h"
#include "gateway_events.h"
#include "gateway_config.h"
#include <zeplod/app_config.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <string.h>

#include <zeplod/event_system.h>
#include <zeplod/module_manager.h>

LOG_MODULE_REGISTER(network_manager, CONFIG_SYS_LOG_LEVEL);

/* =============================================================================
 * 配置
 * ============================================================================= */

#define NET_MGR_THREAD_PRIORITY   GATEWAY_THREAD_PRIORITY_DEFAULT
#define NET_MGR_THREAD_STACK_SIZE GATEWAY_THREAD_STACK_SIZE_DEFAULT
#define NET_MGR_POLL_INTERVAL_MS  1000

/* =============================================================================
 * 内部数据结构
 * ============================================================================= */

typedef struct {
    module_status_t status;
    struct k_thread worker_thread;
    K_KERNEL_STACK_MEMBER(worker_stack, NET_MGR_THREAD_STACK_SIZE);
    bool net_up;
} network_manager_cb_t;

/* =============================================================================
 * 静态变量
 * ============================================================================= */

static network_manager_cb_t g_net_mgr;

/* =============================================================================
 * 前向声明
 * ============================================================================= */

static void network_manager_worker_thread(void* p1, void* p2, void* p3);

/* =============================================================================
 * 模块接口实现
 * ============================================================================= */

int network_manager_init(void* config)
{
    ARG_UNUSED(config);
    LOG_INF("初始化网络管理模块...");

    memset(&g_net_mgr, 0, sizeof(g_net_mgr));
    g_net_mgr.status = MODULE_STATUS_INITIALIZED;

    event_register_type(EVENT_TYPE_NET_UP, "net_up");
    event_register_type(EVENT_TYPE_NET_DOWN, "net_down");

    LOG_INF("网络管理模块初始化完成");
    return 0;
}

int network_manager_start(void)
{
    if (g_net_mgr.status != MODULE_STATUS_INITIALIZED &&
        g_net_mgr.status != MODULE_STATUS_STOPPED) {
        return -1;
    }

    g_net_mgr.status = MODULE_STATUS_RUNNING;
    g_net_mgr.net_up = false;

    k_thread_create(
        &g_net_mgr.worker_thread, g_net_mgr.worker_stack,
        K_THREAD_STACK_SIZEOF(g_net_mgr.worker_stack),
        network_manager_worker_thread, NULL, NULL, NULL,
        NET_MGR_THREAD_PRIORITY, 0, K_FOREVER);
    k_thread_name_set(&g_net_mgr.worker_thread, "net_mgr");
    k_thread_start(&g_net_mgr.worker_thread);

    LOG_INF("网络管理模块已启动");
    return 0;
}

int network_manager_stop(void)
{
    if (g_net_mgr.status != MODULE_STATUS_RUNNING) {
        return 0;
    }

    /* 置位 STOPPED：工作线程在分片短睡中检查状态后自然退出（不使用 k_thread_abort） */
    g_net_mgr.status = MODULE_STATUS_STOPPED;
    k_thread_join(&g_net_mgr.worker_thread, K_FOREVER);

    LOG_INF("网络管理模块已停止");
    return 0;
}

int network_manager_shutdown(void)
{
    network_manager_stop();
    g_net_mgr.status = MODULE_STATUS_UNINITIALIZED;
    return 0;
}

void network_manager_on_event(const event_t* event, void* user_data)
{
    ARG_UNUSED(user_data);
    ARG_UNUSED(event);
}

module_status_t network_manager_get_status(void)
{
    return g_net_mgr.status;
}

int network_manager_control(int cmd, void* arg)
{
    ARG_UNUSED(cmd);
    ARG_UNUSED(arg);
    return -1;
}

/* =============================================================================
 * 模块特定 API
 * ============================================================================= */

bool network_is_up(void)
{
    struct net_if* iface = net_if_get_default();
    return (iface != NULL && net_if_is_up(iface));
}

/* =============================================================================
 * 内部函数
 * ============================================================================= */

static void network_manager_worker_thread(void* p1, void* p2, void* p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    LOG_INF("网络管理工作线程已启动");

    /* 分片睡眠：每次 50ms，使 stop 后 join 在 ~50ms 内完成 */
#define NET_MGR_SLICE_MS    50
#define NET_MGR_SLICES      (NET_MGR_POLL_INTERVAL_MS / NET_MGR_SLICE_MS)

    while (g_net_mgr.status == MODULE_STATUS_RUNNING) {
        bool net_was_up = g_net_mgr.net_up;
        g_net_mgr.net_up = network_is_up();

        if (g_net_mgr.net_up && !net_was_up) {
            LOG_INF("网络接口已上线");
            event_publish_copy(EVENT_TYPE_NET_UP, EVENT_PRIORITY_HIGH, NULL, 0);
        } else if (!g_net_mgr.net_up && net_was_up) {
            LOG_INF("网络接口已下线");
            event_publish_copy(EVENT_TYPE_NET_DOWN, EVENT_PRIORITY_HIGH, NULL, 0);
        }

        /* 分片轮询：每 50ms 检查停止标志，保证 stop 后快速退出 */
        for (int s = 0; s < NET_MGR_SLICES && g_net_mgr.status == MODULE_STATUS_RUNNING; s++) {
            k_sleep(K_MSEC(NET_MGR_SLICE_MS));
        }
    }

    LOG_INF("网络管理工作线程已退出");
}

/* =============================================================================
 * 模块接口声明与自动注册
 * ============================================================================= */

static const char* const network_manager_deps[] = {NULL};

DECLARE_MODULE_INTERFACE_WITH_DEPS(network_manager, network_manager_deps);

const module_interface_t* network_manager_get_interface(void)
{
    return &network_manager_interface;
}

static int network_manager_auto_register(void)
{
    uint32_t module_id;
    if (module_manager_register(network_manager_get_interface(), NULL, &module_id) != 0) {
        LOG_ERR("网络管理模块注册失败");
        return -EIO;
    }
    LOG_INF("网络管理模块已注册 (id=%u)", module_id);
    return 0;
}

SYS_INIT(network_manager_auto_register, POST_KERNEL, GATEWAY_INIT_PRIO_NETWORK_MANAGER);
