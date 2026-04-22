#include "../include/Router.h"
#include <spdlog/spdlog.h>

std::unordered_map<std::uint16_t, ClientIngressRouter::Handler>
    ClientIngressRouter::callbacks_;

std::unordered_map<std::uint16_t, BackendIngressRouter::Handler>
    BackendIngressRouter::callbacks_;

void ClientIngressRouter::Init() {
    RegisterCallback(1001, [](const MsgContext& ctx) {
        // 登录处理
    });

    RegisterCallback(2001, [](const MsgContext& ctx) {
        // 转发到游戏服
    });
}

void ClientIngressRouter::HandleMsg(std::uint16_t msgid, const MsgContext& context) {
    auto it = callbacks_.find(msgid);
    if (it == callbacks_.end()) {
        spdlog::warn("ClientIngressRouter unknown msgid {}", msgid);
        return;
    }
    it->second(context);
}

void ClientIngressRouter::RegisterCallback(std::uint16_t msgid, Handler handler) {
    callbacks_[msgid] = std::move(handler);
}

void BackendIngressRouter::Init() {
    RegisterCallback(3001, [](const BackendMsgContext& ctx) {
        // 游戏服回包转发客户端
    });
}

void BackendIngressRouter::HandleMsg(std::uint16_t msgid, const BackendMsgContext& context) {
    auto it = callbacks_.find(msgid);
    if (it == callbacks_.end()) {
        spdlog::warn("BackendIngressRouter unknown msgid {}", msgid);
        return;
    }
    it->second(context);
}

void BackendIngressRouter::RegisterCallback(std::uint16_t msgid, Handler handler) {
    callbacks_[msgid] = std::move(handler);
}
