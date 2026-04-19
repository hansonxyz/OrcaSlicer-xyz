#pragma once

// OpenBambu Device Tab — custom monitor panel for OpenBambu LAN mode.
// Replaces the stock MonitorPanel when bambu_connection_mode == "openbambu".

#include "OpenBambuStatus.hpp"
#include <wx/panel.h>
#include <wx/timer.h>
#include <wx/splitter.h>
#include <wx/listbox.h>
#include <wx/stattext.h>
#include <wx/button.h>
#include <wx/sizer.h>
#include <wx/scrolwin.h>
#include <mutex>
#include <map>

namespace Slic3r { namespace GUI {

class MediaPlayCtrl;

class OpenBambuMonitorPanel : public wxPanel {
public:
    OpenBambuMonitorPanel(wxWindow *parent);
    ~OpenBambuMonitorPanel() override;

    // Called by MainFrame when tab is shown/hidden
    bool Show(bool show = true) override;

private:
    void build_ui();
    void build_sidebar(wxWindow *parent, wxBoxSizer *sizer);
    void build_content(wxWindow *parent, wxBoxSizer *sizer);

    // Timer-driven UI refresh (1 Hz)
    void on_timer(wxTimerEvent &evt);

    // MQTT status callback (from background thread — must marshal to main thread)
    void on_mqtt_status(const std::string &dev_id, const std::string &payload);

    // SSDP discovery callback
    void on_printer_discovered(const std::string &json);

    // Update all UI elements from current status
    void refresh_ui();

    // Refresh the printer list display
    void refresh_printer_list();

    // Draw temperature sparkline on a wxPanel
    void paint_sparkline(wxPanel *panel, const TempHistory &history, wxColour color);

    // Printer list selection
    void on_printer_selected(int index);
    void on_printer_pair(const std::string &serial);

    // Controls
    void on_pause_resume();
    void on_stop();
    void on_speed_change(int level);
    void on_light_toggle();
    void on_unload_filament();

    // Storage
    void on_browse_storage();
    void on_storage_file_action(const std::string &filename);

    // Current printer status (updated from MQTT thread, read from UI thread)
    PrinterStatus m_status;
    std::mutex m_status_mutex;

    // Discovered printers
    std::map<std::string, DiscoveredPrinter> m_printers; // keyed by serial
    std::mutex m_printers_mutex;
    std::string m_selected_serial;

    // UI elements — sidebar
    wxScrolledWindow *m_sidebar = nullptr;
    wxStaticText *m_lbl_printer_name = nullptr;
    wxStaticText *m_lbl_print_status = nullptr;
    wxStaticText *m_lbl_nozzle_temp = nullptr;
    wxStaticText *m_lbl_bed_temp = nullptr;
    wxStaticText *m_lbl_layer = nullptr;
    wxStaticText *m_lbl_time_left = nullptr;
    wxStaticText *m_lbl_speed = nullptr;
    wxStaticText *m_lbl_fan = nullptr;
    wxPanel *m_nozzle_sparkline = nullptr;
    wxPanel *m_bed_sparkline = nullptr;
    wxListBox *m_printer_listbox = nullptr;
    wxListBox *m_storage_listbox = nullptr;

    // UI elements — content
    wxPanel *m_camera_panel = nullptr;
    wxPanel *m_preview_placeholder = nullptr;
    wxButton *m_btn_pause = nullptr;
    wxButton *m_btn_stop = nullptr;
    wxButton *m_btn_light = nullptr;
    wxButton *m_btn_unload = nullptr;

    wxTimer m_refresh_timer;

    // Storage file list cache
    std::vector<std::string> m_storage_files;
    bool m_storage_loaded = false;
};

}} // namespace Slic3r::GUI
