#include "DebugConsolePanel.hpp"
#include "GUI_App.hpp"
#include "Widgets/WebView.hpp"
#include "DeviceManager.hpp"
#include "DeviceCore/DevManager.h"
#include "slic3r/Utils/NetworkAgent.hpp"
#include "slic3r/Utils/OpenBambu/OpenBambuPrinterAgent.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/log/trivial.hpp>
#include <boost/filesystem.hpp>
#include <wx/filename.h>
#include <wx/filesys.h>
#include <sstream>

namespace fs = boost::filesystem;

namespace Slic3r {
namespace GUI {

DebugConsolePanel::DebugConsolePanel(wxWindow *parent)
    : wxPanel(parent, wxID_ANY)
{
    SetBackgroundColour(wxColour(30, 30, 30));

    auto *sizer = new wxBoxSizer(wxVERTICAL);

    fs::path html_path = fs::path(resources_dir()) / "web" / "debug" / "index.html";
    wxString url;
    wxFileName fn(wxString::FromUTF8(html_path.string()));
    if (fn.FileExists()) {
        url = wxFileSystem::FileNameToURL(fn);
    } else {
        BOOST_LOG_TRIVIAL(error) << "DebugConsolePanel: HTML not found at " << html_path.string();
        url = "about:blank";
    }

    m_browser = WebView::CreateWebView(this, "about:blank");
    if (!m_browser) {
        BOOST_LOG_TRIVIAL(error) << "DebugConsolePanel: failed to create WebView";
        SetSizer(sizer);
        return;
    }

    m_browser->AddScriptMessageHandler("debugConsole");
    m_browser->Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &DebugConsolePanel::on_script_message, this);

    sizer->Add(m_browser, 1, wxEXPAND);
    SetSizer(sizer);

    // Load the debug console HTML after the script handler is registered
    m_browser->LoadURL(url);
}

DebugConsolePanel::~DebugConsolePanel()
{
    SetEvtHandlerEnabled(false);
}

void DebugConsolePanel::on_show()
{
    if (m_browser) {
        m_browser->Show();
        Layout();
    }
}

void DebugConsolePanel::on_script_message(wxWebViewEvent &evt)
{
    std::string cmd = evt.GetString().ToUTF8().data();
    BOOST_LOG_TRIVIAL(debug) << "DebugConsole command: " << cmd;

    std::string response = handle_command(cmd);
    write_to_terminal(response);
}

static std::string escape_js(const std::string &s)
{
    std::string out;
    out.reserve(s.size() + 20);
    for (char c : s) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '\'': out += "\\'"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        default:   out += c; break;
        }
    }
    return out;
}

void DebugConsolePanel::write_to_terminal(const std::string &text)
{
    if (!m_browser) return;
    std::string js = "window.termWrite('" + escape_js(text) + "');";
    WebView::RunScript(m_browser, wxString::FromUTF8(js));
}

std::string DebugConsolePanel::handle_command(const std::string &cmd)
{
    if (cmd == "hello")    return cmd_hello();
    if (cmd == "printers") return cmd_printers();
    if (cmd == "ssdp")     return cmd_ssdp();
    if (cmd == "help")     return cmd_help();

    return "\x1b[31mUnknown command: " + cmd + "\x1b[0m\r\nType \x1b[32mhelp\x1b[0m for available commands.";
}

std::string DebugConsolePanel::cmd_hello()
{
    return "Hello world!";
}

std::string DebugConsolePanel::cmd_help()
{
    std::string out;
    out += "\x1b[33mAvailable commands:\x1b[0m\r\n";
    out += "  \x1b[32mhello\x1b[0m      - Hello world test\r\n";
    out += "  \x1b[32mprinters\x1b[0m   - List all printers and online status\r\n";
    out += "  \x1b[32mssdp\x1b[0m       - SSDP discovery diagnostics\r\n";
    out += "  \x1b[32mhelp\x1b[0m       - Show this help";
    return out;
}

std::string DebugConsolePanel::cmd_printers()
{
    Slic3r::DeviceManager *dev = wxGetApp().getDeviceManager();
    if (!dev) return "\x1b[31mDeviceManager not available\x1b[0m";

    auto machines = dev->get_local_machinelist();
    if (machines.empty()) return "\x1b[33mNo printers found\x1b[0m";

    auto *selected = dev->get_selected_machine();
    std::string selected_id = selected ? selected->get_dev_id() : "";

    std::string out;
    out += "\x1b[33mPrinters:\x1b[0m";

    for (auto &pair : machines) {
        auto *m = pair.second;
        if (!m) continue;

        std::string id   = m->get_dev_id();
        std::string name = m->get_dev_name();
        bool online      = m->m_is_online;
        bool connected   = m->is_connected();
        bool is_selected = (id == selected_id);

        out += "\r\n  ";
        if (is_selected) out += "\x1b[36m> \x1b[0m";
        else             out += "  ";

        out += "\x1b[1m" + name + "\x1b[0m";
        out += " (" + id + ")";

        if (online && connected)
            out += " \x1b[32m[online, connected]\x1b[0m";
        else if (online)
            out += " \x1b[33m[online, not connected]\x1b[0m";
        else
            out += " \x1b[31m[offline]\x1b[0m";
    }

    return out;
}

std::string DebugConsolePanel::cmd_ssdp()
{
    auto *agent = wxGetApp().getAgent();
    if (!agent) return "\x1b[31mNetworkAgent not available\x1b[0m";

    auto printer_agent = agent->get_printer_agent();
    if (!printer_agent) return "\x1b[31mNo printer agent loaded\x1b[0m";

    auto *ob = dynamic_cast<Slic3r::OpenBambuPrinterAgent *>(printer_agent.get());
    if (!ob) return "\x1b[33mNot using OpenBambu agent (using proprietary DLL)\x1b[0m";

    const auto &disc = ob->agent().discovery();
    const auto &d = disc.diag();
    auto now = std::chrono::steady_clock::now();

    std::string out;
    out += "\x1b[33mSSDP Discovery Diagnostics:\x1b[0m\r\n";
    out += "  Running: " + std::string(disc.is_running() ? "\x1b[32myes\x1b[0m" : "\x1b[31mno\x1b[0m") + "\r\n";

    // Interface info
    int ifaces = d.interfaces_found.load();
    out += "  Interfaces:       " + std::to_string(ifaces) + " (1 socket each)\r\n";

    // Send info
    int send_rc = d.last_send_result.load();
    int send_err = d.last_send_error.load();
    out += "  Probes sent:      " + std::to_string(d.probes_sent.load());
    if (send_rc < 0)
        out += " \x1b[31m(last sendto failed, err " + std::to_string(send_err) + ")\x1b[0m";
    else if (send_rc > 0)
        out += " (last sendto: " + std::to_string(send_rc) + " bytes)";
    out += "\r\n";

    // Receive info
    out += "  Packets received: " + std::to_string(d.packets_received.load()) + "\r\n";
    out += "  Recv timeouts:    " + std::to_string(d.recv_timeouts.load()) + "\r\n";
    uint64_t rerr = d.recv_errors.load();
    if (rerr > 0)
        out += "  Recv errors:      \x1b[31m" + std::to_string(rerr)
            + " (last err " + std::to_string(d.last_recv_error.load()) + ")\x1b[0m\r\n";
    out += "  Bambu responses:  " + std::to_string(d.bambu_responses.load()) + "\r\n";
    out += "  Callbacks fired:  " + std::to_string(d.callbacks_fired.load()) + "\r\n";
    out += "  Dedup suppressed: " + std::to_string(d.dedup_suppressed.load()) + "\r\n";

    if (d.last_probe_time != std::chrono::steady_clock::time_point{}) {
        auto secs = std::chrono::duration_cast<std::chrono::seconds>(now - d.last_probe_time).count();
        out += "  Last probe:       " + std::to_string(secs) + "s ago\r\n";
    } else {
        out += "  Last probe:       never\r\n";
    }

    if (d.last_response_time != std::chrono::steady_clock::time_point{}) {
        auto secs = std::chrono::duration_cast<std::chrono::seconds>(now - d.last_response_time).count();
        out += "  Last response:    " + std::to_string(secs) + "s ago\r\n";
    } else {
        out += "  Last response:    \x1b[31mnever\x1b[0m\r\n";
    }

    auto last_seen = disc.get_last_seen();
    if (last_seen.empty()) {
        out += "\r\n  \x1b[31mNo devices seen\x1b[0m";
    } else {
        out += "\r\n  \x1b[33mDevices seen:\x1b[0m";
        for (auto &pair : last_seen) {
            auto secs = std::chrono::duration_cast<std::chrono::seconds>(now - pair.second).count();
            out += "\r\n    " + pair.first + " (" + std::to_string(secs) + "s ago)";
        }
    }

    return out;
}

} // namespace GUI
} // namespace Slic3r
