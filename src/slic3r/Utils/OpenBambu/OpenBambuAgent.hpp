#pragma once

// OpenBambuAgent — Open-source IPrinterAgent implementation for LAN-only mode.
//
// Replaces the proprietary bambu_networking DLL for all LAN printer operations:
//   - SSDP discovery (find printers on the network)
//   - MQTT over TLS (status updates, commands)
//   - FTPS (file upload to printer)
//   - Camera URL construction
//
// Cloud features (account login, remote printing, firmware updates) are not
// supported — this is LAN-only by design.

#include "OpenBambuDiscovery.hpp"
#include "OpenBambuMqtt.hpp"
#include "OpenBambuFtp.hpp"
#include "OpenBambuCommands.hpp"

#include <string>
#include <mutex>
#include <functional>
#include <map>

// Forward declare OrcaSlicer types we need
// (When integrated into OrcaSlicer, these come from bambu_networking.hpp)
#ifndef BAMBU_NETWORK_SUCCESS
#define BAMBU_NETWORK_SUCCESS 0
#define BAMBU_NETWORK_ERR_INVALID_HANDLE -1
#define BAMBU_NETWORK_ERR_CONNECT_FAILED -2
#define BAMBU_NETWORK_ERR_DISCONNECTED -3
#define BAMBU_NETWORK_ERR_SEND_MSG_FAILED -4
#define BAMBU_NETWORK_ERR_BIND_CREATE_SOCKET_FAILED -5
#define BAMBU_NETWORK_ERR_BIND_ECODE_FAILED -6
#endif

namespace OpenBambu {

// Callback types matching OrcaSlicer's bambu_networking.hpp
using OnMsgArrivedFn = std::function<void(std::string)>;
using OnPrinterConnectedFn = std::function<void(std::string, std::string, bool)>;
using OnLocalConnectedFn = std::function<void(int, std::string, std::string)>;
using OnMessageFn = std::function<void(std::string, std::string, std::string)>;
using OnUpdateStatusFn = std::function<void(int, int, std::string)>;
using WasCancelledFn = std::function<bool()>;
using QueueOnMainFn = std::function<void(std::function<void()>)>;

// Printer model families for camera URL determination
enum class PrinterFamily {
    X1,     // X1, X1C, X1E — RTSPS on port 322
    P1,     // P1S, P1P — TCP+TLS on port 6000
    A1,     // A1, A1 Mini — TCP+TLS on port 6000
    P2S,    // P2S — RTSPS on port 322
    Unknown
};

PrinterFamily model_to_family(const std::string &model_code);

// Camera URL construction for LAN mode
std::string build_camera_url(const std::string &ip, const std::string &access_code,
                              const std::string &model_code);

class Agent {
public:
    Agent();
    ~Agent();

    // ========================================================================
    // Lifecycle
    // ========================================================================

    // Start the agent (begins SSDP discovery)
    void start();

    // Stop the agent (disconnects everything)
    void stop();

    // ========================================================================
    // Discovery
    // ========================================================================

    void set_on_ssdp_msg_fn(OnMsgArrivedFn fn);
    bool start_discovery(bool start, bool sending);

    // ========================================================================
    // Printer Connection (MQTT)
    // ========================================================================

    // Connect to a specific printer via MQTT
    int connect_printer(const std::string &dev_id, const std::string &dev_ip,
                        const std::string &access_code);
    int disconnect_printer();

    // Send a JSON command to the connected printer
    int send_message_to_printer(const std::string &dev_id, const std::string &json_str);

    void set_on_local_connect_fn(OnLocalConnectedFn fn);
    void set_on_local_message_fn(OnMessageFn fn);

    // ========================================================================
    // Print Operations
    // ========================================================================

    // Upload a file and start printing
    int start_local_print(const std::string &dev_ip, const std::string &access_code,
                          const std::string &local_file, const std::string &remote_name,
                          int plate_idx, bool use_ams, const std::vector<int> &ams_mapping,
                          OnUpdateStatusFn update_fn = nullptr,
                          WasCancelledFn cancel_fn = nullptr);

    // Upload a file to SD card without starting print
    int send_gcode_to_sdcard(const std::string &dev_ip, const std::string &access_code,
                              const std::string &local_file, const std::string &remote_name,
                              OnUpdateStatusFn update_fn = nullptr,
                              WasCancelledFn cancel_fn = nullptr);

    // ========================================================================
    // Camera
    // ========================================================================

    // Get camera URL for a printer (LAN mode)
    std::string get_camera_url(const std::string &dev_ip, const std::string &access_code,
                                const std::string &model_code);

    const Discovery &discovery() const { return m_discovery; }

private:
    Discovery m_discovery;
    MqttClient m_mqtt;

    // Callbacks
    OnMsgArrivedFn m_on_ssdp_msg_fn;
    OnLocalConnectedFn m_on_local_connect_fn;
    OnMessageFn m_on_local_message_fn;
    std::mutex m_callback_mutex;

    // Connected printer state
    std::string m_connected_dev_id;
    std::string m_connected_dev_ip;
    std::string m_connected_access_code;
};

} // namespace OpenBambu
