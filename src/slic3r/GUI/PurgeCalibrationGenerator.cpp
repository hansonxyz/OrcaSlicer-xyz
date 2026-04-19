#include "PurgeCalibrationGenerator.hpp"
#include "PurgeCalibrationDialog.hpp"
#include "CalibrationStrokeFont.hpp"
#include "GUI_App.hpp"
#include "Plater.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "WipeTowerDialog.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/PlaceholderParser.hpp"
#include "libslic3r/FlushVolCalc.hpp"

#include <boost/log/trivial.hpp>
#include <boost/filesystem.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <wx/timer.h>
#include <wx/msgdlg.h>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <fstream>
#include <regex>
#include <set>
#include <map>

namespace Slic3r {
namespace GUI {

// Static state for async cleanup after slicing
static struct {
    bool active = false;
    int temp_plate_idx = -1;
    int original_plate_idx = 0;
    size_t obj_start_idx = 0;
    std::string gcode_path;
} s_cleanup_state;

TriangleMesh PurgeCalibrationGenerator::make_rect(double width, double length, double height)
{
    return make_cube(width, length, height);
}

PurgeCalibrationGenerator::StripDims PurgeCalibrationGenerator::calc_strip_dims(
    double flush_volume_mm3, double layer_height, double extrusion_width)
{
    StripDims dims;
    dims.width = 15.0;
    dims.volume_per_mm = layer_height * dims.width;
    dims.length = flush_volume_mm3 / dims.volume_per_mm;
    dims.length = std::max(5.0, std::min(dims.length, 200.0));
    return dims;
}

std::vector<PurgeCalibrationGenerator::TransitionPair>
PurgeCalibrationGenerator::order_transitions(const std::vector<TransitionPair> &pairs)
{
    if (pairs.empty()) return {};

    std::vector<TransitionPair> remaining = pairs;
    std::vector<TransitionPair> ordered;

    ordered.push_back(remaining[0]);
    remaining.erase(remaining.begin());

    while (!remaining.empty()) {
        int current_b = ordered.back().to_filament;
        auto it = std::find_if(remaining.begin(), remaining.end(),
            [current_b](const TransitionPair &p) { return p.from_filament == current_b; });

        if (it != remaining.end()) {
            ordered.push_back(*it);
            remaining.erase(it);
        } else {
            ordered.push_back(remaining[0]);
            remaining.erase(remaining.begin());
        }
    }

    return ordered;
}

bool PurgeCalibrationGenerator::generate(const Options &opts)
{
    if (opts.pairs.empty()) return false;

    Plater *plater = wxGetApp().plater();
    if (!plater) return false;

    auto &preset_bundle = *wxGetApp().preset_bundle;
    auto full_config = preset_bundle.full_config();

    // Get bed dimensions
    double bed_width = 256.0, bed_depth = 256.0; // defaults
    auto printable_area = full_config.option<ConfigOptionPoints>("printable_area");
    if (printable_area && printable_area->values.size() >= 4) {
        double min_x = 1e9, max_x = -1e9, min_y = 1e9, max_y = -1e9;
        for (const auto &pt : printable_area->values) {
            min_x = std::min(min_x, pt.x());
            max_x = std::max(max_x, pt.x());
            min_y = std::min(min_y, pt.y());
            max_y = std::max(max_y, pt.y());
        }
        bed_width = max_x - min_x;
        bed_depth = max_y - min_y;
    }

    double nozzle_diameter = full_config.opt_float("nozzle_diameter", 0);
    if (nozzle_diameter <= 0) nozzle_diameter = 0.4;
    double layer_height = nozzle_diameter * 1.25;
    double extrusion_width = nozzle_diameter * 1.125;

    auto recommended = WipingDialog::CalcFlushingVolumes(0);
    auto ordered_pairs = order_transitions(opts.pairs);

    BOOST_LOG_TRIVIAL(info) << "PurgeCalibration: generating " << ordered_pairs.size()
        << " pairs, bed=" << bed_width << "x" << bed_depth
        << " nozzle=" << nozzle_diameter << " layer_h=" << layer_height;

    // Calculate grid layout to fit on bed
    double border = 5.0;
    double usable_width = bed_width - 2 * border;
    double usable_depth = bed_depth - 2 * border;

    double strip_width = 15.0;
    double notch_area = 5.0; // space for ruler notches
    double strip_gap = 3.0;
    double cell_width = strip_width + notch_area + strip_gap;

    // Calculate max strip length
    double max_flush_vol = 0;
    for (const auto &pair : ordered_pairs) {
        double vol = 300;
        if (pair.from_filament < (int)recommended.size() &&
            pair.to_filament < (int)recommended[pair.from_filament].size())
            vol = recommended[pair.from_filament][pair.to_filament];
        vol = std::max(200.0, vol);
        max_flush_vol = std::max(max_flush_vol, vol);
    }

    auto max_dims = calc_strip_dims(max_flush_vol, layer_height, extrusion_width);
    double label_height = 4.0; // mm — height of label text
    double label_gap = 1.5;   // mm — gap between strip and label
    double cell_depth = max_dims.length + label_gap + label_height + strip_gap;

    // How many fit on the bed?
    int max_cols = std::max(1, (int)(usable_width / cell_width));
    int max_rows = std::max(1, (int)(usable_depth / cell_depth));
    int max_pairs = max_cols * max_rows;

    int n_pairs = std::min((int)ordered_pairs.size(), max_pairs);
    if (n_pairs < (int)ordered_pairs.size()) {
        BOOST_LOG_TRIVIAL(warning) << "PurgeCalibration: only " << n_pairs
            << " of " << ordered_pairs.size() << " pairs fit on bed";
        ordered_pairs.resize(n_pairs);
    }

    int grid_cols = std::min(n_pairs, max_cols);
    int grid_rows = (int)std::ceil((double)n_pairs / grid_cols);

    // Center the grid on the bed
    double grid_width = grid_cols * cell_width - strip_gap;
    double grid_depth = grid_rows * cell_depth - strip_gap;
    double offset_x = (bed_width - grid_width) / 2.0;
    double offset_y = (bed_depth - grid_depth) / 2.0;

    // --- Take undo snapshot so we can restore after gcode export ---
    plater->take_snapshot("Purge Calibration");

    // --- Save state and create new plate ---
    auto &plate_list = plater->get_partplate_list();
    int original_plate = plate_list.get_curr_plate_index();
    int temp_plate_idx = plate_list.create_plate(true);
    plate_list.select_plate(temp_plate_idx);

    // Get the plate origin — objects must be offset by this to land on the right plate
    Vec3d plate_origin = plate_list.get_plate(temp_plate_idx)->get_origin();

    // Store model state for the new objects
    auto &model = plater->model();
    size_t obj_start_idx = model.objects.size();

    double base_height = opts.base_layers > 0 ? opts.base_layers * layer_height : 0;

    // --- Add per-strip base pads (optional) ---
    // Each strip gets its own base pad with 3mm margin on top and sides,
    // extending down to cover the ruler notches and label text below
    double base_pad_margin = 3.0;
    if (opts.base_layers > 0) {
        for (int i = 0; i < n_pairs; ++i) {
            const auto &pair = ordered_pairs[i];
            int col = i % grid_cols;
            int row = i / grid_cols;

            double flush_vol = 300;
            if (pair.from_filament < (int)recommended.size() &&
                pair.to_filament < (int)recommended[pair.from_filament].size())
                flush_vol = recommended[pair.from_filament][pair.to_filament];
            flush_vol = std::max(200.0, flush_vol);
            auto dims = calc_strip_dims(flush_vol, layer_height, extrusion_width);

            double x = offset_x + col * cell_width;
            double y = offset_y + row * cell_depth;

            // Pad covers: strip + notch area on right + label below
            double pad_w = dims.width + notch_area + base_pad_margin * 2;
            double pad_top = base_pad_margin;
            double pad_bottom = label_gap + label_height + base_pad_margin;
            double pad_h = dims.length + pad_top + pad_bottom;

            TriangleMesh pad_mesh = make_rect(pad_w, pad_h, base_height);
            std::string pname = "base_" + std::to_string(pair.from_filament) + "_" + std::to_string(pair.to_filament);
            ModelObject *pad_obj = model.add_object(pname.c_str(), "", std::move(pad_mesh));
            auto *pad_inst = pad_obj->add_instance();
            pad_inst->set_offset(Vec3d(
                plate_origin.x() + x - base_pad_margin,
                plate_origin.y() + y - pad_bottom,
                0));

            if (!pad_obj->volumes.empty())
                pad_obj->volumes[0]->config.set_key_value("extruder", new ConfigOptionInt(opts.base_filament + 1));
        }
    }

    // --- Add calibration strip objects ---
    for (int i = 0; i < n_pairs; ++i) {
        const auto &pair = ordered_pairs[i];
        int col = i % grid_cols;
        int row = i / grid_cols;

        double flush_vol = 300;
        if (pair.from_filament < (int)recommended.size() &&
            pair.to_filament < (int)recommended[pair.from_filament].size())
            flush_vol = recommended[pair.from_filament][pair.to_filament];
        flush_vol = std::max(200.0, flush_vol);

        auto dims = calc_strip_dims(flush_vol, layer_height, extrusion_width);

        double x = offset_x + col * cell_width;
        double y = offset_y + row * cell_depth;

        TriangleMesh strip_mesh = make_rect(dims.width, dims.length, layer_height);

        std::string name = "calib_" + std::to_string(pair.from_filament) + "_" + std::to_string(pair.to_filament);
        ModelObject *obj = model.add_object(name.c_str(), "", std::move(strip_mesh));
        auto *inst = obj->add_instance();
        inst->set_offset(Vec3d(plate_origin.x() + x, plate_origin.y() + y, base_height));

        // All strips assigned to strip_filament
        if (!obj->volumes.empty())
            obj->volumes[0]->config.set_key_value("extruder", new ConfigOptionInt(opts.strip_filament + 1));

        // Per-object speed override
        obj->config.set_key_value("inner_wall_speed", new ConfigOptionFloat(opts.strip_speed));
        obj->config.set_key_value("outer_wall_speed", new ConfigOptionFloat(opts.strip_speed));
        obj->config.set_key_value("sparse_infill_speed", new ConfigOptionFloat(opts.strip_speed));
        obj->config.set_key_value("internal_solid_infill_speed", new ConfigOptionFloat(opts.strip_speed));
        obj->config.set_key_value("top_surface_speed", new ConfigOptionFloat(opts.strip_speed));

        // Per-object fill pattern: rectilinear along X axis for consistent purge gradient
        obj->config.set_key_value("top_surface_pattern", new ConfigOptionEnum<InfillPattern>(ipRectilinear));
        obj->config.set_key_value("infill_direction", new ConfigOptionFloat(0.0)); // X-axis aligned

        // No walls on test strips — only infill, so the color transition is fully visible
        obj->config.set_key_value("wall_loops", new ConfigOptionInt(0));
        obj->config.set_key_value("alternate_extra_wall", new ConfigOptionBool(false));

        BOOST_LOG_TRIVIAL(info) << "PurgeCalibration: strip " << name
            << " at (" << x << "," << y << ") " << dims.width << "x" << dims.length;
    }

    // --- Add ruler notch objects ---
    for (int i = 0; i < n_pairs; ++i) {
        const auto &pair = ordered_pairs[i];
        int col = i % grid_cols;
        int row = i / grid_cols;

        double flush_vol = 300;
        if (pair.from_filament < (int)recommended.size() &&
            pair.to_filament < (int)recommended[pair.from_filament].size())
            flush_vol = recommended[pair.from_filament][pair.to_filament];
        flush_vol = std::max(200.0, flush_vol);
        auto dims = calc_strip_dims(flush_vol, layer_height, extrusion_width);

        double x_notch = offset_x + col * cell_width + dims.width + 1.0;
        double y_base = offset_y + row * cell_depth;

        double mm_per_20mm3 = 20.0 / dims.volume_per_mm;
        for (double y_off = 0; y_off < dims.length; y_off += mm_per_20mm3) {
            int notch_vol = (int)(y_off * dims.volume_per_mm);
            double notch_len = (notch_vol % 100 < 20) ? 3.0 : 1.5;

            TriangleMesh notch_mesh = make_rect(notch_len, 0.5, layer_height);
            std::string nn = "notch_" + std::to_string(i) + "_" + std::to_string(notch_vol);
            ModelObject *nobj = model.add_object(nn.c_str(), "", std::move(notch_mesh));
            auto *ninst = nobj->add_instance();
            ninst->set_offset(Vec3d(plate_origin.x() + x_notch, plate_origin.y() + y_base + y_off, base_height));

            if (!nobj->volumes.empty())
                nobj->volumes[0]->config.set_key_value("extruder", new ConfigOptionInt(opts.label_filament + 1));
        }
    }

    // --- Add label text objects below each strip ---
    for (int i = 0; i < n_pairs; ++i) {
        const auto &pair = ordered_pairs[i];
        int col = i % grid_cols;
        int row = i / grid_cols;

        auto dims = calc_strip_dims(200.0, layer_height, extrusion_width); // just for positioning
        double flush_vol = 300;
        if (pair.from_filament < (int)recommended.size() &&
            pair.to_filament < (int)recommended[pair.from_filament].size())
            flush_vol = recommended[pair.from_filament][pair.to_filament];
        flush_vol = std::max(200.0, flush_vol);
        dims = calc_strip_dims(flush_vol, layer_height, extrusion_width);

        double x = offset_x + col * cell_width;
        double y = offset_y + row * cell_depth;

        // Label: "F1>F3" style (from_filament > to_filament)
        std::string label = std::to_string(pair.from_filament + 1) + ">"
                          + std::to_string(pair.to_filament + 1);

        TriangleMesh text_mesh = CalibrationStrokeFont::render_text(label, label_height, layer_height);
        if (text_mesh.empty()) continue;

        // Position below the strip
        double label_y = y - label_gap - label_height;

        std::string lname = "label_" + std::to_string(pair.from_filament) + "_" + std::to_string(pair.to_filament);
        ModelObject *lobj = model.add_object(lname.c_str(), "", std::move(text_mesh));
        auto *linst = lobj->add_instance();
        linst->set_offset(Vec3d(plate_origin.x() + x, plate_origin.y() + label_y, base_height));

        if (!lobj->volumes.empty())
            lobj->volumes[0]->config.set_key_value("extruder", new ConfigOptionInt(opts.label_filament + 1));
    }

    // --- Assign all new objects to the temp plate ---
    size_t obj_end_idx = model.objects.size();
    for (size_t idx = obj_start_idx; idx < obj_end_idx; ++idx) {
        auto *obj = model.objects[idx];
        if (obj && !obj->instances.empty()) {
            plate_list.get_plate(temp_plate_idx)->add_instance(obj->id().id, 0, false);
        }
    }

    // --- Disable prime tower for calibration ---
    auto &print_config = preset_bundle.prints.get_edited_preset().config;
    bool original_prime_tower = print_config.opt_bool("enable_prime_tower");
    print_config.set_key_value("enable_prime_tower", new ConfigOptionBool(false));

    // --- Update view and trigger slicing ---
    plater->update();
    plater->get_view3D_canvas3D()->reload_scene(true);

    BOOST_LOG_TRIVIAL(info) << "PurgeCalibration: created " << (obj_end_idx - obj_start_idx)
        << " objects on plate " << temp_plate_idx;

    // Generate temp gcode output path
    boost::filesystem::path temp_dir = boost::filesystem::temp_directory_path() / "orcaslicer_calib";
    boost::filesystem::create_directories(temp_dir);
    boost::filesystem::path gcode_path = temp_dir / "purge_calibration.gcode";
    std::string gcode_path_str = gcode_path.string();

    // Store cleanup state
    s_cleanup_state.active = true;
    s_cleanup_state.temp_plate_idx = temp_plate_idx;
    s_cleanup_state.original_plate_idx = original_plate;
    s_cleanup_state.obj_start_idx = obj_start_idx;
    s_cleanup_state.gcode_path = gcode_path_str;

    // Trigger reslice (just slices, no save dialog)
    BOOST_LOG_TRIVIAL(info) << "PurgeCalibration: triggering reslice...";
    plater->reslice();

    // Capture ordered_pairs by value for the async callback
    auto calib_pairs = ordered_pairs;

    // Poll for slice completion, then post-process gcode, cleanup, and show result
    auto *timer = new wxTimer();
    timer->Bind(wxEVT_TIMER, [plater, gcode_path_str, temp_plate_idx, obj_start_idx, original_prime_tower, calib_pairs, timer](wxTimerEvent &) {
        int original_plate = s_cleanup_state.original_plate_idx;

        if (plater->is_background_process_slicing())
            return; // still slicing

        timer->Stop();
        BOOST_LOG_TRIVIAL(info) << "PurgeCalibration: slicing complete, starting post-processing";

        // Get the sliced gcode from the plate's temp path
        auto *plate = plater->get_partplate_list().get_plate(temp_plate_idx);
        std::string sliced_gcode_path;
        if (plate)
            sliced_gcode_path = plate->get_tmp_gcode_path();

        if (sliced_gcode_path.empty() || !boost::filesystem::exists(sliced_gcode_path)) {
            BOOST_LOG_TRIVIAL(warning) << "PurgeCalibration: no gcode file found";
            plater->deselect_all();
            plater->undo();
            wxMessageBox(_L("Slicing did not produce gcode. Please try again."),
                _L("Purge Calibration"), wxOK | wxICON_WARNING);
            delete timer;
            return;
        }

        // Read the sliced gcode
        std::string raw_gcode;
        {
            std::ifstream ifs(sliced_gcode_path, std::ios::binary);
            if (ifs) {
                std::ostringstream oss;
                oss << ifs.rdbuf();
                raw_gcode = oss.str();
            }
        }

        if (raw_gcode.empty()) {
            BOOST_LOG_TRIVIAL(error) << "PurgeCalibration: failed to read gcode file";
            plater->deselect_all();
            plater->undo();
            wxMessageBox(_L("Failed to read sliced gcode."),
                _L("Purge Calibration"), wxOK | wxICON_WARNING);
            delete timer;
            return;
        }

        // Post-process: inject calibration filament changes on the top layer
        auto full_config = wxGetApp().preset_bundle->full_config();
        static constexpr double CALIBRATION_PURGE_MM3 = 50.0;
        std::string processed_gcode = postprocess_gcode(raw_gcode, calib_pairs, full_config, CALIBRATION_PURGE_MM3);

        // Write post-processed gcode to our temp file
        {
            std::ofstream ofs(gcode_path_str, std::ios::binary);
            if (ofs) {
                ofs << processed_gcode;
                BOOST_LOG_TRIVIAL(info) << "PurgeCalibration: post-processed gcode written to " << gcode_path_str;
            } else {
                BOOST_LOG_TRIVIAL(error) << "PurgeCalibration: failed to write gcode to " << gcode_path_str;
            }
        }

        // --- Keep calibration plate, switch to Preview ---
        BOOST_LOG_TRIVIAL(info) << "PurgeCalibration: switching to preview";

        if (boost::filesystem::exists(gcode_path_str)) {
            // Point the plate's gcode path to our file and mark as valid
            auto *plate = plater->get_partplate_list().get_plate(temp_plate_idx);
            if (plate) {
                plate->set_tmp_gcode_path(gcode_path_str);
                plate->update_slice_result_valid_state(true);
            }

            // Switch to Preview tab and zoom to the calibration plate
            wxGetApp().mainframe->select_tab(MainFrame::tpPreview);
            plater->get_view3D_canvas3D()->zoom_to_plate(temp_plate_idx);

            // Open the print dialog (blocks until user prints or cancels)
            int orig_plate = s_cleanup_state.original_plate_idx;
            bool orig_prime = original_prime_tower;
            wxGetApp().CallAfter([plater, temp_plate_idx, gcode_path_str, orig_plate, orig_prime] {
                BOOST_LOG_TRIVIAL(info) << "PurgeCalibration: opening print dialog";
                plater->send_to_printer(false);

                // Print dialog has closed (user printed or cancelled).
                BOOST_LOG_TRIVIAL(info) << "PurgeCalibration: print dialog closed, restoring project";

                // Switch back to Prepare tab
                wxGetApp().mainframe->select_tab(MainFrame::tp3DEditor);

                // Clear selection before undo to prevent stale references
                // (the Move gizmo render crashes if selection points to deleted objects)
                plater->deselect_all();

                // Undo to remove calibration plate and restore model
                plater->undo();

                // Restore prime tower setting (undo may not restore edited preset config)
                auto &print_cfg = wxGetApp().preset_bundle->prints.get_edited_preset().config;
                print_cfg.set_key_value("enable_prime_tower", new ConfigOptionBool(orig_prime));

                // Zoom camera back to the original plate
                plater->get_view3D_canvas3D()->zoom_to_plate(orig_plate);

                // Show post-calibration dialog
                wxGetApp().CallAfter([] {
                    open_purge_calibration_result_dialog(wxGetApp().mainframe);
                });
            });
        } else {
            // Slicing failed — clear selection and undo
            plater->deselect_all();
            plater->undo();
            wxMessageBox(_L("Slicing did not produce gcode. Please try again."),
                _L("Purge Calibration"), wxOK | wxICON_WARNING);
        }

        BOOST_LOG_TRIVIAL(info) << "PurgeCalibration: done";
        delete timer;
    });

    timer->Start(500);
    return true;
}

// Build a DynamicConfig override for PlaceholderParser to evaluate change_filament_gcode.
// Mirrors the config setup in GCode.cpp WipeTowerIntegration (lines 828-946).
static DynamicConfig build_toolchange_config(
    const DynamicPrintConfig &full_config,
    int prev_filament_id,
    int next_filament_id,
    double layer_z,
    double max_layer_z,
    double purge_volume_mm3,
    int toolchange_count)
{
    DynamicConfig config;

    config.set_key_value("previous_extruder", new ConfigOptionInt(prev_filament_id));
    config.set_key_value("next_extruder", new ConfigOptionInt(next_filament_id));
    config.set_key_value("layer_num", new ConfigOptionInt(0));
    config.set_key_value("layer_z", new ConfigOptionFloat(layer_z));
    config.set_key_value("toolchange_z", new ConfigOptionFloat(max_layer_z + 0.4));
    config.set_key_value("max_layer_z", new ConfigOptionFloat(max_layer_z));
    config.set_key_value("toolchange_count", new ConfigOptionInt(toolchange_count));
    config.set_key_value("relative_e_axis", new ConfigOptionBool(full_config.opt_bool("use_relative_e_distances")));
    config.set_key_value("fan_speed", new ConfigOptionInt(0));

    // Retraction
    float old_retract_length = (prev_filament_id >= 0) ? full_config.opt_float("retraction_length", prev_filament_id) : 0;
    float new_retract_length = full_config.opt_float("retraction_length", next_filament_id);
    float old_retract_tc = (prev_filament_id >= 0) ? full_config.opt_float("retract_length_toolchange", prev_filament_id) : 0;
    float new_retract_tc = full_config.opt_float("retract_length_toolchange", next_filament_id);
    config.set_key_value("old_retract_length", new ConfigOptionFloat(old_retract_length));
    config.set_key_value("new_retract_length", new ConfigOptionFloat(new_retract_length));
    config.set_key_value("old_retract_length_toolchange", new ConfigOptionFloat(old_retract_tc));
    config.set_key_value("new_retract_length_toolchange", new ConfigOptionFloat(new_retract_tc));

    // Temperatures
    int old_filament_temp = (prev_filament_id >= 0) ? full_config.opt_int("nozzle_temperature", prev_filament_id) : 210;
    int new_filament_temp = full_config.opt_int("nozzle_temperature", next_filament_id);
    config.set_key_value("old_filament_temp", new ConfigOptionInt(old_filament_temp));
    config.set_key_value("new_filament_temp", new ConfigOptionInt(new_filament_temp));

    // Feed rates
    float filament_diameter = full_config.opt_float("filament_diameter", next_filament_id);
    float filament_area = float((M_PI / 4.f) * filament_diameter * filament_diameter);
    int old_e_feedrate = (prev_filament_id >= 0) ?
        (int)(60.0 * full_config.opt_float("filament_max_volumetric_speed", prev_filament_id) / filament_area) : 200;
    if (old_e_feedrate == 0) old_e_feedrate = 100;
    int new_e_feedrate = (int)(60.0 * full_config.opt_float("filament_max_volumetric_speed", next_filament_id) / filament_area);
    if (new_e_feedrate == 0) new_e_feedrate = 100;
    config.set_key_value("old_filament_e_feedrate", new ConfigOptionInt(old_e_feedrate));
    config.set_key_value("new_filament_e_feedrate", new ConfigOptionInt(new_e_feedrate));

    // Flush length and per-pass distribution
    float purge_length = (float)(purge_volume_mm3 / filament_area);
    config.set_key_value("flush_length", new ConfigOptionFloat(purge_length));
    config.set_key_value("first_flush_volume", new ConfigOptionFloat(purge_length / 2.f));
    config.set_key_value("second_flush_volume", new ConfigOptionFloat(purge_length / 2.f));

    // Distribute flush across passes (same logic as GCode.cpp)
    static constexpr int    g_max_flush_count     = 4;
    static constexpr double g_purge_volume_one_time = 135.0;
    int flush_count = std::min(g_max_flush_count, std::max(1, (int)std::round(purge_volume_mm3 / g_purge_volume_one_time)));
    float flush_unit = purge_length / flush_count;
    for (int i = 0; i < g_max_flush_count; ++i) {
        char key[64];
        snprintf(key, sizeof(key), "flush_length_%d", i + 1);
        config.set_key_value(key, new ConfigOptionFloat(i < flush_count ? flush_unit : 0.f));
    }

    // Position after toolchange (not critical — printer travels to next extrusion anyway)
    config.set_key_value("x_after_toolchange", new ConfigOptionFloat(0.f));
    config.set_key_value("y_after_toolchange", new ConfigOptionFloat(0.f));
    config.set_key_value("z_after_toolchange", new ConfigOptionFloat((float)layer_z));

    // Travel points (BBL uses these for wipe avoidance — set to safe defaults)
    config.set_key_value("travel_point_1_x", new ConfigOptionFloat(0.f));
    config.set_key_value("travel_point_1_y", new ConfigOptionFloat(0.f));
    config.set_key_value("travel_point_2_x", new ConfigOptionFloat(0.f));
    config.set_key_value("travel_point_2_y", new ConfigOptionFloat(0.f));
    config.set_key_value("travel_point_3_x", new ConfigOptionFloat(0.f));
    config.set_key_value("travel_point_3_y", new ConfigOptionFloat(0.f));

    // Flush volumetric speeds and temperatures
    auto flush_v_speeds = full_config.option<ConfigOptionFloats>("filament_flush_volumetric_speed");
    auto flush_temps = full_config.option<ConfigOptionInts>("filament_flush_temp");
    if (flush_v_speeds) {
        auto vals = flush_v_speeds->values;
        for (size_t i = 0; i < vals.size(); ++i) {
            if (vals[i] == 0)
                vals[i] = full_config.opt_float("filament_max_volumetric_speed", i);
        }
        config.set_key_value("flush_volumetric_speeds", new ConfigOptionFloats(vals));
    }
    if (flush_temps) {
        auto vals = flush_temps->values;
        for (size_t i = 0; i < vals.size(); ++i) {
            if (vals[i] == 0)
                vals[i] = full_config.opt_int("nozzle_temperature_range_high", i);
        }
        config.set_key_value("flush_temperatures", new ConfigOptionInts(vals));
    }

    // Outer wall volumetric speed (used for dynamic extrusion calibration)
    float nozzle_diameter = full_config.opt_float("nozzle_diameter", 0);
    float outer_wall_speed = full_config.opt_float("outer_wall_speed", 0);
    float outer_wall_line_width = full_config.opt_float("outer_wall_line_width", 0);
    if (outer_wall_line_width <= 0) outer_wall_line_width = nozzle_diameter;
    float layer_height = full_config.opt_float("layer_height", 0);
    if (layer_height <= 0) layer_height = 0.2f;
    float outer_wall_vol_speed = outer_wall_speed * outer_wall_line_width * layer_height;
    config.set_key_value("outer_wall_volumetric_speed", new ConfigOptionFloat(outer_wall_vol_speed));

    // Acceleration
    config.set_key_value("initial_layer_acceleration", new ConfigOptionFloat(full_config.opt_float("initial_layer_acceleration")));
    config.set_key_value("default_acceleration", new ConfigOptionFloat(full_config.opt_float("default_acceleration")));

    // Wipe avoidance
    config.set_key_value("wipe_avoid_perimeter", new ConfigOptionBool(false));
    config.set_key_value("wipe_avoid_pos_x", new ConfigOptionFloat(0.f));

    // Prime tower interface (not applicable for calibration)
    config.set_key_value("is_prime_tower_interface", new ConfigOptionBool(false));
    config.set_key_value("filament_tower_interface_purge_volume", new ConfigOptionFloat(0.f));
    config.set_key_value("filament_tower_interface_print_temp", new ConfigOptionInt(0));

    return config;
}

// Parse filament indices from a "calib_A_B" object name.
// Returns true if parsed successfully.
static bool parse_calib_name(const std::string &name, int &from_filament, int &to_filament)
{
    // Match "calib_X_Y" where X and Y are integers
    std::regex re("calib_(\\d+)_(\\d+)");
    std::smatch m;
    if (std::regex_search(name, m, re) && m.size() == 3) {
        from_filament = std::stoi(m[1].str());
        to_filament = std::stoi(m[2].str());
        return true;
    }
    return false;
}

std::string PurgeCalibrationGenerator::postprocess_gcode(
    const std::string &gcode,
    const std::vector<TransitionPair> &ordered_pairs,
    const DynamicPrintConfig &full_config,
    double calibration_purge_volume_mm3)
{
    if (ordered_pairs.empty()) return gcode;

    // Get the change_filament_gcode template from the config
    std::string change_template = full_config.opt_string("change_filament_gcode");
    if (change_template.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "PurgeCalibration postprocess: no change_filament_gcode template";
        return gcode;
    }

    // Split gcode into lines
    std::vector<std::string> lines;
    {
        std::istringstream iss(gcode);
        std::string line;
        while (std::getline(iss, line))
            lines.push_back(line);
    }

    // Find the LAST CHANGE_LAYER marker (topmost layer)
    int last_change_layer_line = -1;
    double last_layer_z = 0;
    for (int i = (int)lines.size() - 1; i >= 0; --i) {
        if (lines[i] == "; CHANGE_LAYER") {
            last_change_layer_line = i;
            // Parse Z from the next line: "; Z_HEIGHT: X.XX"
            if (i + 1 < (int)lines.size() && boost::starts_with(lines[i + 1], "; Z_HEIGHT:")) {
                try {
                    last_layer_z = std::stod(lines[i + 1].substr(12));
                } catch (...) {}
            }
            break;
        }
    }

    if (last_change_layer_line < 0) {
        BOOST_LOG_TRIVIAL(warning) << "PurgeCalibration postprocess: no CHANGE_LAYER found";
        return gcode;
    }

    BOOST_LOG_TRIVIAL(info) << "PurgeCalibration postprocess: top layer at line "
        << last_change_layer_line << " Z=" << last_layer_z;

    // Find all calib_X_Y markers on the top layer (from CHANGE_LAYER to end or next CHANGE_LAYER)
    struct StripMarker {
        int line_idx;
        int from_filament;
        int to_filament;
    };
    std::vector<StripMarker> strip_markers;
    for (int i = last_change_layer_line; i < (int)lines.size(); ++i) {
        if (boost::starts_with(lines[i], "; printing object calib_")) {
            int a, b;
            if (parse_calib_name(lines[i], a, b)) {
                strip_markers.push_back({i, a, b});
            }
        }
    }

    if (strip_markers.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "PurgeCalibration postprocess: no calib_ markers on top layer";
        return gcode;
    }

    BOOST_LOG_TRIVIAL(info) << "PurgeCalibration postprocess: found " << strip_markers.size()
        << " strip markers on top layer";

    // Also find and collect existing M620...M621 filament change blocks on the top layer
    // so we can remove them. A block starts with "M620 S[0-9]A" and ends with "M621 S[0-9]A".
    struct ChangeBlock {
        int start_line; // line of the ";===== machine:" comment (or M620 line)
        int end_line;   // line of M621 + a few trailing lines
    };
    std::vector<ChangeBlock> existing_changes;
    {
        std::regex m620_re("^M620 S\\dA");
        std::regex m621_re("^M621 S\\dA");
        for (int i = last_change_layer_line; i < (int)lines.size(); ++i) {
            if (std::regex_search(lines[i], m620_re)) {
                // Find the start of this block — look backwards for ";===== machine:" comment
                int block_start = i;
                for (int j = i - 1; j >= last_change_layer_line && j >= i - 5; --j) {
                    if (boost::starts_with(lines[j], ";===== machine:") ||
                        boost::starts_with(lines[j], "G392 S0") ||
                        boost::starts_with(lines[j], "M1007 S0")) {
                        block_start = j;
                    }
                }
                // Find M621 end
                int block_end = i;
                for (int j = i + 1; j < (int)lines.size(); ++j) {
                    if (std::regex_search(lines[j], m621_re)) {
                        block_end = j;
                        // Include a few trailing lines (G392, M1007, M106, M104, etc.)
                        for (int k = j + 1; k < (int)lines.size() && k <= j + 5; ++k) {
                            if (boost::starts_with(lines[k], "G392") ||
                                boost::starts_with(lines[k], "M1007") ||
                                boost::starts_with(lines[k], "M106 S") ||
                                boost::starts_with(lines[k], "M104 S") ||
                                lines[k].empty()) {
                                block_end = k;
                            } else {
                                break;
                            }
                        }
                        break;
                    }
                }
                existing_changes.push_back({block_start, block_end});
            }
        }
    }

    BOOST_LOG_TRIVIAL(info) << "PurgeCalibration postprocess: found " << existing_changes.size()
        << " existing filament change blocks to remove on top layer";

    // Build set of lines to remove (existing filament change blocks on the top layer)
    std::set<int> lines_to_remove;
    for (const auto &block : existing_changes) {
        for (int i = block.start_line; i <= block.end_line; ++i)
            lines_to_remove.insert(i);
    }

    // Set up PlaceholderParser with the full config
    PlaceholderParser pp(&full_config);
    pp.apply_config(DynamicPrintConfig(full_config));

    // Determine what filament was loaded at the start of the top layer.
    // Scan backwards from the top layer to find the last M620 S[x]A command.
    int current_filament = 0;
    {
        std::regex t_re("^M620 S(\\d)A");
        for (int i = last_change_layer_line - 1; i >= 0; --i) {
            std::smatch m;
            if (std::regex_search(lines[i], m, t_re)) {
                current_filament = std::stoi(m[1].str());
                break;
            }
        }
    }

    BOOST_LOG_TRIVIAL(info) << "PurgeCalibration postprocess: filament at top layer start = " << current_filament;

    // Generate filament change gcode for each strip
    // Map: strip marker line index -> gcode to insert before it
    std::map<int, std::string> insertions;
    int toolchange_count = 10; // arbitrary starting count (must be > 1 for spiral lift)

    // Get the standard purge volume for "loading" changes (from the flush matrix)
    auto recommended = WipingDialog::CalcFlushingVolumes(0);

    for (const auto &marker : strip_markers) {
        std::string insert_gcode;
        int filament_a = marker.from_filament;
        int filament_b = marker.to_filament;

        // Step 1: If current filament != A, change to A with standard purge
        if (current_filament != filament_a) {
            double load_purge = 200.0; // default
            if (current_filament < (int)recommended.size() &&
                filament_a < (int)recommended[current_filament].size())
                load_purge = recommended[current_filament][filament_a];
            load_purge = std::max(50.0, load_purge);

            BOOST_LOG_TRIVIAL(info) << "PurgeCalibration: inserting change " << current_filament
                << " -> " << filament_a << " (load, " << load_purge << "mm3)";

            auto load_config = build_toolchange_config(full_config,
                current_filament, filament_a, last_layer_z, last_layer_z,
                load_purge, toolchange_count++);

            try {
                insert_gcode += "; --- PURGE CALIBRATION: load filament " + std::to_string(filament_a + 1) + " ---\n";
                insert_gcode += pp.process(change_template, filament_a, &load_config);
                insert_gcode += "\n;_FORCE_RESUME_FAN_SPEED\n";
            } catch (const std::exception &e) {
                BOOST_LOG_TRIVIAL(error) << "PurgeCalibration: template eval failed (load): " << e.what();
                return gcode; // bail out on template error
            }
            current_filament = filament_a;
        }

        // Step 2: Change A -> B with calibration purge amount
        {
            BOOST_LOG_TRIVIAL(info) << "PurgeCalibration: inserting change " << filament_a
                << " -> " << filament_b << " (calib, " << calibration_purge_volume_mm3 << "mm3)";

            auto calib_config = build_toolchange_config(full_config,
                filament_a, filament_b, last_layer_z, last_layer_z,
                calibration_purge_volume_mm3, toolchange_count++);

            try {
                insert_gcode += "; --- PURGE CALIBRATION: test " + std::to_string(filament_a + 1)
                    + " -> " + std::to_string(filament_b + 1) + " ("
                    + std::to_string((int)calibration_purge_volume_mm3) + "mm3) ---\n";
                insert_gcode += pp.process(change_template, filament_b, &calib_config);
                insert_gcode += "\n;_FORCE_RESUME_FAN_SPEED\n";
            } catch (const std::exception &e) {
                BOOST_LOG_TRIVIAL(error) << "PurgeCalibration: template eval failed (calib): " << e.what();
                return gcode;
            }
            current_filament = filament_b;
        }

        insertions[marker.line_idx] = insert_gcode;
    }

    // Reassemble gcode: skip removed lines, insert new changes before strip markers
    std::ostringstream out;
    for (int i = 0; i < (int)lines.size(); ++i) {
        // Insert filament change before this line if it's a strip marker
        auto it = insertions.find(i);
        if (it != insertions.end()) {
            out << it->second;
        }

        // Skip lines that are part of removed filament change blocks
        if (lines_to_remove.count(i))
            continue;

        out << lines[i] << "\n";
    }

    std::string result = out.str();
    BOOST_LOG_TRIVIAL(info) << "PurgeCalibration postprocess: done, "
        << insertions.size() << " changes inserted, "
        << lines_to_remove.size() << " lines removed";
    return result;
}

} // namespace GUI
} // namespace Slic3r
