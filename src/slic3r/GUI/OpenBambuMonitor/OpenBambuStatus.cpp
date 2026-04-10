#include "OpenBambuStatus.hpp"
#include <nlohmann/json.hpp>
#include <boost/log/trivial.hpp>

using json = nlohmann::json;

namespace Slic3r { namespace GUI {

// Convert fan speed string ("0"-"15") to percentage (0-100)
static int fan_speed_to_percent(const std::string &s)
{
    int v = 0;
    try { v = std::stoi(s); } catch (...) {}
    // Bambu reports 0-15 (4-bit), map to 0-100
    return (v * 100 + 7) / 15; // round
}

void parse_mqtt_status(const std::string &json_str, PrinterStatus &status)
{
    try {
        auto root = json::parse(json_str);

        // The status lives under "print" key
        json j;
        if (root.contains("print"))
            j = root["print"];
        else
            j = root; // sometimes the print object is the root

        // Print state
        if (j.contains("gcode_state"))      status.gcode_state = j["gcode_state"].get<std::string>();
        if (j.contains("mc_percent"))        status.percent = j["mc_percent"].get<int>();
        if (j.contains("mc_remaining_time")) status.remaining_min = j["mc_remaining_time"].get<int>();
        if (j.contains("layer_num"))         status.current_layer = j["layer_num"].get<int>();
        if (j.contains("total_layer_num"))   status.total_layers = j["total_layer_num"].get<int>();
        if (j.contains("gcode_file"))        status.gcode_file = j["gcode_file"].get<std::string>();
        if (j.contains("subtask_name"))      status.subtask_name = j["subtask_name"].get<std::string>();

        // Temperatures
        if (j.contains("nozzle_temper"))        status.nozzle_temp = j["nozzle_temper"].get<float>();
        if (j.contains("nozzle_target_temper")) status.nozzle_target = j["nozzle_target_temper"].get<float>();
        if (j.contains("bed_temper"))           status.bed_temp = j["bed_temper"].get<float>();
        if (j.contains("bed_target_temper"))    status.bed_target = j["bed_target_temper"].get<float>();
        if (j.contains("chamber_temper"))       status.chamber_temp = j["chamber_temper"].get<float>();

        // Push temperature history
        status.nozzle_history.push(status.nozzle_temp);
        status.bed_history.push(status.bed_temp);

        // Fans
        if (j.contains("cooling_fan_speed"))  status.fan_part = fan_speed_to_percent(j["cooling_fan_speed"].get<std::string>());
        if (j.contains("big_fan1_speed"))     status.fan_aux = fan_speed_to_percent(j["big_fan1_speed"].get<std::string>());
        if (j.contains("big_fan2_speed"))     status.fan_chamber = fan_speed_to_percent(j["big_fan2_speed"].get<std::string>());
        if (j.contains("heatbreak_fan_speed")) status.fan_heatbreak = fan_speed_to_percent(j["heatbreak_fan_speed"].get<std::string>());

        // Speed
        if (j.contains("spd_lvl")) status.speed_level = j["spd_lvl"].get<int>();
        if (j.contains("spd_mag")) status.speed_mag = j["spd_mag"].get<int>();

        // AMS
        if (j.contains("ams") && j["ams"].contains("ams") && j["ams"]["ams"].is_array()) {
            status.ams_units.clear();
            for (const auto &ams_json : j["ams"]["ams"]) {
                AmsUnit unit;
                if (ams_json.contains("id")) unit.id = std::stoi(ams_json["id"].get<std::string>());
                if (ams_json.contains("humidity")) unit.humidity = ams_json["humidity"].get<std::string>();
                if (ams_json.contains("temp")) unit.temp = std::stof(ams_json["temp"].get<std::string>());

                if (ams_json.contains("tray") && ams_json["tray"].is_array()) {
                    for (const auto &tray_json : ams_json["tray"]) {
                        int tray_id = -1;
                        if (tray_json.contains("id")) {
                            auto id_val = tray_json["id"];
                            tray_id = id_val.is_string() ? std::stoi(id_val.get<std::string>()) : id_val.get<int>();
                        }
                        if (tray_id >= 0 && tray_id < 4) {
                            auto &tray = unit.trays[tray_id];
                            tray.id = tray_id;
                            tray.present = tray_json.contains("tray_type");
                            if (tray_json.contains("tray_type"))     tray.type = tray_json["tray_type"].get<std::string>();
                            if (tray_json.contains("tray_color"))    tray.color = tray_json["tray_color"].get<std::string>();
                            if (tray_json.contains("nozzle_temp_min")) {
                                auto v = tray_json["nozzle_temp_min"];
                                tray.nozzle_temp_min = v.is_string() ? std::stoi(v.get<std::string>()) : v.get<int>();
                            }
                            if (tray_json.contains("nozzle_temp_max")) {
                                auto v = tray_json["nozzle_temp_max"];
                                tray.nozzle_temp_max = v.is_string() ? std::stoi(v.get<std::string>()) : v.get<int>();
                            }
                        }
                    }
                }
                status.ams_units.push_back(std::move(unit));
            }
        }
        if (j.contains("ams") && j["ams"].contains("tray_now")) {
            auto tn = j["ams"]["tray_now"];
            status.current_tray = tn.is_string() ? std::stoi(tn.get<std::string>()) : tn.get<int>();
        }

        // Lights
        if (j.contains("lights_report") && j["lights_report"].is_array()) {
            for (const auto &light : j["lights_report"]) {
                if (light.contains("node") && light["node"] == "chamber_light" && light.contains("mode"))
                    status.chamber_light = light["mode"].get<std::string>();
            }
        }

        // Camera
        if (j.contains("ipcam")) {
            auto &cam = j["ipcam"];
            if (cam.contains("resolution"))   status.ipcam_resolution = cam["resolution"].get<std::string>();
            if (cam.contains("ipcam_record")) status.ipcam_recording = (cam["ipcam_record"] == "enable");
            if (cam.contains("timelapse"))     status.ipcam_timelapse = (cam["timelapse"] == "enable");
        }

        // WiFi
        if (j.contains("wifi_signal")) status.wifi_signal = j["wifi_signal"].get<std::string>();

        status.last_update = std::chrono::steady_clock::now();

    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(warning) << "OpenBambu: failed to parse MQTT status: " << e.what();
    }
}

}} // namespace Slic3r::GUI
