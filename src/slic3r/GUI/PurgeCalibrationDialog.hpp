#pragma once

#include <wx/dialog.h>
#include <wx/webview.h>
#include <vector>
#include <string>

namespace Slic3r {
namespace GUI {

// xyz fork: Purge calibration test print setup wizard (WebView-based).
class PurgeCalibrationDialog : public wxDialog {
public:
    PurgeCalibrationDialog(wxWindow *parent);

private:
    wxWebView *m_webview = nullptr;
    wxString build_init_data();
    void on_message(const std::string &message);
};

void open_purge_calibration_dialog(wxWindow *parent);
void open_purge_calibration_result_dialog(wxWindow *parent);

} // namespace GUI
} // namespace Slic3r
