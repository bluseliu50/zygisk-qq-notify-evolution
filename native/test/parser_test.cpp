// Host unit test for the QQ notification parser.
// Compiles & runs with a plain C++17 compiler (no Android/NDK needed):
//   g++ -std=c++17 native/src/parser.cpp native/test/parser_test.cpp -o pt && ./pt
//
// Cases are ported verbatim (inputs + expected fields) from
// refer/QQ-Notify-Evolution/.../core/QQNotificationResolverTest.kt.

#include <cstdlib>
#include <iostream>
#include <string>

#include "../src/parser.hpp"

static int g_failures = 0;

static const char* typeName(MsgType t) {
    switch (t) {
        case MsgType::None:         return "None";
        case MsgType::Private:      return "Private";
        case MsgType::Group:        return "Group";
        case MsgType::QZone:        return "QZone";
        case MsgType::QZoneSpecial: return "QZoneSpecial";
        case MsgType::Binding:      return "Binding";
        case MsgType::Hidden:       return "Hidden";
    }
    return "?";
}

#define CHECK_EQ(actual, expected)                                          \
    do {                                                                    \
        auto _a = (actual);                                                 \
        auto _e = (expected);                                               \
        if (!(_a == _e)) {                                                  \
            std::cerr << "FAIL [" << case_name << "] " << #actual           \
                      << " actual=" << _a << " expected=" << _e << "\n";    \
            g_failures++;                                                   \
        }                                                                   \
    } while (0)

#define CHECK_TYPE(parsed, expected_type)                                   \
    do {                                                                    \
        if ((parsed).type != (expected_type)) {                            \
            std::cerr << "FAIL [" << case_name << "] type actual="          \
                      << typeName((parsed).type) << " expected="            \
                      << typeName(expected_type) << "\n";                   \
            g_failures++;                                                   \
        }                                                                   \
    } while (0)

int main() {
    // --- private ---------------------------------------------------------
    {
        const char* case_name = "private_normal";
        ParsedMsg p = parseNotification("咕咕咕", "咕咕咕: qqq", "123qqq");
        CHECK_TYPE(p, MsgType::Private);
        CHECK_EQ(p.name, std::string("咕咕咕"));
        CHECK_EQ(p.content, std::string("123qqq"));
        CHECK_EQ(p.num, 1);
        CHECK_EQ(p.special, false);
    }
    {
        const char* case_name = "private_special_MultiMessage";
        ParsedMsg p = parseNotification("[特别关心]咕咕咕(2条新消息)",
                                        "[特别关心]咕咕咕(2条新消息): 222", "222");
        CHECK_TYPE(p, MsgType::Private);
        CHECK_EQ(p.name, std::string("咕咕咕"));
        CHECK_EQ(p.content, std::string("222"));
        CHECK_EQ(p.num, 2);
        CHECK_EQ(p.special, true);
    }
    {
        const char* case_name = "private_special";
        ParsedMsg p = parseNotification("[特别关心]咕咕咕",
                                        "[特别关心]咕咕咕: ok111", "ok111");
        CHECK_TYPE(p, MsgType::Private);
        CHECK_EQ(p.name, std::string("咕咕咕"));
        CHECK_EQ(p.content, std::string("ok111"));
        CHECK_EQ(p.num, 1);
        CHECK_EQ(p.special, true);
    }

    // --- group -----------------------------------------------------------
    {
        const char* case_name = "group_normal";
        ParsedMsg p = parseNotification("测试群", "测试群: 咕咕咕: from group",
                                        "咕咕咕: from group");
        CHECK_TYPE(p, MsgType::Group);
        CHECK_EQ(p.groupName, std::string("测试群"));
        CHECK_EQ(p.name, std::string("咕咕咕"));
        CHECK_EQ(p.content, std::string("from group"));
        CHECK_EQ(p.num, 1);
        CHECK_EQ(p.special, false);
    }
    {
        const char* case_name = "group_multiMessage";
        ParsedMsg p = parseNotification("测试群(2条新消息)",
                                        "测试群(2条新消息): 咕咕咕: 2222",
                                        "咕咕咕: 2222");
        CHECK_TYPE(p, MsgType::Group);
        CHECK_EQ(p.groupName, std::string("测试群"));
        CHECK_EQ(p.name, std::string("咕咕咕"));
        CHECK_EQ(p.content, std::string("2222"));
        CHECK_EQ(p.num, 2);
        CHECK_EQ(p.special, false);
    }
    {
        const char* case_name = "group_special_multiMessage";
        ParsedMsg p = parseNotification("测试群(3条新消息)",
                                        "测试群(3条新消息): [特别关心]咕咕咕: 333",
                                        "[特别关心]咕咕咕: 333");
        CHECK_TYPE(p, MsgType::Group);
        CHECK_EQ(p.groupName, std::string("测试群"));
        CHECK_EQ(p.name, std::string("咕咕咕"));
        CHECK_EQ(p.content, std::string("333"));
        CHECK_EQ(p.num, 3);
        CHECK_EQ(p.special, true);
    }
    {
        const char* case_name = "group_special";
        ParsedMsg p = parseNotification("测试群",
                                        "测试群: [特别关心]咕咕咕: from group",
                                        "[特别关心]咕咕咕: from group");
        CHECK_TYPE(p, MsgType::Group);
        CHECK_EQ(p.groupName, std::string("测试群"));
        CHECK_EQ(p.name, std::string("咕咕咕"));
        CHECK_EQ(p.content, std::string("from group"));
        CHECK_EQ(p.num, 1);
        CHECK_EQ(p.special, true);
    }

    // --- QQ Zone ---------------------------------------------------------
    {
        const char* case_name = "qzone_specialPost";
        ParsedMsg p = parseNotification("QQ空间动态",
                                        "【特别关心】咕咕咕：QZone post",
                                        "【特别关心】咕咕咕：QZone post");
        CHECK_TYPE(p, MsgType::QZoneSpecial);
    }
    {
        const char* case_name = "qzone_message";
        ParsedMsg p = parseNotification("QQ空间动态(共1条未读)",
                                        "咕咕咕赞了你的说说", "咕咕咕赞了你的说说");
        CHECK_TYPE(p, MsgType::QZone);
    }

    // --- other -----------------------------------------------------------
    {
        const char* case_name = "hidden";
        ParsedMsg p = parseNotification("QQ", "QQ: 你收到了1条新消息",
                                        "你收到了1条新消息");
        CHECK_TYPE(p, MsgType::Hidden);
    }
    {
        // "\n" in the C++ literal is a real newline, matching the JSON-decoded
        // value used by QQNotificationResolverTest.binding_multiMessage_multiLine.
        const char* case_name = "binding_multiMessage_multiLine";
        ParsedMsg p = parseNotification("关联QQ号 (3条新消息)",
                                        "关联QQ号-/dev/urandom:d\nd",
                                        "/dev/urandom:d\nd");
        CHECK_TYPE(p, MsgType::Binding);
        CHECK_EQ(p.name, std::string("/dev/urandom"));
        CHECK_EQ(p.content, std::string("d\nd"));
        CHECK_EQ(p.num, 3);
    }

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) FAILED\n";
        return 1;
    }
    std::cout << "All parser tests passed\n";
    return 0;
}
