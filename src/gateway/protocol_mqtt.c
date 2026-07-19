/**
 * @file protocol_mqtt.c
 * @brief MQTT 客户端模块实现
 *
 * 管理 MQTT 会话：broker 解析、连接、重连、心跳、发布。
 * 订阅 network_manager 的网络事件，网络上线后尝试连接。
 */

#include "protocol_mqtt.h"
#include "gateway_events.h"
#include "gateway_config.h"
#include "network_manager.h"
#include <zeplod/app_config.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <string.h>

#include <zeplod/event_system.h>
#include <zeplod/module_manager.h>

LOG_MODULE_REGISTER(protocol_mqtt, CONFIG_SYS_LOG_LEVEL);

/* =============================================================================
 * 配置
 * ============================================================================= */

#define MQTT_THREAD_PRIORITY   GATEWAY_THREAD_PRIORITY_DEFAULT
#define MQTT_THREAD_STACK_SIZE GATEWAY_THREAD_STACK_SIZE_LARGE
#define MQTT_RX_BUF_SIZE       256
#define MQTT_TX_BUF_SIZE       256
/** mqtt_on_command() 命令钩子单次接收的最大 payload 字节数（栈上缓冲容量） */
#define MQTT_CMD_PAYLOAD_MAX   128
#define RECONNECT_MIN_MS       GATEWAY_MQTT_RECONNECT_MIN_MS
#define RECONNECT_MAX_MS       GATEWAY_MQTT_RECONNECT_MAX_MS
#define BROKER_ADDR_MAX_LEN    64

/* =============================================================================
 * 内部数据结构
 * ============================================================================= */

typedef enum {
    MQTT_STATE_DISCONNECTED = 0,
    MQTT_STATE_CONNECTING,
    MQTT_STATE_CONNECTED,
    MQTT_STATE_SUBSCRIBED,
} mqtt_state_t;

typedef struct {
    module_status_t      status;
    struct k_thread      worker_thread;
    K_KERNEL_STACK_MEMBER(worker_stack, MQTT_THREAD_STACK_SIZE);
    /* MQTT */
    struct mqtt_client   client;
    struct sockaddr_storage broker;
    uint8_t              rx_buffer[MQTT_RX_BUF_SIZE];
    uint8_t              tx_buffer[MQTT_TX_BUF_SIZE];
    /* 状态 */
    mqtt_state_t         state;
    atomic_t             net_up;          /**< 原子标志：1=网络已就绪，0=网络断开 */
    atomic_t             pending_disconnect; /**< 原子标志：1=需要主动断开 */
    /* 重连 */
    uint32_t             reconnect_delay_ms;
    int64_t              last_connect_attempt;
    /* 统计 */
    uint32_t             connect_count;
    uint32_t             disconnect_count;
    uint32_t             msg_tx_count;
    uint32_t             msg_rx_count;
    /* Broker（支持运行时设置） */
    char                 broker_addr[BROKER_ADDR_MAX_LEN];
    uint16_t             broker_port;
    /* 认证（由 cloud provider 设置）；容量契约：超长由 set_auth() 拒绝，不静默截断 */
    char                 mqtt_username[192];
    char                 mqtt_password[128];
    bool                 has_auth;
    struct mqtt_utf8     mqtt_user_name;
    struct mqtt_utf8     mqtt_password_utf8;
    /* 连接期用户名/密码快照：mqtt_do_connect() 持锁拷贝自 mqtt_username/mqtt_password，
     * 与 conn_client_id 同一模式——mqtt_user_name.utf8/mqtt_password_utf8.utf8 需指向
     * 生命周期覆盖整个连接期的持久内存，不能指向 mqtt_do_connect() 的栈局部变量
     * （函数返回后即悬空，虽然当前 Zephyr MQTT 库仅在 connect 期间同步解引用，
     * 实际不可达，但仍是脆弱反模式，故与 conn_client_id 一并纳入持久快照）。 */
    char                 conn_username[192];
    char                 conn_password[128];
    /* 运行时 clientId（由 cloud provider 通过 protocol_mqtt_set_client_id 设置） */
    char                 client_id[192];
    /* 连接期 clientId 快照：mqtt_do_connect() 持锁拷贝自 client_id，供本次连接
     * 全程（含 mqtt_client.client_id 与 evt_handler 拼接订阅主题）稳定使用，
     * 避免与运行时 protocol_mqtt_set_client_id() 的写入竞争 */
    char                 conn_client_id[192];
    /* TLS 配置（CONFIG_MQTT_LIB_TLS 守护） */
#if defined(CONFIG_MQTT_LIB_TLS)
    bool                 use_tls;
    sec_tag_t            sec_tags[3];
    size_t               sec_tag_count;
    char                 tls_hostname[64];
    /* 连接期 TLS 参数快照：与 conn_client_id 同一模式，mqtt_do_connect() 持锁
     * 拷贝。TLS 握手在 mqtt_connect() 返回后经后续 mqtt_input() 异步完成，
     * tls_cfg->hostname 等指针需在整个连接期保持有效；若直接指向可变的
     * tls_hostname/sec_tags，运行时 protocol_mqtt_set_tls()/clear_tls() 的
     * 并发写入会撕裂正在进行的握手读取，故快照到独立的持久 conn_* 字段。 */
    sec_tag_t            conn_sec_tags[3];
    size_t               conn_sec_tag_count;
    char                 conn_tls_hostname[64];
#endif
    /* 同步 */
    struct k_mutex       client_mutex;
} protocol_mqtt_cb_t;

/* =============================================================================
 * 静态变量
 * ============================================================================= */

static protocol_mqtt_cb_t g_mqtt;

/* =============================================================================
 * 前向声明
 * ============================================================================= */

static void mqtt_worker_thread(void* p1, void* p2, void* p3);
static int  mqtt_do_connect(void);
static void mqtt_do_disconnect(void);
static void mqtt_publish_state_event(bool connected);
static void mqtt_evt_handler(struct mqtt_client* client, const struct mqtt_evt* evt);
static int  resolve_broker_addr(struct sockaddr_storage* addr, const char* broker_addr,
                                 uint16_t broker_port);
static void mqtt_on_command(const char* topic, size_t topic_len,
                             const uint8_t* payload, size_t payload_len);

/* =============================================================================
 * 模块接口实现
 * ============================================================================= */

int protocol_mqtt_init(void* config)
{
    ARG_UNUSED(config);
    LOG_INF("初始化 MQTT 模块...");

    memset(&g_mqtt, 0, sizeof(g_mqtt));
    g_mqtt.status = MODULE_STATUS_INITIALIZED;
    g_mqtt.state = MQTT_STATE_DISCONNECTED;
    g_mqtt.reconnect_delay_ms = RECONNECT_MIN_MS;
    atomic_set(&g_mqtt.net_up, 0);
    atomic_set(&g_mqtt.pending_disconnect, 0);

    strncpy(g_mqtt.broker_addr, CONFIG_GATEWAY_MQTT_BROKER_ADDR, sizeof(g_mqtt.broker_addr) - 1);
    g_mqtt.broker_addr[sizeof(g_mqtt.broker_addr) - 1] = '\0';
    g_mqtt.broker_port = CONFIG_GATEWAY_MQTT_BROKER_PORT;

    /* 初始化 clientId：默认使用 Kconfig 中配置的值 */
    strncpy(g_mqtt.client_id, CONFIG_GATEWAY_MQTT_CLIENT_ID, sizeof(g_mqtt.client_id) - 1);
    g_mqtt.client_id[sizeof(g_mqtt.client_id) - 1] = '\0';

    /* 注册本模块发布的云连接状态事件类型；缺失注册会导致 event_subscribe()
     * 和 event_publish_copy() 均返回 EVENT_ERR_NOT_FOUND（见 event_system_publish.c
     * 的 event_publish_common() 与 event_system_pubsub.c 的 event_subscribe()）。 */
    event_register_type(EVENT_TYPE_CLOUD_CONNECTED, "cloud_connected");
    event_register_type(EVENT_TYPE_CLOUD_DISCONNECTED, "cloud_disconnected");

    k_mutex_init(&g_mqtt.client_mutex);

    LOG_INF("MQTT 模块初始化完成");
    return 0;
}

int protocol_mqtt_start(void)
{
    if (g_mqtt.status != MODULE_STATUS_INITIALIZED &&
        g_mqtt.status != MODULE_STATUS_STOPPED) {
        return -1;
    }

    g_mqtt.status = MODULE_STATUS_RUNNING;
    /* EVENT_TYPE_NET_UP/NET_DOWN 是边沿事件，不带重放：若本模块单独重启
     * （如 recovery 场景）而网络本身未发生抖动，则不会再收到一次 NET_UP，
     * 无条件置 0 会导致 net_up 永久停留在 0、MQTT 永久不发起连接。改为
     * 用 network_is_up() 读取当前网络状态做初始化。network_manager.c 与
     * protocol_mqtt.c 同受 CONFIG_GATEWAY_MQTT_ENABLE 编译门控，调用安全。 */
    atomic_set(&g_mqtt.net_up, network_is_up() ? 1 : 0);
    g_mqtt.state = MQTT_STATE_DISCONNECTED;
    atomic_set(&g_mqtt.pending_disconnect, 0);

    k_thread_create(
        &g_mqtt.worker_thread, g_mqtt.worker_stack,
        K_THREAD_STACK_SIZEOF(g_mqtt.worker_stack),
        mqtt_worker_thread, NULL, NULL, NULL,
        MQTT_THREAD_PRIORITY, 0, K_FOREVER);
    k_thread_name_set(&g_mqtt.worker_thread, "proto_mqtt");
    k_thread_start(&g_mqtt.worker_thread);

    LOG_INF("MQTT 模块已启动");
    return 0;
}

int protocol_mqtt_stop(void)
{
    if (g_mqtt.status != MODULE_STATUS_RUNNING) {
        return 0;
    }

    /* 置位 STOPPED：工作线程检查状态后将自然退出循环，执行 mqtt_do_disconnect 清理 */
    g_mqtt.status = MODULE_STATUS_STOPPED;
    atomic_set(&g_mqtt.pending_disconnect, 1);

    /* 优雅等待线程自然退出（不使用 k_thread_abort，保留清理路径） */
    k_thread_join(&g_mqtt.worker_thread, K_FOREVER);

    LOG_INF("MQTT 模块已停止");
    return 0;
}

int protocol_mqtt_shutdown(void)
{
    protocol_mqtt_stop();
    g_mqtt.status = MODULE_STATUS_UNINITIALIZED;
    return 0;
}

void protocol_mqtt_on_event(const event_t* event, void* user_data)
{
    ARG_UNUSED(user_data);
    if (event == NULL) {
        return;
    }

    switch (event->type) {
    case EVENT_TYPE_NET_UP:
        atomic_set(&g_mqtt.net_up, 1);
        break;
    case EVENT_TYPE_NET_DOWN:
        atomic_set(&g_mqtt.net_up, 0);
        break;
    default:
        break;
    }
}

module_status_t protocol_mqtt_get_status(void)
{
    return g_mqtt.status;
}

int protocol_mqtt_control(int cmd, void* arg)
{
    switch (cmd) {
    case MQTT_CMD_PUBLISH:
        if (arg == NULL) return -1;
        {
            const char** params = (const char**)arg;
            if (params[0] == NULL || params[1] == NULL) return -1;
            return protocol_mqtt_publish(params[0], params[1], (uint16_t)strlen(params[1]));
        }
    case MQTT_CMD_GET_STATUS:
        if (arg != NULL) {
            *(bool*)arg = protocol_mqtt_is_connected();
        }
        return 0;
    default:
        return -1;
    }
}

/* =============================================================================
 * 模块特定 API
 * ============================================================================= */

bool protocol_mqtt_is_connected(void)
{
    /* CONNECTED 即可发布，SUBSCRIBED 仅表示命令通道就绪，订阅被拒不应阻断上行 */
    return (g_mqtt.status == MODULE_STATUS_RUNNING &&
            g_mqtt.state >= MQTT_STATE_CONNECTED);
}

int protocol_mqtt_publish(const char* topic, const char* payload, uint16_t payload_len)
{
    if (!protocol_mqtt_is_connected() || topic == NULL || payload == NULL) {
        return -1;
    }

    int ret = k_mutex_lock(&g_mqtt.client_mutex, K_MSEC(100));
    if (ret != 0) {
        return ret;
    }

    struct mqtt_publish_param param;
    param.message.topic.qos = MQTT_QOS_0_AT_MOST_ONCE;
    param.message.topic.topic.utf8 = (uint8_t*)topic;
    param.message.topic.topic.size = strlen(topic);
    param.message.payload.data = (uint8_t*)payload;
    param.message.payload.len = payload_len;
    param.message_id = 0;
    param.dup_flag = 0;
    param.retain_flag = 0;

    ret = mqtt_publish(&g_mqtt.client, &param);
    if (ret == 0) {
        g_mqtt.msg_tx_count++;
    }

    k_mutex_unlock(&g_mqtt.client_mutex);
    return ret;
}

void protocol_mqtt_get_stats(uint32_t* connect_count, uint32_t* disconnect_count,
                              uint32_t* msg_count)
{
    if (connect_count != NULL) *connect_count = g_mqtt.connect_count;
    if (disconnect_count != NULL) *disconnect_count = g_mqtt.disconnect_count;
    if (msg_count != NULL) *msg_count = g_mqtt.msg_tx_count;
}

int protocol_mqtt_set_auth(const char* username, const char* password)
{
    if (username != NULL && password != NULL) {
        if (strlen(username) >= sizeof(g_mqtt.mqtt_username) ||
            strlen(password) >= sizeof(g_mqtt.mqtt_password)) {
            LOG_ERR("MQTT 认证参数超长: user_len=%zu(max %zu) pass_len=%zu(max %zu)",
                    strlen(username), sizeof(g_mqtt.mqtt_username) - 1,
                    strlen(password), sizeof(g_mqtt.mqtt_password) - 1);
            return -EINVAL;
        }
    }

    k_mutex_lock(&g_mqtt.client_mutex, K_FOREVER);

    if (username != NULL && password != NULL) {
        strncpy(g_mqtt.mqtt_username, username, sizeof(g_mqtt.mqtt_username) - 1);
        g_mqtt.mqtt_username[sizeof(g_mqtt.mqtt_username) - 1] = '\0';
        strncpy(g_mqtt.mqtt_password, password, sizeof(g_mqtt.mqtt_password) - 1);
        g_mqtt.mqtt_password[sizeof(g_mqtt.mqtt_password) - 1] = '\0';
        g_mqtt.has_auth = true;
        LOG_INF("MQTT 认证参数已设置: user=%s", g_mqtt.mqtt_username);
    } else {
        g_mqtt.has_auth = false;
        g_mqtt.mqtt_username[0] = '\0';
        g_mqtt.mqtt_password[0] = '\0';
        LOG_INF("MQTT 认证参数已清除");
    }

    k_mutex_unlock(&g_mqtt.client_mutex);

    /* 触发重连以应用新认证 */
    atomic_set(&g_mqtt.pending_disconnect, 1);
    return 0;
}

int protocol_mqtt_set_broker(const char* addr, uint16_t port)
{
    if (addr == NULL || port == 0) {
        return -EINVAL;
    }
    if (strlen(addr) >= sizeof(g_mqtt.broker_addr)) {
        LOG_ERR("MQTT broker 地址超长: len=%zu(max %zu)", strlen(addr),
                sizeof(g_mqtt.broker_addr) - 1);
        return -EINVAL;
    }

    k_mutex_lock(&g_mqtt.client_mutex, K_FOREVER);
    strncpy(g_mqtt.broker_addr, addr, sizeof(g_mqtt.broker_addr) - 1);
    g_mqtt.broker_addr[sizeof(g_mqtt.broker_addr) - 1] = '\0';
    g_mqtt.broker_port = port;
    k_mutex_unlock(&g_mqtt.client_mutex);

    LOG_INF("MQTT Broker 已设置为 %s:%u", addr, port);

    /* 触发重连以应用新 broker */
    atomic_set(&g_mqtt.pending_disconnect, 1);
    return 0;
}

int protocol_mqtt_set_client_id(const char* id)
{
    if (id == NULL || id[0] == '\0') {
        return -EINVAL;
    }
    if (strlen(id) >= sizeof(g_mqtt.client_id)) {
        LOG_ERR("MQTT clientId 超长: len=%zu(max %zu)", strlen(id),
                sizeof(g_mqtt.client_id) - 1);
        return -EINVAL;
    }

    k_mutex_lock(&g_mqtt.client_mutex, K_FOREVER);
    strncpy(g_mqtt.client_id, id, sizeof(g_mqtt.client_id) - 1);
    g_mqtt.client_id[sizeof(g_mqtt.client_id) - 1] = '\0';
    k_mutex_unlock(&g_mqtt.client_mutex);

    LOG_INF("MQTT clientId 已设置为: %s", g_mqtt.client_id);

    /* 触发重连以应用新 clientId */
    atomic_set(&g_mqtt.pending_disconnect, 1);
    return 0;
}

#if defined(CONFIG_MQTT_LIB_TLS)
int protocol_mqtt_set_tls(const sec_tag_t* tags, size_t count, const char* hostname)
{
    if (tags == NULL || count == 0 || count > ARRAY_SIZE(g_mqtt.sec_tags)) {
        return -EINVAL;
    }

    k_mutex_lock(&g_mqtt.client_mutex, K_FOREVER);
    g_mqtt.use_tls = true;
    g_mqtt.sec_tag_count = count;
    for (size_t i = 0; i < count; i++) {
        g_mqtt.sec_tags[i] = tags[i];
    }
    if (hostname != NULL) {
        strncpy(g_mqtt.tls_hostname, hostname, sizeof(g_mqtt.tls_hostname) - 1);
        g_mqtt.tls_hostname[sizeof(g_mqtt.tls_hostname) - 1] = '\0';
    } else {
        g_mqtt.tls_hostname[0] = '\0';
    }
    k_mutex_unlock(&g_mqtt.client_mutex);

    LOG_INF("MQTT TLS 已配置: sec_tag_count=%zu hostname=%s",
            count, hostname ? hostname : "(none)");
    /* 触发重连以应用 TLS 配置 */
    atomic_set(&g_mqtt.pending_disconnect, 1);
    return 0;
}

void protocol_mqtt_clear_tls(void)
{
    k_mutex_lock(&g_mqtt.client_mutex, K_FOREVER);
    g_mqtt.use_tls = false;
    g_mqtt.sec_tag_count = 0;
    g_mqtt.tls_hostname[0] = '\0';
    k_mutex_unlock(&g_mqtt.client_mutex);

    LOG_INF("MQTT TLS 已清除，切换为明文连接");
    atomic_set(&g_mqtt.pending_disconnect, 1);
}
#else /* !CONFIG_MQTT_LIB_TLS */
int protocol_mqtt_set_tls(const sec_tag_t* tags, size_t count, const char* hostname)
{
    ARG_UNUSED(tags);
    ARG_UNUSED(count);
    ARG_UNUSED(hostname);
    LOG_WRN("CONFIG_MQTT_LIB_TLS 未启用，TLS 不可用");
    return -ENOTSUP;
}

void protocol_mqtt_clear_tls(void)
{
    /* TLS 未启用，无操作 */
}
#endif /* CONFIG_MQTT_LIB_TLS */

/* =============================================================================
 * 内部函数
 * ============================================================================= */

/**
 * @brief MQTT 工作线程：驱动连接状态机与 mqtt_input/mqtt_live 轮询
 *
 * @note mqtt_input()/mqtt_live() 均访问 g_mqtt.client，与持锁的
 *       protocol_mqtt_publish() 竞争；本函数对每次调用单独加解锁
 *       client_mutex（不整段持锁），避免长时间饿死 publish() 侧。
 */
static void mqtt_worker_thread(void* p1, void* p2, void* p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    LOG_INF("MQTT 工作线程已启动");

    while (g_mqtt.status == MODULE_STATUS_RUNNING) {
        /* 处理回调或设置变更请求的断开 */
        if (atomic_cas(&g_mqtt.pending_disconnect, 1, 0)) {
            mqtt_do_disconnect();
        }

        if (atomic_get(&g_mqtt.net_up) == 0) {
            if (g_mqtt.state != MQTT_STATE_DISCONNECTED) {
                mqtt_do_disconnect();
            }
            k_sleep(K_MSEC(50));
            continue;
        }

        /* MQTT 状态机 */
        switch (g_mqtt.state) {
        case MQTT_STATE_DISCONNECTED:
            if (k_uptime_get() - g_mqtt.last_connect_attempt >= g_mqtt.reconnect_delay_ms) {
                g_mqtt.last_connect_attempt = k_uptime_get();
                if (mqtt_do_connect() == 0) {
                    g_mqtt.state = MQTT_STATE_CONNECTING;
                } else {
                    /* 指数退避 */
                    g_mqtt.reconnect_delay_ms *= 2;
                    if (g_mqtt.reconnect_delay_ms > RECONNECT_MAX_MS) {
                        g_mqtt.reconnect_delay_ms = RECONNECT_MAX_MS;
                    }
                }
            }
            k_sleep(K_MSEC(100));
            break;

        case MQTT_STATE_CONNECTING:
        case MQTT_STATE_CONNECTED:
        case MQTT_STATE_SUBSCRIBED: {
            /* mqtt_input/mqtt_live 访问 g_mqtt.client，与持锁的 protocol_mqtt_publish()
             * 竞争；逐次调用单独加解锁，不整段持锁以免饿死 publish()。mqtt_evt_handler
             * 在 mqtt_input() 内部同线程同步触发，其中的 mqtt_subscribe() 调用不访问
             * client_mutex，不会造成死锁。 */
            k_mutex_lock(&g_mqtt.client_mutex, K_FOREVER);
            int ret = mqtt_input(&g_mqtt.client);
            k_mutex_unlock(&g_mqtt.client_mutex);
            if (ret != 0 && ret != -EAGAIN) {
                LOG_WRN("MQTT input 错误: %d", ret);
                mqtt_do_disconnect();
                break;
            }

            k_mutex_lock(&g_mqtt.client_mutex, K_FOREVER);
            ret = mqtt_live(&g_mqtt.client);
            k_mutex_unlock(&g_mqtt.client_mutex);
            if (ret != 0 && ret != -EAGAIN) {
                LOG_WRN("MQTT live 错误: %d", ret);
                mqtt_do_disconnect();
                break;
            }

            k_sleep(K_MSEC(100));
            break;
        }
        }
    }

    mqtt_do_disconnect();
    LOG_INF("MQTT 工作线程已退出");
}

/**
 * @brief 发起一次 MQTT 连接
 *
 * @return 0 表示连接请求已发出（实际结果见 CONNACK 回调）；非 0 为失败错误码
 *
 * @note 持锁快照 broker 地址/端口、clientId、认证参数、TLS 参数后再使用，
 *       避免与 protocol_mqtt_set_broker()/set_client_id()/set_auth()/set_tls()
 *       的运行时写入竞争（原实现中 TLS 字段是锁外裸读，与持锁的 set_tls()
 *       写入之间存在撕裂窗口，现并入同一持锁区间）；mqtt_connect() 调用本身
 *       也单独加锁，与 protocol_mqtt_publish() 互斥访问 g_mqtt.client。
 */
static int mqtt_do_connect(void)
{
    int ret;

    memset(&g_mqtt.client, 0, sizeof(g_mqtt.client));

    /* 持锁快照本次连接使用的 broker 地址/端口、clientId、认证参数与 TLS 参数，
     * 避免与 protocol_mqtt_set_broker()/set_client_id()/set_auth()/set_tls()
     * 的运行时写入竞争。这些值需要在整个连接期间保持稳定（clientId 用于
     * mqtt_client.client_id 与 mqtt_evt_handler 拼接订阅主题；用户名/密码/
     * TLS hostname 与 sec_tag 列表被 mqtt_connect() 后异步进行的握手持续
     * 引用），因此均快照到持久缓冲 g_mqtt.conn_*，而非仅存活于本函数栈帧的
     * 局部变量——用户名/密码原实现用栈局部变量存放、却把指针存进生命周期
     * 更长的 g_mqtt.client.user_name/password，属于悬空指针反模式（当前
     * Zephyr MQTT 库仅在 connect 期间同步解引用，故实际不可达，但仍需修正）。 */
    k_mutex_lock(&g_mqtt.client_mutex, K_FOREVER);
    char broker_addr[BROKER_ADDR_MAX_LEN];
    strncpy(broker_addr, g_mqtt.broker_addr, sizeof(broker_addr) - 1);
    broker_addr[sizeof(broker_addr) - 1] = '\0';
    uint16_t broker_port = g_mqtt.broker_port;
    strncpy(g_mqtt.conn_client_id, g_mqtt.client_id, sizeof(g_mqtt.conn_client_id) - 1);
    g_mqtt.conn_client_id[sizeof(g_mqtt.conn_client_id) - 1] = '\0';
    bool has_auth = g_mqtt.has_auth;
    if (has_auth) {
        strncpy(g_mqtt.conn_username, g_mqtt.mqtt_username, sizeof(g_mqtt.conn_username) - 1);
        g_mqtt.conn_username[sizeof(g_mqtt.conn_username) - 1] = '\0';
        strncpy(g_mqtt.conn_password, g_mqtt.mqtt_password, sizeof(g_mqtt.conn_password) - 1);
        g_mqtt.conn_password[sizeof(g_mqtt.conn_password) - 1] = '\0';
    }
#if defined(CONFIG_MQTT_LIB_TLS)
    bool use_tls = g_mqtt.use_tls;
    g_mqtt.conn_sec_tag_count = g_mqtt.sec_tag_count;
    for (size_t i = 0; i < g_mqtt.sec_tag_count && i < ARRAY_SIZE(g_mqtt.conn_sec_tags); i++) {
        g_mqtt.conn_sec_tags[i] = g_mqtt.sec_tags[i];
    }
    strncpy(g_mqtt.conn_tls_hostname, g_mqtt.tls_hostname, sizeof(g_mqtt.conn_tls_hostname) - 1);
    g_mqtt.conn_tls_hostname[sizeof(g_mqtt.conn_tls_hostname) - 1] = '\0';
#endif
    k_mutex_unlock(&g_mqtt.client_mutex);

    ret = resolve_broker_addr(&g_mqtt.broker, broker_addr, broker_port);
    if (ret != 0) {
        LOG_ERR("解析 broker 地址失败");
        return ret;
    }

    mqtt_client_init(&g_mqtt.client);

    g_mqtt.client.broker = &g_mqtt.broker;
    g_mqtt.client.evt_cb = mqtt_evt_handler;
    /* 使用本次连接快照的 clientId（可由 cloud provider 通过 protocol_mqtt_set_client_id 设置） */
    g_mqtt.client.client_id.utf8 = (uint8_t*)g_mqtt.conn_client_id;
    g_mqtt.client.client_id.size = strlen(g_mqtt.conn_client_id);
    g_mqtt.client.protocol_version = MQTT_VERSION_3_1_1;
    g_mqtt.client.rx_buf = g_mqtt.rx_buffer;
    g_mqtt.client.rx_buf_size = sizeof(g_mqtt.rx_buffer);
    g_mqtt.client.tx_buf = g_mqtt.tx_buffer;
    g_mqtt.client.tx_buf_size = sizeof(g_mqtt.tx_buffer);

    /* 设置 keepalive */
    g_mqtt.client.keepalive = GATEWAY_MQTT_KEEPALIVE_S;

    /* 设置认证（如有）：utf8 指针指向持久快照字段 conn_username/conn_password，
     * 而非本函数栈局部变量，生命周期覆盖整个连接期 */
    if (has_auth) {
        g_mqtt.mqtt_user_name.utf8 = (uint8_t*)g_mqtt.conn_username;
        g_mqtt.mqtt_user_name.size = strlen(g_mqtt.conn_username);
        g_mqtt.mqtt_password_utf8.utf8 = (uint8_t*)g_mqtt.conn_password;
        g_mqtt.mqtt_password_utf8.size = strlen(g_mqtt.conn_password);
        g_mqtt.client.user_name = &g_mqtt.mqtt_user_name;
        g_mqtt.client.password = &g_mqtt.mqtt_password_utf8;
    }

    /* TLS 传输配置（仅在 CONFIG_MQTT_LIB_TLS 启用且 use_tls 置位时生效）；
     * sec_tag 列表与 hostname 均使用上面持锁快照的 conn_* 字段 */
#if defined(CONFIG_MQTT_LIB_TLS)
    if (use_tls) {
        g_mqtt.client.transport.type = MQTT_TRANSPORT_SECURE;
        struct mqtt_sec_config* tls_cfg = &g_mqtt.client.transport.tls.config;
        tls_cfg->peer_verify = TLS_PEER_VERIFY_REQUIRED;
        tls_cfg->cipher_list  = NULL;  /* 使用默认加密套件 */
        tls_cfg->sec_tag_list = g_mqtt.conn_sec_tags;
        tls_cfg->sec_tag_count = g_mqtt.conn_sec_tag_count;
        tls_cfg->hostname     = g_mqtt.conn_tls_hostname[0] ? g_mqtt.conn_tls_hostname : NULL;
        LOG_INF("MQTT TLS 已启用，hostname=%s", g_mqtt.conn_tls_hostname);
    } else {
        g_mqtt.client.transport.type = MQTT_TRANSPORT_NON_SECURE;
    }
#else
    g_mqtt.client.transport.type = MQTT_TRANSPORT_NON_SECURE;
#endif /* CONFIG_MQTT_LIB_TLS */

    /* mqtt_connect() 访问 g_mqtt.client，与持锁的 protocol_mqtt_publish() 竞争，
     * 单独加解锁包住本次调用 */
    k_mutex_lock(&g_mqtt.client_mutex, K_FOREVER);
    ret = mqtt_connect(&g_mqtt.client);
    k_mutex_unlock(&g_mqtt.client_mutex);
    if (ret != 0) {
        LOG_ERR("MQTT 连接失败: %d", ret);
        return ret;
    }

    LOG_INF("MQTT 连接中...");
    return 0;
}

/**
 * @brief 断开当前 MQTT 连接并复位状态（幂等：已断开时空操作）
 *
 * @note 全程持有 client_mutex，与 protocol_mqtt_publish() 及 worker 线程
 *       的 mqtt_input()/mqtt_live()/mqtt_connect() 互斥访问 g_mqtt.client。
 */
static void mqtt_do_disconnect(void)
{
    k_mutex_lock(&g_mqtt.client_mutex, K_FOREVER);
    if (g_mqtt.state != MQTT_STATE_DISCONNECTED) {
        mqtt_disconnect(&g_mqtt.client, NULL);
        g_mqtt.state = MQTT_STATE_DISCONNECTED;
        g_mqtt.disconnect_count++;
        mqtt_publish_state_event(false);
        LOG_INF("MQTT 已断开");
    }
    k_mutex_unlock(&g_mqtt.client_mutex);
}

static void mqtt_publish_state_event(bool connected)
{
    if (connected) {
        event_publish_copy(EVENT_TYPE_CLOUD_CONNECTED, EVENT_PRIORITY_HIGH, NULL, 0);
    } else {
        event_publish_copy(EVENT_TYPE_CLOUD_DISCONNECTED, EVENT_PRIORITY_HIGH, NULL, 0);
    }
}

/**
 * @brief MQTT 下行命令分发钩子（扩展点，当前未实现具体命令）
 *
 * @param topic       命令主题字符串（非 NUL 结尾，长度见 topic_len）
 * @param topic_len   主题长度
 * @param payload     命令载荷（最多 MQTT_CMD_PAYLOAD_MAX 字节，超出部分已在
 *                    调用方排空丢弃，不会出现在这里）
 * @param payload_len 载荷实际捕获长度
 *
 * @note 当前仅记录日志。后续如需支持云端下行控制指令（如远程重启、参数下发），
 *       在此解析 payload（如 JSON）并路由到具体命令处理器。
 */
static void mqtt_on_command(const char* topic, size_t topic_len,
                             const uint8_t* payload, size_t payload_len)
{
    ARG_UNUSED(payload);
    LOG_INF("MQTT 命令主题 %.*s 载荷 %zu 字节（命令分发未实现，扩展点见 mqtt_on_command）",
            (int)topic_len, topic, payload_len);
}

/**
 * @brief Zephyr MQTT 库事件回调（CONNACK/DISCONNECT/PUBLISH/SUBACK 等）
 *
 * @param client 触发事件的 MQTT 客户端（即 &g_mqtt.client）
 * @param evt    事件参数
 *
 * @note 由 mqtt_input() 在 worker 线程上下文同步调用（非独立线程/ISR），
 *       此处对 g_mqtt.state 等字段的写入与 worker 线程本身天然同线程有序，
 *       无需额外加锁；CONNACK 成功即发布 CLOUD_CONNECTED，SUBSCRIBED 状态
 *       改为在 SUBACK 校验通过后才置位（见 MQTT_EVT_SUBACK 分支）。
 */
static void mqtt_evt_handler(struct mqtt_client* client, const struct mqtt_evt* evt)
{
    ARG_UNUSED(client);
    int ret;

    switch (evt->type) {
    case MQTT_EVT_CONNACK:
        if (evt->result != 0) {
            LOG_ERR("MQTT CONNACK 错误: %d", evt->result);
            atomic_set(&g_mqtt.pending_disconnect, 1);
            break;
        }
        g_mqtt.state = MQTT_STATE_CONNECTED;
        g_mqtt.connect_count++;
        g_mqtt.reconnect_delay_ms = RECONNECT_MIN_MS;
        /* CONNACK 成功即视为云连接建立；SUBSCRIBED 是更细的“命令通道已就绪”状态，
         * 不再等订阅结果才发布 CLOUD_CONNECTED（订阅失败不代表连接本身失败）。 */
        mqtt_publish_state_event(true);
        LOG_INF("MQTT 已连接");

        /* 订阅命令主题（使用本次连接快照的 clientId g_mqtt.conn_client_id，拼接到静态缓冲）*/
        static char cmd_topic[224];
        snprintf(cmd_topic, sizeof(cmd_topic), "gateway/cmd/%s", g_mqtt.conn_client_id);
        struct mqtt_topic topic = {
            .topic = {
                .utf8 = (uint8_t*)cmd_topic,
                .size = strlen(cmd_topic),
            },
            .qos = MQTT_QOS_0_AT_MOST_ONCE,
        };
        struct mqtt_subscription_list sub_list = {
            .list = &topic,
            .list_count = 1,
        };
        ret = mqtt_subscribe(client, &sub_list);
        if (ret != 0) {
            LOG_WRN("MQTT 订阅失败: %d", ret);
            atomic_set(&g_mqtt.pending_disconnect, 1);
            break;
        }
        /* 订阅请求已发出，等待 MQTT_EVT_SUBACK 校验结果后才置 SUBSCRIBED */
        break;

    case MQTT_EVT_DISCONNECT:
        LOG_INF("MQTT 断开通知");
        if (g_mqtt.state != MQTT_STATE_DISCONNECTED) {
            g_mqtt.state = MQTT_STATE_DISCONNECTED;
            g_mqtt.disconnect_count++;
            mqtt_publish_state_event(false);
        }
        break;

    case MQTT_EVT_PUBLISH: {
        const struct mqtt_publish_param* p = &evt->param.publish;
        uint32_t payload_len = p->message.payload.len;
        g_mqtt.msg_rx_count++;
        LOG_INF("MQTT 收到消息: topic=%.*s len=%u", p->message.topic.topic.size,
                p->message.topic.topic.utf8, payload_len);

        /* PUBLISH 事件本身不含 payload 数据，必须调用 mqtt_read_publish_payload_blocking()
         * 主动读出并排空，否则残留数据会污染下一个包的解析。命令主题预期较短：整段
         * （最多 MQTT_CMD_PAYLOAD_MAX 字节）读入栈缓冲供 mqtt_on_command() 使用；
         * 超出部分按 128B 分段继续读出丢弃，只为排空缓冲，不再保留。 */
        uint8_t cmd_buf[MQTT_CMD_PAYLOAD_MAX];
        size_t  captured_len = 0;
        size_t  first_read = MIN((size_t)payload_len, sizeof(cmd_buf));
        if (first_read > 0) {
            ret = mqtt_read_publish_payload_blocking(client, cmd_buf, first_read);
            if (ret > 0) {
                captured_len = (size_t)ret;
            } else {
                LOG_WRN("MQTT PUBLISH payload 读取失败: %d", ret);
            }
        }
        uint32_t remaining = (payload_len > captured_len) ?
                              (payload_len - (uint32_t)captured_len) : 0;
        while (remaining > 0) {
            uint8_t discard[128];
            size_t  chunk_len = MIN((size_t)remaining, sizeof(discard));
            ret = mqtt_read_publish_payload_blocking(client, discard, chunk_len);
            if (ret <= 0) {
                LOG_WRN("MQTT PUBLISH payload 排空失败: %d", ret);
                break;
            }
            remaining -= (uint32_t)ret;
        }

        mqtt_on_command((const char*)p->message.topic.topic.utf8,
                         p->message.topic.topic.size, cmd_buf, captured_len);

        /* QoS1 消息需回 PUBACK 确认，否则 broker 会重传 */
        if (p->message.topic.qos == MQTT_QOS_1_AT_LEAST_ONCE) {
            struct mqtt_puback_param puback_param = { .message_id = p->message_id };
            ret = mqtt_publish_qos1_ack(client, &puback_param);
            if (ret != 0) {
                LOG_WRN("MQTT PUBACK 发送失败: %d", ret);
            }
        }
        break;
    }

    case MQTT_EVT_SUBACK: {
        const struct mqtt_suback_param* p = &evt->param.suback;
        if (evt->result != 0) {
            /* evt->result 反映 SUBACK 报文解码是否成功，非订阅授予结果 */
            LOG_ERR("MQTT SUBACK 解码错误: %d", evt->result);
            break;
        }
        uint8_t return_code = MQTT_SUBACK_FAILURE;
        if (p->return_codes.data != NULL && p->return_codes.len > 0) {
            return_code = p->return_codes.data[0];
        }
        if (return_code == MQTT_SUBACK_FAILURE) {
            /* 订阅被 broker 拒绝：保持 CONNECTED，不引入重试，仅记录错误 */
            LOG_ERR("MQTT 订阅被 Broker 拒绝 (SUBACK failure)");
            break;
        }
        g_mqtt.state = MQTT_STATE_SUBSCRIBED;
        LOG_INF("MQTT 订阅成功 (SUBACK), qos=%u", return_code);
        break;
    }

    default:
        break;
    }
}

/**
 * @brief 解析 broker 地址并写入 sockaddr_storage
 *
 * @param addr        输出：解析结果
 * @param broker_addr broker 主机名/IP（调用方已持锁快照，避免直接读取 g_mqtt.broker_addr
 *                     与 protocol_mqtt_set_broker() 竞争）
 * @param broker_port broker 端口（同上，调用方快照值）
 * @return 0 成功；非 0 为 getaddrinfo 错误码
 */
static int resolve_broker_addr(struct sockaddr_storage* addr, const char* broker_addr,
                                uint16_t broker_port)
{
    struct zsock_addrinfo* res;
    struct zsock_addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };

    int ret = zsock_getaddrinfo(broker_addr, NULL, &hints, &res);
    if (ret != 0) {
        LOG_ERR("getaddrinfo 失败: %d", ret);
        return ret;
    }

    /* 防御性检查：res->ai_addrlen 理论上不应超过 sizeof(*addr)（sockaddr_storage
     * 已是最大地址结构容量），但一旦底层解析器返回异常长度，越界 memcpy 会破坏
     * addr 之后的内存，故先校验再拷贝 */
    if (res->ai_addrlen > sizeof(*addr)) {
        LOG_ERR("getaddrinfo 返回地址长度异常: %u > %zu", (unsigned)res->ai_addrlen,
                sizeof(*addr));
        zsock_freeaddrinfo(res);
        return -EINVAL;
    }

    memcpy(addr, res->ai_addr, res->ai_addrlen);

    if (addr->ss_family == AF_INET) {
        struct sockaddr_in* sin = (struct sockaddr_in*)addr;
        sin->sin_port = htons(broker_port);
    } else if (addr->ss_family == AF_INET6) {
        struct sockaddr_in6* sin6 = (struct sockaddr_in6*)addr;
        sin6->sin6_port = htons(broker_port);
    }

    zsock_freeaddrinfo(res);
    return 0;
}

/* =============================================================================
 * 模块接口声明与自动注册
 * ============================================================================= */

static const char* const protocol_mqtt_deps[] = {"network_manager", NULL};

DECLARE_MODULE_INTERFACE_WITH_DEPS(protocol_mqtt, protocol_mqtt_deps);

const module_interface_t* protocol_mqtt_get_interface(void)
{
    return &protocol_mqtt_interface;
}

static int protocol_mqtt_auto_register(void)
{
    uint32_t module_id;
    if (module_manager_register(protocol_mqtt_get_interface(), NULL, &module_id) != 0) {
        LOG_ERR("MQTT 模块注册失败");
        return -EIO;
    }
    LOG_INF("MQTT 模块已注册 (id=%u)", module_id);
    return 0;
}

SYS_INIT(protocol_mqtt_auto_register, POST_KERNEL, GATEWAY_INIT_PRIO_PROTOCOL_MQTT);
