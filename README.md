# Zephyr 工业边缘网关

基于 [zephyr_framework](https://github.com/QZehao/zephyr_framework) 的工业边缘网关参考设计。支持 CAN/485/Modbus/以太网多工业协议、自适应阈值异常检测、MQTT 数据上云、断网续传。

| 项 | 值 |
|---|---|
| **版本** | v1.0 |
| **目标硬件** | 野火 RT1052 Pro / STM32H743 Pro / Nucleo L4R5ZI（验证） |
| **开源许可** | GPL-3.0（开源层）+ 商业模块授权（Pro 层） |

---

## 架构概览

```
src/gateway/
├── gateway_init.c         # 业务入口：事件订阅总控
├── gateway_events.h/c     # 事件类型定义 + JSON 格式化
├── gateway_config.h       # 模块公共配置
├── protocol_can.c/h       # CAN 总线数据采集
├── protocol_modbus.c/h    # Modbus RTU Master (UART/485)
├── protocol_eth.c/h       # 以太网/MQTT 连接管理
├── anomaly_detection.c/h  # 滑动窗口自适应阈值异常检测
├── cloud_upload.c/h       # MQTT 数据上云
├── offline_cache.c/h      # 断网续传本地缓存 (NVS)
└── webshell.c/h           # Shell 命令扩展
```

### 事件流

```
CAN/Modbus 数据 ──EVENT_TYPE_SENSOR_DATA──┐
                                           ├──> anomaly_detection
                                           │          │
                                           │          ├──EVENT_TYPE_ANOMALY_*──> cloud_upload
                                           │                                   │
                                           │                                   ├──(在线)──MQTT──>云端
                                           │                                   │
                                           └───────────────────────────────────┼─(断网)──EVENT_TYPE_CLOUD_UPLOAD──> offline_cache
                                                                               │                                      │
                                                                               │                                      │(恢复)
                                                                               │                                      └──EVENT_TYPE_CLOUD_UPLOAD──> cloud_upload
```

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

### PowerShell 脚本

```powershell
.\scripts\build.ps1 -Board nucleo_l4r5zi
```

---

## Shell 命令

| 命令 | 说明 |
|---|---|
| `gateway status` | 显示所有模块状态 |
| `can stats` | CAN 接收/发送/错误统计 |
| `modbus read <addr> <count>` | 手动读取 Modbus 保持寄存器 |
| `anomaly config <sensor> <w> <c> [e]` | 配置异常检测阈值 |
| `cloud status` | MQTT 连接状态和上传统计 |
| `cache info` | 离线缓存条目数 |
| `cache clear` | 清空离线缓存 |
| `module list` | 列出所有已注册模块 |

---

## 双层策略

本设计采用开源层 + 商业层的双层 demo 策略：

| 功能 | 开源层 | 商业层（Pro） |
|---|---|---|
| 异常检测 | 本地告警（屏幕打印） | 事件路由 + 速率限制 + 持久化 |
| 黑匣子 | 仅实时事件流 | 事件持久化 + 回放 |
| OTA | MCUboot 基础引导 | A/B 槽 + ECDSA 签名 + 灰度 |
| WebShell | 基础命令 | RBAC + 审计日志 + TLS |

商业层功能在代码中用 `#ifdef CONFIG_USE_EVENT_SYSTEM_PRO` 包裹，升级时应用代码零修改。

---

## 详细设计

见 `docs/industrial_gateway_design.md`。

---

## 与 framework 的关系

- `framework/` 为子模块，不动核心代码
- `src/gateway/` 为业务代码，完全开源
- `boards/` 为本项目设备树 overlay，不覆盖 framework 中的文件
