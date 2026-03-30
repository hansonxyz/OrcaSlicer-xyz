#include "libslic3r/FlushVolCalc.hpp"
#include "PurgeCalibrationDialog.hpp"
#include "PurgeCalibrationGenerator.hpp"
#include "WipeTowerDialog.hpp"
#include "GUI_App.hpp"
#include "Plater.hpp"
#include "I18N.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"

#include <wx/sizer.h>
#include <wx/msgdlg.h>
#include <wx/display.h>
#include <wx/filename.h>
#include <wx/filesys.h>

#include <boost/filesystem.hpp>
#include <fstream>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>

using json = nlohmann::json;
namespace fs = boost::filesystem;

namespace Slic3r {
namespace GUI {

void open_purge_calibration_dialog(wxWindow *parent)
{
    PurgeCalibrationDialog dlg(parent);
    dlg.ShowModal();
}

PurgeCalibrationDialog::PurgeCalibrationDialog(wxWindow *parent)
    : wxDialog(parent, wxID_ANY, _L("Purge Calibration Test Print"),
               wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    SetBackgroundColour(*wxWHITE);

    auto &preset_bundle = *wxGetApp().preset_bundle;
    auto *color_opt = preset_bundle.project_config.option<ConfigOptionStrings>("filament_colour");
    int filament_count = color_opt ? (int)color_opt->values.size() : 2;

    wxSize dialog_size = wxSize(FromDIP(500), FromDIP(450));
    if (filament_count > 4) dialog_size = wxSize(FromDIP(600), FromDIP(500));
    if (filament_count > 8) dialog_size = wxSize(FromDIP(750), FromDIP(550));

    wxBoxSizer *main_sizer = new wxBoxSizer(wxVERTICAL);

    m_webview = wxWebView::New(this, wxID_ANY, wxEmptyString,
        wxDefaultPosition, dialog_size, wxWebViewBackendDefault, wxNO_BORDER);
    m_webview->AddScriptMessageHandler("purgeCalibration");
    main_sizer->Add(m_webview, 1, wxEXPAND);

    // Load HTML
    fs::path filepath = fs::path(resources_dir()) / "web/flush/PurgeCalibration.html";
    wxFileName fn(wxString::FromUTF8(filepath.string()));
    if (fn.FileExists()) {
        m_webview->LoadURL(wxFileSystem::FileNameToURL(fn));
    } else {
        BOOST_LOG_TRIVIAL(error) << "PurgeCalibration.html not found: " << filepath.string();
    }

    // Handle messages from JS
    m_webview->Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, [this](wxWebViewEvent &evt) {
        on_message(evt.GetString().ToStdString());
    });

    m_webview->Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent &e) {
        if (e.GetKeyCode() == WXK_ESCAPE) {
            if (IsModal()) EndModal(wxID_CANCEL);
            else Close();
        } else { e.Skip(); }
    });

    SetSizer(main_sizer);
    main_sizer->SetSizeHints(this);
    main_sizer->Fit(this);
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

wxString PurgeCalibrationDialog::build_init_data()
{
    auto &preset_bundle = *wxGetApp().preset_bundle;
    auto *color_opt = preset_bundle.project_config.option<ConfigOptionStrings>("filament_colour");
    if (!color_opt) return "{}";

    auto colors = color_opt->values;
    auto recommended = WipingDialog::CalcFlushingVolumes(0);

    json data;
    data["is_dark_mode"] = wxGetApp().app_config->get("dark_color_mode") == "1";

    json filaments = json::array();
    for (size_t i = 0; i < colors.size(); ++i) {
        json f;
        f["index"] = (int)i;
        f["color"] = colors[i];
        f["name"] = "Filament " + std::to_string(i + 1);
        f["type"] = "PLA";
        f["is_soluble"] = false;

        if (i < preset_bundle.filament_presets.size()) {
            const auto *preset = preset_bundle.filaments.find_preset(
                preset_bundle.filament_presets[i]);
            if (preset) {
                f["name"] = preset->name;
                auto *type_opt = preset->config.option<ConfigOptionStrings>("filament_type");
                if (type_opt && !type_opt->values.empty()) {
                    f["type"] = type_opt->values[0];
                    std::string ft = type_opt->values[0];
                    f["is_soluble"] = (ft == "PVA" || ft == "BVOH");
                }
            }
        }
        filaments.push_back(f);
    }
    data["filaments"] = filaments;

    json matrix = json::array();
    for (size_t i = 0; i < recommended.size(); ++i) {
        json row = json::array();
        for (size_t j = 0; j < recommended[i].size(); ++j)
            row.push_back(recommended[i][j]);
        matrix.push_back(row);
    }
    data["recommended_matrix"] = matrix;

    // Calculate max strips that fit on the bed
    auto full_config = preset_bundle.full_config();
    double bed_w = 256, bed_d = 256;
    auto *printable_area = full_config.option<ConfigOptionPoints>("printable_area");
    if (printable_area && printable_area->values.size() >= 4) {
        double min_x = 1e9, max_x = -1e9, min_y = 1e9, max_y = -1e9;
        for (const auto &pt : printable_area->values) {
            min_x = std::min(min_x, pt.x()); max_x = std::max(max_x, pt.x());
            min_y = std::min(min_y, pt.y()); max_y = std::max(max_y, pt.y());
        }
        bed_w = max_x - min_x;
        bed_d = max_y - min_y;
    }
    double nozzle = full_config.opt_float("nozzle_diameter", 0);
    if (nozzle <= 0) nozzle = 0.4;
    double lh = nozzle * 1.25;
    double strip_w = 15.0, notch_area = 5.0, gap = 3.0, label_h = 4.0, label_gap = 1.5;
    double cell_w = strip_w + notch_area + gap;
    double border = 5.0;
    double usable_w = bed_w - 2 * border;
    double usable_d = bed_d - 2 * border;
    // Estimate max strip length from a typical flush volume of 300
    double max_strip_len = 300.0 / (lh * strip_w);
    double cell_d = max_strip_len + label_gap + label_h + gap;
    int max_cols = std::max(1, (int)(usable_w / cell_w));
    int max_rows = std::max(1, (int)(usable_d / cell_d));
    data["max_strips"] = max_cols * max_rows;

    return wxString::FromUTF8(data.dump());
}

void PurgeCalibrationDialog::on_message(const std::string &message)
{
    try {
        json j = json::parse(message);
        std::string msg = j["msg"].get<std::string>();

        if (msg == "init") {
            // JS is ready — send data via buildCalibration()
            wxString init_data = build_init_data();
            CallAfter([this, init_data] {
                wxString script = wxString::Format("buildCalibration(%s)", init_data);
                m_webview->RunScript(script);
            });
        }
        else if (msg == "cancel") {
            if (IsModal()) EndModal(wxID_CANCEL);
            else Close();
        }
        else if (msg == "generate") {
            // Build options from the dialog's selections
            PurgeCalibrationGenerator::Options opts;
            for (const auto &p : j["pairs"]) {
                PurgeCalibrationGenerator::TransitionPair tp;
                tp.from_filament = p["from"].get<int>();
                tp.to_filament = p["to"].get<int>();
                opts.pairs.push_back(tp);
            }
            opts.base_layers = j.value("base_layers", 2);
            opts.base_filament = j.value("base_filament", 0);
            opts.label_filament = j.value("label_filament", 0);

            // Pick strip filament: distinct from both base and label if 3+ filaments,
            // otherwise same as base (only 2 filaments available)
            auto &preset_bundle = *wxGetApp().preset_bundle;
            auto *color_opt = preset_bundle.project_config.option<ConfigOptionStrings>("filament_colour");
            int n_filaments = color_opt ? (int)color_opt->values.size() : 2;
            opts.strip_filament = opts.base_filament; // default: same as base
            if (n_filaments >= 3) {
                // Find a filament that isn't base or label
                for (int i = 0; i < n_filaments; ++i) {
                    if (i != opts.base_filament && i != opts.label_filament) {
                        opts.strip_filament = i;
                        break;
                    }
                }
            }

            // Close this dialog before generating
            if (IsModal()) EndModal(wxID_OK);

            // Run generator
            CallAfter([opts] {
                bool ok = PurgeCalibrationGenerator::generate(opts);
                if (!ok) {
                    wxMessageBox(_L("Failed to generate calibration test print."),
                        _L("Purge Calibration"), wxOK | wxICON_ERROR);
                }
            });
        }
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << "PurgeCalibrationDialog message error: " << e.what()
            << " message: " << message;
    }
}

void open_purge_calibration_result_dialog(wxWindow *parent)
{
    // Placeholder WebView dialog — will become the measurement input UI
    wxDialog dlg(parent, wxID_ANY, _L("Calibration Results"),
        wxDefaultPosition, wxSize(parent->FromDIP(500), parent->FromDIP(520)),
        wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    dlg.SetBackgroundColour(wxGetApp().get_window_default_clr());

    auto *sizer = new wxBoxSizer(wxVERTICAL);

    auto *webview = wxWebView::New(&dlg, wxID_ANY, wxEmptyString,
        wxDefaultPosition, wxDefaultSize, wxWebViewBackendDefault, wxNO_BORDER);
    sizer->Add(webview, 1, wxEXPAND);

    bool is_dark = wxGetApp().app_config->get("dark_color_mode") == "1";
    std::string bg = is_dark ? "#2d2d31" : "#f5f5f5";
    std::string fg = is_dark ? "#e0e0e0" : "#333333";
    std::string panel_bg = is_dark ? "#4c4c55" : "#eeeeee";

    std::string html_str =
        "<html><head><style>"
        "body { font-family: sans-serif; padding: 20px; background: " + bg + "; color: " + fg + "; }"
        "h2 { color: #26A69A; margin-top: 0; }"
        ".tip { background: " + panel_bg + "; padding: 12px; border-radius: 6px; margin: 12px 0; font-size: 13px; line-height: 1.6; }"
        ".step { margin: 8px 0; }"
        ".step b { color: #26A69A; }"
        ".formula { background: " + panel_bg + "; padding: 10px 14px; border-radius: 4px; font-family: monospace; font-size: 14px; margin: 10px 0; }"
        "</style></head><body>"
        "<h2>Calibration Print Complete</h2>"
        "<div class='tip'>"
        "<div class='step'><b>Step 1:</b> Examine each test strip on the printed calibration grid.</div>"
        "<div class='step'><b>Step 2:</b> For each filament transition pair, count the number of ruler "
        "notches from the bottom of the strip to the point where the new filament color "
        "is unquestionably pure &mdash; no trace of the previous color remains.</div>"
        "<div class='step'><b>Step 3:</b> Calculate the flush volume:</div>"
        "<div class='formula'>Flush Volume = (notches &times; 10) + 50</div>"
        "<div class='step'><b>Step 4:</b> Enter the calculated value in the Flushing Volumes dialog "
        "for each transition pair you tested.</div>"
        "</div>"
        "<p style='font-size:12px; color:#888;'>Each notch represents approximately 10mm&sup3; of purge material. "
        "The first 50mm&sup3; was purged to waste before the strip began printing.</p>"
        "</body></html>";
    // Write HTML to a temp file and load as URL — SetPage doesn't work
    // reliably with the Edge WebView2 backend
    boost::filesystem::path temp_html = boost::filesystem::temp_directory_path()
        / "orcaslicer_calib" / "calibration_result.html";
    boost::filesystem::create_directories(temp_html.parent_path());
    {
        std::ofstream f(temp_html.string());
        f << html_str;
    }
    wxFileName fn(wxString::FromUTF8(temp_html.string()));
    if (fn.FileExists())
        webview->LoadURL(wxFileSystem::FileNameToURL(fn));

    dlg.SetSizer(sizer);
    wxGetApp().UpdateDlgDarkUI(&dlg);
    dlg.CenterOnParent();
    dlg.ShowModal();
}

} // namespace GUI
} // namespace Slic3r
