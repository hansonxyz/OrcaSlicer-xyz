#pragma once

// OpenBambuPrinterAgent — IPrinterAgent implementation backed by OpenBambu.
// This is the bridge between OrcaSlicer's networking interface and our
// open-source LAN protocol stack. When use_bambu_network_plugin is false
// (the default), this agent is used instead of BBLPrinterAgent.

#include "../IPrinterAgent.hpp"
#include "../ICloudServiceAgent.hpp"
#include "OpenBambuAgent.hpp"

#include <string>
#include <mutex>
#include <memory>

namespace Slic3r {

class OpenBambuPrinterAgent : public IPrinterAgent {
public:
    explicit OpenBambuPrinterAgent(std::string log_dir);
    ~OpenBambuPrinterAgent() override;

    // IPrinterAgent interface
    void set_cloud_agent(std::shared_ptr<ICloudServiceAgent> cloud) override;

    // Communication
    int send_message(std::string dev_id, std::string json_str, int qos, int flag) override;
    int connect_printer(std::string dev_id, std::string dev_ip, std::string username, std::string password, bool use_ssl) override;
    int disconnect_printer() override;
    int send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag) override;

    // Certificates (no-op for OpenBambu)
    int check_cert() override;
    void install_device_cert(std::string dev_id, bool lan_only) override;

    // Discovery
    bool start_discovery(bool start, bool sending) override;

    // Binding (no-op for LAN-only)
    int ping_bind(std::string ping_code) override;
    int bind_detect(std::string dev_ip, std::string sec_link, detectResult& detect) override;
    int bind(std::string dev_ip, std::string dev_id, std::string sec_link, std::string timezone, bool improved, OnUpdateStatusFn update_fn) override;
    int unbind(std::string dev_id) override;
    int request_bind_ticket(std::string* ticket) override;
    int set_server_callback(OnServerErrFn fn) override;

    // Machine Selection
    std::string get_user_selected_machine() override;
    int set_user_selected_machine(std::string dev_id) override;

    // Agent info
    static AgentInfo get_agent_info_static();
    AgentInfo get_agent_info() override { return get_agent_info_static(); }

    // Print Job Operations
    int start_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override;
    int start_local_print_with_record(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override;
    int start_send_gcode_to_sdcard(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override;
    int start_local_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn) override;
    int start_sdcard_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn) override;

    // Callbacks
    int set_on_ssdp_msg_fn(OnMsgArrivedFn fn) override;
    int set_on_printer_connected_fn(OnPrinterConnectedFn fn) override;
    int set_on_subscribe_failure_fn(GetSubscribeFailureFn fn) override;
    int set_on_message_fn(OnMessageFn fn) override;
    int set_on_user_message_fn(OnMessageFn fn) override;
    int set_on_local_connect_fn(OnLocalConnectedFn fn) override;
    int set_on_local_message_fn(OnMessageFn fn) override;
    int set_queue_on_main_fn(QueueOnMainFn fn) override;

private:
    OpenBambu::Agent m_agent;
    std::string m_log_dir;
    std::string m_selected_machine;
    std::shared_ptr<ICloudServiceAgent> m_cloud_agent;

    // Store the last connected printer's access code for FTP uploads
    std::string m_connected_ip;
    std::string m_connected_access_code;
    std::string m_connected_dev_id;

    mutable std::mutex m_mutex;
};

} // namespace Slic3r
