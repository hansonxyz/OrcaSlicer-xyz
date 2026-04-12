// OpenBambuMonitorPanel — Clone of MonitorPanel for OpenBambu (LAN-only) mode.
// Differences from stock MonitorPanel:
// - No "Update" (firmware) tab
// - No "Assistant(HMS)" tab
// - StatusPanel configured in OpenBambu mode (no axis/extruder/bed controls)

#include "../Tab.hpp"
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

    Bind(wxEVT_TIMER, &OpenBambuMonitorPanel::on_timer, this);
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
    m_side_tools = new SideTools(this, wxID_ANY);
    wxBoxSizer* sizer_side_tools = new wxBoxSizer(wxVERTICAL);
    sizer_side_tools->Add(m_side_tools, 1, wxEXPAND, 0);
    m_tabpanel = new Tabbook(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, sizer_side_tools, wxNB_LEFT | wxTAB_TRAVERSAL | wxNB_NOPAGETHEME);
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

    // No Update or HMS tabs in OpenBambu mode

    m_tabpanel->SetFooterText(_L("OpenBambu (LAN Only)"));

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

    // No HMS update in OpenBambu mode
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
    }
    return wxPanel::Show(show);
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
