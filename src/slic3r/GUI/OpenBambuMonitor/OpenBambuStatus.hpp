#pragma once

// OpenBambu printer status model.
// Parsed from MQTT JSON status updates. The UI renders this struct.

#include <string>
#include <vector>
#include <array>
#include <chrono>
#include <mutex>
#include <cstdint>

namespace Slic3r { namespace GUI {

// Ring buffer for temperature sparkline (5 minutes at 1Hz = 300 samples)
class TempHistory {
public:
    static constexpr int CAPACITY = 300;

    void push(float temp) {
        m_data[m_write_pos % CAPACITY] = temp;
        ++m_write_pos;
    }

    int size() const { return m_write_pos < CAPACITY ? m_write_pos : CAPACITY; }

    // Get sample at index i (0 = oldest visible, size()-1 = newest)
    float at(int i) const {
        int start = m_write_pos < CAPACITY ? 0 : (m_write_pos % CAPACITY);
        return m_data[(start + i) % CAPACITY];
    }

    float min_val() const {
        float v = 999;
        for (int i = 0; i < size(); ++i) v = std::min(v, at(i));
        return v;
    }

    float max_val() const {
        float v = -999;
        for (int i = 0; i < size(); ++i) v = std::max(v, at(i));
        return v;
    }

private:
    std::array<float, CAPACITY> m_data{};
    int m_write_pos = 0;
};

struct AmsTray {
    int id = -1;
    std::string type;       // "PLA", "PETG", etc.
    std::string color;      // "FF0000FF" (RRGGBBAA hex)
    int nozzle_temp_min = 0;
    int nozzle_temp_max = 0;
    bool present = false;
};

struct AmsUnit {
    int id = -1;
    std::string humidity;   // "1"-"5" or raw number
    float temp = 0;
    std::array<AmsTray, 4> trays;
};

struct PrinterStatus {
    // Identity
    std::string serial;
    std::string model;      // "BL-P001", "N1", etc.
    std::string name;       // SSDP dev_name or user alias
    std::string ip;

    // Print state
    std::string gcode_state;    // IDLE, RUNNING, PAUSED, PREPARE, FINISH, FAILED
    int         percent = 0;    // mc_percent (0-100)
    int         remaining_min = 0; // mc_remaining_time
    int         current_layer = 0;
    int         total_layers = 0;
    std::string gcode_file;
    std::string subtask_name;

    // Temperatures
    float nozzle_temp = 0;
    float nozzle_target = 0;
    float bed_temp = 0;
    float bed_target = 0;
    float chamber_temp = 0;

    // Fans (string "0"-"15" from MQTT, converted to 0-100%)
    int fan_part = 0;       // cooling_fan_speed
    int fan_aux = 0;        // big_fan1_speed
    int fan_chamber = 0;    // big_fan2_speed
    int fan_heatbreak = 0;  // heatbreak_fan_speed

    // Speed
    int speed_level = 2;    // 1=silent, 2=standard, 3=sport, 4=ludicrous
    int speed_mag = 100;    // percentage

    // AMS
    std::vector<AmsUnit> ams_units;
    int current_tray = -1;  // tray_now (255=none, 254=external)

    // Lights
    std::string chamber_light; // "on", "off", "flashing"

    // Camera
    std::string ipcam_resolution; // "1080p", "720p"
    bool ipcam_recording = false;
    bool ipcam_timelapse = false;

    // Connection
    bool connected = false;
    std::string wifi_signal;    // RSSI dBm as string

    // Temperature history for sparklines
    TempHistory nozzle_history;
    TempHistory bed_history;

    // Timestamp of last update
    std::chrono::steady_clock::time_point last_update;
};

// Parse an MQTT status JSON (the "print" object from pushall) into PrinterStatus.
// Updates fields in-place (only overwrites fields present in the JSON).
void parse_mqtt_status(const std::string &json_str, PrinterStatus &status);

// Discovered printer info (from SSDP, before MQTT connection)
struct DiscoveredPrinter {
    std::string serial;
    std::string name;       // SSDP dev_name
    std::string model;
    std::string ip;
    std::string signal;     // WiFi RSSI
    std::string alias;      // user-assigned alias (empty if not set)
    std::string access_code; // stored access code (empty if not paired)
    bool connected = false; // currently connected via MQTT
    std::chrono::steady_clock::time_point last_seen;
};

}} // namespace Slic3r::GUI
