#include "../include/Metrics.h"
#include "../include/MetricsServer.h"

#include <boost/asio/read_until.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/write.hpp>
#include <spdlog/spdlog.h>

#include <string>

using boost::asio::ip::tcp;

MetricsServer::MetricsServer(unsigned short port) : port_(port) {}

MetricsServer::~MetricsServer() {
    Stop();
}

void MetricsServer::Start() {
    stopping_ = false;
    ioc_.restart();

    boost::system::error_code ec;
    tcp::endpoint endpoint(tcp::v4(), port_);
    acceptor_ = std::make_unique<tcp::acceptor>(ioc_);
    acceptor_->open(endpoint.protocol(), ec);
    acceptor_->set_option(tcp::acceptor::reuse_address(true), ec);
    acceptor_->bind(endpoint, ec);
    if (ec) {
        spdlog::warn("metrics server bind {}: {}", port_, ec.message());
        acceptor_.reset();
        return;
    }
    acceptor_->listen(boost::asio::socket_base::max_listen_connections, ec);

    boost::asio::post(ioc_, [this]() { DoAccept(); });
    thread_ = std::thread([this]() { ioc_.run(); });
    spdlog::info("metrics endpoint on http://0.0.0.0:{}/metrics", port_);
}

void MetricsServer::Stop() {
    if (stopping_.exchange(true)) {
        return;
    }
    boost::asio::post(ioc_, [this]() {
        boost::system::error_code ec;
        if (acceptor_) {
            acceptor_->close(ec);
        }
    });
    ioc_.stop();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void MetricsServer::DoAccept() {
    if (stopping_ || !acceptor_) {
        return;
    }
    auto sock = std::make_shared<tcp::socket>(ioc_);
    acceptor_->async_accept(*sock, [this, sock](const boost::system::error_code& ec) {
        if (!ec) {
            HandleConn(sock);
        }
        if (!stopping_) {
            DoAccept();
        }
    });
}

void MetricsServer::HandleConn(std::shared_ptr<tcp::socket> sock) {
    auto buf = std::make_shared<boost::asio::streambuf>();
    // read the request headers (we don't branch on the path; always serve metrics)
    boost::asio::async_read_until(
        *sock, *buf, "\r\n\r\n",
        [sock, buf](const boost::system::error_code&, std::size_t) {
            auto body = std::make_shared<std::string>(metrics::Render());
            auto resp = std::make_shared<std::string>(
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain; version=0.0.4\r\n"
                "Content-Length: " + std::to_string(body->size()) + "\r\n"
                "Connection: close\r\n\r\n" + *body);
            boost::asio::async_write(
                *sock, boost::asio::buffer(*resp),
                [sock, resp](const boost::system::error_code&, std::size_t) {
                    boost::system::error_code ig;
                    sock->shutdown(tcp::socket::shutdown_both, ig);
                    sock->close(ig);
                });
        });
}
