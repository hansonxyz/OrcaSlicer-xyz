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
#include "libslic3r/FlushVolCalc.hpp"

#include <boost/log/trivial.hpp>
#include <boost/filesystem.hpp>
#include <wx/timer.h>
#include <wx/msgdlg.h>
#include <algorithm>
#include <cmath>

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

    // Poll for slice completion, then copy gcode, cleanup, and show result
    auto *timer = new wxTimer();
    timer->Bind(wxEVT_TIMER, [plater, gcode_path_str, temp_plate_idx, obj_start_idx, original_prime_tower, timer](wxTimerEvent &) {
        int original_plate = s_cleanup_state.original_plate_idx;

        if (plater->is_background_process_slicing())
            return; // still slicing

        timer->Stop();
        BOOST_LOG_TRIVIAL(info) << "PurgeCalibration: slicing complete, starting cleanup";

        // Get the sliced gcode from the plate's temp path
        BOOST_LOG_TRIVIAL(info) << "PurgeCalibration: getting gcode path from plate " << temp_plate_idx;
        auto *plate = plater->get_partplate_list().get_plate(temp_plate_idx);
        std::string sliced_gcode;
        if (plate)
            sliced_gcode = plate->get_tmp_gcode_path();
        BOOST_LOG_TRIVIAL(info) << "PurgeCalibration: sliced_gcode=" << sliced_gcode;

        if (!sliced_gcode.empty() && boost::filesystem::exists(sliced_gcode)) {
            try {
                boost::filesystem::copy_file(sliced_gcode, gcode_path_str,
                    boost::filesystem::copy_options::overwrite_existing);
                BOOST_LOG_TRIVIAL(info) << "PurgeCalibration: gcode copied to " << gcode_path_str;
            } catch (const std::exception &e) {
                BOOST_LOG_TRIVIAL(error) << "PurgeCalibration: copy failed: " << e.what();
            }
        } else {
            BOOST_LOG_TRIVIAL(warning) << "PurgeCalibration: no gcode file found";
        }

        // Don't restore prime tower yet — keep it disabled while the calibration
        // plate is active. The undo (Ctrl+Z) will restore all settings including
        // prime tower when the user is done with the calibration print.

        // NOTE: We do NOT delete the calibration objects/plate here.
        // Deleting model objects while the Print still references them causes
        // an access violation in Print::support_material_extruders.
        // The user can delete the calibration plate manually, or we add
        // a cleanup step later that properly invalidates the Print first.

        // --- Keep calibration plate, switch to Preview ---
        // Don't undo yet — the plate must remain for the Print button to work.
        // The gcode is on the plate from slicing. Switch to Preview tab.
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

std::string PurgeCalibrationGenerator::postprocess_gcode(
    const std::string &gcode,
    const std::vector<TransitionPair> &ordered_pairs,
    int label_filament,
    int strip_filament,
    const std::string &change_filament_gcode_template)
{
    // TODO: implement gcode post-processing
    return gcode;
}

} // namespace GUI
} // namespace Slic3r
