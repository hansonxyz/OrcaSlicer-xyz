// OpenBambuMonitorPanel — Clone of MonitorPanel for OpenBambu (LAN-only) mode.
// Differences from stock MonitorPanel:
// - No "Update" (firmware) tab
// - No "Assistant(HMS)" tab
// - StatusPanel configured in OpenBambu mode (no axis/extruder/bed controls)

#include "../Tab.hpp"
#include "../TabButton.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/AppConfig.hpp"
#include "slic3r/Utils/bambu_networking.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"

#include <wx/app.h>
#include <wx/button.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>

#include <wx/bmpcbox.h>
#include <wx/bmpbuttn.h>
#include <wx/treectrl.h>
#include <wx/imaglist.h>
#include <wx/settings.h>
#include <wx/filedlg.h>
#include <wx/wupdlock.h>
#include <wx/dataview.h>
#include <wx/tglbtn.h>

#include "../wxExtensions.hpp"
#include "../GUI_App.hpp"
#include "../GUI_ObjectList.hpp"
#include "../Plater.hpp"
#include "../MainFrame.hpp"
#include "../Widgets/Label.hpp"
#include "../format.hpp"
#include "../MediaPlayCtrl.h"
#include "../MediaFilePanel.h"
#include "../BindDialog.hpp"

#include "../DeviceCore/DevManager.h"

#include "OpenBambuMonitorPanel.hpp"

namespace Slic3r {
namespace GUI {

#define REFRESH_INTERVAL       1000

OpenBambuMonitorPanel::OpenBambuMonitorPanel(wxWindow* parent, wxWindowID id, const wxPoint& pos, const wxSize& size, long style)
    : wxPanel(parent, id, pos, size, style),
    m_select_machine(SelectMachinePopup(this))
{
#ifdef __WINDOWS__
    SetDoubleBuffered(true);
#endif //__WINDOWS__

    init_bitmap();
    init_tabpanel();

    m_main_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_main_sizer->Add(m_tabpanel, 1, wxEXPAND | wxLEFT, 0);
    SetSizer(m_main_sizer);

    init_timer();

    m_side_tools->get_panel()->Connect(wxEVT_LEFT_DOWN, wxMouseEventHandler(OpenBambuMonitorPanel::on_printer_clicked), NULL, this);

    Bind(wxEVT_TIMER, [this](wxTimerEvent& e) {
        if (m_printer_list_timer && e.GetTimer().GetId() == m_printer_list_timer->GetId())
            update_printer_list();
        else if (m_spinner_timer && e.GetTimer().GetId() == m_spinner_timer->GetId()) {
            m_spinner_angle = (m_spinner_angle + 30) % 360;
            if (m_camera_spinner) m_camera_spinner->Refresh();
        }
        else
            on_timer(e);
    });
    Bind(wxEVT_SIZE, &OpenBambuMonitorPanel::on_size, this);
    Bind(wxEVT_COMMAND_CHOICE_SELECTED, &OpenBambuMonitorPanel::on_select_printer, this);

    m_select_machine.Bind(EVT_FINISHED_UPDATE_MACHINE_LIST, [this](wxCommandEvent& e) {
        m_side_tools->start_interval();
        });

    // No HMS event binding in OpenBambu mode
}

OpenBambuMonitorPanel::~OpenBambuMonitorPanel()
{
    m_side_tools->get_panel()->Disconnect(wxEVT_LEFT_DOWN, wxMouseEventHandler(OpenBambuMonitorPanel::on_printer_clicked), NULL, this);

    if (m_refresh_timer)
        m_refresh_timer->Stop();
    delete m_refresh_timer;

    if (m_printer_list_timer)
        m_printer_list_timer->Stop();
    delete m_printer_list_timer;

    if (m_spinner_timer)
        m_spinner_timer->Stop();
    delete m_spinner_timer;
}

void OpenBambuMonitorPanel::init_bitmap()
{
    m_signal_strong_img = create_scaled_bitmap("monitor_signal_strong", nullptr, 24);
    m_signal_middle_img = create_scaled_bitmap("monitor_signal_middle", nullptr, 24);
    m_signal_weak_img = create_scaled_bitmap("monitor_signal_weak", nullptr, 24);
    m_signal_no_img   = create_scaled_bitmap("monitor_signal_no", nullptr, 24);
    m_printer_img = create_scaled_bitmap("monitor_printer", nullptr, 26);
    m_arrow_img = create_scaled_bitmap("monitor_arrow",nullptr, 14);
}

void OpenBambuMonitorPanel::init_timer()
{
    m_refresh_timer = new wxTimer();
    m_refresh_timer->SetOwner(this);
    m_refresh_timer->Start(REFRESH_INTERVAL);
    if (update_flag) { update_all();}
}

void OpenBambuMonitorPanel::init_tabpanel()
{
    // Create a sidebar sizer with the printer list (no SideTools dropdown)
    m_side_tools = new SideTools(this, wxID_ANY);
    m_side_tools->Hide(); // Hide the old printer selector

    m_tabpanel = new Tabbook(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, nullptr, wxNB_LEFT | wxTAB_TRAVERSAL | wxNB_NOPAGETHEME);
    m_side_tools->set_table_panel(m_tabpanel);
    m_tabpanel->SetBackgroundColour(wxColour("#FEFFFF"));
    m_tabpanel->Bind(wxEVT_BOOKCTRL_PAGE_CHANGED, [this](wxBookCtrlEvent& e) {
        auto page = m_tabpanel->GetCurrentPage();
        if (page == m_media_file_panel) {
            auto title = m_tabpanel->GetPageText(m_tabpanel->GetSelection());
            m_media_file_panel->SwitchStorage(title == _L("Storage"));
        }
        page->SetFocus();
        update_all();
        }, m_tabpanel->GetId());

    // Status tab — with OpenBambu mode (no axis/extruder controls)
    m_status_info_panel = new StatusPanel(m_tabpanel);
    m_status_info_panel->set_openbambu_mode(true);
    m_tabpanel->AddPage(m_status_info_panel, _L("Status"), "", true);

    // Storage tab
    m_media_file_panel = new MediaFilePanel(m_tabpanel);
    m_tabpanel->AddPage(m_media_file_panel, _L("Storage"), "", false);

    m_tabpanel->SetFooterText(_L("OpenBambu (LAN Only)"));

    // Add printer list below the tab buttons in the sidebar
    auto *btns_ctrl = m_tabpanel->GetBtnsListCtrl();
    auto *sidebar_sizer = btns_ctrl->GetSizer();
    if (sidebar_sizer) {
        // Separator line
        auto *sep = new wxPanel(btns_ctrl, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
        sep->SetBackgroundColour(wxColour(200, 200, 200));
        sidebar_sizer->Insert(sidebar_sizer->GetItemCount() - 1, sep, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, FromDIP(8));

        // Printer list panel (plain panel, auto-sizes to content)
        m_printer_list_panel = new wxScrolledWindow(btns_ctrl, wxID_ANY);
        m_printer_list_panel->SetScrollRate(0, 0); // no scrolling, just auto-size
        m_printer_list_panel->SetBackgroundColour(wxColour("#FEFFFF"));
        m_printer_list_sizer = new wxBoxSizer(wxVERTICAL);
        m_printer_list_panel->SetSizer(m_printer_list_sizer);

        // Insert before footer text (last item in sizer), takes remaining space
        sidebar_sizer->Insert(sidebar_sizer->GetItemCount() - 1, m_printer_list_panel, 0, wxEXPAND, 0);
    }

    // Printer list refresh timer — every 20 seconds
    m_printer_list_timer = new wxTimer(this, wxNewId());

    // Camera loading spinner — 3 bouncing gray dots
    m_camera_spinner = new wxPanel(m_status_info_panel, wxID_ANY, wxDefaultPosition, wxSize(100, 40));
    m_camera_spinner->SetBackgroundStyle(wxBG_STYLE_PAINT);
    m_camera_spinner->Hide();
    m_camera_spinner->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
        wxAutoBufferedPaintDC dc(m_camera_spinner);
        dc.SetBackground(wxBrush(*wxBLACK));
        dc.Clear();
        int w = m_camera_spinner->GetSize().x;
        int h = m_camera_spinner->GetSize().y;
        int dot_r = 8;
        int spacing = 30;
        int base_y = h / 2;
        // 3 dots with phase-offset bounce
        for (int i = 0; i < 3; i++) {
            double phase = (m_spinner_angle + i * 120) % 360;
            double bounce = sin(phase * M_PI / 180.0);
            int y = base_y - (int)(10.0 * std::max(0.0, bounce));
            int x = w / 2 + (i - 1) * spacing;
            // Fade based on bounce height
            int alpha = 120 + (int)(135 * std::max(0.0, bounce));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(wxColour(160, 160, 160, alpha)));
            dc.DrawCircle(x, y, dot_r);
        }
    });
    m_spinner_timer = new wxTimer(this, wxNewId());

    m_initialized = true;
    show_status((int)MonitorStatus::MONITOR_NO_PRINTER);
}

void OpenBambuMonitorPanel::set_default()
{
    obj = nullptr;
    last_conn_type = "undefined";
    m_status_info_panel->set_default();
}

wxWindow* OpenBambuMonitorPanel::create_side_tools()
{
    wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);
    auto        panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(0, FromDIP(50)));
    panel->SetBackgroundColour(wxColour(135,206,250));
    panel->SetSizer(sizer);
    sizer->Layout();
    panel->Fit();
    return panel;
}

void OpenBambuMonitorPanel::on_sys_color_changed()
{
    m_status_info_panel->on_sys_color_changed();
    m_media_file_panel->Rescale();
}

void OpenBambuMonitorPanel::msw_rescale()
{
    init_bitmap();

    m_side_tools->msw_rescale();
    m_tabpanel->Rescale();
    m_status_info_panel->msw_rescale();
    m_media_file_panel->Rescale();

    Layout();
    Refresh();
}

void OpenBambuMonitorPanel::select_machine(std::string machine_sn)
{
    wxCommandEvent *event = new wxCommandEvent(wxEVT_COMMAND_CHOICE_SELECTED);
    event->SetString(machine_sn);
    wxQueueEvent(this, event);
}

void OpenBambuMonitorPanel::on_timer(wxTimerEvent& event)
{
    if (update_flag) {
        update_all();
    }
}

void OpenBambuMonitorPanel::on_select_printer(wxCommandEvent& event)
{
    Slic3r::DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (!dev) return;

    if (!dev->set_selected_machine(event.GetString().ToStdString()))
        return;

    set_default();
    update_all();

    MachineObject *obj_ = dev->get_selected_machine();
    if (obj_) {
        obj_->last_cali_version = -1;
        obj_->reset_pa_cali_history_result();
        obj_->reset_pa_cali_result();
        Sidebar &sidebar = GUI::wxGetApp().sidebar();
        sidebar.update_sync_status(obj_);
        sidebar.set_need_auto_sync_after_connect_printer(sidebar.need_auto_sync_extruder_list_after_connect_priner(obj_));
    }

    Layout();
}

void OpenBambuMonitorPanel::on_printer_clicked(wxMouseEvent &event)
{
    auto mouse_pos = ClientToScreen(event.GetPosition());
    wxPoint rect = m_side_tools->ClientToScreen(wxPoint(0, 0));

    if (!m_side_tools->is_in_interval()) {
        wxPoint pos = m_side_tools->ClientToScreen(wxPoint(0, 0));
        pos.y += m_side_tools->GetRect().height;
        m_select_machine.Move(pos);

#ifdef __linux__
        m_select_machine.SetSize(wxSize(m_side_tools->GetSize().x, -1));
        m_select_machine.SetMaxSize(wxSize(m_side_tools->GetSize().x, -1));
        m_select_machine.SetMinSize(wxSize(m_side_tools->GetSize().x, -1));
#endif

        m_select_machine.Popup();
    }
}

void OpenBambuMonitorPanel::on_size(wxSizeEvent &event)
{
    Layout();
    event.Skip();
}

void OpenBambuMonitorPanel::update_all()
{
    if (!m_initialized)
        return;

    Slic3r::DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (!dev) return;
    obj = dev->get_selected_machine();

    if (!obj) {
        show_status((int)MONITOR_NO_PRINTER);
        if (m_status_info_panel->IsShown()) {
            m_status_info_panel->m_media_play_ctrl->SetMachineObject(obj);
            m_status_info_panel->update(obj);
        }
        return;
    }

    if (obj->connection_type() != last_conn_type) { last_conn_type = obj->connection_type(); }

    m_side_tools->update_status(obj);

    if (obj->is_connecting()) {
        show_status(MONITOR_CONNECTING);
        return;
    } else if (!obj->is_connected()) {
        show_status((int) MONITOR_DISCONNECTED);
        return;
    }

    show_status(MONITOR_NORMAL);

    auto current_page = m_tabpanel->GetCurrentPage();
    if (current_page == m_status_info_panel) {
        if (m_status_info_panel->IsShown()) {
            m_status_info_panel->obj = obj;
            m_status_info_panel->m_media_play_ctrl->SetMachineObject(obj);
            m_status_info_panel->update(obj);
        }
    } else if (current_page == m_media_file_panel) {
        m_media_file_panel->UpdateByObj(obj);
    }

    // Update camera loading spinner and printer list highlight
    update_camera_spinner();
    update_printer_list();

    // After first data arrives for a new printer, trigger a layout refresh
    // so AMS cards and other dynamic content render correctly.
    // Also auto-start the camera feed if the preference is set.
    if (m_needs_layout_kick && obj->is_info_ready()) {
        m_needs_layout_kick = false;
        CallAfter([this]() {
            wxWindow *w = this;
            for (int i = 0; i < 4 && w; i++) {
                wxSizeEvent evt(w->GetSize());
                evt.SetEventObject(w);
                w->GetEventHandler()->ProcessEvent(evt);
                w = w->GetParent();
            }

            // Auto-start camera if preference is set
            if (wxGetApp().app_config->get_bool("auto_start_camera")) {
                auto *ctrl = m_status_info_panel->get_media_play_ctrl();
                if (ctrl && ctrl->is_idle()) {
                    try { ctrl->jump_to_play(); } catch (...) {}
                }
            }
        });
    }
}

bool OpenBambuMonitorPanel::Show(bool show)
{
    BOOST_LOG_TRIVIAL(info) << "OpenBambuMonitorPanel::Show(" << show << ") called";
#ifdef __APPLE__
    wxGetApp().mainframe->SetMinSize(wxGetApp().plater()->GetMinSize());
#endif

    DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (show) {
        start_update();

        m_refresh_timer->Stop();
        m_refresh_timer->SetOwner(this);
        m_refresh_timer->Start(REFRESH_INTERVAL);
        if (update_flag) { update_all(); }

        // Trigger layout kick when data arrives
        m_needs_layout_kick = true;

        // Start printer list refresh
        update_printer_list();
        if (m_printer_list_timer) {
            m_printer_list_timer->Start(20000); // refresh every 20 seconds
        }

        if (dev) {
            obj = dev->get_selected_machine();
            if (obj == nullptr) {
                dev->load_last_machine();
                obj = dev->get_selected_machine();
            } else {
                obj->reset_update_time();
            }
        }

        // Deferred layout kick — initial show often has stale sizes from
        // construction. CallAfter runs after the current event loop iteration,
        // by which time the page has its real size from the Tabbook.
        CallAfter([this]() {
            if (auto *parent = GetParent()) {
                SetSize(parent->GetClientSize());
            }
            Layout();
            Refresh();
        });
    } else {
        stop_update();
        m_refresh_timer->Stop();
        if (m_printer_list_timer) m_printer_list_timer->Stop();

        // Stop camera feed when leaving the Device tab
        if (m_status_info_panel) {
            auto *ctrl = m_status_info_panel->get_media_play_ctrl();
            if (ctrl) {
                try {
                    ctrl->SetMachineObject(nullptr);
                    ctrl->stop_stream();
                } catch (...) {}
            }
        }
    }
    return wxPanel::Show(show);
}

void OpenBambuMonitorPanel::update_printer_list()
{
    if (!m_printer_list_panel) return;

    Slic3r::DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (!dev) return;

    auto machines = dev->get_local_machinelist();
    std::string selected_id;
    if (auto *sel = dev->get_selected_machine())
        selected_id = sel->get_dev_id();

    // Check if list actually changed
    if (m_printer_buttons.size() == machines.size()) {
        bool same = true;
        size_t i = 0;
        for (auto &pair : machines) {
            if (i >= m_printer_buttons.size() || m_printer_buttons[i]->GetName() != pair.first) {
                same = false;
                break;
            }
            i++;
        }
        if (same) {
            // Just update selection highlight
            for (auto *w : m_printer_buttons) {
                auto *btn = dynamic_cast<TabButton*>(w);
                if (!btn) continue;
                bool is_sel = (btn->GetName() == selected_id);
                btn->SetBackgroundColor(is_sel ? wxColour("#BFE1DE") : wxColour("#FEFFFF"));
                btn->Refresh();
            }
            return;
        }
    }

    // Rebuild the list
    m_printer_list_sizer->Clear(true);
    m_printer_buttons.clear();

    // Use TabButton — the exact same widget as Status/Storage buttons
    static const wxColour BG_NORMAL("#FEFFFF");
    static const wxColour BG_SELECTED("#BFE1DE");
    ScalableBitmap arrow_img(m_printer_list_panel, "monitor_arrow", 14);
    int em = em_unit(m_printer_list_panel);

    for (auto &pair : machines) {
        auto *machine = pair.second;
        if (!machine) continue;

        std::string dev_id = machine->get_dev_id();
        bool is_sel = (dev_id == selected_id);

        auto *btn = new TabButton(m_printer_list_panel, wxString::FromUTF8(dev_id), arrow_img, wxNO_BORDER);
        btn->SetCornerRadius(0);
        btn->SetMinSize({220 * em / 10, 46 * em / 10});
        btn->SetBackgroundColor(is_sel ? BG_SELECTED : BG_NORMAL);
        btn->SetTextColor(*wxBLACK);
        btn->SetName(dev_id);

        btn->Bind(wxEVT_BUTTON, [this, dev_id](wxCommandEvent&) {
            select_printer_by_id(dev_id);
        });

        m_printer_list_sizer->Add(btn, 0, wxEXPAND | wxLEFT, 0);
        m_printer_buttons.push_back(btn);
    }

    m_printer_list_panel->Layout();
    m_printer_list_panel->Fit();
    // Re-layout the sidebar to accommodate the new button heights
    if (auto *parent = m_printer_list_panel->GetParent())
        parent->Layout();
}

void OpenBambuMonitorPanel::select_printer_by_id(const std::string &dev_id)
{
    Slic3r::DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (!dev) return;

    // Stop current camera feed BEFORE switching machines
    auto *ctrl = m_status_info_panel->get_media_play_ctrl();
    if (ctrl) {
        try {
            ctrl->SetMachineObject(nullptr);
            ctrl->stop_stream();
        } catch (...) {}
    }

    if (!dev->set_selected_machine(dev_id))
        return;

    set_default();
    m_needs_layout_kick = true;
    update_all();
    update_printer_list(); // refresh highlights

    MachineObject *obj_ = dev->get_selected_machine();
    if (obj_) {
        obj_->last_cali_version = -1;
        obj_->reset_pa_cali_history_result();
        obj_->reset_pa_cali_result();
        Sidebar &sidebar = GUI::wxGetApp().sidebar();
        sidebar.update_sync_status(obj_);
        sidebar.set_need_auto_sync_after_connect_printer(sidebar.need_auto_sync_extruder_list_after_connect_priner(obj_));
    }

    Layout();
}

void OpenBambuMonitorPanel::update_camera_spinner()
{
    if (!m_camera_spinner || !m_status_info_panel) return;

    auto *ctrl = m_status_info_panel->get_media_play_ctrl();
    bool loading = ctrl && !ctrl->is_idle() && !ctrl->IsStreaming();

    if (loading) {
        // Position spinner centered on the camera widget
        auto *cam = m_status_info_panel->get_media_ctrl();
        if (cam && cam->IsShown()) {
            wxPoint cam_pos = cam->GetScreenPosition();
            wxPoint panel_pos = m_status_info_panel->GetScreenPosition();
            wxSize cam_size = cam->GetSize();
            wxSize spin_size = m_camera_spinner->GetSize();
            int x = (cam_pos.x - panel_pos.x) + (cam_size.x - spin_size.x) / 2;
            int y = (cam_pos.y - panel_pos.y) + (cam_size.y - spin_size.y) / 2;
            m_camera_spinner->SetPosition(wxPoint(x, y));
        }
        if (!m_camera_spinner->IsShown()) {
            m_camera_spinner->Show();
            m_camera_spinner->Raise();
            m_spinner_timer->Start(50);
        }
    } else if (m_camera_spinner->IsShown()) {
        m_camera_spinner->Hide();
        m_spinner_timer->Stop();
    }
}

void OpenBambuMonitorPanel::show_status(int status)
{
    if (!m_initialized) return;
    if (last_status == status) return;
    last_status = status;

    BOOST_LOG_TRIVIAL(info) << "OpenBambuMonitorPanel: show_status = " << status;

    if (m_side_tools) { m_side_tools->show_status(status); };
    m_status_info_panel->show_status(status);

    if ((status & (int)MonitorStatus::MONITOR_NO_PRINTER) != 0) {
        set_default();
        m_tabpanel->Layout();
    } else if (((status & (int)MonitorStatus::MONITOR_NORMAL) != 0)
        || ((status & (int)MonitorStatus::MONITOR_DISCONNECTED) != 0)
        || ((status & (int)MonitorStatus::MONITOR_CONNECTING) != 0) )
    {
        if (((status & (int)MonitorStatus::MONITOR_DISCONNECTED) != 0)
            || ((status & (int)MonitorStatus::MONITOR_CONNECTING) != 0))
        {
            set_default();
        }
        m_tabpanel->Layout();
    }
    Layout();
}

std::string OpenBambuMonitorPanel::get_string_from_tab(PrinterTab tab)
{
    switch (tab) {
    case PT_STATUS:
        return "status";
    case PT_MEDIA:
        return "sd_card";
    default:
        return "";
    }
    return "";
}

void OpenBambuMonitorPanel::jump_to_LiveView()
{
    if (!this->IsShown()) { return; }

    auto page = m_tabpanel->GetCurrentPage();
    if (page) {
        m_tabpanel->SetSelection(PT_STATUS);
    }

    m_status_info_panel->get_media_play_ctrl()->jump_to_play();
}

void OpenBambuMonitorPanel::check_camera_auto_start()
{
    if (!this->IsShown()) return;
    if (!wxGetApp().app_config->get_bool("auto_start_camera")) return;
    if (m_camera_user_stopped) return;

    DeviceManager *dev = wxGetApp().getDeviceManager();
    if (!dev) return;
    MachineObject *machine = dev->get_selected_machine();
    if (!machine || !MachineObject::is_in_printing_status(machine->print_status))
        return;

    auto *ctrl = m_status_info_panel->get_media_play_ctrl();
    if (!ctrl) return;

    if (ctrl->is_idle()) {
        try {
            ctrl->jump_to_play();
            m_camera_auto_state = CameraAutoState::ACTIVE;
        } catch (...) {
            // Suppress errors on automatic start
        }
    } else {
        m_camera_auto_state = CameraAutoState::ACTIVE;
    }
}

} // GUI
} // Slic3r
