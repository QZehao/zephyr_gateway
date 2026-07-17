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
#include <zeplod/app_config.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/sys/atomic.h>
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
    /* 认证（由 cloud provider 设置） */
    char                 mqtt_username[64];
    char                 mqtt_password[128];
    bool                 has_auth;
    struct mqtt_utf8     mqtt_user_name;
    struct mqtt_utf8     mqtt_password_utf8;
    /* 运行时 clientId（由 cloud provider 通过 protocol_mqtt_set_client_id 设置） */
    char                 client_id[128];
    /* TLS 配置（CONFIG_MQTT_LIB_TLS 守护） */
#if defined(CONFIG_MQTT_LIB_TLS)
    bool                 use_tls;
    sec_tag_t            sec_tags[3];
    size_t               sec_tag_count;
    char                 tls_hostname[64];
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
static int  resolve_broker_addr(struct sockaddr_storage* addr);

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
    atomic_set(&g_mqtt.net_up, 0);
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
    return (g_mqtt.status == MODULE_STATUS_RUNNING &&
            g_mqtt.state == MQTT_STATE_SUBSCRIBED);
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
            int ret = mqtt_input(&g_mqtt.client);
            if (ret != 0 && ret != -EAGAIN) {
                LOG_WRN("MQTT input 错误: %d", ret);
                mqtt_do_disconnect();
                break;
            }

            ret = mqtt_live(&g_mqtt.client);
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

static int mqtt_do_connect(void)
{
    int ret;

    memset(&g_mqtt.client, 0, sizeof(g_mqtt.client));

    ret = resolve_broker_addr(&g_mqtt.broker);
    if (ret != 0) {
        LOG_ERR("解析 broker 地址失败");
        return ret;
    }

    mqtt_client_init(&g_mqtt.client);

    g_mqtt.client.broker = &g_mqtt.broker;
    g_mqtt.client.evt_cb = mqtt_evt_handler;
    /* 使用运行时 clientId（可由 cloud provider 通过 protocol_mqtt_set_client_id 设置） */
    g_mqtt.client.client_id.utf8 = (uint8_t*)g_mqtt.client_id;
    g_mqtt.client.client_id.size = strlen(g_mqtt.client_id);
    g_mqtt.client.protocol_version = MQTT_VERSION_3_1_1;
    g_mqtt.client.rx_buf = g_mqtt.rx_buffer;
    g_mqtt.client.rx_buf_size = sizeof(g_mqtt.rx_buffer);
    g_mqtt.client.tx_buf = g_mqtt.tx_buffer;
    g_mqtt.client.tx_buf_size = sizeof(g_mqtt.tx_buffer);

    /* 设置 keepalive */
    g_mqtt.client.keepalive = GATEWAY_MQTT_KEEPALIVE_S;

    /* 读取认证参数时持有锁 */
    k_mutex_lock(&g_mqtt.client_mutex, K_FOREVER);
    bool has_auth = g_mqtt.has_auth;
    char username[64] = {0};
    char password[128] = {0};
    if (has_auth) {
        strncpy(username, g_mqtt.mqtt_username, sizeof(username) - 1);
        strncpy(password, g_mqtt.mqtt_password, sizeof(password) - 1);
    }
    k_mutex_unlock(&g_mqtt.client_mutex);

    /* 设置认证（如有） */
    if (has_auth) {
        g_mqtt.mqtt_user_name.utf8 = (uint8_t*)username;
        g_mqtt.mqtt_user_name.size = strlen(username);
        g_mqtt.mqtt_password_utf8.utf8 = (uint8_t*)password;
        g_mqtt.mqtt_password_utf8.size = strlen(password);
        g_mqtt.client.user_name = &g_mqtt.mqtt_user_name;
        g_mqtt.client.password = &g_mqtt.mqtt_password_utf8;
    }

    /* TLS 传输配置（仅在 CONFIG_MQTT_LIB_TLS 启用且 use_tls 置位时生效） */
#if defined(CONFIG_MQTT_LIB_TLS)
    k_mutex_lock(&g_mqtt.client_mutex, K_FOREVER);
    bool use_tls = g_mqtt.use_tls;
    k_mutex_unlock(&g_mqtt.client_mutex);

    if (use_tls) {
        g_mqtt.client.transport.type = MQTT_TRANSPORT_SECURE;
        struct mqtt_sec_config* tls_cfg = &g_mqtt.client.transport.tls.config;
        tls_cfg->peer_verify = TLS_PEER_VERIFY_REQUIRED;
        tls_cfg->cipher_list  = NULL;  /* 使用默认加密套件 */
        tls_cfg->sec_tag_list = g_mqtt.sec_tags;
        tls_cfg->sec_tag_count = g_mqtt.sec_tag_count;
        tls_cfg->hostname     = g_mqtt.tls_hostname[0] ? g_mqtt.tls_hostname : NULL;
        LOG_INF("MQTT TLS 已启用，hostname=%s", g_mqtt.tls_hostname);
    } else {
        g_mqtt.client.transport.type = MQTT_TRANSPORT_NON_SECURE;
    }
#else
    g_mqtt.client.transport.type = MQTT_TRANSPORT_NON_SECURE;
#endif /* CONFIG_MQTT_LIB_TLS */

    ret = mqtt_connect(&g_mqtt.client);
    if (ret != 0) {
        LOG_ERR("MQTT 连接失败: %d", ret);
        return ret;
    }

    LOG_INF("MQTT 连接中...");
    return 0;
}

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
        LOG_INF("MQTT 已连接");

        /* 订阅命令主题（使用运行时 clientId，拼接到静态缓冲）*/
        static char cmd_topic[160];
        snprintf(cmd_topic, sizeof(cmd_topic), "gateway/cmd/%s", g_mqtt.client_id);
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
        g_mqtt.state = MQTT_STATE_SUBSCRIBED;
        mqtt_publish_state_event(true);
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
        g_mqtt.msg_rx_count++;
        LOG_INF("MQTT 收到消息: topic=%.*s", p->message.topic.topic.size,
                p->message.topic.topic.utf8);
        break;
    }

    case MQTT_EVT_SUBACK:
        LOG_DBG("MQTT SUBACK");
        break;

    default:
        break;
    }
}

static int resolve_broker_addr(struct sockaddr_storage* addr)
{
    struct zsock_addrinfo* res;
    struct zsock_addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };

    int ret = zsock_getaddrinfo(g_mqtt.broker_addr, NULL, &hints, &res);
    if (ret != 0) {
        LOG_ERR("getaddrinfo 失败: %d", ret);
        return ret;
    }

    memcpy(addr, res->ai_addr, res->ai_addrlen);

    if (addr->ss_family == AF_INET) {
        struct sockaddr_in* sin = (struct sockaddr_in*)addr;
        sin->sin_port = htons(g_mqtt.broker_port);
    } else if (addr->ss_family == AF_INET6) {
        struct sockaddr_in6* sin6 = (struct sockaddr_in6*)addr;
        sin6->sin6_port = htons(g_mqtt.broker_port);
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
