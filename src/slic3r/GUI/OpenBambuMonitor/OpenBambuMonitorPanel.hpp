#pragma once

// OpenBambuMonitorPanel — Alternative Device tab for OpenBambu (LAN-only) mode.
// This is a clone of MonitorPanel with Update and HMS tabs removed,
// and StatusPanel configured in OpenBambu mode (no axis/extruder controls).
// Stock MonitorPanel is used when the proprietary BBL DLL is active.

#include "../Tabbook.hpp"
#include <wx/notebook.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/bmpcbox.h>
#include <wx/bmpbuttn.h>
#include <wx/treectrl.h>
#include <wx/imaglist.h>
#include <wx/artprov.h>
#include <wx/xrc/xmlres.h>
#include <wx/string.h>
#include <wx/stattext.h>
#include <wx/gdicmn.h>
#include <wx/font.h>
#include <wx/colour.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/grid.h>
#include <wx/dataview.h>
#include <wx/panel.h>
#include <wx/statline.h>
#include <wx/bitmap.h>
#include <wx/image.h>
#include <wx/icon.h>
#include <wx/bmpbuttn.h>
#include <wx/button.h>
#include <wx/gbsizer.h>
#include <wx/statbox.h>
#include <wx/tglbtn.h>
#include <wx/popupwin.h>
#include <wx/spinctrl.h>
#include <wx/artprov.h>
#include <wx/webrequest.h>
#include <map>
#include <vector>
#include <memory>
#include "../Event.hpp"
#include "libslic3r/ProjectTask.hpp"
#include "../wxExtensions.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/MonitorBasePanel.h"
#include "slic3r/GUI/StatusPanel.hpp"
#include "slic3r/GUI/AmsWidgets.hpp"
#include "../Widgets/SideTools.hpp"
#include "../SelectMachinePop.hpp"

namespace Slic3r {
namespace GUI {

class MediaFilePanel;

class OpenBambuMonitorPanel : public wxPanel
{
private:
    Tabbook*        m_tabpanel{ nullptr };
    wxSizer*        m_main_sizer{ nullptr };

    StatusPanel*        m_status_info_panel{ nullptr };
    MediaFilePanel*     m_media_file_panel{ nullptr };
    // No UpgradePanel or HMSPanel in OpenBambu mode

    /* side tools */
    SideTools*      m_side_tools{nullptr};
    wxStaticBitmap* m_bitmap_printer_type;
    wxStaticBitmap* m_bitmap_arrow;
    wxStaticText*   m_staticText_printer_name;
    wxStaticBitmap* m_bitmap_wifi_signal;
    wxBoxSizer*     m_side_tools_sizer;
    SelectMachinePopup m_select_machine;

    /* printer list */
    wxScrolledWindow* m_printer_list_panel{nullptr};
    wxBoxSizer*       m_printer_list_sizer{nullptr};
    wxTimer*          m_printer_list_timer{nullptr};
    std::vector<wxWindow*> m_printer_buttons;
    void            update_printer_list();
    void            select_printer_by_id(const std::string &dev_id);

    /* camera loading spinner */
    wxPanel*        m_camera_spinner{nullptr};
    wxTimer*        m_spinner_timer{nullptr};
    int             m_spinner_angle{0};
    void            update_camera_spinner();

    /* images */
    wxBitmap m_signal_strong_img;
    wxBitmap m_signal_middle_img;
    wxBitmap m_signal_weak_img;
    wxBitmap m_signal_no_img;
    wxBitmap m_printer_img;
    wxBitmap m_arrow_img;

    int last_wifi_signal = -1;
    int last_status;
    bool m_initialized { false };
    bool update_flag{false};
    bool m_needs_layout_kick{false}; // set true on printer change, cleared after first data arrives
    wxTimer* m_refresh_timer = nullptr;

public:
    OpenBambuMonitorPanel(wxWindow* parent, wxWindowID id = wxID_ANY, const wxPoint& pos = wxDefaultPosition, const wxSize& size = wxDefaultSize, long style = wxTAB_TRAVERSAL);
    ~OpenBambuMonitorPanel();

    enum PrinterTab {
        PT_STATUS  = 0,
        PT_MEDIA   = 1,
        PT_MAX_NUM = 2
    };

    void init_bitmap();
    void init_timer();
    void init_tabpanel();
    Tabbook* get_tabpanel() { return m_tabpanel; };
    void set_default();
    wxWindow* create_side_tools();

    void on_sys_color_changed();
    void msw_rescale();

    StatusPanel* get_status_panel() {return m_status_info_panel;};
    void select_machine(std::string machine_sn);
    void on_timer(wxTimerEvent& event);
    void on_select_printer(wxCommandEvent& event);
    void on_printer_clicked(wxMouseEvent &event);
    void on_size(wxSizeEvent &event);

    /* update apis */
    void update_all();

    bool Show(bool show);

    void show_status(int status);

    std::string get_string_from_tab(PrinterTab tab);

    void jump_to_LiveView();

    MachineObject *obj { nullptr };
    std::string last_conn_type = "undefined";

    void stop_update() {update_flag = false;};
    void start_update() {update_flag = true;};

    // Camera auto-start (same as stock MonitorPanel)
    enum class CameraAutoState { OFF, ACTIVE };
    CameraAutoState m_camera_auto_state = CameraAutoState::OFF;
    bool m_camera_user_stopped = false;
    bool m_was_printing = false;

    void check_camera_auto_start();
};

} // GUI
} // Slic3r
