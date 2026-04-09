#include "OpenBambuPrinterAgent.hpp"
#include <boost/log/trivial.hpp>

namespace Slic3r {

OpenBambuPrinterAgent::OpenBambuPrinterAgent(std::string log_dir)
    : m_log_dir(std::move(log_dir))
{
    BOOST_LOG_TRIVIAL(info) << "OpenBambuPrinterAgent: created (LAN-only mode, no proprietary DLL)";
}

OpenBambuPrinterAgent::~OpenBambuPrinterAgent()
{
    m_agent.stop();
}

AgentInfo OpenBambuPrinterAgent::get_agent_info_static()
{
    return {"openbambu", "OpenBambu", "1.0.0", "Open-source LAN-only printer agent"};
}

void OpenBambuPrinterAgent::set_cloud_agent(std::shared_ptr<ICloudServiceAgent> cloud)
{
    m_cloud_agent = cloud;
    // OpenBambu doesn't use cloud services — this is a no-op
}

// ============================================================================
// Communication
// ============================================================================

int OpenBambuPrinterAgent::send_message(std::string dev_id, std::string json_str, int qos, int flag)
{
    // Cloud relay — not supported in OpenBambu, forward to local
    return send_message_to_printer(dev_id, json_str, qos, flag);
}

int OpenBambuPrinterAgent::connect_printer(std::string dev_id, std::string dev_ip,
                                            std::string username, std::string password, bool use_ssl)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_connected_dev_id = dev_id;
    m_connected_ip = dev_ip;
    m_connected_access_code = password; // LAN access code is passed as password
    return m_agent.connect_printer(dev_id, dev_ip, password);
}

int OpenBambuPrinterAgent::disconnect_printer()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_connected_dev_id.clear();
    m_connected_ip.clear();
    m_connected_access_code.clear();
    return m_agent.disconnect_printer();
}

int OpenBambuPrinterAgent::send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag)
{
    return m_agent.send_message_to_printer(dev_id, json_str);
}

// ============================================================================
// Certificates (no-op — OpenBambu uses SSL_VERIFY_NONE for self-signed certs)
// ============================================================================

int OpenBambuPrinterAgent::check_cert() { return 0; }
void OpenBambuPrinterAgent::install_device_cert(std::string, bool) {}

// ============================================================================
// Discovery
// ============================================================================

bool OpenBambuPrinterAgent::start_discovery(bool start, bool sending)
{
    return m_agent.start_discovery(start, sending);
}

// ============================================================================
// Binding (LAN-only — no cloud binding needed)
// ============================================================================

int OpenBambuPrinterAgent::ping_bind(std::string) { return 0; }
int OpenBambuPrinterAgent::bind_detect(std::string, std::string, detectResult&) { return 0; }
int OpenBambuPrinterAgent::bind(std::string, std::string, std::string, std::string, bool, OnUpdateStatusFn) { return 0; }
int OpenBambuPrinterAgent::unbind(std::string) { return 0; }
int OpenBambuPrinterAgent::request_bind_ticket(std::string*) { return 0; }
int OpenBambuPrinterAgent::set_server_callback(OnServerErrFn) { return 0; }

// ============================================================================
// Machine Selection
// ============================================================================

std::string OpenBambuPrinterAgent::get_user_selected_machine()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_selected_machine;
}

int OpenBambuPrinterAgent::set_user_selected_machine(std::string dev_id)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_selected_machine = std::move(dev_id);
    return 0;
}

// ============================================================================
// Print Job Operations
// ============================================================================

int OpenBambuPrinterAgent::start_print(PrintParams params, OnUpdateStatusFn update_fn,
                                        WasCancelledFn cancel_fn, OnWaitFn wait_fn)
{
    // Cloud print not supported — delegate to local print
    return start_local_print(params, update_fn, cancel_fn);
}

int OpenBambuPrinterAgent::start_local_print_with_record(PrintParams params, OnUpdateStatusFn update_fn,
                                                          WasCancelledFn cancel_fn, OnWaitFn wait_fn)
{
    // No cloud record in OpenBambu — just do local print
    return start_local_print(params, update_fn, cancel_fn);
}

int OpenBambuPrinterAgent::start_send_gcode_to_sdcard(PrintParams params, OnUpdateStatusFn update_fn,
                                                       WasCancelledFn cancel_fn, OnWaitFn wait_fn)
{
    std::string ip, code;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ip = m_connected_ip;
        code = m_connected_access_code;
    }

    if (ip.empty() || code.empty()) {
        BOOST_LOG_TRIVIAL(error) << "OpenBambuPrinterAgent: no printer connected for FTP upload";
        return -1;
    }

    std::string remote_name = params.filename;
    // Extract just the filename from the path
    auto sep = remote_name.find_last_of("/\\");
    if (sep != std::string::npos) remote_name = remote_name.substr(sep + 1);

    return m_agent.send_gcode_to_sdcard(ip, code, params.filename, remote_name, update_fn, cancel_fn);
}

// Parse a JSON integer array string like "[4,0,1,2]" into a vector<int>
static std::vector<int> parse_int_array(const std::string &s)
{
    std::vector<int> result;
    size_t pos = 0;
    while ((pos = s.find_first_of("-0123456789", pos)) != std::string::npos) {
        size_t end = s.find_first_not_of("-0123456789", pos);
        result.push_back(std::stoi(s.substr(pos, end - pos)));
        pos = (end == std::string::npos) ? end : end + 1;
    }
    return result;
}

int OpenBambuPrinterAgent::start_local_print(PrintParams params, OnUpdateStatusFn update_fn,
                                              WasCancelledFn cancel_fn)
{
    std::string ip, code;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ip = m_connected_ip;
        code = m_connected_access_code;
    }

    if (ip.empty() || code.empty()) {
        BOOST_LOG_TRIVIAL(error) << "OpenBambuPrinterAgent: no printer connected for local print";
        return -1;
    }

    // Determine remote filename
    std::string remote_name = params.ftp_file.empty() ? params.filename : params.ftp_file;
    auto sep = remote_name.find_last_of("/\\");
    if (sep != std::string::npos) remote_name = remote_name.substr(sep + 1);

    // Step 1: Upload file via FTPS
    if (update_fn) update_fn(0, 0, "Uploading...");

    bool uploaded = OpenBambu::FtpUpload::upload(ip, code, params.filename, remote_name,
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

    // Step 2: Parse AMS mapping from PrintParams
    std::vector<int> ams_mapping = parse_int_array(params.ams_mapping);
    BOOST_LOG_TRIVIAL(info) << "OpenBambuPrinterAgent: start_local_print"
        << " file=" << remote_name
        << " plate=" << params.plate_index
        << " use_ams=" << params.task_use_ams
        << " ams_mapping=" << params.ams_mapping
        << " bed_level=" << params.task_bed_leveling
        << " flow_cali=" << params.task_flow_cali
        << " vib_cali=" << params.task_vibration_cali
        << " timelapse=" << params.task_record_timelapse
        << " layer_inspect=" << params.task_layer_inspect;

    // Step 3: Send project_file command with all print options
    std::string cmd = OpenBambu::Commands::project_file(
        remote_name, params.task_name.empty() ? remote_name : params.task_name,
        params.plate_index, params.task_use_ams, ams_mapping,
        params.task_bed_leveling, params.task_flow_cali,
        params.task_vibration_cali, params.task_record_timelapse,
        params.task_layer_inspect);

    if (update_fn) update_fn(100, 0, "Starting print...");

    if (!m_agent.send_message_to_printer(m_connected_dev_id, cmd)) {
        if (update_fn) update_fn(-1, 0, "Failed to send print command");
        return BAMBU_NETWORK_ERR_SEND_MSG_FAILED;
    }

    if (update_fn) update_fn(100, 100, "Print started");
    return BAMBU_NETWORK_SUCCESS;
}

int OpenBambuPrinterAgent::start_sdcard_print(PrintParams params, OnUpdateStatusFn update_fn,
                                               WasCancelledFn cancel_fn)
{
    // SD card print: file is already on the printer, just send the command
    std::string cmd = OpenBambu::Commands::gcode_file(params.filename);
    return send_message_to_printer(params.dev_id, cmd, 0, 0);
}

// ============================================================================
// Callbacks — delegate to OpenBambu::Agent
// ============================================================================

int OpenBambuPrinterAgent::set_on_ssdp_msg_fn(OnMsgArrivedFn fn)
{
    m_agent.set_on_ssdp_msg_fn([fn](std::string json) {
        if (fn) fn(json);
    });
    return 0;
}

int OpenBambuPrinterAgent::set_on_printer_connected_fn(OnPrinterConnectedFn fn)
{
    // Not directly supported — OpenBambu fires on_local_connect instead
    return 0;
}

int OpenBambuPrinterAgent::set_on_subscribe_failure_fn(GetSubscribeFailureFn fn)
{
    return 0;
}

int OpenBambuPrinterAgent::set_on_message_fn(OnMessageFn fn)
{
    // Cloud messages — not supported in OpenBambu
    return 0;
}

int OpenBambuPrinterAgent::set_on_user_message_fn(OnMessageFn fn)
{
    return 0;
}

int OpenBambuPrinterAgent::set_on_local_connect_fn(OnLocalConnectedFn fn)
{
    m_agent.set_on_local_connect_fn([fn](int status, std::string dev_id, std::string msg) {
        if (fn) fn(status, dev_id, msg);
    });
    return 0;
}

int OpenBambuPrinterAgent::set_on_local_message_fn(OnMessageFn fn)
{
    m_agent.set_on_local_message_fn([fn](std::string dev_id, std::string /*topic*/, std::string payload) {
        if (fn) fn(dev_id, payload);
    });
    return 0;
}

int OpenBambuPrinterAgent::set_queue_on_main_fn(QueueOnMainFn fn)
{
    // OpenBambu callbacks are already safe to call from any thread
    // (the GUI marshals via CallAfter), so this is a no-op
    return 0;
}

} // namespace Slic3r
