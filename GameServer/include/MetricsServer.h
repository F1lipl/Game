#pragma once

#include <boost/asio.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

// Tiny HTTP server that serves the Prometheus metrics text on /metrics.
// Runs its own single-threaded io_context so it never competes with the
// game's logic/network shards for their threads.
class MetricsServer {
public:
    explicit MetricsServer(unsigned short port);
    ~MetricsServer();

    void Start();
    void Stop();

private:
    void DoAccept();
    void HandleConn(std::shared_ptr<boost::asio::ip::tcp::socket> sock);

    unsigned short port_;
    boost::asio::io_context ioc_;
    std::unique_ptr<boost::asio::ip::tcp::acceptor> acceptor_;
    std::thread thread_;
    std::atomic<bool> stopping_{true};
};
