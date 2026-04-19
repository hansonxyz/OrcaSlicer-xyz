#include "OpenBambuAgent.hpp"
#include <cstdio>

namespace OpenBambu {

// ============================================================================
// Printer model classification
// ============================================================================

PrinterFamily model_to_family(const std::string &model)
{
    // BL-P001 = X1 Carbon, BL-P002 = X1
    // C11 = P1P, C12 = P1S, C13 = X1E
    // N1 = A1 Mini, N2S = A1
    // 3DPrinter-X1-Carbon = X1C (legacy SSDP name)

    if (model == "BL-P001" || model == "BL-P002" ||
        model == "3DPrinter-X1-Carbon" || model == "3DPrinter-X1" ||
        model == "C13")
        return PrinterFamily::X1;

    if (model == "C11" || model == "C12")
        return PrinterFamily::P1;

    if (model == "N1" || model == "N2S")
        return PrinterFamily::A1;

    // P2S, H2, etc.
    if (model.find("P2") != std::string::npos || model.find("H2") != std::string::npos)
        return PrinterFamily::P2S;

    return PrinterFamily::Unknown;
}

// ============================================================================
// Camera URL construction
// ============================================================================

std::string build_camera_url(const std::string &ip, const std::string &access_code,
                              const std::string &model_code)
{
    PrinterFamily family = model_to_family(model_code);

    switch (family) {
    case PrinterFamily::X1:
    case PrinterFamily::P2S:
        // RTSPS on port 322
        return "rtsps://bblp:" + access_code + "@" + ip + ":322/streaming/live/1";

    case PrinterFamily::P1:
    case PrinterFamily::A1:
        // Custom TCP+TLS on port 6000 — MediaPlayCtrl handles this via bambu:/// URL
        return "bambu:///local/" + ip + ".?port=6000&user=bblp&passwd=" + access_code;

    case PrinterFamily::Unknown:
    default:
        // Try RTSPS as default
        return "rtsps://bblp:" + access_code + "@" + ip + ":322/streaming/live/1";
    }
}

// ============================================================================
// Agent lifecycle
// ============================================================================

Agent::Agent() {}

Agent::~Agent()
{
    stop();
}

void Agent::start()
{
    // Wire up SSDP discovery to forward to the registered callback
    m_discovery.set_callback([this](std::string json) {
        std::lock_guard<std::mutex> lock(m_callback_mutex);
        if (m_on_ssdp_msg_fn)
            m_on_ssdp_msg_fn(json);
    });
    m_discovery.start();
}

void Agent::stop()
{
    disconnect_printer();
    m_discovery.stop();
}

// ============================================================================
// Discovery
// ============================================================================

void Agent::set_on_ssdp_msg_fn(OnMsgArrivedFn fn)
{
    std::lock_guard<std::mutex> lock(m_callback_mutex);
    m_on_ssdp_msg_fn = std::move(fn);
}

bool Agent::start_discovery(bool start_flag, bool sending)
{
    if (start_flag) {
        // Wire up the discovery callback before starting (same as start())
        m_discovery.set_callback([this](std::string json) {
            std::lock_guard<std::mutex> lock(m_callback_mutex);
            if (m_on_ssdp_msg_fn)
                m_on_ssdp_msg_fn(json);
        });
        m_discovery.start();
    } else {
        m_discovery.stop();
    }
    return true;
}

// ============================================================================
// Printer Connection (MQTT)
// ============================================================================

int Agent::connect_printer(const std::string &dev_id, const std::string &dev_ip,
                            const std::string &access_code)
{
    // Disconnect any existing connection
    disconnect_printer();

    m_connected_dev_id = dev_id;
    m_connected_dev_ip = dev_ip;
    m_connected_access_code = access_code;

    // Wire up MQTT message callback
    m_mqtt.set_on_message([this, dev_id](const std::string &topic, const std::string &payload) {
        std::lock_guard<std::mutex> lock(m_callback_mutex);
        if (m_on_local_message_fn)
            m_on_local_message_fn(dev_id, topic, payload);
    });

    // Wire up connection status callback
    m_mqtt.set_on_connect([this, dev_id, dev_ip](bool connected) {
        std::lock_guard<std::mutex> lock(m_callback_mutex);
        if (m_on_local_connect_fn) {
            int status = connected ? 0 : -1;
            m_on_local_connect_fn(status, dev_id, connected ? "connected" : "disconnected");
        }
    });

    // Connect via MQTT over TLS
    if (!m_mqtt.connect(dev_ip, 8883, dev_id, access_code)) {
        fprintf(stderr, "OpenBambuAgent: MQTT connect to %s failed\n", dev_ip.c_str());
        return BAMBU_NETWORK_ERR_CONNECT_FAILED;
    }

    // Request full status dump
    m_mqtt.request_pushall();

    return BAMBU_NETWORK_SUCCESS;
}

int Agent::disconnect_printer()
{
    m_mqtt.disconnect();
    m_connected_dev_id.clear();
    m_connected_dev_ip.clear();
    m_connected_access_code.clear();
    return BAMBU_NETWORK_SUCCESS;
}

int Agent::send_message_to_printer(const std::string &dev_id, const std::string &json_str)
{
    if (!m_mqtt.is_connected() || dev_id != m_connected_dev_id)
        return BAMBU_NETWORK_ERR_DISCONNECTED;

    if (!m_mqtt.publish(json_str))
        return BAMBU_NETWORK_ERR_SEND_MSG_FAILED;

    return BAMBU_NETWORK_SUCCESS;
}

void Agent::set_on_local_connect_fn(OnLocalConnectedFn fn)
{
    std::lock_guard<std::mutex> lock(m_callback_mutex);
    m_on_local_connect_fn = std::move(fn);
}

void Agent::set_on_local_message_fn(OnMessageFn fn)
{
    std::lock_guard<std::mutex> lock(m_callback_mutex);
    m_on_local_message_fn = std::move(fn);
}

// ============================================================================
// Print Operations
// ============================================================================

int Agent::start_local_print(const std::string &dev_ip, const std::string &access_code,
                              const std::string &local_file, const std::string &remote_name,
                              int plate_idx, bool use_ams, const std::vector<int> &ams_mapping,
                              OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    // Step 1: Upload file via FTPS
    if (update_fn) update_fn(0, 0, "Uploading...");

    bool uploaded = FtpUpload::upload(dev_ip, access_code, local_file, remote_name,
        [&update_fn, &cancel_fn](size_t uploaded, size_t total) -> bool {
            if (cancel_fn && cancel_fn()) return false;
            if (update_fn && total > 0) {
                int pct = (int)(100.0 * uploaded / total);
                update_fn(pct, 0, "Uploading...");
            }
            return true;
        });

    if (!uploaded) {
        if (update_fn) update_fn(-1, 0, "Upload failed");
        return BAMBU_NETWORK_ERR_SEND_MSG_FAILED;
    }

    // Step 2: Send print command via MQTT
    if (update_fn) update_fn(100, 0, "Starting print...");

    std::string cmd = Commands::project_file(
        remote_name, remote_name, plate_idx, use_ams, ams_mapping);

    if (!m_mqtt.publish(cmd)) {
        if (update_fn) update_fn(-1, 0, "Failed to send print command");
        return BAMBU_NETWORK_ERR_SEND_MSG_FAILED;
    }

    if (update_fn) update_fn(100, 100, "Print started");
    return BAMBU_NETWORK_SUCCESS;
}

int Agent::send_gcode_to_sdcard(const std::string &dev_ip, const std::string &access_code,
                                 const std::string &local_file, const std::string &remote_name,
                                 OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    if (update_fn) update_fn(0, 0, "Uploading...");

    bool uploaded = FtpUpload::upload(dev_ip, access_code, local_file, remote_name,
        [&update_fn, &cancel_fn](size_t uploaded, size_t total) -> bool {
            if (cancel_fn && cancel_fn()) return false;
            if (update_fn && total > 0) {
                int pct = (int)(100.0 * uploaded / total);
                update_fn(pct, 0, "Uploading...");
            }
            return true;
        });

    if (!uploaded) {
        if (update_fn) update_fn(-1, 0, "Upload failed");
        return BAMBU_NETWORK_ERR_SEND_MSG_FAILED;
    }

    if (update_fn) update_fn(100, 100, "Upload complete");
    return BAMBU_NETWORK_SUCCESS;
}

// ============================================================================
// Camera
// ============================================================================

std::string Agent::get_camera_url(const std::string &dev_ip, const std::string &access_code,
                                   const std::string &model_code)
{
    return build_camera_url(dev_ip, access_code, model_code);
}

} // namespace OpenBambu
