#pragma  once
#include"Const.h"
#include "Cserver.h"
#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

//进行tcp的连接管理，进行粘包处理，心跳保活，收发信息
// session应该有接收发送缓冲区，socket，并且有唯一uid，同时映射一个玩家uid
namespace asio=boost::asio;
using boost::asio::ip::tcp;
class Cserver;
class Csession:std::enable_shared_from_this<Csession>
{
public:
    Csession(Cserver* server,asio::io_context& content);
    tcp::socket& get_socket(){
        return socket_;
    }
    void start();
    void Set_state(uint8_t state){
        state_=state;
    }
    bool is_clothing()const{
        return state_==Session_state::Closing;
    }
    bool is_connectd() const{
        return state_==Session_state::Conected;
    }



private:
   boost::asio::awaitable<void> heartbeat();//心跳检测 
   void close();
    Cserver* server_;
    std::string uuid_;
    tcp::socket socket_;
    int buffer[Buffer_size];
    boost::asio::steady_timer timer_;
    uint8_t state_;
    std::chrono::steady_clock::time_point last_recv_time;//最后一次收到包的时间
    
};