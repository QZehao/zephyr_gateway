# zephyr_gateway

基于 [zephyr_framework](https://github.com/QZehao/zephyr_framework) 的业务仓库：`framework/` 为子模块；业务在 **`src/`**，经树外模块 **`modules/zephyr_gateway/`** 编入固件。

## `framework/CMakeLists.txt` 的改动（兼容两种用法）

- **`FW_APP_ROOT`**：始终等于本 `CMakeLists.txt` 所在目录。单独 `west build … framework` 时与原先 `CMAKE_SOURCE_DIR` 等价；被仓库根 `add_subdirectory(framework)` 时仍指向 `framework/` 内的 `src` 与生成头路径。
- **`ZEPHYR_GATEWAY_TOPLEVEL_BOOTSTRAP`**：仅当仓库根已执行 `find_package(Zephyr)` + `project()` 时，在子目录内跳过重复引导（避免 Windows 下误链主机库）。**仅克隆 framework、无外部根 CMake 时该变量未定义，行为与上游一致。**

升级子模块后若冲突，请保留或重新套用上述片段。

## 初始化

```bash
git submodule update --init --recursive
```

复制 `framework/zephyr_config.env.template` → `framework/zephyr_config.env` 并填写 `ZEPHYR_BASE` 等。`ZEPHYR_EXTRA_MODULES` 可由根 `CMakeLists.txt` 在未设置时默认指向 `modules/zephyr_gateway`；也可按 **`env.example`** 写进 `zephyr_config.env`。

## 构建

### 在仓库根（`west build … .`）

根目录 **`Kconfig`** 会 `rsource "framework/Kconfig"`（路径相对应用根；勿用 `source`，否则会按 Zephyr 的 `$srctree` 解析），否则 `framework/prj.conf` 里的选项在 Kconfig 阶段全是未定义符号。根目录 **`CMakeLists.txt`** 会合并 `framework/prj.conf` + `gateway_prj.conf`，附加 `framework/app.overlay`（若存在），并 `add_subdirectory(framework)`：

```powershell
west build -b nucleo_l4r5zi -d build . -p always
```

### 仅子模块、无外部仓库（`west build … framework`）

与上游用法相同，**不需要**设置 `ZEPHYR_GATEWAY_TOPLEVEL_BOOTSTRAP`：

```powershell
west build -b nucleo_l4r5zi -d build framework -p always
```

若需业务模块与 `gateway_prj.conf`，仍要设置 `ZEPHYR_EXTRA_MODULES` 并传 `CONF_FILE`（见 `env.example` / `scripts/build.ps1 -SourceDir framework`）。

### 脚本

```powershell
.\scripts\build.ps1 -Board nucleo_l4r5zi
```

默认 `-SourceDir .`。仅编框架时：`.\scripts\build.ps1 -Board nucleo_l4r5zi -SourceDir framework`。

## 业务入口

不要在 `src/` 再实现第二个 `main()`；示例使用 `SYS_INIT`（`src/gateway/gateway_init.c`）。
