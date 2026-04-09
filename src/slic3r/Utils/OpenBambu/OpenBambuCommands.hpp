#pragma once

// OpenBambu Command Builders
//
// JSON command builders for all Bambu printer MQTT commands.
// Each function returns a JSON string ready to publish to device/{serial}/request.
// Sequence IDs are auto-incremented.

#include <string>
#include <vector>
#include <sstream>
#include <cstdint>

namespace OpenBambu {
namespace Commands {

// Auto-incrementing sequence ID
inline int &next_sequence_id()
{
    static int id = 0;
    return id;
}

inline std::string seq()
{
    return std::to_string(next_sequence_id()++);
}

// ========================================================================
// Info commands
// ========================================================================

inline std::string get_version()
{
    return R"({"info":{"sequence_id":")" + seq() + R"(","command":"get_version"}})";
}

// ========================================================================
// Pushing commands
// ========================================================================

inline std::string pushall()
{
    return R"({"pushing":{"sequence_id":")" + seq() + R"(","command":"pushall","version":1,"push_target":1}})";
}

inline std::string start_push()
{
    return R"({"pushing":{"sequence_id":")" + seq() + R"(","command":"start"}})";
}

// ========================================================================
// Print control commands
// ========================================================================

inline std::string pause()
{
    return R"({"print":{"sequence_id":")" + seq() + R"(","command":"pause","param":""}})";
}

inline std::string resume()
{
    return R"({"print":{"sequence_id":")" + seq() + R"(","command":"resume","param":""}})";
}

inline std::string stop()
{
    return R"({"print":{"sequence_id":")" + seq() + R"(","command":"stop","param":""}})";
}

// Print speed: 1=silent, 2=standard, 3=sport, 4=ludicrous
inline std::string print_speed(int level)
{
    return R"({"print":{"sequence_id":")" + seq() + R"(","command":"print_speed","param":")" + std::to_string(level) + R"("}})";
}

// ========================================================================
// GCode commands
// ========================================================================

// Send raw gcode to execute. Use \n for multiple lines.
inline std::string gcode_line(const std::string &gcode)
{
    // Escape any quotes in the gcode string
    std::string escaped;
    for (char c : gcode) {
        if (c == '"') escaped += "\\\"";
        else if (c == '\\') escaped += "\\\\";
        else escaped += c;
    }
    return R"({"print":{"sequence_id":")" + seq() + R"(","command":"gcode_line","param":")" + escaped + R"("}})";
}

// Print a gcode file already on the printer's filesystem
inline std::string gcode_file(const std::string &filename)
{
    return R"({"print":{"sequence_id":")" + seq() + R"(","command":"gcode_file","param":")" + filename + R"("}})";
}

// ========================================================================
// Project file (start a print from uploaded 3MF/gcode)
// ========================================================================

inline std::string project_file(const std::string &filename,
                                 const std::string &subtask_name,
                                 int plate_idx,
                                 bool use_ams,
                                 const std::vector<int> &ams_mapping,
                                 bool bed_levelling = true,
                                 bool flow_cali = true,
                                 bool vibration_cali = true,
                                 bool timelapse = false,
                                 bool layer_inspect = true)
{
    std::ostringstream json;
    json << R"({"print":{"sequence_id":")" << seq() << R"(",)"
         << R"("command":"project_file",)"
         << R"("param":"Metadata/plate_)" << plate_idx << R"(.gcode",)"
         << R"("project_id":"0","profile_id":"0","task_id":"0","subtask_id":"0",)"
         << R"("subtask_name":")" << subtask_name << R"(",)"
         << R"("url":"file:///sdcard/)" << filename << R"(",)"
         << R"("md5":"",)"
         << R"("timelapse":)" << (timelapse ? "true" : "false") << ","
         << R"("bed_type":"auto",)"
         << R"("bed_levelling":)" << (bed_levelling ? "true" : "false") << ","
         << R"("flow_cali":)" << (flow_cali ? "true" : "false") << ","
         << R"("vibration_cali":)" << (vibration_cali ? "true" : "false") << ","
         << R"("layer_inspect":)" << (layer_inspect ? "true" : "false") << ","
         << R"("use_ams":)" << (use_ams ? "true" : "false") << ","
         << R"("ams_mapping":[)";
    for (size_t i = 0; i < ams_mapping.size(); ++i) {
        if (i > 0) json << ",";
        json << ams_mapping[i];
    }
    json << "]}}";
    return json.str();
}

// ========================================================================
// Filament commands
// ========================================================================

inline std::string unload_filament()
{
    return R"({"print":{"sequence_id":")" + seq() + R"(","command":"unload_filament"}})";
}

inline std::string ams_change_filament(int target_tray, int curr_temp, int tar_temp)
{
    std::ostringstream json;
    json << R"({"print":{"sequence_id":")" << seq() << R"(",)"
         << R"("command":"ams_change_filament",)"
         << R"("target":)" << target_tray << ","
         << R"("curr_temp":)" << curr_temp << ","
         << R"("tar_temp":)" << tar_temp << "}}";
    return json.str();
}

// ========================================================================
// Calibration
// ========================================================================

// options: bitmask — 1=lidar, 2=bed_level, 4=vibration, 8=motor
inline std::string calibration(int options)
{
    return R"({"print":{"sequence_id":")" + seq() + R"(","command":"calibration","option":)" + std::to_string(options) + "}}";
}

// ========================================================================
// LED control
// ========================================================================

// node: "chamber_light" or "work_light"
// mode: "on", "off", or "flashing"
inline std::string ledctrl(const std::string &node, const std::string &mode,
                            int on_time_ms = 500, int off_time_ms = 500,
                            int loop_times = 1, int interval_ms = 1000)
{
    std::ostringstream json;
    json << R"({"system":{"sequence_id":")" << seq() << R"(",)"
         << R"("command":"ledctrl",)"
         << R"("led_node":")" << node << R"(",)"
         << R"("led_mode":")" << mode << R"(",)"
         << R"("led_on_time":)" << on_time_ms << ","
         << R"("led_off_time":)" << off_time_ms << ","
         << R"("loop_times":)" << loop_times << ","
         << R"("interval_time":)" << interval_ms << "}}";
    return json.str();
}

// ========================================================================
// Skip objects (cancel individual objects mid-print)
// ========================================================================

inline std::string skip_objects(const std::vector<int> &object_ids)
{
    std::ostringstream json;
    json << R"({"print":{"sequence_id":")" << seq() << R"(",)"
         << R"("command":"skip_objects","obj_list":[)";
    for (size_t i = 0; i < object_ids.size(); ++i) {
        if (i > 0) json << ",";
        json << object_ids[i];
    }
    json << "]}}";
    return json.str();
}

// ========================================================================
// Camera control
// ========================================================================

// recording: "enable" or "disable"
inline std::string ipcam_record_set(const std::string &action)
{
    return R"({"camera":{"sequence_id":")" + seq() + R"(","command":"ipcam_record_set","control":")" + action + R"("}})";
}

// timelapse: "enable" or "disable"
inline std::string ipcam_timelapse(const std::string &action)
{
    return R"({"camera":{"sequence_id":")" + seq() + R"(","command":"ipcam_timelapse","control":")" + action + R"("}})";
}

// ========================================================================
// Print options
// ========================================================================

inline std::string print_option(const std::string &option, bool value)
{
    return R"({"print":{"sequence_id":")" + seq() + R"(","command":"print_option",")" + option + R"(":)" + (value ? "true" : "false") + "}}";
}

} // namespace Commands
} // namespace OpenBambu
