// QQ notification parser — pure Standard C++ (no Android/JNI dependency).
//
// Ported (semantics, not source) from
// refer/QQ-Notify-Evolution/.../core/QQNotificationResolver.kt
// so it can be unit-tested on host with a plain C++ compiler.

#pragma once

#include <string>

enum class MsgType {
    None,
    Private,
    Group,
    QZone,
    QZoneSpecial,
    Binding,
    Hidden,
};

struct ParsedMsg {
    MsgType type = MsgType::None;
    std::string name;       // sender nickname (Private/Group/Binding)
    std::string groupName;  // group name (Group only)
    std::string content;    // message body (semantics differ per type)
    int num = 1;            // new-message count
    bool special = false;   // "特别关心" flag
};

// Parse a QQ notification's title/ticker/content into a structured message.
// Returns ParsedMsg with type == MsgType::None when no known pattern matches.
ParsedMsg parseNotification(const std::string& title,
                            const std::string& ticker,
                            const std::string& content);
