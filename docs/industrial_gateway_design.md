# Zephyr 工业边缘网关 — 详细设计文档

| 项 | 值 |
|---|---|
| **版本** | v1.1.0（云平台 Provider 模块化） |
| **日期** | 2026-05-14 |
| **状态** | 基础 MVP 框架（W1-W8） |
| **目标硬件** | 野火 RT1052 Pro / STM32H743 Pro / Nucleo L4R5ZI（验证） |

---

## 1. 架构总览

### 1.1 模块拓扑

```
+-------------------------------------------------------------+
|                    zephyr_gateway (业务仓库)                  |
|  +-------------------------------------------------------+  |
|  |  src/gateway/ — 工业网关业务模块（本设计文档范围）       |  |
|  |                                                       |  |
|  |  协议层 (Protocol Layer)                               |  |
|  |    ├─ protocol_can.c/h      CAN 总线数据采集           |  |
|  |    ├─ protocol_modbus.c/h   Modbus RTU (485)          |  |
|  |    └─ protocol_eth.c/h      以太网/MQTT 连接管理        |  |
|  |                                                       |  |
|  |  抽象层 (Abstraction Layer)                            |  |
|  |    └─ cloud_provider.c/h     云平台 Provider 注册表     |  |
|  |                                                       |  |
|  |  业务层 (Business Layer)                                |  |
|  |    ├─ anomaly_detection.c/h  自适应阈值异常检测         |  |
|  |    ├─ cloud_upload.c/h      数据上云（JSON + 速率控制）  |  |
|  |    ├─ offline_cache.c/h     断网本地缓存               |  |
|  |    └─ webshell.c/h          远程 Shell 基础版          |  |
|  |                                                       |  |
|  |  云平台层 (Cloud Provider Layer)                       |  |
|  |    ├─ cloud_private.c/h     私有 MQTT Broker           |  |
|  |    ├─ cloud_aliyun.c/h      阿里云 IoT 平台            |  |
|  |    ├─ cloud_tencent.c/h     腾讯云 IoT Hub             |  |
|  |    └─ cloud_aws.c/h         AWS IoT Core               |  |
|  |                                                       |  |
|  |  入口层 (Entry Layer)                                   |  |
|  |    └─ gateway_init.c        模块注册 + 事件订阅总控     |  |
|  |                                                       |  |
|  |  公共定义                                               |  |
|  |    ├─ gateway_events.h/c    事件类型/通道初始化 helper  |  |
|  |    └─ gateway_config.h      模块公共配置宏             |  |
|  +-------------------------------------------------------+  |
|                             |                               |
|  +----------------v----------v---------------------------+  |
|  |  modules/zephyr_gateway/ — 树外模块（构建入口）         |  |
|  |    CMakeLists.txt  +  Kconfig                          |  |
|  +-------------------------------------------------------+  |
|                             |                               |
|  +----------------v----------v---------------------------+  |
|  |  framework/ — zephyr_framework 核心框架（子模块）       |  |
|  |    事件系统 / 模块管理器 / 系统服务 / Shell / ...        |  |
|  +-------------------------------------------------------+  |
+-------------------------------------------------------------+
```

### 1.2 设计原则

1. **事件驱动解耦**：业务模块之间不直接调用，**默认**通过 `event_publish` / `event_subscribe` 通信（见 §1.3 允许的例外）。
2. **统一模块模式**：所有业务模块遵循 `module_interface_t` + `SYS_INIT()` 自动注册
3. **Kconfig 条件编译**：每个模块独立开关，未启用时不编译不占空间
4. **跨平台同源**：业务代码 100% 共享，硬件差异通过设备树 overlay 隔离
5. **双层策略**：开源层完全公开，商业层功能用 `#ifdef CONFIG_USE_EVENT_SYSTEM_PRO` 包裹占位
6. **云平台 Provider 抽象**：`cloud_upload` 不感知具体云平台，通过 `cloud_provider_publish_all()` 向所有已注册 Provider 分发；各 Provider 独立管理连接、认证、Topic 映射

### 1.3 架构例外（允许的直接调用）

以下不属于「业务模块互调」，而是**协议栈适配层对外能力**，避免把大块 MQTT 载荷再拷贝进事件总线、并集中处理阻塞式网络 I/O：

| 调用方 | 被调用方 | 说明 |
|--------|----------|------|
| `cloud_upload` | `cloud_provider_publish_all()` | 向所有已注册 Provider 分发 JSON；全部失败才触发离线缓存 |
| `cloud_private` / `cloud_aliyun` / `cloud_tencent` / `cloud_aws` | `protocol_eth_mqtt_publish()` | 各 Provider 将格式化后的数据通过公共 MQTT 通道发送 |
| `cloud_private` / `cloud_aliyun` / `cloud_tencent` / `cloud_aws` | `protocol_eth_mqtt_set_auth()` | 各 Provider 在启动时设置 MQTT 认证参数（用户名/密码/证书） |
| `webshell`（可选） | `protocol_eth_mqtt_publish()` | 将 Shell 输出回写响应 topic |

**约定**：除上表外，业务模块之间仍禁止互相 `#include` 并直接调用对方 API；状态与数据仍以事件为主。

---

## 2. 事件系统设计

### 2.1 事件类型定义

```c
/* gateway_events.h — 与源码保持一致 */

/* 传感器与原始帧高频数据通过 data_bus 通道分发（详见 §2.3），不在
 * gateway_events.h 中定义事件类型。
 * framework 定义 EVENT_TYPE_SENSOR_DATA = 10，但 src/gateway 不发布该事件。 */

/* 异常检测输出 */
#define EVENT_TYPE_ANOMALY_WARNING  110   /* |x-μ|/σ ≥ warning_sigma（默认 2.0） */
#define EVENT_TYPE_ANOMALY_CRITICAL   111   /* |x-μ|/σ ≥ critical_sigma（默认 3.0） */
#define EVENT_TYPE_ANOMALY_EMERGENCY  112   /* |x-μ|/σ ≥ emergency_sigma（默认 4.0）或绝对上下限 */

/* 网络状态 */
#define EVENT_TYPE_CLOUD_CONNECTED    120
#define EVENT_TYPE_CLOUD_DISCONNECTED 121

/* 数据上云（离线缓存链路：发布待发送 JSON，由 offline_cache / cloud_upload 消费） */
#define EVENT_TYPE_CLOUD_UPLOAD     130
```

### 2.2 事件数据载荷

```c
/* 传感器数据点（CAN / Modbus 统一格式） */
typedef struct {
    uint32_t timestamp;      /* 采集时间戳：MVP 为 k_uptime_get_32()，单调毫秒 */
    uint8_t  channel_id;     /* 通道/从站 ID */
    uint8_t  sensor_type;    /* gateway_sensor_type_t：电流/温度/电压/... */
    float    value;          /* 物理量值 */
    uint16_t raw_u16;        /* 原始寄存器值（用于调试） */
} gateway_sensor_data_t;

/* 异常告警 */
typedef struct {
    uint32_t timestamp;
    uint8_t  sensor_type;
    uint8_t  level;          /* 0=Warning, 1=Critical, 2=Emergency（与实现一致） */
    float    current_value;
    float    baseline_mean;
    float    baseline_stddev;
    float    threshold_sigma;/* 触发时的 |x-μ|/σ */
} gateway_anomaly_event_t;

/* 云端上传数据 */
typedef struct {
    uint32_t timestamp;
    uint8_t  data_type;      /* 0=传感器, 1=异常, 2=心跳 */
    char     json_payload[192]; /* 轻量 JSON；超过事件 inline 容量时通过 ptr 承载（见 §2.4） */
} gateway_cloud_data_t;
```

### 2.3 数据流图

```
[data_simulator] ─┐
[protocol_can]    ├──gateway_sensor_publish()──▶ data_bus 通道 "sensor" ─┬─▶[anomaly_detection]
[protocol_modbus] ─┘                                                       └─▶[cloud_upload]
        │                                                                          │
        ├──gateway_can_raw_publish()    ──▶ data_bus 通道 "can_raw"      (诊断/调试可选订阅)
        └──gateway_modbus_raw_publish() ──▶ data_bus 通道 "modbus_raw"   (诊断/调试可选订阅)

[anomaly_detection] ──EVENT_TYPE_ANOMALY_*──▶[cloud_upload]
                                                  │
                                                  ├──(在线)──cloud_provider_publish_all()
                                                  │              │
                                                  │              ├─>[cloud_private]──MQTT──>私有Broker
                                                  │              ├─>[cloud_aliyun]──MQTT──>阿里云
                                                  │              ├─>[cloud_tencent]──MQTT──>腾讯云
                                                  │              └─>[cloud_aws]──MQTT──>AWS
                                                  │
                                                  └──(断网)──EVENT_TYPE_CLOUD_UPLOAD──>[offline_cache]
                                                                                          │(网络恢复)
                                                                                          └──EVENT_TYPE_CLOUD_UPLOAD──>[cloud_upload]

[protocol_eth] ──EVENT_TYPE_CLOUD_{CONNECTED,DISCONNECTED}──▶[offline_cache]
```

> **数据通路分工：**
> - 高频（sensor / CAN raw / Modbus raw）走 `data_bus`，零拷贝多消费者扇出。
> - 低频（异常、网络状态、上云）走 `event_system`，类型化订阅。
> - 通道初始化在 `SYS_INIT(APPLICATION, GATEWAY_INIT_PRIO_CHANNELS=68)` 阶段完成。


### 2.4 事件内联与大数据载荷

框架事件若支持 **inline**（例如约 48B）与 **指针/动态缓冲** 两种模式：

| 载荷 | 推荐方式 | 说明 |
|------|----------|------|
| `gateway_sensor_data_t` | inline | 体积极小，适合高频采集 |
| `gateway_anomaly_event_t` | inline 或 ptr | 视 `event_t` 内联容量而定 |
| `gateway_cloud_data_t`（含最长 192B JSON） | **ptr**：堆或静态池分配结构体，事件中置 `EVENT_FLAG_DATA_DYNAMIC`（或等价），发送完成后释放 | 与「48B inline」不矛盾：短 JSON 可 inline，**长 JSON 必须走 ptr**，文档与 `gateway_events.h` 注释一致 |

**时间语义**：MVP 时间戳以设备上电单调时钟为主；若需与云端/SCADA 对齐 wall-clock，须另行引入 SNTP/RTC（列入后续演进）。

---

## 3. 模块详细设计

### 3.1 protocol_can — CAN 数据采集

**职责**：通过 CAN 总线接收工业传感器数据，解析为标准传感器数据格式，发布事件。

**Zephyr API**：`can_set_mode()`, `can_add_rx_filter()`, `can_send()`

**配置（Kconfig）**：
```
CONFIG_GATEWAY_CAN_ENABLE=y
CONFIG_GATEWAY_CAN_DEVICE="can0"
CONFIG_GATEWAY_CAN_FILTER_ID=0x100
CONFIG_GATEWAY_CAN_FILTER_MASK=0x7F0
```

**线程模型**：
- 使用 Zephyr CAN 的 `can_add_rx_filter()` 回调（ISR 上下文）
- ISR 中将原始帧放入 ring buffer
- 独立线程从 ring buffer 取出，解析为 `gateway_sensor_data_t`，通过 `gateway_sensor_publish()` 发布到 data_bus 通道 "sensor"（原始帧另发布到 "can_raw"）

**线程生命周期**：线程终止使用状态标志 + `k_msleep` 等待，不调用 `k_thread_abort`（嵌入式上稳妥的停线程做法）。**说明**：这不等同于整机 SIL-2 认证；若需功能安全等级，须单独安全需求矩阵与验证。

### 3.2 protocol_modbus — Modbus RTU Master

**职责**：通过 UART/RS-485 轮询 Modbus 从站，读取保持寄存器，发布传感器数据。

**Zephyr API**：`uart_poll_out()`, `uart_irq_callback_user_data_set()`, `uart_fifo_read()`, GPIO API

**配置（Kconfig）**：
```
CONFIG_GATEWAY_MODBUS_ENABLE=y
CONFIG_GATEWAY_MODBUS_DEVICE="uart1"
CONFIG_GATEWAY_MODBUS_BAUDRATE=9600
CONFIG_GATEWAY_MODBUS_SLAVE_ID=1
CONFIG_GATEWAY_MODBUS_POLL_INTERVAL_MS=1000
```

**RS-485 方向控制**：
- 设备树中定义 `rs485_de` GPIO alias
- 发送前拉 high，发送完成后拉 low
- 使用 `k_usleep()` 等待发送完成（poll 模式）

**线程模型**：
- 独立线程周期性轮询（`k_sleep(K_MSEC(poll_interval))`）
- 发送请求 → 等待响应（带超时）→ 解析帧 → 通过 `gateway_sensor_publish()` 发布到 data_bus 通道 "sensor"（原始寄存器块另发布到 "modbus_raw"）

**Modbus 帧格式（简化）**：
```
请求:  [SLAVE_ID][0x03][ADDR_HI][ADDR_LO][COUNT_HI][COUNT_LO][CRC_LO][CRC_HI]
响应:  [SLAVE_ID][0x03][BYTE_COUNT][DATA...][CRC_LO][CRC_HI]
```

**错误与超时（MVP 须实现）**：
- 从站返回功能码最高位为 1 表示异常响应，解析异常码并记日志，不发布有效传感器事件。
- 响应超时：丢弃本轮，下一轮轮询继续；避免阻塞过长占用总线。
- MVP 单 `CONFIG_GATEWAY_MODBUS_SLAVE_ID`；多从站为 v1.x 扩展（轮询表 + Kconfig 或 NVS）。

**RS-485 时序**：高波特率或启用 UART DMA 时，应以「发送完成中断 / `uart_irq_tx_complete` 类回调」确认 DE 切换时机；`k_usleep` 仅作 poll 模式下的保守兜底，并在文档与板级验证中校准。

### 3.3 protocol_eth — 以太网/MQTT

**职责**：管理网络连接和 MQTT 会话，提供连接状态事件，为各 Cloud Provider 提供底层 MQTT 发送接口和认证参数设置。

**Zephyr API**：`net_if_up()`, `mqtt_connect()`, `mqtt_publish()`, `mqtt_input()`

**配置（Kconfig）**：
```
CONFIG_GATEWAY_MQTT_ENABLE=y
CONFIG_GATEWAY_MQTT_BROKER_ADDR="broker.emqx.io"
CONFIG_GATEWAY_MQTT_BROKER_PORT=1883
CONFIG_GATEWAY_MQTT_CLIENT_ID="zephyr_gateway_01"
```

**状态机**：
```
DISCONNECTED → CONNECTING → CONNECTED → SUBSCRIBED
      ↑              │           │            │
      └──────────────┴───────────┴────────────┘ (断网/错误触发重连)
```

**线程模型**：
- 独立线程调用 `mqtt_input()` 处理接收（订阅的下行消息）
- 使用 `net_mgmt` 回调监听网络接口状态变化
- 重连间隔指数退避（1s → 2s → 4s → ... → 60s 上限）

**对外接口**（非事件系统，直接函数调用）：
```c
int  protocol_eth_mqtt_publish(const char* topic, const char* payload, uint16_t payload_len);
bool protocol_eth_is_connected(void);
int  protocol_eth_mqtt_set_auth(const char* username, const char* password);
int  protocol_eth_mqtt_set_broker(const char* addr, uint16_t port);
```

**认证机制**：
- `protocol_eth_mqtt_set_auth()` 由 Cloud Provider 在 `start()` 阶段调用，设置 MQTT 连接的用户名和密码
- 认证信息存储在 `protocol_eth_cb_t` 中，在 `eth_mqtt_connect()` 时注入 `struct mqtt_client`
- 切换 Provider 时（如从阿里云切换到腾讯云），先 `stop()` 当前 Provider（清除认证），再 `start()` 新 Provider（设置新认证）

### 3.4 anomaly_detection — 自适应阈值异常检测

**职责**：订阅传感器数据，维护滑动窗口统计基线，检测偏离并发布分级异常事件。

**算法**：滑动窗口均值 + 标准差（与 `anomaly_detection.c` 一致）

```
窗口大小 W = 100（可配置）
对新样本 x（注意：实现中先按旧窗口做检测，再写入新值更新基线）:
  1. 若窗口未满或 σ < 0.001：不告警（基线未就绪）
  2. 否则计算 deviation = |x - μ|, sigma = deviation / σ
  3. 若 |x| 超过绝对上下限 → EMERGENCY
  4. 否则若 sigma ≥ emergency_sigma（默认 4.0）→ EMERGENCY
  5. 否则若 sigma ≥ critical_sigma（默认 3.0）→ CRITICAL
  6. 否则若 sigma ≥ warning_sigma（默认 2.0）→ WARNING
```

**说明**：`EVENT_TYPE_ANOMALY_*` 与 `gateway_anomaly_event_t.level` 对应 Warning / Critical / Emergency 三档；同一传感器有**最小告警间隔**（实现默认 5s）以防抖动洪泛。

**多维度联动**：
- MVP 为**每个 sensor_type 一个窗口**（非每个 `channel_id` 独立）；同类型多通道会共享基线，现场若需按通道基线，后续改为 `(sensor_type, channel_id)` 复合键。
- 配置联动规则：如 sensor_type 0（电流）+ sensor_type 1（温度）同时 Critical → 升级为 Emergency（实现见 `anomaly_check_multi_dim`）。

**配置（Kconfig）**：
```
CONFIG_GATEWAY_ANOMALY_ENABLE=y
CONFIG_GATEWAY_ANOMALY_WINDOW_SIZE=100
CONFIG_GATEWAY_ANOMALY_WARNING_SIGMA=20   /* 2.0 * 10 (fixed point) */
CONFIG_GATEWAY_ANOMALY_CRITICAL_SIGMA=30  /* 3.0 */
CONFIG_GATEWAY_ANOMALY_EMERGENCY_SIGMA=40 /* 4.0 */
```

**内存优化**：
- 使用固定窗口（循环缓冲区），不用动态分配
- 每个 sensor_type 预分配独立窗口（最多 4 个类型）
- 使用 `float` 而非 `double`；无 FPU 的 MCU（如部分 M4）依赖软浮点，须在目标板上做性能评估，必要时改为定点或缩小窗口。

### 3.5 cloud_upload — 数据上云

**职责**：通过 `data_bus` 通道 "sensor" 消费传感器数据，通过 `event_system` 订阅 `EVENT_TYPE_ANOMALY_*` 异常事件；格式化为 JSON，通过 `cloud_provider_publish_all()` 向所有已注册 Provider 分发。断网时将数据发布 `EVENT_TYPE_CLOUD_UPLOAD` 供 offline_cache 存储。

**JSON 格式（轻量）**：
```json
{"ts":1234567890,"ch":0,"type":0,"val":23.5}
{"ts":1234567890,"ch":0,"type":1,"lvl":2,"val":45.2,"mean":25.0,"std":5.0}
```

**上云流程**：
```
sensor/anomaly events → cloud_upload.c (JSON格式化 + 速率控制 + 离线缓存)
                              ↓
                    cloud_provider_publish_all() (向所有已注册Provider分发)
                              ↓
        ┌─────────┬──────────┼──────────┬─────────┐
        ↓         ↓          ↓          ↓         ↓
   cloud_private  cloud_aliyun  cloud_tencent  cloud_aws  ...
        ↑         ↑          ↑          ↑
   protocol_eth  protocol_eth  protocol_eth  protocol_eth + TLS
```

**断网处理**：
- `cloud_provider_publish_all()` 全部 Provider 失败时才触发离线缓存
- 发布 `EVENT_TYPE_CLOUD_UPLOAD`，`offline_cache` 订阅并存储
- 网络恢复后，`offline_cache` 重新发布存储的数据，`cloud_upload` 再次走 `cloud_provider_publish_all()` 分发

**背压与队列**：MQTT 阻塞或事件积压时，须有策略（降采样、合并上报、丢弃非关键样条或阻塞反压）；MVP 至少通过上报间隔与离线缓存上限避免 RAM 无限增长，具体阈值在板级联调中确定。

**配置（Kconfig）**：
```
CONFIG_GATEWAY_CLOUD_UPLOAD_ENABLE=y
CONFIG_GATEWAY_CLOUD_UPLOAD_INTERVAL_MS=5000  /* 正常上报间隔 */
```

### 3.5a cloud_provider — 云平台 Provider 注册表

**职责**：维护已注册 Cloud Provider 的指针数组，提供 `publish_all` 分发能力和状态查询接口。

**接口定义**：
```c
typedef enum {
    CLOUD_MSG_TELEMETRY = 0,
    CLOUD_MSG_ANOMALY,
} cloud_msg_type_t;

typedef struct {
    const char* name;
    int (*init)(void);
    int (*start)(void);
    int (*stop)(void);
    int (*shutdown)(void);
    bool (*is_connected)(void);
    int (*publish)(cloud_msg_type_t type, const char* json_payload);
    void (*print_status)(const struct shell* sh);
} cloud_provider_t;

int cloud_provider_register(const cloud_provider_t* provider);
int cloud_provider_publish_all(cloud_msg_type_t type, const char* json_payload);
uint8_t cloud_provider_get_count(void);
const cloud_provider_t* cloud_provider_get_by_index(uint8_t idx);
const char* cloud_provider_status_str(const cloud_provider_t* provider);
```

**分发语义**：
- `cloud_provider_publish_all()` 遍历所有已注册 Provider，逐个调用 `publish()`
- 任一 Provider 返回非零，`overall_ret` 记录错误，调用方（`cloud_upload`）据此触发离线缓存
- 加锁保护：使用 `k_mutex` 保护 Provider 数组的读写

**编译时选择**：
- `CONFIG_GATEWAY_CLOUD_PROVIDER_PRIVATE` — 私有 MQTT Broker（默认启用）
- `CONFIG_GATEWAY_CLOUD_PROVIDER_ALIYUN` — 阿里云 IoT 平台
- `CONFIG_GATEWAY_CLOUD_PROVIDER_TENCENT` — 腾讯云 IoT Hub
- `CONFIG_GATEWAY_CLOUD_PROVIDER_AWS` — AWS IoT Core
- 支持多选：同时启用多个 Provider 可实现多云并行/灾备

### 3.5b cloud_private — 私有 MQTT Broker

**职责**：对接私有/自部署 MQTT Broker，行为与重构前的 `cloud_upload` 直接发 MQTT 完全一致。

**Topic**：
- Telemetry: `CONFIG_GATEWAY_MQTT_TOPIC_TELEMETRY`
- Anomaly: `CONFIG_GATEWAY_MQTT_TOPIC_ANOMALY`

**认证**：无特殊认证（或依赖 `protocol_eth` 中配置的用户名密码）

**Payload**：轻量 JSON（`cloud_upload` 已格式化好的原始 JSON）

### 3.5c cloud_aliyun — 阿里云 IoT 平台

**职责**：对接阿里云物联网平台，支持一机一密认证、Alink JSON 格式、物模型 Topic。

**MQTT 连接参数**：
- Broker: `${productKey}.iot-as-mqtt.${region}.aliyuncs.com:1883`
- ClientId: `${clientId}|securemode=3,signmethod=hmacsha1|`
- Username: `${deviceName}&${productKey}`
- Password: `HMAC-SHA1(DeviceSecret, content)` → Hex（需 crypto 库）

**Topic**：`/sys/${productKey}/${deviceName}/thing/event/property/post`

**Payload**：Alink JSON 格式
```json
{"id":"12345","version":"1.0","params":{"Current":1.23},"method":"thing.event.property.post"}
```

**当前状态**：Password 使用 `DeviceSecret` 直接占位，生产环境需集成 mbedtls 计算 HMAC-SHA1 签名。

**配置（Kconfig）**：
```
CONFIG_GATEWAY_CLOUD_PROVIDER_ALIYUN=y
CONFIG_GATEWAY_ALIYUN_PRODUCT_KEY="a1Xxxxxx"
CONFIG_GATEWAY_ALIYUN_DEVICE_NAME="device_01"
CONFIG_GATEWAY_ALIYUN_DEVICE_SECRET=""
CONFIG_GATEWAY_ALIYUN_REGION="cn-shanghai"
```

### 3.5d cloud_tencent — 腾讯云 IoT Hub

**职责**：对接腾讯云物联网通信平台，支持密钥认证、腾讯云 Topic 格式。

**MQTT 连接参数**：
- Broker: `${productId}.iotcloud.tencentdevices.com:1883`
- ClientId: `${productId}${deviceName}`
- Username: `${productId}${deviceName};${sdkappid};${connid};${expiry}`
- Password: HMAC-SHA256 签名（需 crypto 库）

**Topic**：`${productId}/${deviceName}/event`

**当前状态**：Password 使用 `DeviceSecret` 直接占位，生产环境需集成 crypto 库计算 HMAC-SHA256 签名。

**配置（Kconfig）**：
```
CONFIG_GATEWAY_CLOUD_PROVIDER_TENCENT=y
CONFIG_GATEWAY_TENCENT_PRODUCT_ID="ProductID"
CONFIG_GATEWAY_TENCENT_DEVICE_NAME="device_01"
CONFIG_GATEWAY_TENCENT_DEVICE_SECRET=""
```

### 3.5e cloud_aws — AWS IoT Core

**职责**：对接 AWS IoT Core，支持 X.509 证书认证、Device Shadow Topic。

**MQTT 连接参数**：
- Broker: `${endpoint}-ats.iot.${region}.amazonaws.com:8883`
- 端口: 8883（TLS，必须启用 `CONFIG_NET_TLS` + `CONFIG_MBEDTLS`）
- 认证: X.509 客户端证书（双向 TLS）
- ClientId: `${thingName}`

**Topic**：`$aws/things/${thingName}/shadow/update`

**TLS 要求**：
- 启用 `CONFIG_NET_TLS=y`, `CONFIG_MBEDTLS=y`
- 加载证书链：`AmazonRootCA1.pem` + `device-cert.pem` + `private-key.pem`
- 配置 `mqtt_client` 的 transport 为 TLS

**当前状态**：未启用 TLS，仅做框架占位；生产环境必须配置 X.509 证书。

**配置（Kconfig）**：
```
CONFIG_GATEWAY_CLOUD_PROVIDER_AWS=y
CONFIG_GATEWAY_AWS_ENDPOINT="xxxxxx-ats.iot.us-east-1.amazonaws.com"
CONFIG_GATEWAY_AWS_THING_NAME="my_thing"
CONFIG_GATEWAY_AWS_REGION="us-east-1"
```

### 3.6 offline_cache — 断网续传本地缓存

**职责**：网络断开时缓存数据到 NVS，网络恢复后批量上报。

**存储模型**：
- 使用 Zephyr `NVS`（Non-Volatile Storage）API
- 环形缓冲区：固定条数，满时覆盖最旧数据
- 每条数据格式：`{timestamp, json_payload}`

**状态机**：
```
IDLE → CACHING (收到 CLOUD_DISCONNECTED) → RECOVERING (收到 CLOUD_CONNECTED) → IDLE
```

**批量上报**：
- 恢复后逐条读取 NVS，发布 `EVENT_TYPE_CLOUD_UPLOAD`
- `cloud_upload` 收到后通过 `cloud_provider_publish_all()` 分发到所有 Provider
- 发送成功后删除 NVS 中对应条目

**可靠性与幂等**：
- 单条发送失败：应保留 NVS 条目并重试（指数退避或下一轮恢复流程），避免「已丢数据」。
- 云端若按 `ts`+`ch` 去重，可在载荷中保留单调序号（后续 v1.x）；MVP 接受少量重复计数时须在文档/对接规范中声明。

**与实时数据优先级**：`RECOVERING` 状态可优先排空 NVS，或对实时采样限流，避免恢复瞬间洪峰占满 MQTT 与 CPU。

**配置（Kconfig）**：
```
CONFIG_GATEWAY_OFFLINE_CACHE_ENABLE=y
CONFIG_GATEWAY_OFFLINE_CACHE_MAX_ENTRIES=256
CONFIG_GATEWAY_OFFLINE_CACHE_ENTRY_SIZE=256
```

### 3.7 webshell — 远程 Shell 基础版

**职责**：扩展 framework Shell，添加工业网关专用命令，通过 MQTT 透传命令/响应。

**MVP 安全边界（必读）**：
- 明文 MQTT + 可订阅的 `gateway/cmd/<client_id>` **等价于未认证远程执行**，仅适用于内网调试或受控 broker（ACL 限制发布者）。
- 默认建议 **`CONFIG_GATEWAY_WEBSHELL_ENABLE=n`**，量产开启前必须：TLS、身份认证、命令白名单、速率限制、审计日志（与 §5 商业层路线图对齐）。

**Shell 命令**：
```
gateway status          — 显示所有模块状态
gateway can stats       — CAN 模块统计（接收帧数、错误数）
gateway modbus read <addr> <count>  — 手动读取 Modbus 寄存器
gateway anomaly config <sensor_type> <warning_sigma> <critical_sigma>  — 调整阈值
gateway cloud status    — 显示所有已注册 Provider 的连接状态和上传统计
gateway cache info      — 显示离线缓存条目数
```

**`gateway cloud status` 输出示例**：
```
=== 云端状态 ===
已注册 Provider 数: 2
  [私有 MQTT] 已连接
    Topic: gateway/telemetry
  [阿里云 IoT] 已连接
    Product:  a1Xxxxxx
    Device:   device_01
    Region:   cn-shanghai
    说明：Password 当前为 DeviceSecret 占位，生产环境需 HMAC-SHA1 签名
上传成功:  1234
上传失败:  5
缓存数据:  0
```

**MQTT 透传**：
- 订阅 MQTT topic：`gateway/cmd/<client_id>`
- 收到命令后本地执行，结果发布到 `gateway/resp/<client_id>`
- 为后续 Web 前端穿透做准备

---

## 4. 跨平台同源策略

### 4.1 设计目标

同一份业务代码（`src/gateway/` 下所有 `.c/.h`）在以下三个平台零修改编译运行：
- 野火 RT1052 Pro（NXP i.MX RT1052，ARM Cortex-M7）
- 野火 STM32H743 Pro（ST STM32H743，ARM Cortex-M7）
- Nucleo L4R5ZI（ST STM32L4R5，ARM Cortex-M4，验证用）

### 4.2 实现方式

**硬件抽象层**：Zephyr 设备树（DTS）

| 硬件资源 | 设备树 alias / chosen | 业务代码访问方式 |
|---------|----------------------|----------------|
| CAN | `can0` | `DEVICE_DT_GET(DT_ALIAS(can0))` |
| UART (485) | `rs485_uart` | `DEVICE_DT_GET(DT_ALIAS(rs485_uart))` |
| RS-485 DE GPIO | `rs485_de` | `GPIO_DT_SPEC_GET(DT_ALIAS(rs485_de), gpios)` |
| 以太网 | Zephyr 默认网络接口 | `net_if_get_default()` |

**Nucleo L4R5ZI 说明**：部分板卡无板载以太网；MVP 验证可采用以太网扩展板、WiFi 模块，或在该板上**仅验证 CAN + Modbus**（`CONFIG_GATEWAY_MQTT_ENABLE=n`），不因单一板型阻塞其余模块联调。

**设备树 overlay 文件**：
```
boards/
  rt1052_pro.overlay    — RT1052 硬件定义
  h743_pro.overlay      — H743 硬件定义
  nucleo_l4r5zi.overlay — L4R5ZI 补充定义（放在本项目，不覆盖 framework 中的）
```

**编译方式**：
```bash
# RT1052
west build -b mimxrt1050_fire -d build_rt1052 . -p always

# H743
west build -b stm32h743_pro -d build_h743 . -p always

# L4R5ZI（验证）
west build -b nucleo_l4r5zi -d build_l4 . -p always
```

---

## 5. 双层策略（开源层 + 商业层）

### 5.1 策略说明

本设计遵循 zephyr_framework 的双层 demo 策略：

| 维度 | 开源层 | 商业层 |
|---|---|---|
| 配置 | `CONFIG_USE_EVENT_SYSTEM_PRO=n` | `CONFIG_USE_EVENT_SYSTEM_PRO=y` + 链接 Pro .a |
| 代码可见性 | `src/gateway/` 完全开源 | Pro 模块源码不开源 |
| 异常检测 | 本地告警（屏幕打印） | 事件路由 + 速率限制 + 持久化 |
| 黑匣子 | 仅实时事件流 | 事件持久化 + 回放（`event_system_pro`） |
| OTA | MCUboot 基础引导 | A/B 槽 + ECDSA 签名 + 灰度（`ota_manager`） |
| WebShell | 基础命令 | RBAC + 审计日志 + TLS（`security_crypto`） |
| 多云管理 | 编译时静态选择 | 运行时动态切换 + 云间负载均衡 |

### 5.2 代码占位方式

商业层功能在开源代码中用条件编译包裹：

```c
/* anomaly_detection.c */
void anomaly_on_event(const event_t* event, void* user_data) {
    ...
    if (deviation > critical_threshold) {
        /* 开源层：本地告警 */
        printk("ANOMALY CRITICAL: sensor=%d value=%.2f\n", ...);

#ifdef CONFIG_USE_EVENT_SYSTEM_PRO
        /* 商业层：事件路由 + 速率限制 + 持久化 */
        event_system_pro_route_with_qos(&anomaly_event, QOS_GUARANTEED);
        event_system_pro_persist_event(&anomaly_event);
#endif
    }
}
```

---

## 6. 内存估算

### 6.1 静态内存（ROM + RAM）

| 组件 | 代码 (Flash) | 数据 (RAM) | 说明 |
|------|-------------|-----------|------|
| protocol_can | ~4 KB | ~1 KB | ring buffer + 解析状态 |
| protocol_modbus | ~6 KB | ~1.5 KB | UART buffer + 帧缓冲 |
| protocol_eth | ~8 KB | ~4 KB | MQTT 状态 + 网络缓冲 |
| cloud_provider | ~1 KB | ~0.2 KB | Provider 注册表（最多 4 个） |
| cloud_private | ~1 KB | ~0.1 KB | Topic 字符串缓冲 |
| cloud_aliyun | ~2 KB | ~0.3 KB | Alink JSON 包装 + 认证缓冲 |
| cloud_tencent | ~2 KB | ~0.3 KB | Topic 构造 + 认证缓冲 |
| cloud_aws | ~2 KB | ~0.2 KB | Shadow Topic 构造 |
| anomaly_detection | ~4 KB | ~2 KB | 4 窗口 × 100 float = 1.6KB |
| cloud_upload | ~4 KB | ~1 KB | JSON 缓冲 |
| offline_cache | ~4 KB | ~1 KB | NVS 元数据 |
| webshell | ~3 KB | ~0.5 KB | Shell 命令表 |
| **小计** | **~41 KB** | **~12 KB** | |
| Zephyr 网络栈 | ~80 KB | ~20 KB | 依赖 Kconfig |
| Zephyr CAN | ~10 KB | ~2 KB | |
| **总计（含 Zephyr）** | **~131 KB** | **~34 KB** | |

### 6.2 运行时线程

| 模块 | 线程数 | 栈大小 | 优先级 |
|------|--------|--------|--------|
| protocol_can | 1 | 2048 | 5 |
| protocol_modbus | 1 | 2048 | 5 |
| protocol_eth | 1 | 4096 | 5 |
| anomaly_detection | 0（事件回调） | — | — |
| cloud_upload | 0（事件回调） | — | — |
| offline_cache | 1 | 2048 | 6 |

---

## 7. 编译与运行

### 7.1 依赖

```bash
# 初始化子模块
git submodule update --init --recursive

# 配置 Zephyr 环境（复制模板并编辑）
cp framework/zephyr_config.env.template framework/zephyr_config.env
# 编辑 ZEPHYR_BASE 等路径
```

### 7.2 编译

```bash
# 默认板子（Nucleo L4R5ZI）
west build -b nucleo_l4r5zi -d build . -p always

# RT1052
west build -b mimxrt1050_fire -d build_rt1052 . -p always

# H743
west build -b stm32h743_pro -d build_h743 . -p always
```

**多 Provider 编译示例**：
```bash
# 私有云 + 阿里云 并行
west build -b nucleo_l4r5zi -d build . -p always \
  -- -DCONFIG_GATEWAY_CLOUD_PROVIDER_PRIVATE=y \
     -DCONFIG_GATEWAY_CLOUD_PROVIDER_ALIYUN=y

# 仅腾讯云
west build -b nucleo_l4r5zi -d build . -p always \
  -- -DCONFIG_GATEWAY_CLOUD_PROVIDER_PRIVATE=n \
     -DCONFIG_GATEWAY_CLOUD_PROVIDER_TENCENT=y
```

### 7.3 运行验证

1. **模块注册**：Shell 中输入 `module list`，确认所有网关模块已注册
2. **CAN 数据**：Shell 中 `gateway can stats` 查看接收计数
3. **Modbus 轮询**：Shell 中 `gateway modbus read 0 10` 手动读取
4. **异常检测**：观察传感器数据偏离时的事件输出
5. **MQTT 上云**：Shell 中 `gateway cloud status` 查看所有 Provider 连接状态
6. **多云并行**：同时启用多个 Provider，确认各平台均收到数据
7. **断网续传**：断开网络，确认数据缓存；恢复后确认所有 Provider 均收到重发数据

---

## 8. 后续演进路线

| 阶段 | 内容 | 时间 |
|------|------|------|
| v1.0（当前） | 基础 MVP：多协议 + 异常检测 + 上云 + 断网续传 | W1-W8 |
| v1.1 | 云平台 Provider 模块化：阿里云/腾讯云/AWS/私有云（已完成） | W8 |
| v1.2 | HMAC 签名实现：集成 mbedtls 完成阿里云 HMAC-SHA1、腾讯云 HMAC-SHA256 | W9 |
| v1.3 | AWS TLS + 证书链加载：启用 CONFIG_NET_TLS，支持 X.509 mTLS | W10 |
| v1.4 | 黑匣子事件回放（商业层） | W11 |
| v1.5 | 企业级 OTA（A/B 槽 + 签名） | W12 |
| v1.6 | 远程 Web Shell 增强版（RBAC + 审计） | W13 |
| v2.0 | LoRa Mesh 扩展 + 多网关协同 | 待定 |
