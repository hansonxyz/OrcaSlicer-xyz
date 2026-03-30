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
        wxDefaultPosition, wxSize(parent->FromDIP(500), parent->FromDIP(350)),
        wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    dlg.SetBackgroundColour(wxGetApp().get_window_default_clr());

    auto *sizer = new wxBoxSizer(wxVERTICAL);

    auto *webview = wxWebView::New(&dlg, wxID_ANY, wxEmptyString,
        wxDefaultPosition, wxDefaultSize, wxWebViewBackendDefault, wxNO_BORDER);
    sizer->Add(webview, 1, wxEXPAND);

    // Load a simple inline page
    wxString html = R"(
        <html><head><style>
            body { font-family: sans-serif; padding: 20px; background: #2d2d31; color: #e0e0e0; }
            h2 { color: #26A69A; }
        </style></head><body>
            <h2>Hello World</h2>
            <p>This dialog will become the measurement input screen where you enter
            your calibration results to update the flushing volume matrix.</p>
        </body></html>
    )";
    webview->SetPage(html, "");

    dlg.SetSizer(sizer);
    wxGetApp().UpdateDlgDarkUI(&dlg);
    dlg.CenterOnParent();
    dlg.ShowModal();
}

} // namespace GUI
} // namespace Slic3r
