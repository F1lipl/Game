#include "../include/Const.h"
#include "../include/GameServer.h"
#include "../include/GatewayLinkSession.h"
#include "../include/IniConfig.h"
#include "../include/NetworkShard.h"

#include <boost/asio.hpp>
#include <boost/system/error_code.hpp>
#include <spdlog/spdlog.h>

#include <csignal>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <string>

namespace {

bool LoadConfig(IniConfig& ini) {
    std::string err;
    const char* paths[] = {
        "config/config.ini",
        "../config/config.ini",
        "./config.ini",
    };

    for (const auto* path : paths) {
        if (ini.Load(path, &err)) {
            spdlog::info("loaded config from {}", path);
            return true;
        }
    }

    spdlog::warn("load config failed, using defaults. last error: {}", err);
    return false;
}

boost::asio::ip::tcp::acceptor CreateAcceptor(boost::asio::io_context& ioc,
                                              const std::string& listen_ip,
                                              unsigned short port) {
    using boost::asio::ip::tcp;

    tcp::endpoint endpoint(boost::asio::ip::make_address(listen_ip), port);
    tcp::acceptor acceptor(ioc);
    acceptor.open(endpoint.protocol());
    acceptor.set_option(tcp::acceptor::reuse_address(true));
    acceptor.bind(endpoint);
    acceptor.listen(boost::asio::socket_base::max_listen_connections);
    return acceptor;
}

} // namespace

int main() {
    try {
        IniConfig ini;
        LoadConfig(ini);

        const int port_value = ini.Get<int>("GameServer.port", 50051);
        if (port_value <= 0 || port_value > 65535) {
            spdlog::error("invalid GameServer.port {}", port_value);
            return 1;
        }

        const auto listen_ip = ini.Get<std::string>("GameServer.listen_ip", "0.0.0.0");
        const auto logic_shards = ini.Get<std::size_t>("GameServer.logic_shards", WORK_SHARD_NUMBER);
        const auto default_gateway_links = WORK_SHARD_NUMBER * GAMESERVER_CONN_CNT;
        const auto gateway_links = ini.Get<std::size_t>(
            "GameServer.gateway_link_count",
            default_gateway_links);

        boost::asio::io_context ioc;
        auto acceptor = CreateAcceptor(
            ioc,
            listen_ip,
            static_cast<unsigned short>(port_value));

        GameServer server(ioc, logic_shards, gateway_links);
        server.Start();

        boost::asio::signal_set signals(ioc, SIGINT, SIGTERM);
        signals.async_wait([&](const boost::system::error_code& ec, int signal_number) {
            if (ec) {
                return;
            }

            spdlog::info("GameServer received signal {}, stopping", signal_number);
            boost::system::error_code ignored;
            acceptor.cancel(ignored);
            ignored.clear();
            acceptor.close(ignored);
            server.Stop();
            ioc.stop();
        });

        auto retry_timer = std::make_shared<boost::asio::steady_timer>(ioc);
        auto do_accept = std::make_shared<std::function<void()>>();
        *do_accept = [&server, &acceptor, retry_timer, do_accept]() {
            if (!acceptor.is_open()) {
                return;
            }

            auto session = server.GetNetworkShard().CreateAcceptSession();
            if (!session) {
                retry_timer->expires_after(LINK_DETECTION_TIME);
                retry_timer->async_wait([do_accept](const boost::system::error_code& ec) {
                    if (!ec) {
                        (*do_accept)();
                    }
                });
                return;
            }

            acceptor.async_accept(
                session->get_socket(),
                [&server, &acceptor, session, do_accept](const boost::system::error_code& ec) {
                    if (ec) {
                        server.GetNetworkShard().OnAcceptFailed(
                            session->SlotId(),
                            session->Generation());

                        if (acceptor.is_open() && ec != boost::asio::error::operation_aborted) {
                            spdlog::error("accept gateway link failed: {}", ec.message());
                        }
                    } else {
                        server.GetNetworkShard().OnAcceptSuccess(
                            session->SlotId(),
                            session->Generation());
                    }

                    if (acceptor.is_open()) {
                        (*do_accept)();
                    }
                });
        };

        (*do_accept)();

        spdlog::info("GameServer listening on {}:{}", listen_ip, port_value);
        ioc.run();
        server.Stop();
        return 0;
    } catch (const std::exception& e) {
        spdlog::critical("GameServer fatal error: {}", e.what());
        return 1;
    }
}
