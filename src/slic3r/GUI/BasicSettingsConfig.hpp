#pragma once

#include <string>
#include <set>

namespace Slic3r { namespace GUI {

// Manages an optional configuration file that defines which process settings
// appear in the "basic" (non-advanced) view. When the file is present,
// only the listed fields are shown in basic mode instead of all comSimple fields.
class BasicSettingsConfig {
public:
    static BasicSettingsConfig& instance();

    // Load the config file from the OrcaSlicer config directory.
    // Called once at startup. Returns true if a file was found and loaded.
    bool load();

    // Returns true if a custom basic settings file is active.
    bool is_active() const { return m_active; }

    // Returns true if the given config key should be visible in basic mode.
    bool is_field_visible(const std::string &opt_key) const;

private:
    BasicSettingsConfig() = default;

    bool                    m_active = false;
    std::set<std::string>   m_fields;
};

}} // namespace Slic3r::GUI
