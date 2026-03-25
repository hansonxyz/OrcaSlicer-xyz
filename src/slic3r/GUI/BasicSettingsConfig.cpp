#include "BasicSettingsConfig.hpp"

#include <fstream>
#include <boost/filesystem.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/log/trivial.hpp>

#include "libslic3r/Utils.hpp"

namespace Slic3r { namespace GUI {

BasicSettingsConfig& BasicSettingsConfig::instance()
{
    static BasicSettingsConfig s_instance;
    return s_instance;
}

bool BasicSettingsConfig::load()
{
    m_active = false;
    m_fields.clear();

    // Look for basic_settings.cfg in the OrcaSlicer data directory
    boost::filesystem::path config_path = boost::filesystem::path(Slic3r::data_dir()) / "basic_settings.cfg";
    if (!boost::filesystem::exists(config_path)) {
        BOOST_LOG_TRIVIAL(info) << "BasicSettingsConfig: No basic_settings.cfg found at " << config_path.string();
        return false;
    }

    BOOST_LOG_TRIVIAL(info) << "BasicSettingsConfig: Loading " << config_path.string();

    std::ifstream file(config_path.string());
    if (!file.is_open())
        return false;

    enum class Section { None, Fields };
    Section section = Section::None;
    std::string line;

    while (std::getline(file, line)) {
        boost::algorithm::trim(line);

        // Skip empty lines and comments
        if (line.empty() || line[0] == '#' || line[0] == ';')
            continue;

        // Section headers
        if (line == "[fields]") {
            section = Section::Fields;
            continue;
        }
        // Skip unknown sections
        if (line[0] == '[') {
            section = Section::None;
            continue;
        }

        if (section == Section::Fields) {
            // Each line is a config key name
            m_fields.insert(line);
        }
    }

    if (!m_fields.empty()) {
        m_active = true;
        BOOST_LOG_TRIVIAL(info) << "BasicSettingsConfig: Loaded " << m_fields.size() << " fields";
    }

    return m_active;
}

bool BasicSettingsConfig::is_field_visible(const std::string &opt_key) const
{
    return m_fields.count(opt_key) > 0;
}

}} // namespace Slic3r::GUI
