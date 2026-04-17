#pragma once

#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>

class IniConfig {
public:
    bool Load(const std::string& file_path, std::string* err = nullptr) {
        try {
            boost::property_tree::ini_parser::read_ini(file_path, tree_);
            file_path_ = file_path;
            return true;
        } catch (const std::exception& e) {
            if (err) {
                *err = e.what();
            }
            return false;
        }
    }

    bool Save(const std::string& file_path = "", std::string* err = nullptr) const {
        try {
            const std::string target = file_path.empty() ? file_path_ : file_path;
            if (target.empty()) {
                if (err) {
                    *err = "save path is empty";
                }
                return false;
            }

            const std::filesystem::path p(target);
            if (!p.parent_path().empty()) {
                std::filesystem::create_directories(p.parent_path());
            }

            boost::property_tree::ini_parser::write_ini(target, tree_);
            return true;
        } catch (const std::exception& e) {
            if (err) {
                *err = e.what();
            }
            return false;
        }
    }

    bool Has(const std::string& key) const {
        return tree_.get_optional<std::string>(key).has_value();
    }

    template <typename T>
    T Get(const std::string& key, const T& default_value) const {
        return tree_.get<T>(key, default_value);
    }

    template <typename T>
    T Require(const std::string& key) const {
        auto value = tree_.get_optional<T>(key);
        if (!value.has_value()) {
            throw std::runtime_error("missing config key: " + key);
        }
        return *value;
    }

    template <typename T>
    void Set(const std::string& key, const T& value) {
        tree_.put(key, value);
    }

    void Remove(const std::string& key) {
        tree_.erase(key);
    }

private:
    boost::property_tree::ptree tree_;
    std::string file_path_;
};

struct GatewayConfig {
    std::string ip = "127.0.0.1";
    std::uint16_t port = 7000;
    std::size_t io_threads = 4;
    std::size_t max_conn = 1024;

    static GatewayConfig FromIni(const IniConfig& ini) {
        GatewayConfig cfg;
        cfg.ip = ini.Get<std::string>("gateway.ip", cfg.ip);
        cfg.port = static_cast<std::uint16_t>(ini.Get<int>("gateway.port", cfg.port));
        cfg.io_threads = ini.Get<std::size_t>("gateway.io_threads", cfg.io_threads);
        cfg.max_conn = ini.Get<std::size_t>("gateway.max_conn", cfg.max_conn);
        return cfg;
    }

    void ToIni(IniConfig& ini) const {
        ini.Set("gateway.ip", ip);
        ini.Set("gateway.port", static_cast<int>(port));
        ini.Set("gateway.io_threads", io_threads);
        ini.Set("gateway.max_conn", max_conn);
    }
};
