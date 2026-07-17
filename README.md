# QQ Notify Evolution

通过 Zygisk 将 QQ 不符合 Android 规范的通知改写为标准的 **MessagingStyle** 会话样式（多会话 / 分渠道 / 带头像，兼容 WearOS 手表显示）。不依赖 LSPlant / Xposed / LSPosed，不被 QQ 的框架检测识别。

## 效果

刷入并重启后，QQ 来消息时系统通知栏显示标准会话通知：

- 多会话独立分组，带头像
- 私聊 / 群聊 / 特别关心 / QQ 空间动态分别走独立通知渠道
- 同一会话内多条消息折叠展示（会话历史）
- 输出本身即为标准 MessagingStyle，配对的 WearOS 手表自动正常显示
- 点击跳转、划掉行为与原通知一致（复用 QQ 的 contentIntent / deleteIntent）

## 环境要求

- 已 root 的 Android 设备
- **任意一种 Zygisk 实现**（支持多种组合）：
  - Magisk（内置 Zygisk）
  - KernelSU + ZygiskNext / ReZygisk
  - APatch + ZygiskNext / ReZygisk
- Android 8.0（API 26）及以上（API 28+ 为完整功能基准）
- QQ（`com.tencent.mobileqq`）或 TIM（`com.tencent.tim`）

## 安装

1. 下载最新版模块 zip（从 [Releases](../../releases) 页面）
2. 在 root 管理器中刷入该 zip
3. 重启设备
4. 打开 QQ，让他人给你发消息（私聊 / 群聊均可）验证效果

## 使用

安装即生效，**无可开关项**。模块禁用并重启即可恢复 QQ 原始通知。

> v1 仅做通知改写展示，不含通知栏直接回复 / 气泡（Bubble）功能。

## 更新

在 root 管理器中检查更新即可。本模块通过 `updateJson` 机制实现 OTA，有新版本时会自动提示。

## 构建

### 前置要求

- Android NDK r25+
- CMake 3.18+
- `zip`、`cmake`

### 步骤

```bash
export ANDROID_NDK_HOME=/path/to/ndk
chmod +x build.sh
./build.sh
```

生成的 zip 位于 `build/` 目录。

也可通过 GitHub Actions 自动构建：推送 `v*` 格式的 tag 即可触发发布 Release。

## 技术原理

1. **Zygisk 注入**：模块由 Zygisk 加载到 QQ 进程内；非 QQ / TIM 进程调用 `setOption(DLCLOSE_MODULE_LIBRARY)` 卸载自身 `.so`。

2. **框架层单点 Hook**：直接 hook `android.app.NotificationManager.notify` 的两个重载（与 QQ 版本无关）。这比 hook QQ 内部 `NotificationFacade` 更稳定。

3. **直接改 ArtMethod（非框架 hook）**：将目标方法的 `access_flags_` 置 `kAccNative`、`entry_point` 指向 libart 标准的 `art_quick_generic_jni_trampoline`、`data_` 指向我们的 native 函数。`entry_point` 指向 libart 标准函数（非匿名 RWX 内存），不会触发 QQ 对 hook 框架的检测。

4. **临时还原调原法**：hook 内部需要投递通知时，临时把 ArtMethod 三字段还原为原始值、调用原始 `notify(String,int,Notification)` 字节码、再重新 hook。全程在全局互斥锁 + 线程本地重入守卫内，消除并发 / 重入竞态。

5. **解析 + 重建**：从原通知的 title/ticker/content 解析出消息类型（私聊 / 群聊 / 特别关心 / QQ 空间 / 关联账号），用 `Notification.MessagingStyle` + `Person` 重建标准会话通知。

6. **失败永不丢通知**：解析为未知 / 隐藏消息，或重建过程任意异常，一律回退到投递原通知。投递使用原 tag/id，QQ 的取消 / 更新逻辑不受影响。

## 致谢

本项目参考并移植了以下项目的核心逻辑，在此致谢：

- [QQ-Notify-Evolution](https://github.com/ichenhe/QQ-Notify-Evolution)（@ichenhe）— 通知解析正则与判定逻辑
- [QAuxiliary](https://github.com/cinit/QAuxiliary)（@cinit）— 通知渠道与会话样式构建模式

## 免责声明

- 本项目仅供个人学习和研究使用
- 本项目不隶属于腾讯公司，也未获得腾讯公司的授权或认可
- "QQ" 和 "腾讯" 是腾讯公司的商标
- 使用本模块可能导致 QQ 账号风险，用户需自行承担一切后果
- 本项目不保证任何明示或暗示的功能适用性
- 如果本项目侵犯了您的合法权益，请联系作者删除
