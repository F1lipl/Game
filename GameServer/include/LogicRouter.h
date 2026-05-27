#include "GameServerTypes.h"
#include "Singleton.h"
#include <functional>
#include <unordered_map>
class LogicShard;
class LogicRouter:public Singleton<LogicRouter>{
    friend Singleton<LogicRouter>;
public:
    using Task= std::function<void (LogicShard*,LogicTask)>;
      bool Dispatch(LogicShard* shard, LogicTask task) {
        auto iter = handlers_.find(task.msg_id);
        if (iter == handlers_.end()) {
            return false;
        }

        iter->second(shard, std::move(task));
        return true;
    }

private:
    LogicRouter() = default;
    void Register(MsgId id, Task task) {
        handlers_[id] = std::move(task);
    }

private:
    std::unordered_map<MsgId, Task> handlers_;
};