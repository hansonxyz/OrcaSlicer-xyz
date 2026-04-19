#pragma once

#include <wx/panel.h>
#include <wx/webview.h>
#include <wx/sizer.h>
#include <string>

namespace Slic3r {
namespace GUI {

class DebugConsolePanel : public wxPanel
{
public:
    DebugConsolePanel(wxWindow *parent);
    ~DebugConsolePanel();

    void on_show();

private:
    wxWebView *m_browser{nullptr};

    void on_script_message(wxWebViewEvent &evt);
    std::string handle_command(const std::string &cmd);
    std::string cmd_help();
    std::string cmd_printers();
    std::string cmd_hello();
    std::string cmd_ssdp();

    void write_to_terminal(const std::string &text);
};

} // namespace GUI
} // namespace Slic3r
