/**
 * @file protocol_eth.c
 * @brief 以太网/MQTT 连接管理模块实现
 *
 * 管理网络连接和 MQTT 会话，发布网络状态事件，
 * 为 cloud_upload 提供 MQTT 发送接口。
 */

#include "protocol_eth.h"
#include "gateway_events.h"
#include "gateway_config.h"
#include "app_config.h"

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/socket.h>
#include <string.h>

#include "event_system.h"
#include "module_manager.h"

LOG_MODULE_REGISTER(protocol_eth, CONFIG_SYS_LOG_LEVEL);

/* =============================================================================
 * 配置
 * ============================================================================= */

#define ETH_THREAD_PRIORITY   GATEWAY_THREAD_PRIORITY_DEFAULT
#define ETH_THREAD_STACK_SIZE GATEWAY_THREAD_STACK_SIZE_LARGE
#define MQTT_RX_BUF_SIZE      256
#define MQTT_TX_BUF_SIZE      256
#define RECONNECT_MIN_MS      GATEWAY_MQTT_RECONNECT_MIN_MS
#define RECONNECT_MAX_MS      GATEWAY_MQTT_RECONNECT_MAX_MS

/* =============================================================================
 * 内部数据结构
 * ============================================================================= */

typedef enum {
    ETH_STATE_DISCONNECTED = 0,
    ETH_STATE_CONNECTING,
    ETH_STATE_CONNECTED,
    ETH_STATE_SUBSCRIBED,
} eth_state_t;

typedef struct {
    module_status_t      status;
    struct k_thread      worker_thread;
    K_KERNEL_STACK_MEMBER(worker_stack, ETH_THREAD_STACK_SIZE);
    /* MQTT */
    struct mqtt_client   client;
    struct sockaddr_storage broker;
    uint8_t              rx_buffer[MQTT_RX_BUF_SIZE];
    uint8_t              tx_buffer[MQTT_TX_BUF_SIZE];
    /* 状态 */
    eth_state_t          state;
    bool                 net_up;
    bool                 pending_disconnect;
    /* 重连 */
    uint32_t             reconnect_delay_ms;
    int64_t              last_connect_attempt;
    /* 统计 */
    uint32_t             connect_count;
    uint32_t             disconnect_count;
    uint32_t             msg_tx_count;
    uint32_t             msg_rx_count;
    /* 认证（由 cloud provider 设置） */
    char                 mqtt_username[64];
    char                 mqtt_password[128];
    bool                 has_auth;
    struct mqtt_utf8     mqtt_user_name;
    struct mqtt_utf8     mqtt_password_utf8;
    /* 同步 */
    struct k_mutex       client_mutex;
} protocol_eth_cb_t;

/* =============================================================================
 * 静态变量
 * ============================================================================= */

static protocol_eth_cb_t g_eth;

/* =============================================================================
 * 前向声明
 * ============================================================================= */

static void eth_worker_thread(void* p1, void* p2, void* p3);
static int  eth_mqtt_connect(void);
static void eth_mqtt_disconnect(void);
static void eth_publish_state_event(bool connected);
static void mqtt_evt_handler(struct mqtt_client* client,
                              const struct mqtt_evt* evt);
static int  resolve_broker_addr(struct sockaddr_storage* addr);

/* =============================================================================
 * 模块接口实现
 * ============================================================================= */

int protocol_eth_init(void* config)
{
    ARG_UNUSED(config);
    LOG_INF("初始化 Ethernet/MQTT 模块...");

    memset(&g_eth, 0, sizeof(g_eth));
    g_eth.status = MODULE_STATUS_INITIALIZED;
    g_eth.state = ETH_STATE_DISCONNECTED;
    g_eth.reconnect_delay_ms = RECONNECT_MIN_MS;
    g_eth.pending_disconnect = false;

    k_mutex_init(&g_eth.client_mutex);

    event_register_type(EVENT_TYPE_CLOUD_CONNECTED, "cloud_connected");
    event_register_type(EVENT_TYPE_CLOUD_DISCONNECTED, "cloud_disconnected");

    LOG_INF("Ethernet/MQTT 模块初始化完成");
    return 0;
}

int protocol_eth_start(void)
{
    if (g_eth.status != MODULE_STATUS_INITIALIZED &&
        g_eth.status != MODULE_STATUS_STOPPED) {
        return -1;
    }

    g_eth.status = MODULE_STATUS_RUNNING;
    g_eth.net_up = false;
    g_eth.state = ETH_STATE_DISCONNECTED;
    g_eth.pending_disconnect = false;

    k_thread_create(
        &g_eth.worker_thread, g_eth.worker_stack,
        K_THREAD_STACK_SIZEOF(g_eth.worker_stack),
        eth_worker_thread, NULL, NULL, NULL,
        ETH_THREAD_PRIORITY, 0, K_FOREVER);
    k_thread_name_set(&g_eth.worker_thread, "proto_eth");
    k_thread_start(&g_eth.worker_thread);

    LOG_INF("Ethernet/MQTT 模块已启动");
    return 0;
}

int protocol_eth_stop(void)
{
    if (g_eth.status != MODULE_STATUS_RUNNING) {
        return 0;
    }

    eth_mqtt_disconnect();
    g_eth.status = MODULE_STATUS_STOPPED;

    /* 等待工作线程实际退出 */
    k_thread_join(&g_eth.worker_thread, K_MSEC(500));

    LOG_INF("Ethernet/MQTT 模块已停止");
    return 0;
}

int protocol_eth_shutdown(void)
{
    protocol_eth_stop();
    g_eth.status = MODULE_STATUS_UNINITIALIZED;
    return 0;
}

void protocol_eth_on_event(const event_t* event, void* user_data)
{
    ARG_UNUSED(user_data);
    if (event == NULL) return;
}

module_status_t protocol_eth_get_status(void)
{
    return g_eth.status;
}

int protocol_eth_control(int cmd, void* arg)
{
    switch (cmd) {
    case ETH_CMD_PUBLISH:
        if (arg == NULL) return -1;
        {
            const char** params = (const char**)arg;
            if (params[0] == NULL || params[1] == NULL) return -1;
            return protocol_eth_mqtt_publish(params[0], params[1], (uint16_t)strlen(params[1]));
        }
    case ETH_CMD_GET_STATUS:
        if (arg != NULL) {
            *(bool*)arg = protocol_eth_is_connected();
        }
        return 0;
    default:
        return -1;
    }
}

/* =============================================================================
 * 模块特定 API
 * ============================================================================= */

bool protocol_eth_is_connected(void)
{
    return (g_eth.status == MODULE_STATUS_RUNNING &&
            g_eth.state == ETH_STATE_SUBSCRIBED);
}

int protocol_eth_mqtt_publish(const char* topic, const char* payload, uint16_t payload_len)
{
    if (!protocol_eth_is_connected() || topic == NULL || payload == NULL) {
        return -1;
    }

    int ret = k_mutex_lock(&g_eth.client_mutex, K_MSEC(100));
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

    ret = mqtt_publish(&g_eth.client, &param);
    if (ret == 0) {
        g_eth.msg_tx_count++;
    }

    k_mutex_unlock(&g_eth.client_mutex);
    return ret;
}

void protocol_eth_get_stats(uint32_t* connect_count, uint32_t* disconnect_count,
                             uint32_t* msg_count)
{
    if (connect_count != NULL) *connect_count = g_eth.connect_count;
    if (disconnect_count != NULL) *disconnect_count = g_eth.disconnect_count;
    if (msg_count != NULL) *msg_count = g_eth.msg_tx_count;
}

int protocol_eth_mqtt_set_auth(const char* username, const char* password)
{
    k_mutex_lock(&g_eth.client_mutex, K_FOREVER);

    if (username != NULL && password != NULL) {
        strncpy(g_eth.mqtt_username, username, sizeof(g_eth.mqtt_username) - 1);
        g_eth.mqtt_username[sizeof(g_eth.mqtt_username) - 1] = '\0';
        strncpy(g_eth.mqtt_password, password, sizeof(g_eth.mqtt_password) - 1);
        g_eth.mqtt_password[sizeof(g_eth.mqtt_password) - 1] = '\0';
        g_eth.has_auth = true;
        LOG_INF("MQTT 认证参数已设置: user=%s", g_eth.mqtt_username);
    } else {
        g_eth.has_auth = false;
        g_eth.mqtt_username[0] = '\0';
        g_eth.mqtt_password[0] = '\0';
        LOG_INF("MQTT 认证参数已清除");
    }

    k_mutex_unlock(&g_eth.client_mutex);
    return 0;
}

int protocol_eth_mqtt_set_broker(const char* addr, uint16_t port)
{
    if (addr == NULL) {
        return -EINVAL;
    }

    LOG_WRN("MQTT Broker 运行时设置尚未实现 (requested: %s:%u)", addr, port);
    return -ENOTSUP;
}

/* =============================================================================
 * 内部函数
 * ============================================================================= */

static void eth_worker_thread(void* p1, void* p2, void* p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    LOG_INF("Ethernet 工作线程已启动");

    while (g_eth.status == MODULE_STATUS_RUNNING) {
        /* 处理回调请求的断开 */
        if (g_eth.pending_disconnect) {
            g_eth.pending_disconnect = false;
            eth_mqtt_disconnect();
        }

        /* 检查网络接口状态 */
        struct net_if* iface = net_if_get_default();
        bool net_was_up = g_eth.net_up;
        g_eth.net_up = (iface != NULL && net_if_is_up(iface));

        if (g_eth.net_up && !net_was_up) {
            LOG_INF("网络接口已上线");
            g_eth.reconnect_delay_ms = RECONNECT_MIN_MS;
        }

        if (!g_eth.net_up) {
            if (g_eth.state != ETH_STATE_DISCONNECTED) {
                eth_mqtt_disconnect();
            }
            g_eth.pending_disconnect = false;
            k_sleep(K_MSEC(1000));
            continue;
        }

        /* MQTT 状态机 */
        switch (g_eth.state) {
        case ETH_STATE_DISCONNECTED:
            if (k_uptime_get() - g_eth.last_connect_attempt >= g_eth.reconnect_delay_ms) {
                g_eth.last_connect_attempt = k_uptime_get();
                if (eth_mqtt_connect() == 0) {
                    g_eth.state = ETH_STATE_CONNECTING;
                } else {
                    /* 指数退避 */
                    g_eth.reconnect_delay_ms *= 2;
                    if (g_eth.reconnect_delay_ms > RECONNECT_MAX_MS) {
                        g_eth.reconnect_delay_ms = RECONNECT_MAX_MS;
                    }
                }
            }
            k_sleep(K_MSEC(100));
            break;

        case ETH_STATE_CONNECTING:
        case ETH_STATE_CONNECTED:
        case ETH_STATE_SUBSCRIBED: {
            int ret = mqtt_input(&g_eth.client);
            if (ret != 0 && ret != -EAGAIN) {
                LOG_WRN("MQTT input 错误: %d", ret);
                eth_mqtt_disconnect();
                break;
            }

            ret = mqtt_live(&g_eth.client);
            if (ret != 0 && ret != -EAGAIN) {
                LOG_WRN("MQTT live 错误: %d", ret);
                eth_mqtt_disconnect();
                break;
            }

            k_sleep(K_MSEC(100));
            break;
        }
        }
    }

    eth_mqtt_disconnect();
    LOG_INF("Ethernet 工作线程已退出");
}

static int eth_mqtt_connect(void)
{
    int ret;

    memset(&g_eth.client, 0, sizeof(g_eth.client));

    ret = resolve_broker_addr(&g_eth.broker);
    if (ret != 0) {
        LOG_ERR("解析 broker 地址失败");
        return ret;
    }

    mqtt_client_init(&g_eth.client);

    g_eth.client.broker = &g_eth.broker;
    g_eth.client.evt_cb = mqtt_evt_handler;
    g_eth.client.client_id.utf8 = (uint8_t*)CONFIG_GATEWAY_MQTT_CLIENT_ID;
    g_eth.client.client_id.size = strlen(CONFIG_GATEWAY_MQTT_CLIENT_ID);
    g_eth.client.protocol_version = MQTT_VERSION_3_1_1;
    g_eth.client.rx_buf = g_eth.rx_buffer;
    g_eth.client.rx_buf_size = sizeof(g_eth.rx_buffer);
    g_eth.client.tx_buf = g_eth.tx_buffer;
    g_eth.client.tx_buf_size = sizeof(g_eth.tx_buffer);

    /* 设置 keepalive */
    g_eth.client.keepalive = GATEWAY_MQTT_KEEPALIVE_S;

    /* 设置认证（如有） */
    if (g_eth.has_auth) {
        g_eth.mqtt_user_name.utf8 = (uint8_t*)g_eth.mqtt_username;
        g_eth.mqtt_user_name.size = strlen(g_eth.mqtt_username);
        g_eth.mqtt_password_utf8.utf8 = (uint8_t*)g_eth.mqtt_password;
        g_eth.mqtt_password_utf8.size = strlen(g_eth.mqtt_password);
        g_eth.client.user_name = &g_eth.mqtt_user_name;
        g_eth.client.password = &g_eth.mqtt_password_utf8;
    }

    ret = mqtt_connect(&g_eth.client);
    if (ret != 0) {
        LOG_ERR("MQTT 连接失败: %d", ret);
        return ret;
    }

    LOG_INF("MQTT 连接中...");
    return 0;
}

static void eth_mqtt_disconnect(void)
{
    k_mutex_lock(&g_eth.client_mutex, K_FOREVER);
    if (g_eth.state != ETH_STATE_DISCONNECTED) {
        mqtt_disconnect(&g_eth.client, NULL);
        g_eth.state = ETH_STATE_DISCONNECTED;
        g_eth.disconnect_count++;
        eth_publish_state_event(false);
        LOG_INF("MQTT 已断开");
    }
    k_mutex_unlock(&g_eth.client_mutex);
}

static void eth_publish_state_event(bool connected)
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
            g_eth.pending_disconnect = true;
            break;
        }
        g_eth.state = ETH_STATE_CONNECTED;
        g_eth.connect_count++;
        g_eth.reconnect_delay_ms = RECONNECT_MIN_MS;
        LOG_INF("MQTT 已连接");

        /* 订阅命令主题 */
        struct mqtt_topic topic = {
            .topic = {
                .utf8 = (uint8_t*)"gateway/cmd/" CONFIG_GATEWAY_MQTT_CLIENT_ID,
                .size = strlen("gateway/cmd/" CONFIG_GATEWAY_MQTT_CLIENT_ID),
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
            g_eth.pending_disconnect = true;
            break;
        }
        g_eth.state = ETH_STATE_SUBSCRIBED;
        eth_publish_state_event(true);
        break;

    case MQTT_EVT_DISCONNECT:
        LOG_INF("MQTT 断开通知");
        g_eth.state = ETH_STATE_DISCONNECTED;
        g_eth.disconnect_count++;
        eth_publish_state_event(false);
        break;

    case MQTT_EVT_PUBLISH: {
        const struct mqtt_publish_param* p = &evt->param.publish;
        g_eth.msg_rx_count++;
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

    int ret = zsock_getaddrinfo(CONFIG_GATEWAY_MQTT_BROKER_ADDR, NULL, &hints, &res);
    if (ret != 0) {
        LOG_ERR("getaddrinfo 失败: %d", ret);
        return ret;
    }

    memcpy(addr, res->ai_addr, res->ai_addrlen);

    if (addr->ss_family == AF_INET) {
        struct sockaddr_in* sin = (struct sockaddr_in*)addr;
        sin->sin_port = htons(CONFIG_GATEWAY_MQTT_BROKER_PORT);
    } else if (addr->ss_family == AF_INET6) {
        struct sockaddr_in6* sin6 = (struct sockaddr_in6*)addr;
        sin6->sin6_port = htons(CONFIG_GATEWAY_MQTT_BROKER_PORT);
    }

    zsock_freeaddrinfo(res);
    return 0;
}

/* =============================================================================
 * 模块接口声明与自动注册
 * ============================================================================= */

static const char* const protocol_eth_deps[] = {NULL};

DECLARE_MODULE_INTERFACE_WITH_DEPS(protocol_eth, protocol_eth_deps);

const module_interface_t* protocol_eth_get_interface(void)
{
    return &protocol_eth_interface;
}

static int protocol_eth_auto_register(void)
{
    uint32_t module_id;
    if (module_manager_register(protocol_eth_get_interface(), NULL, &module_id) != 0) {
        LOG_ERR("Ethernet 模块注册失败");
        return -EIO;
    }
    LOG_INF("Ethernet 模块已注册 (id=%u)", module_id);
    return 0;
}

SYS_INIT(protocol_eth_auto_register, POST_KERNEL, GATEWAY_INIT_PRIO_PROTOCOL_ETH);
