# zephyr_gateway

基于 [zephyr_framework](https://github.com/QZehao/zephyr_framework) 的业务仓库：**`framework/` 子模块可与上游完全一致**（不改其中任何已跟踪文件）；产品代码放在仓库根目录 **`src/`**，通过 Zephyr **树外模块** `modules/zephyr_gateway/` 编进固件。

## 目录约定

| 路径 | 说明 |
|------|------|
| `framework/` | 上游 `zephyr_framework` 子模块；保持 `git clean` 与上游一致即可 |
| `src/` | 业务源码（示例：`src/gateway/`） |
| `modules/zephyr_gateway/` | Zephyr 树外模块（`zephyr/module.yml` + `CMakeLists.txt` + `Kconfig`） |
| `gateway_prj.conf` | 业务 `CONFIG_*`（含 `CONFIG_ZEPHYR_GATEWAY_BUSINESS`） |
| `env.example` | 将 `ZEPHYR_EXTRA_MODULES` 写进 **`framework/zephyr_config.env`** 的示例（该 env 文件被 gitignore） |

## 原理（为何不碰框架文件）

Zephyr 支持环境变量 **`ZEPHYR_EXTRA_MODULES`**：指向若干模块根目录后，构建 **`framework/` 应用**时也会编译这些模块。  
把 `ZEPHYR_EXTRA_MODULES` 设在 **`framework/zephyr_config.env`**（由模板复制，**不提交**）或系统环境里即可，**无需**改 `framework` 里任何已跟踪文件。

## 初始化

```bash
git clone <你的业务仓库 URL> zephyr_gateway
cd zephyr_gateway
git submodule update --init --recursive
```

按框架文档复制 `framework/zephyr_config.env.template` → `framework/zephyr_config.env` 并填写 `ZEPHYR_BASE`、SDK 等；再按 **`env.example`** 增加一行 **`ZEPHYR_EXTRA_MODULES`**，指向本仓库的 `modules/zephyr_gateway`（建议绝对路径）。

## 构建

应用 CMake 入口始终在 **`framework/`**（与 Zephyr 独立应用约定一致，Windows 下也避免主机库误链）。

**方式一（推荐，PowerShell）**，在仓库根：

```powershell
.\scripts\build.ps1 -Board <你的板型>
```

脚本会设置 `ZEPHYR_EXTRA_MODULES` 并合并 `prj.conf` + `../gateway_prj.conf`。

**方式二（手动）**，在仓库根先设置模块路径，再指定合并配置：

```powershell
$env:ZEPHYR_EXTRA_MODULES = "$PWD\modules\zephyr_gateway"
west build -b <你的板型> framework -- "-DCONF_FILE=prj.conf;../gateway_prj.conf"
```

（在 `framework` 为应用源码目录时，`../gateway_prj.conf` 即仓库根的 `gateway_prj.conf`。）

## 业务入口说明

不要在 `src/` 再实现第二个 `main()`；框架已提供应用入口。示例使用 `SYS_INIT`（见 `src/gateway/gateway_init.c`）；关闭业务编译可将 `gateway_prj.conf` 中的 `CONFIG_ZEPHYR_GATEWAY_BUSINESS` 设为 `n` 或删除该项。

## 若坚持「根目录 west build .」

需要顶层 `CMakeLists.txt` 聚合子目录，且历史上在 Windows 上易踩交叉编译与路径问题；当前仓库**刻意不提供**根目录应用 CMake，以保持 **`framework` 零修改**与构建行为简单可预期。
