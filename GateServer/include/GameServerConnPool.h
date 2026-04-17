#include"Const.h"
#include "ClientSession.h"
#include <boost/asio/awaitable.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/system/detail/error_code.hpp>
#include <cstddef>
#include <memory>
#include <queue>
#include <system_error>
#include <unordered_map>
#include <vector>
#include"IOservicePool.h"
#include "Cserver.h"

//管理与后端的连接
//要实现一个检测机制，如果队列里的连接损坏了


class Cserver;
class GameServerConnPool{

public:
    using Connptr=ClientSession*;
    GameServerConnPool(Cserver*,boost::asio::io_context&);
    ~GameServerConnPool();
    void Init();//创建连接
    boost::asio::awaitable<std::shared_ptr<ClientSession>>accquir();
    void release(std::shared_ptr<ClientSession>);
private:
    boost::asio::awaitable<void>detection();

    std::vector<std::shared_ptr<ClientSession>>sessions_;
    boost::asio::experimental::channel<void(boost::system::error_code ,std::shared_ptr<ClientSession>)>chann_;
    boost::asio::io_context& ioc_;
    Cserver* server_;    
    size_t conn_cnt_;
    boost::asio::steady_timer timer_;





};