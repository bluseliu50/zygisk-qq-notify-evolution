SKIPUNZIP=0

ui_print "- QQ Notify Evolution"
ui_print "- 功能：将 QQ 通知改写为标准 MessagingStyle 样式"
ui_print "- 多会话 / 分渠道 / 带头像，兼容 WearOS 显示"
ui_print "- 需要 Zygisk 支持（Magisk / KernelSU + ZygiskNext / APatch + ReZygisk）"
ui_print "- 安装后重启生效，无可开关项"

# 当前 root 方案探测（仅提示，无功能影响）
if [ "$KSU" = "true" ] || [ -n "$KSU_VER" ] || [ -n "$KSU_VER_CODE" ]; then
  ui_print "- 检测到 KernelSU"
elif [ "$APATCH" = "true" ] || [ -n "$APATCH_VER" ] || [ -n "$APATCH_VER_CODE" ]; then
  ui_print "- 检测到 APatch"
elif [ -n "$MAGISK_VER" ] || [ -n "$MAGISK_VER_CODE" ]; then
  ui_print "- 检测到 Magisk"
else
  ui_print "- 未识别 root 方案（请确保已启用 Zygisk）"
fi
