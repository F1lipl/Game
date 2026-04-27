#pragma once

#include "Const.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>

class WorkShard;
class Csession;
class RecvNode;
class ClientSession;

class ClientIngressRouter {
public:
    struct MsgContext {
        WorkShard* shard {};
        std::shared_ptr<Csession> session;
        std::shared_ptr<RecvNode> body;
    };

    using Handler = std::function<void(const MsgContext&)>;

    static void Init();
    static void HandleMsg(std::uint16_t msgid, const MsgContext& context);

private:
    static void RegisterCallback(std::uint16_t msgid, Handler handler);
    static  bool is_init_;

private:
    static std::unordered_map<std::uint16_t, Handler> callbacks_;
};
ClientIngressRouter::is_init_

class BackendIngressRouter {
public:
    struct BackendMsgContext {
        WorkShard* shard {};
        std::shared_ptr<ClientSession> backend_session;
        std::shared_ptr<RecvNode> body;
    };

    using Handler = std::function<void(const BackendMsgContext&)>;

    static void Init();
    static void HandleMsg(std::uint16_t msgid, const BackendMsgContext& context);

private:
    static void RegisterCallback(std::uint16_t msgid, Handler handler);

private:
    static std::unordered_map<std::uint16_t, Handler> callbacks_;
};
