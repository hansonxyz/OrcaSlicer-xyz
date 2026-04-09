#pragma once

// OpenBambu SSDP Discovery — see OpenBambuDiscovery.cpp for protocol details.

#include <functional>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <cstdint>
#include <map>
#include <chrono>

namespace OpenBambu {

class Discovery {
public:
    // Platform socket type
#ifdef _WIN32
    using socket_t = unsigned long long; // SOCKET
#else
    using socket_t = int;
#endif
    static constexpr socket_t INVALID_SOCK = (socket_t)(~0);

    using OnDeviceFound = std::function<void(std::string json_str)>;

    Discovery();
    ~Discovery();

    void set_callback(OnDeviceFound cb);
    bool start();
    void stop();
    bool is_running() const { return m_running.load(); }

private:
    void listener_thread_func();
    void send_msearch(socket_t sock, uint16_t port);
    void send_msearch_broadcast(socket_t sock, uint16_t port);
    std::string parse_ssdp_message(const std::string &message, const std::string &source_ip);

    OnDeviceFound m_callback;
    std::mutex m_callback_mutex;
    std::thread m_listener_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stop_requested{false};

    // Deduplication: only fire callback for a device at most once per interval
    std::map<std::string, std::chrono::steady_clock::time_point> m_last_seen;
    std::mutex m_dedup_mutex;
    static constexpr int DEDUP_INTERVAL_SEC = 10; // seconds between callbacks for same device
};

} // namespace OpenBambu
