# AGENTS.md

本文件为 AI 代理（如 Claude Code）提供项目导航信息。

## 项目概述

Zygisk 模块，在 QQ / TIM 进程内 hook 框架层 `android.app.NotificationManager.notify`，把 QQ 不符合 Android 规范的通知解析后重建为标准 `MessagingStyle` 会话样式（多会话 / 分渠道 / 带头像，兼容 WearOS）。不依赖 LSPlant / Xposed，不被 QQ 的框架检测识别。

## 技术栈

- **语言**：C++17（原生层）+ Shell（构建）+ 纯 HTML/CSS/JS（WebUI）
- **框架**：Zygisk API v4
- **依赖**：仅 Android 系统库（`log`、`dl`），无第三方依赖
- **构建**：CMake + Android NDK
- **CI**：GitHub Actions（smoke 构建测试 + tag 发布）

## 核心技术

### ArtMethod 直接修改（非框架 hook）

不使用 LSPlant、Dobby、Xposed 等 hook 框架。原因：QQ 会检测这些框架的痕迹。

具体做法：
1. 将目标方法的 `access_flags_` 设置 `kAccNative` 标志位
2. 将 `entry_point_from_compiled_code_` 设置为 `art_quick_generic_jni_trampoline`（libart.so 标准 JNI 桥接函数）
3. 将 `data_`（JNI 入口点）设置为我们的 native 函数指针

关键：`entry_point` 指向 libart.so 中的标准函数（非匿名 RWX 内存），不会触发 QQ 对 hook 框架的检测。

### ArtMethod 偏移运行时确定

通过 `java.lang.Object` 的相邻方法指针差值确定 `sizeof(ArtMethod)`，进而推算 `data_` 和 `entry_point` 偏移。`access_flags_` 固定在偏移 4。从 `Object.hashCode()`（native 方法）的 entry_point 直接读出 JNI trampoline。

### 框架层单点 Hook

hook `android.app.NotificationManager.notify` 两个重载（`notify(String,int,Notification)` 与 `notify(int,Notification)`），与 QQ 版本无关。`notify(int,Notification)` 实现为 `notify(null,id,notif)`，故所有原始投递最终汇聚到 3 参重载。

### 临时还原调原法

hook 体内需要投递通知时：临时还原 3 参 `notify` 的 ArtMethod 三字段 → 调用原始字节码投递 → 重新 hook。配合 **全局互斥锁**（`g_hook_mtx`，串行化整个 hook 体）与 **线程本地重入守卫**（`g_in_hook`，防止调原法时重入二次解析），消除并发 / 重入竞态。

### 解析器（纯 C++，可 host 单测）

`native/src/parser.{hpp,cpp}` 移植自 `QQ-Notify-Evolution` 的 `QQNotificationResolver.kt`（正则与判定优先级逐条对齐，含中文）。命名组改写为编号组（`std::regex` 不支持命名组）。无 Android 依赖，可用 `g++` 直接编译 host 单测（`native/test/parser_test.cpp`，覆盖 QNE 全部用例）。

### 失败永不丢通知

解析为 None / Hidden，或重建过程任意异常，一律回退到投递原通知。投递使用原 tag/id，QQ 的取消 / 更新逻辑不受影响。

### 反检测

非 QQ / TIM 进程调用 `setOption(DLCLOSE_MODULE_LIBRARY)` 卸载 `.so`；`entry_point` 指向 libart 标准函数。

## 架构

```
native/
├── zygisk.hpp          # Zygisk API v4 头文件（官方，不得修改）
├── CMakeLists.txt      # CMake 构建配置
└── src/
    ├── module.cpp      # Zygisk 入口 + ArtMethod hook + 临时还原调原法 + 通知重建 JNI
    ├── parser.hpp      # 解析器接口（纯 C++）
    └── parser.cpp      # 解析器实现（移植自 QNE）
native/test/
└── parser_test.cpp     # host 单测（g++ 编译，QNE 用例）

module/
├── module.prop         # 模块元数据（含 updateJson OTA 地址）
├── customize.sh        # 安装时输出信息 + root 方案探测
└── webroot/
    └── index.html      # KernelSU/APatch WebUI（Material 3 状态展示，无开关）

build.sh                # 一键构建：CMake 双 ABI 编译 → 模块打包（含 webroot）
update.json             # OTA 元数据（手动维护）
.github/workflows/
├── smoke.yml           # CI：push/PR 触发构建 + host 解析单测（不发布）
└── release.yml         # CI：tag push 自动构建并发布 Release
```

## 构建

```bash
export ANDROID_NDK_HOME=/path/to/ndk
./build.sh
```

Host 解析单测（无需 NDK / 真机）：

```bash
g++ -std=c++17 native/src/parser.cpp native/test/parser_test.cpp -o pt && ./pt
```

CI：推送 `v*` tag 触发 `release.yml` 自动构建并创建 Release；push / PR 触发 `smoke.yml` 验证双 ABI `.so` 产出与解析单测。

## OTA 更新机制

`module.prop` 中的 `updateJson` 指向仓库中的 `update.json`（GitHub raw）。root 管理器定期请求此 JSON 比对 `versionCode`。发布新版本后需手动更新 `update.json` 并提交到 main 分支。不使用 Action 自动更新此 JSON。

## 关键约定

- `所有 commit message 必须使用英文`
- **不使用** LSPosed / Xposed / LSPlant 等 hook 框架（QQ 检测）
- **不使用** Dobby / inline hook（减少检测面）
- Hook 点为框架层 `NotificationManager.notify`（非 QQ 内部类，与版本无关）
- 临时还原调原法全程在全局锁 + 线程本地重入守卫内
- 失败永不丢通知（异常 / 未知类型一律回退原通知）
- 投递复用原 tag/id，QQ 取消 / 更新逻辑不受影响
- 目标进程仅 `com.tencent.mobileqq` 与 `com.tencent.tim`
- `entry_point` 指向 libart.so 标准函数，非匿名 RWX 内存
- 非 QQ / TIM 进程 `setOption(DLCLOSE_MODULE_LIBRARY)` 卸载 .so
- v1 不含直接回复 / 气泡功能（会话 shortcut 仍做）
- 致谢段必须点名 `ichenhe/QQ-Notify-Evolution` 与 `cinit/QAuxiliary`
