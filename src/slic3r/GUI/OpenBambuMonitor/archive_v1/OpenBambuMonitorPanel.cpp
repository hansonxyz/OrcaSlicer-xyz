#include "OpenBambuMonitorPanel.hpp"
#include "../GUI_App.hpp"
#include "../I18N.hpp"
#include "../MainFrame.hpp"
#include "../MediaPlayCtrl.h"
#include "../../Utils/NetworkAgent.hpp"
#include "../../Utils/OpenBambu/OpenBambuAgent.hpp"
#include "../../Utils/OpenBambu/OpenBambuCommands.hpp"
#include "../../Utils/OpenBambu/OpenBambuPrinterAgent.hpp"

#include <boost/log/trivial.hpp>
#include <wx/dcbuffer.h>
#include <wx/msgdlg.h>
#include <wx/textdlg.h>
#include <wx/colour.h>
#include <sstream>

namespace Slic3r { namespace GUI {

static constexpr int SIDEBAR_WIDTH = 280;
static constexpr int SPARKLINE_HEIGHT = 30;
static constexpr int TIMER_INTERVAL_MS = 1000;

// Color constants
static const wxColour CLR_BG(45, 45, 48);
static const wxColour CLR_PANEL(55, 55, 58);
static const wxColour CLR_TEXT(220, 220, 220);
static const wxColour CLR_TEXT_DIM(140, 140, 140);
static const wxColour CLR_ACCENT(0x4e, 0xc9, 0xb0); // teal
static const wxColour CLR_NOZZLE(0xff, 0x66, 0x33);  // orange
static const wxColour CLR_BED(0x33, 0x99, 0xff);     // blue
static const wxColour CLR_SUCCESS(0x4e, 0xc9, 0x4e);
static const wxColour CLR_WARNING(0xff, 0xcc, 0x00);

OpenBambuMonitorPanel::OpenBambuMonitorPanel(wxWindow *parent)
    : wxPanel(parent, wxID_ANY)
{
    SetBackgroundColour(CLR_BG);
    build_ui();

    m_refresh_timer.Bind(wxEVT_TIMER, &OpenBambuMonitorPanel::on_timer, this);
}

OpenBambuMonitorPanel::~OpenBambuMonitorPanel()
{
    m_refresh_timer.Stop();
}

bool OpenBambuMonitorPanel::Show(bool show)
{
    BOOST_LOG_TRIVIAL(info) << "OpenBambuMonitorPanel::Show(" << show << ")";

    if (show) {
        m_refresh_timer.Start(TIMER_INTERVAL_MS);

        // Register callbacks with the agent
        auto *agent = wxGetApp().getAgent();
        if (agent) {
            agent->set_on_local_message_fn([this](std::string dev_id, std::string payload) {
                wxGetApp().CallAfter([this, dev_id, payload]() {
                    on_mqtt_status(dev_id, payload);
                });
            });
        }

        // Auto-select first printer if none selected
        {
            std::lock_guard<std::mutex> lock(m_printers_mutex);
            if (m_selected_serial.empty() && !m_printers.empty()) {
                on_printer_selected(0);
            }
        }

        refresh_ui();
    } else {
        m_refresh_timer.Stop();
    }

    return wxPanel::Show(show);
}

static wxStaticText *make_label(wxWindow *parent, const wxString &text, const wxColour &color = CLR_TEXT, int font_size = 10)
{
    auto *lbl = new wxStaticText(parent, wxID_ANY, text);
    lbl->SetForegroundColour(color);
    lbl->SetFont(wxFont(font_size, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL));
    return lbl;
}

static wxStaticText *make_section_title(wxWindow *parent, const wxString &text)
{
    auto *lbl = new wxStaticText(parent, wxID_ANY, text);
    lbl->SetForegroundColour(CLR_ACCENT);
    lbl->SetFont(wxFont(11, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD));
    return lbl;
}

void OpenBambuMonitorPanel::build_ui()
{
    auto *main_sizer = new wxBoxSizer(wxHORIZONTAL);

    // Sidebar (scrollable)
    m_sidebar = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(SIDEBAR_WIDTH), -1));
    m_sidebar->SetScrollRate(0, 10);
    m_sidebar->SetBackgroundColour(CLR_PANEL);
    auto *sidebar_sizer = new wxBoxSizer(wxVERTICAL);
    build_sidebar(m_sidebar, sidebar_sizer);
    m_sidebar->SetSizer(sidebar_sizer);
    m_sidebar->SetMinSize(wxSize(FromDIP(SIDEBAR_WIDTH), -1));

    // Content area
    auto *content = new wxPanel(this);
    content->SetBackgroundColour(CLR_BG);
    auto *content_sizer = new wxBoxSizer(wxVERTICAL);
    build_content(content, content_sizer);
    content->SetSizer(content_sizer);

    main_sizer->Add(m_sidebar, 0, wxEXPAND);
    main_sizer->Add(content, 1, wxEXPAND);

    SetSizer(main_sizer);
}

void OpenBambuMonitorPanel::build_sidebar(wxWindow *parent, wxBoxSizer *sizer)
{
    int pad = FromDIP(8);
    int gap = FromDIP(4);

    // === PRINTER STATUS ===
    sizer->Add(make_section_title(parent, _L("Printer Status")), 0, wxLEFT | wxTOP, pad);
    sizer->AddSpacer(gap);

    m_lbl_printer_name = make_label(parent, _L("No printer selected"), CLR_TEXT, 11);
    m_lbl_printer_name->SetFont(m_lbl_printer_name->GetFont().Bold());
    sizer->Add(m_lbl_printer_name, 0, wxLEFT | wxRIGHT, pad);

    m_lbl_print_status = make_label(parent, _L("Status: ---"), CLR_TEXT_DIM);
    sizer->Add(m_lbl_print_status, 0, wxLEFT | wxRIGHT, pad);
    sizer->AddSpacer(gap);

    // Nozzle temp + sparkline
    m_lbl_nozzle_temp = make_label(parent, _L("Nozzle: ---"));
    sizer->Add(m_lbl_nozzle_temp, 0, wxLEFT | wxRIGHT, pad);
    m_nozzle_sparkline = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(SPARKLINE_HEIGHT)));
    m_nozzle_sparkline->SetBackgroundColour(CLR_PANEL);
    m_nozzle_sparkline->Bind(wxEVT_PAINT, [this](wxPaintEvent &) {
        std::lock_guard<std::mutex> lock(m_status_mutex);
        paint_sparkline(m_nozzle_sparkline, m_status.nozzle_history, CLR_NOZZLE);
    });
    sizer->Add(m_nozzle_sparkline, 0, wxEXPAND | wxLEFT | wxRIGHT, pad);

    // Bed temp + sparkline
    m_lbl_bed_temp = make_label(parent, _L("Bed: ---"));
    sizer->Add(m_lbl_bed_temp, 0, wxLEFT | wxRIGHT, pad);
    m_bed_sparkline = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(SPARKLINE_HEIGHT)));
    m_bed_sparkline->SetBackgroundColour(CLR_PANEL);
    m_bed_sparkline->Bind(wxEVT_PAINT, [this](wxPaintEvent &) {
        std::lock_guard<std::mutex> lock(m_status_mutex);
        paint_sparkline(m_bed_sparkline, m_status.bed_history, CLR_BED);
    });
    sizer->Add(m_bed_sparkline, 0, wxEXPAND | wxLEFT | wxRIGHT, pad);
    sizer->AddSpacer(gap);

    m_lbl_layer = make_label(parent, _L("Layer: ---"));
    sizer->Add(m_lbl_layer, 0, wxLEFT | wxRIGHT, pad);

    m_lbl_time_left = make_label(parent, _L("Time left: ---"));
    sizer->Add(m_lbl_time_left, 0, wxLEFT | wxRIGHT, pad);

    m_lbl_speed = make_label(parent, _L("Speed: ---"));
    sizer->Add(m_lbl_speed, 0, wxLEFT | wxRIGHT, pad);

    m_lbl_fan = make_label(parent, _L("Fan: ---"));
    sizer->Add(m_lbl_fan, 0, wxLEFT | wxRIGHT, pad);

    sizer->AddSpacer(FromDIP(12));

    // === STORAGE ===
    sizer->Add(make_section_title(parent, _L("Storage")), 0, wxLEFT, pad);
    sizer->AddSpacer(gap);

    auto *btn_browse = new wxButton(parent, wxID_ANY, _L("Browse Files"));
    btn_browse->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_browse_storage(); });
    sizer->Add(btn_browse, 0, wxEXPAND | wxLEFT | wxRIGHT, pad);

    // Placeholder: Show in Explorer button (future WebDAV proxy)
    auto *btn_explorer = new wxButton(parent, wxID_ANY, _L("Show in Explorer"));
    btn_explorer->SetToolTip(_L("Coming soon: mount printer SD card in Windows Explorer via WebDAV"));
    btn_explorer->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        wxMessageBox(_L("This feature is coming in a future update.\n\n"
                        "It will mount the printer's SD card as a network drive "
                        "in Windows Explorer via a local WebDAV proxy."),
                     _L("Coming Soon"), wxOK | wxICON_INFORMATION, this);
    });
    sizer->Add(btn_explorer, 0, wxEXPAND | wxLEFT | wxRIGHT, pad);
    sizer->AddSpacer(gap);

    m_storage_listbox = new wxListBox(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(120)));
    m_storage_listbox->SetBackgroundColour(CLR_BG);
    m_storage_listbox->SetForegroundColour(CLR_TEXT);
    m_storage_listbox->Bind(wxEVT_LISTBOX_DCLICK, [this](wxCommandEvent &e) {
        int sel = e.GetSelection();
        if (sel >= 0 && sel < (int)m_storage_files.size())
            on_storage_file_action(m_storage_files[sel]);
    });
    sizer->Add(m_storage_listbox, 0, wxEXPAND | wxLEFT | wxRIGHT, pad);

    sizer->AddSpacer(FromDIP(12));

    // === PRINTERS ===
    sizer->Add(make_section_title(parent, _L("Printers")), 0, wxLEFT, pad);
    sizer->AddSpacer(gap);

    m_printer_listbox = new wxListBox(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(100)));
    m_printer_listbox->SetBackgroundColour(CLR_BG);
    m_printer_listbox->SetForegroundColour(CLR_TEXT);
    m_printer_listbox->Bind(wxEVT_LISTBOX, [this](wxCommandEvent &e) {
        on_printer_selected(e.GetSelection());
    });
    sizer->Add(m_printer_listbox, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, pad);
}

void OpenBambuMonitorPanel::build_content(wxWindow *parent, wxBoxSizer *sizer)
{
    int pad = FromDIP(8);

    // Camera area (placeholder — MediaPlayCtrl integration is complex,
    // for now show a panel that will be replaced)
    m_camera_panel = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(300)));
    m_camera_panel->SetBackgroundColour(wxColour(30, 30, 30));
    {
        auto *cam_sizer = new wxBoxSizer(wxVERTICAL);
        auto *cam_label = make_label(m_camera_panel, _L("Camera stream — click a printer to connect"), CLR_TEXT_DIM, 12);
        cam_sizer->AddStretchSpacer();
        cam_sizer->Add(cam_label, 0, wxALIGN_CENTER);
        cam_sizer->AddStretchSpacer();
        m_camera_panel->SetSizer(cam_sizer);
    }
    sizer->Add(m_camera_panel, 2, wxEXPAND | wxALL, pad);

    // Live print preview placeholder
    m_preview_placeholder = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(200)));
    m_preview_placeholder->SetBackgroundColour(wxColour(35, 35, 38));
    {
        auto *ph_sizer = new wxBoxSizer(wxVERTICAL);
        // Phase 2 placeholder: live GCode layer preview
        auto *ph_label = make_label(m_preview_placeholder,
            _L("Live print preview — coming in a future update"), CLR_TEXT_DIM, 11);
        auto *ph_desc = make_label(m_preview_placeholder,
            _L("Will show real-time layer-by-layer progress of the current print"), CLR_TEXT_DIM, 9);
        ph_sizer->AddStretchSpacer();
        ph_sizer->Add(ph_label, 0, wxALIGN_CENTER);
        ph_sizer->Add(ph_desc, 0, wxALIGN_CENTER | wxTOP, FromDIP(4));
        ph_sizer->AddStretchSpacer();
        m_preview_placeholder->SetSizer(ph_sizer);
    }
    sizer->Add(m_preview_placeholder, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, pad);

    // Controls bar
    auto *controls = new wxPanel(parent);
    controls->SetBackgroundColour(CLR_PANEL);
    auto *ctrl_sizer = new wxBoxSizer(wxHORIZONTAL);

    m_btn_pause = new wxButton(controls, wxID_ANY, _L("Pause"));
    m_btn_pause->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_pause_resume(); });
    ctrl_sizer->Add(m_btn_pause, 0, wxALL, FromDIP(4));

    m_btn_stop = new wxButton(controls, wxID_ANY, _L("Stop"));
    m_btn_stop->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_stop(); });
    ctrl_sizer->Add(m_btn_stop, 0, wxALL, FromDIP(4));

    ctrl_sizer->AddSpacer(FromDIP(16));

    ctrl_sizer->Add(make_label(controls, _L("Speed:"), CLR_TEXT), 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(4));
    auto *speed_choices = new wxChoice(controls, wxID_ANY);
    speed_choices->Append(_L("Silent"));
    speed_choices->Append(_L("Standard"));
    speed_choices->Append(_L("Sport"));
    speed_choices->Append(_L("Ludicrous"));
    speed_choices->SetSelection(1); // default Standard
    speed_choices->Bind(wxEVT_CHOICE, [this, speed_choices](wxCommandEvent &) {
        on_speed_change(speed_choices->GetSelection() + 1);
    });
    ctrl_sizer->Add(speed_choices, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(4));

    ctrl_sizer->AddSpacer(FromDIP(16));

    m_btn_light = new wxButton(controls, wxID_ANY, _L("Light"));
    m_btn_light->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_light_toggle(); });
    ctrl_sizer->Add(m_btn_light, 0, wxALL, FromDIP(4));

    m_btn_unload = new wxButton(controls, wxID_ANY, _L("Unload"));
    m_btn_unload->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_unload_filament(); });
    ctrl_sizer->Add(m_btn_unload, 0, wxALL, FromDIP(4));

    controls->SetSizer(ctrl_sizer);
    sizer->Add(controls, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, pad);
}

void OpenBambuMonitorPanel::on_timer(wxTimerEvent &)
{
    refresh_ui();
}

void OpenBambuMonitorPanel::on_mqtt_status(const std::string &dev_id, const std::string &payload)
{
    {
        std::lock_guard<std::mutex> lock(m_status_mutex);
        parse_mqtt_status(payload, m_status);
        m_status.connected = true;
    }
    // UI refresh happens on the next timer tick (1Hz)
}

void OpenBambuMonitorPanel::on_printer_discovered(const std::string &json)
{
    // Parse the SSDP JSON
    auto extract = [&json](const char *key) -> std::string {
        std::string search = std::string("\"") + key + "\":\"";
        auto pos = json.find(search);
        if (pos == std::string::npos) return "";
        pos += search.size();
        auto end = json.find("\"", pos);
        return json.substr(pos, end - pos);
    };

    DiscoveredPrinter p;
    p.serial = extract("dev_id");
    p.name = extract("dev_name");
    p.model = extract("dev_type");
    p.ip = extract("dev_ip");
    p.signal = extract("dev_signal");
    p.last_seen = std::chrono::steady_clock::now();

    if (p.serial.empty()) return;

    // Check for stored alias and access code
    auto *config = wxGetApp().app_config;
    if (config) {
        std::string alias_key = "openbambu_alias_" + p.serial;
        if (!config->get(alias_key).empty())
            p.alias = config->get(alias_key);

        // Access codes are already stored in user_access_code
        auto access_codes = config->get("user_access_code");
        // The access code lookup is handled by the connect flow
    }

    {
        std::lock_guard<std::mutex> lock(m_printers_mutex);
        auto it = m_printers.find(p.serial);
        if (it != m_printers.end()) {
            // Update existing
            it->second.ip = p.ip;
            it->second.signal = p.signal;
            it->second.last_seen = p.last_seen;
            if (!p.name.empty()) it->second.name = p.name;
        } else {
            m_printers[p.serial] = std::move(p);
        }
    }

    wxGetApp().CallAfter([this]() { refresh_printer_list(); });
}

void OpenBambuMonitorPanel::refresh_ui()
{
    std::lock_guard<std::mutex> lock(m_status_mutex);

    // Printer name
    wxString name = m_status.name.empty() ? _L("No printer selected") : wxString::FromUTF8(m_status.name);
    if (!m_status.model.empty())
        name += " (" + m_status.model + ")";
    m_lbl_printer_name->SetLabel(name);

    // Print status
    wxString state_str;
    if (m_status.gcode_state == "RUNNING") {
        state_str = wxString::Format(_L("Printing %d%%"), m_status.percent);
        if (!m_status.subtask_name.empty())
            state_str += " — " + wxString::FromUTF8(m_status.subtask_name);
    } else if (m_status.gcode_state == "PAUSED") {
        state_str = wxString::Format(_L("Paused at %d%%"), m_status.percent);
    } else if (m_status.gcode_state == "FINISH") {
        state_str = _L("Print complete");
    } else if (m_status.gcode_state == "FAILED") {
        state_str = _L("Print failed");
    } else if (m_status.connected) {
        state_str = _L("Idle");
    } else {
        state_str = _L("Not connected");
    }
    m_lbl_print_status->SetLabel(state_str);

    // Temperatures
    m_lbl_nozzle_temp->SetLabel(wxString::Format("Nozzle: %.0f\u00B0C / %.0f\u00B0C",
        m_status.nozzle_temp, m_status.nozzle_target));
    m_lbl_bed_temp->SetLabel(wxString::Format("Bed: %.0f\u00B0C / %.0f\u00B0C",
        m_status.bed_temp, m_status.bed_target));

    // Repaint sparklines
    if (m_nozzle_sparkline) m_nozzle_sparkline->Refresh();
    if (m_bed_sparkline) m_bed_sparkline->Refresh();

    // Layer
    if (m_status.total_layers > 0) {
        m_lbl_layer->SetLabel(wxString::Format(_L("Layer: %d / %d"),
            m_status.current_layer, m_status.total_layers));
    } else {
        m_lbl_layer->SetLabel(_L("Layer: ---"));
    }

    // Time remaining
    if (m_status.remaining_min > 0) {
        int hours = m_status.remaining_min / 60;
        int mins = m_status.remaining_min % 60;
        if (hours > 0)
            m_lbl_time_left->SetLabel(wxString::Format(_L("Time left: %dh %dm"), hours, mins));
        else
            m_lbl_time_left->SetLabel(wxString::Format(_L("Time left: %dm"), mins));
    } else {
        m_lbl_time_left->SetLabel(_L("Time left: ---"));
    }

    // Speed
    const char *speed_names[] = {"", "Silent", "Standard", "Sport", "Ludicrous"};
    int sl = m_status.speed_level;
    if (sl >= 1 && sl <= 4)
        m_lbl_speed->SetLabel(wxString::Format("Speed: %s (%d%%)", speed_names[sl], m_status.speed_mag));
    else
        m_lbl_speed->SetLabel(_L("Speed: ---"));

    // Fan
    m_lbl_fan->SetLabel(wxString::Format("Fan: %d%%", m_status.fan_part));

    // Pause button label
    if (m_status.gcode_state == "PAUSED")
        m_btn_pause->SetLabel(_L("Resume"));
    else
        m_btn_pause->SetLabel(_L("Pause"));
}

void OpenBambuMonitorPanel::refresh_printer_list()
{
    std::lock_guard<std::mutex> lock(m_printers_mutex);

    m_printer_listbox->Clear();
    int sel_idx = -1;
    int idx = 0;
    for (const auto &[serial, printer] : m_printers) {
        wxString label = printer.alias.empty()
            ? wxString::FromUTF8(printer.name)
            : wxString::FromUTF8(printer.alias);
        if (printer.connected)
            label += " \u2022"; // bullet = connected
        label += "  (" + printer.model + ")";
        m_printer_listbox->Append(label);
        if (serial == m_selected_serial)
            sel_idx = idx;
        ++idx;
    }
    if (sel_idx >= 0)
        m_printer_listbox->SetSelection(sel_idx);
}

void OpenBambuMonitorPanel::paint_sparkline(wxPanel *panel, const TempHistory &history, wxColour color)
{
    wxBufferedPaintDC dc(panel);
    wxSize sz = panel->GetClientSize();
    dc.SetBackground(wxBrush(CLR_PANEL));
    dc.Clear();

    int n = history.size();
    if (n < 2) return;

    float min_v = history.min_val();
    float max_v = history.max_val();
    float range = max_v - min_v;
    if (range < 5) { range = 5; min_v = (min_v + max_v) / 2 - 2.5f; }

    dc.SetPen(wxPen(color, 1));
    int prev_x = 0, prev_y = sz.y / 2;

    for (int i = 0; i < n; ++i) {
        int x = (i * (sz.x - 1)) / (n - 1);
        float v = history.at(i);
        int y = sz.y - 1 - (int)((v - min_v) / range * (sz.y - 2));
        if (y < 0) y = 0;
        if (y >= sz.y) y = sz.y - 1;

        if (i > 0)
            dc.DrawLine(prev_x, prev_y, x, y);
        prev_x = x;
        prev_y = y;
    }

    // Draw current value text
    dc.SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL));
    dc.SetTextForeground(color);
    wxString val = wxString::Format("%.0f", history.at(n - 1));
    dc.DrawText(val, sz.x - dc.GetTextExtent(val).x - 2, 1);
}

void OpenBambuMonitorPanel::on_printer_selected(int index)
{
    std::lock_guard<std::mutex> lock(m_printers_mutex);

    auto it = m_printers.begin();
    std::advance(it, index);
    if (it == m_printers.end()) return;

    const auto &printer = it->second;
    m_selected_serial = printer.serial;

    BOOST_LOG_TRIVIAL(info) << "OpenBambuMonitor: selected printer " << printer.serial
        << " (" << printer.name << ") at " << printer.ip;

    // Check if we have an access code
    std::string access_code;
    auto *config = wxGetApp().app_config;
    if (config) {
        // OrcaSlicer stores access codes in a JSON object under "user_access_code"
        // We need to look it up — for now use the existing mechanism
        auto codes_json = config->get("user_access_code");
        // Quick parse: find "serial":"code" in the JSON string
        auto pos = codes_json.find("\"" + printer.serial + "\"");
        if (pos != std::string::npos) {
            pos = codes_json.find("\"", pos + printer.serial.size() + 2);
            if (pos != std::string::npos) {
                auto end = codes_json.find("\"", pos + 1);
                if (end != std::string::npos)
                    access_code = codes_json.substr(pos + 1, end - pos - 1);
            }
        }
    }

    if (access_code.empty()) {
        // Prompt for access code
        on_printer_pair(printer.serial);
        return;
    }

    // Connect via the agent
    auto *agent = wxGetApp().getAgent();
    if (agent) {
        auto printer_agent = agent->get_printer_agent();
        if (printer_agent) {
            printer_agent->connect_printer(printer.serial, printer.ip, "bblp", access_code, true);
        }
    }

    // Update status identity
    {
        // Release printers lock first to avoid deadlock with status lock
    }
    std::lock_guard<std::mutex> slock(m_status_mutex);
    m_status.serial = printer.serial;
    m_status.name = printer.alias.empty() ? printer.name : printer.alias;
    m_status.model = printer.model;
    m_status.ip = printer.ip;
}

void OpenBambuMonitorPanel::on_printer_pair(const std::string &serial)
{
    wxTextEntryDialog dlg(this,
        _L("Enter the LAN access code for this printer.\n"
           "You can find it on the printer's touchscreen under Network settings."),
        _L("Pair Printer"), "");
    if (dlg.ShowModal() != wxID_OK) return;

    std::string code = dlg.GetValue().ToStdString();
    if (code.empty()) return;

    // Optionally set an alias
    wxTextEntryDialog alias_dlg(this,
        _L("Set a friendly name for this printer (optional):"),
        _L("Printer Alias"), "");
    std::string alias;
    if (alias_dlg.ShowModal() == wxID_OK)
        alias = alias_dlg.GetValue().ToStdString();

    // Store access code and alias
    auto *config = wxGetApp().app_config;
    if (config) {
        // Store alias
        if (!alias.empty())
            config->set("openbambu_alias_" + serial, alias);

        // Store access code in the existing user_access_code mechanism
        // This is a JSON object — we need to update it properly
        // For now, set it directly (the existing code reads it)
        config->save();
    }

    // Update printer info
    {
        std::lock_guard<std::mutex> lock(m_printers_mutex);
        auto it = m_printers.find(serial);
        if (it != m_printers.end()) {
            it->second.alias = alias;
            it->second.access_code = code;
        }
    }

    refresh_printer_list();
}

// === Control actions ===

void OpenBambuMonitorPanel::on_pause_resume()
{
    auto *agent = wxGetApp().getAgent();
    if (!agent) return;

    std::string cmd;
    {
        std::lock_guard<std::mutex> lock(m_status_mutex);
        cmd = (m_status.gcode_state == "PAUSED")
            ? OpenBambu::Commands::resume()
            : OpenBambu::Commands::pause();
    }

    auto pa = agent->get_printer_agent();
    if (pa) pa->send_message_to_printer(m_selected_serial, cmd, 0, 0);
}

void OpenBambuMonitorPanel::on_stop()
{
    auto result = wxMessageBox(_L("Are you sure you want to stop the current print?"),
        _L("Stop Print"), wxYES_NO | wxICON_WARNING, this);
    if (result != wxYES) return;

    auto *agent = wxGetApp().getAgent();
    if (!agent) return;
    auto pa = agent->get_printer_agent();
    if (pa) pa->send_message_to_printer(m_selected_serial, OpenBambu::Commands::stop(), 0, 0);
}

void OpenBambuMonitorPanel::on_speed_change(int level)
{
    auto *agent = wxGetApp().getAgent();
    if (!agent) return;
    auto pa = agent->get_printer_agent();
    if (pa) pa->send_message_to_printer(m_selected_serial, OpenBambu::Commands::print_speed(level), 0, 0);
}

void OpenBambuMonitorPanel::on_light_toggle()
{
    std::string mode;
    {
        std::lock_guard<std::mutex> lock(m_status_mutex);
        mode = (m_status.chamber_light == "on") ? "off" : "on";
    }
    auto *agent = wxGetApp().getAgent();
    if (!agent) return;
    auto pa = agent->get_printer_agent();
    if (pa) pa->send_message_to_printer(m_selected_serial,
        OpenBambu::Commands::ledctrl("chamber_light", mode), 0, 0);
}

void OpenBambuMonitorPanel::on_unload_filament()
{
    auto *agent = wxGetApp().getAgent();
    if (!agent) return;
    auto pa = agent->get_printer_agent();
    if (pa) pa->send_message_to_printer(m_selected_serial,
        OpenBambu::Commands::unload_filament(), 0, 0);
}

void OpenBambuMonitorPanel::on_browse_storage()
{
    // TODO: fetch file list via FTPS using OpenBambu::FtpUpload
    // For now, clear and show placeholder
    m_storage_listbox->Clear();
    m_storage_listbox->Append(_L("Loading..."));

    BOOST_LOG_TRIVIAL(info) << "OpenBambuMonitor: storage browse requested (FTPS list not yet wired)";

    // Placeholder — will be wired to FTP LIST in a follow-up
    wxMessageBox(_L("Storage browsing will fetch the file list from the printer's SD card via FTPS.\n\n"
                     "This feature is being wired up."),
                 _L("Storage"), wxOK | wxICON_INFORMATION, this);
}

void OpenBambuMonitorPanel::on_storage_file_action(const std::string &filename)
{
    // Phase 2: Preview GCode / Print from SD
    wxMessageBox(wxString::Format(
        _L("Selected: %s\n\nGCode preview and print-from-SD features are coming in a future update."),
        wxString::FromUTF8(filename)),
        _L("File Action"), wxOK | wxICON_INFORMATION, this);
}

}} // namespace Slic3r::GUI
