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
#include <vector>

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
    void send_probes(const std::vector<socket_t> &socks, uint16_t port);
    std::string parse_ssdp_message(const std::string &message, const std::string &source_ip);

    OnDeviceFound m_callback;
    std::mutex m_callback_mutex;
    std::thread m_listener_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stop_requested{false};

    // Deduplication: only fire callback for a device at most once per interval
    std::map<std::string, std::chrono::steady_clock::time_point> m_last_seen;
    mutable std::mutex m_dedup_mutex;
    static constexpr int DEDUP_INTERVAL_SEC = 10; // seconds between callbacks for same device

public:
    // Diagnostic counters for debugging SSDP issues
    struct DiagInfo {
        std::atomic<uint64_t> probes_sent{0};
        std::atomic<uint64_t> packets_received{0};
        std::atomic<uint64_t> bambu_responses{0};
        std::atomic<uint64_t> callbacks_fired{0};
        std::atomic<uint64_t> dedup_suppressed{0};
        std::atomic<uint64_t> recv_errors{0};
        std::atomic<uint64_t> recv_timeouts{0};
        std::atomic<int> last_send_result{0};
        std::atomic<int> last_send_error{0};
        std::atomic<int> last_recv_error{0};
        std::atomic<int> interfaces_found{0};
        std::chrono::steady_clock::time_point last_probe_time{};
        std::chrono::steady_clock::time_point last_response_time{};
    };
    const DiagInfo &diag() const { return m_diag; }

    // Get a snapshot of last-seen devices (dev_id -> last seen time)
    std::map<std::string, std::chrono::steady_clock::time_point> get_last_seen() const {
        std::lock_guard<std::mutex> lock(m_dedup_mutex);
        return m_last_seen;
    }

private:
    DiagInfo m_diag;
};

} // namespace OpenBambu
