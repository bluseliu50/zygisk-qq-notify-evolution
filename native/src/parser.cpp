// QQ notification parser — pure Standard C++ implementation.
//
// Ported from refer/QQ-Notify-Evolution/.../core/QQNotificationResolver.kt.
// Regex literals are copied character-for-character from the Kotlin source
// (lines L31-L119). Kotlin named groups ((?<name>...)) are rewritten as
// numbered groups because std::regex (ECMAScript) does not support names;
// group indexing is documented next to each pattern.

#include "parser.hpp"

#include <regex>
#include <string>

namespace {

// ---------------------------------------------------------------------------
// Patterns (literal source from QQNotificationResolver.kt)
// ---------------------------------------------------------------------------

// hideMsgPattern (L45): ^QQ: 你收到了(\d+)条新消息$   group 1 = count
const std::regex re_hide(R"(^QQ: 你收到了(\d+)条新消息$)");

// qzoneTitlePattern (L32): ^QQ空间动态(?:\(共(\d+)条未读\))?$   group 1 = count
const std::regex re_qzone_title(R"(^QQ空间动态(?:\(共(\d+)条未读\))?$)");

// groupMsgPattern (L64), ticker:
//   ^(?<name>.+?)(?:\((?<num>\d+)条新消息\))?: (?<sp>\[特别关心])?(?<nickname>.+?): (?<msg>[\s\S]+)$
// numbered groups: 1=name 2=num 3=sp 4=nickname 5=msg
const std::regex re_group_ticker(
    R"(^(.+?)(?:\((\d+)条新消息\))?: (\[特别关心\])?(.+?): ([\s\S]+)$)");

// groupMsgContentPattern (L72):
//   ^(?<sp>\[特别关心])?(?<name>.+?): (?<msg>[\s\S]+)$
// numbered groups: 1=sp 2=name 3=msg
const std::regex re_group_content(R"(^(\[特别关心\])?(.+?): ([\s\S]+)$)");

// msgPattern (L85), private ticker:
//   ^(?<sp>\[特别关心])?(?<nickname>.+?)(\((?<num>\d+)条新消息\))?: (?<msg>[\s\S]+)$
// numbered groups: 1=sp 2=nickname 3=(whole "(N条新消息)") 4=num 5=msg
const std::regex re_private_ticker(
    R"(^(\[特别关心\])?(.+?)(\((\d+)条新消息\))?: ([\s\S]+)$)");

// bindingQQMsgTickerPattern (L103): ^关联QQ号-(.+?):([\s\S]+)$
// numbered groups: 1=sender 2=message
const std::regex re_binding_ticker(R"(^关联QQ号-(.+?):([\s\S]+)$)");

// bindingQQMsgContextPattern (L111): ^有 \d+ 个联系人给你发过来(\d+)条新消息$
// numbered groups: 1=count
const std::regex re_binding_content_num(
    R"(^有 \d+ 个联系人给你发过来(\d+)条新消息$)");

// bindingQQMsgTitlePattern (L119): ^关联QQ号 \((\d+)条新消息\)$
// numbered groups: 1=count
const std::regex re_binding_title_num(R"(^关联QQ号 \((\d+)条新消息\)$)");

// ---------------------------------------------------------------------------

int toInt(const std::ssub_match& g, int def) {
    if (!g.matched) return def;
    try {
        return std::stoi(g.str());
    } catch (...) {
        return def;
    }
}

bool startsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() &&
           s.compare(0, prefix.size(), prefix) == 0;
}

}  // namespace

ParsedMsg parseNotification(const std::string& title,
                            const std::string& ticker,
                            const std::string& content) {
    ParsedMsg r;

    // 1. title or content empty -> None
    if (title.empty() || content.empty()) return r;

    // 2. hidden message?
    std::smatch mh;
    if (std::regex_match(ticker, mh, re_hide)) {
        r.type = MsgType::Hidden;
        r.num = toInt(mh[1], 1);
        return r;
    }

    // 3. QQ Zone?
    std::smatch mq;
    if (!ticker.empty() && std::regex_match(title, mq, re_qzone_title)) {
        if (startsWith(ticker, "【特别关心】")) {
            r.type = MsgType::QZoneSpecial;
            r.content = content;
            return r;
        }
        if (mq[1].matched) {
            r.type = MsgType::QZone;
            r.content = content;
            r.num = toInt(mq[1], 1);
            return r;
        }
        // title matches QZone shape but no count and not special -> fall through
    }

    // 4. ticker empty -> None
    if (ticker.empty()) return r;

    // 5. group message?
    {
        std::smatch mt, mc;
        if (std::regex_match(ticker, mt, re_group_ticker) &&
            std::regex_match(content, mc, re_group_content)) {
            r.type = MsgType::Group;
            r.groupName = mt[1].str();   // name (from ticker)
            r.name      = mt[4].str();   // nickname (from ticker)
            r.content   = mc[3].str();   // msg (from content)
            r.num       = toInt(mt[2], 1);
            r.special   = mc[1].matched; // sp (from content)
            return r;
        }
    }

    // 6. private message?
    {
        std::smatch mt;
        if (std::regex_match(ticker, mt, re_private_ticker)) {
            r.type = MsgType::Private;
            r.name    = mt[2].str();     // nickname
            r.content = content;         // whole content (matches QNE)
            r.num     = toInt(mt[4], 1);
            r.special = mt[1].matched;   // sp
            return r;
        }
    }

    // 7. binding (associated QQ account) message?
    {
        std::smatch mt;
        if (std::regex_match(ticker, mt, re_binding_ticker)) {
            r.type = MsgType::Binding;
            r.name    = mt[1].str();     // sender
            r.content = mt[2].str();     // message
            // matchBindingMsgNum
            int num = 1;
            if (title == "QQ") {
                std::smatch mc;
                if (std::regex_match(content, mc, re_binding_content_num)) {
                    num = toInt(mc[1], 1);
                }
            } else {
                std::smatch mt2;
                if (std::regex_match(title, mt2, re_binding_title_num)) {
                    num = toInt(mt2[1], 1);
                }
            }
            r.num = num;
            return r;
        }
    }

    // 8. unknown -> None
    return r;
}
