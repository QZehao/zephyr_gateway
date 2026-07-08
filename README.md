# Zephyr 工业边缘网关

基于 [zephyr_framework](https://github.com/QZehao/zephyr_framework) 的工业边缘网关参考设计。支持 CAN/485/Modbus/以太网多工业协议、自适应阈值异常检测、多云平台数据上云、断网续传。

| 项 | 值 |
|---|---|
| **版本** | v2.0 |
| **目标硬件** | 野火 RT1052 Pro / STM32H743 Pro / Nucleo L4R5ZI（验证） |
| **开源许可** | GPL-3.0（开源层）+ 商业模块授权（Pro 层） |

---

## 架构概览

```
src/gateway/
├── gateway_init.c          # 业务入口：事件订阅总控
├── gateway_events.h/c      # 事件类型定义 + JSON 格式化
├── gateway_config.h        # 模块公共配置 + SYS_INIT 优先级
├── protocol_can.c/h        # CAN 总线数据采集
├── protocol_modbus.c/h     # Modbus RTU Master (UART/485)
├── network_manager.c/h     # 网络链路状态管理
├── protocol_mqtt.c/h       # MQTT 连接管理 + 认证接口
├── anomaly_detection.c/h   # 滑动窗口自适应阈值异常检测
├── cloud_upload.c/h        # 云数据上传业务（JSON格式化 + 速率控制 + 离线缓存触发）
├── cloud_provider.c/h      # 云平台抽象层（Provider 注册表 + 多播分发）
├── cloud_private.c/h       # 私有 MQTT Broker Provider
├── cloud_aliyun.c/h        # 阿里云 IoT 平台 Provider
├── cloud_tencent.c/h       # 腾讯云 IoT Hub Provider
├── cloud_aws.c/h           # AWS IoT Core Provider
├── offline_cache.c/h       # 断网续传本地缓存 (NVS)
└── webshell.c/h            # Shell 命令扩展
```

### 事件流

> sensor 数据流（含 CAN/Modbus 原始帧）已迁移到 `data_bus`（详见
> `docs/industrial_gateway_design.md`）；event_system 仅承载告警与
> 网络状态等低频事件。

```
data_simulator / protocol_can / protocol_modbus
        │
        │ gateway_sensor_publish()
        ▼
data_bus 通道 "sensor" ──┬──> anomaly_detection
                          │          │
                          │          ├──EVENT_TYPE_ANOMALY_*──> cloud_upload
                          │                                   │
                          │                                   ├──(JSON格式化 + 速率控制)
                          │                                   │
                          │                                   └──> cloud_provider_publish_all()
                          │                                               │
                          │                   ┌───────────────────────────┼───────────────────────────┐
                          │                   ↓                           ↓                           ↓
                          │            cloud_private              cloud_aliyun              cloud_tencent
                          │                   │                           │                           │
                          │                   └──> protocol_mqtt     protocol_mqtt            protocol_mqtt
                          │
                          └──> cloud_upload (旁路订阅)
                                       │
                          (任一 Provider 失败) ──> cloud_handle_offline() ──> offline_cache
                                                                                  │
                                                                                  │(网络恢复)
                                                                                  └──> EVENT_TYPE_CLOUD_UPLOAD ──> cloud_upload (重发)
```


### 多云平台支持

通过 `cloud_provider` 抽象层，支持**单云接入**或**多云并行**：

| 平台 | 认证方式 | 特殊需求 | Kconfig 开关 |
|---|---|---|---|
| 私有 MQTT Broker | 用户名/密码或无 | 无 | `CONFIG_GATEWAY_CLOUD_PROVIDER_PRIVATE` |
| 阿里云 IoT | 一机一密 HMAC-SHA1 | Alink JSON 格式 | `CONFIG_GATEWAY_CLOUD_PROVIDER_ALIYUN` |
| 腾讯云 IoT Hub | 密钥 HMAC-SHA256 | 设备证书 | `CONFIG_GATEWAY_CLOUD_PROVIDER_TENCENT` |
| AWS IoT Core | X.509 证书 mTLS | TLS 8883 + 证书链 | `CONFIG_GATEWAY_CLOUD_PROVIDER_AWS` |

**当前限制**：所有 Provider 共享 `protocol_mqtt` 的单一 MQTT 连接（轮流设置认证参数）。如需真正多 Broker 并行，每个 Provider 需独立管理 `mqtt_client` 实例。

---

## 初始化

```bash
git submodule update --init --recursive
```

复制 `framework/zephyr_config.env.template` → `framework/zephyr_config.env` 并填写 `ZEPHYR_BASE` 等。

---

## 编译

### Nucleo L4R5ZI（验证用）

```powershell
west build -b nucleo_l4r5zi -d build . -p always
```

### 野火 RT1052 Pro

```powershell
west build -b mimxrt1050_fire -d build_rt1052 . -p always
```

### 野火 STM32H743 Pro

```powershell
west build -b stm32h743_pro -d build_h743 . -p always
```

### 多云平台编译示例

```powershell
# 仅私有 MQTT（默认）
west build -b nucleo_l4r5zi -d build .

# 阿里云单云
west build -b nucleo_l4r5zi -d build . -DCONFIG_GATEWAY_CLOUD_PROVIDER_PRIVATE=n -DCONFIG_GATEWAY_CLOUD_PROVIDER_ALIYUN=y

# 私有云 + 阿里云 + 腾讯云 并行
west build -b nucleo_l4r5zi -d build . -DCONFIG_GATEWAY_CLOUD_PROVIDER_PRIVATE=y -DCONFIG_GATEWAY_CLOUD_PROVIDER_ALIYUN=y -DCONFIG_GATEWAY_CLOUD_PROVIDER_TENCENT=y
```

### PowerShell 脚本

```powershell
.\scripts\build.ps1 -Board nucleo_l4r5zi
.\scripts\build.ps1 -Board esp32c6_devkitm -BuildDir build_esp32c6
```

---

## Shell 命令

| 命令 | 说明 |
|---|---|
| `gateway status` | 显示所有模块状态 |
| `can stats` | CAN 接收/发送/错误统计 |
| `modbus read <addr> <count>` | 手动读取 Modbus 保持寄存器 |
| `anomaly config <sensor> <w> <c> [e]` | 配置异常检测阈值 |
| `cloud status` | 所有已启用云平台的连接状态 |
| `cache info` | 离线缓存条目数 |
| `cache clear` | 清空离线缓存 |
| `module list` | 列出所有已注册模块及其依赖 |

---

## 双层策略

本设计采用开源层 + 商业层的双层 demo 策略：

| 功能 | 开源层 | 商业层（Pro） |
|---|---|---|
| 异常检测 | 本地告警（屏幕打印） | 事件路由 + 速率限制 + 持久化 |
| 黑匣子 | 仅实时事件流 | 事件持久化 + 回放 |
| OTA | MCUboot 基础引导 | A/B 槽 + ECDSA 签名 + 灰度 |
| WebShell | 基础命令 | RBAC + 审计日志 + TLS |
| 多云接入 | 基础 MQTT + 阿里云 | 全平台适配 + 自动容灾 + 流量计费 |

商业层功能在代码中用 `#ifdef CONFIG_USE_EVENT_SYSTEM_PRO` 包裹，升级时应用代码零修改。

---

## 详细设计

见 `docs/industrial_gateway_design.md`。

---

## 与 framework 的关系

- `framework/` 为子模块，不动核心代码
- `src/gateway/` 为业务代码，完全开源
- `boards/` 为本项目设备树 overlay，不覆盖 framework 中的文件
