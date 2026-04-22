#include "FilamentLookaheadPostProcessor.hpp"

#include <boost/log/trivial.hpp>

#include <fstream>
#include <sstream>

namespace Slic3r {

namespace {

// Parse a "key=value" token. Returns empty string if key not found.
static std::string get_attr(const std::string &line, const std::string &key)
{
    const std::string needle = " " + key + "=";
    auto pos = line.find(needle);
    if (pos == std::string::npos) return {};
    pos += needle.size();
    auto end = line.find(' ', pos);
    if (end == std::string::npos) end = line.size();
    return line.substr(pos, end - pos);
}

static size_t to_size(const std::string &s) { return s.empty() ? 0 : std::stoul(s); }
static unsigned int to_uint(const std::string &s) { return s.empty() ? 0u : (unsigned int)std::stoul(s); }
static double to_double(const std::string &s) { return s.empty() ? 0.0 : std::stod(s); }

static std::vector<unsigned int> parse_csv_uints(const std::string &s)
{
    std::vector<unsigned int> out;
    if (s.empty() || s == "-") return out;
    size_t start = 0;
    while (start < s.size()) {
        auto comma = s.find(',', start);
        if (comma == std::string::npos) comma = s.size();
        std::string tok = s.substr(start, comma - start);
        if (!tok.empty())
            out.push_back((unsigned int)std::stoul(tok));
        start = comma + 1;
    }
    return out;
}

} // namespace

bool FilamentLookaheadPostProcessor::parse()
{
    m_blocks.clear();
    m_layers.clear();

    // Stack to pair BEGIN/END markers.
    std::vector<size_t> open_stack; // index into m_blocks of currently-open block

    for (size_t i = 0; i < m_lines.size(); ++i) {
        const std::string &line = m_lines[i];
        // Early-out for lines that aren't our markers. Comments starting with
        // "; LOOKAHEAD_" are rare enough that a substring test is fine.
        if (line.find("; LOOKAHEAD_") != 0) continue;

        if (line.rfind("; LOOKAHEAD_LAYER_INFO", 0) == 0) {
            LayerInfo li;
            li.line_idx        = i;
            li.layer_idx       = to_size(get_attr(line, "layer"));
            li.tower_filaments = parse_csv_uints(get_attr(line, "tower_filaments"));
            m_layers.push_back(li);
        } else if (line.rfind("; LOOKAHEAD_BLOCK_BEGIN", 0) == 0) {
            TowerBlock b;
            b.begin_line   = i;
            b.end_line     = (size_t)-1; // filled when END is found
            b.layer        = to_size(get_attr(line, "layer"));
            b.extruder     = to_uint(get_attr(line, "extruder"));
            b.base_layer   = to_size(get_attr(line, "base_layer"));
            b.stack_index  = to_size(get_attr(line, "stack_index"));
            b.extra_layers = to_size(get_attr(line, "extra_layers"));
            const std::string role = get_attr(line, "role");
            b.is_base = (role == "base");
            b.z       = to_double(get_attr(line, "z"));
            m_blocks.push_back(b);
            open_stack.push_back(m_blocks.size() - 1);
        } else if (line.rfind("; LOOKAHEAD_BLOCK_END", 0) == 0) {
            if (open_stack.empty()) {
                BOOST_LOG_TRIVIAL(error) << "[FLA-PP] unbalanced BLOCK_END at line " << i
                    << " - no matching BLOCK_BEGIN";
                return false;
            }
            size_t open_idx = open_stack.back();
            open_stack.pop_back();
            m_blocks[open_idx].end_line = i;

            // Sanity-check: the end marker's layer+base_layer+stack_index+extruder
            // must match the begin.
            TowerBlock &b = m_blocks[open_idx];
            if (b.layer != to_size(get_attr(line, "layer")) ||
                b.extruder != to_uint(get_attr(line, "extruder")) ||
                b.base_layer != to_size(get_attr(line, "base_layer")) ||
                b.stack_index != to_size(get_attr(line, "stack_index"))) {
                BOOST_LOG_TRIVIAL(error) << "[FLA-PP] BLOCK_END at line " << i
                    << " attributes do not match paired BLOCK_BEGIN at line " << b.begin_line;
                return false;
            }
        }
        // LOOKAHEAD_EXCLUSION_ZONE lines are ignored by the post-processor
        // (they're viewer metadata already handled by GCodeProcessor).
    }

    if (!open_stack.empty()) {
        BOOST_LOG_TRIVIAL(error) << "[FLA-PP] " << open_stack.size()
            << " unclosed BLOCK_BEGIN marker(s) at end of file";
        return false;
    }
    return true;
}

bool FilamentLookaheadPostProcessor::write(const std::string &path) const
{
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    for (const auto &line : m_lines) {
        f << line << "\n";
    }
    return (bool)f;
}

bool FilamentLookaheadPostProcessor::process(const std::string &path, bool apply_transform)
{
    m_lines.clear();
    m_blocks.clear();
    m_layers.clear();

    // Read file.
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        BOOST_LOG_TRIVIAL(error) << "[FLA-PP] cannot open " << path;
        return false;
    }
    std::string line;
    while (std::getline(f, line)) {
        // Strip trailing \r for Windows line endings so we write consistent \n.
        if (!line.empty() && line.back() == '\r') line.pop_back();
        m_lines.push_back(std::move(line));
    }
    f.close();

    if (!parse()) {
        BOOST_LOG_TRIVIAL(error) << "[FLA-PP] parse failed, leaving gcode unchanged";
        return false;
    }

    BOOST_LOG_TRIVIAL(warning) << "[FLA-PP] parsed " << m_lines.size() << " lines, "
        << m_blocks.size() << " tower blocks, " << m_layers.size() << " layer-info markers"
        << " (shadow verification only - transform=" << (apply_transform ? "on" : "off") << ")";

    // Option B direction: the actual batching transform moved to
    // process_layer via GCode::emit_lookahead_tower_extras. This
    // post-processor stays as a verification shadow — always writes
    // back verbatim. `apply_transform` is currently unused but retained
    // in the API for future text-based cleanups (e.g. Phase 6d wipe
    // tower rewrite, if implemented as post-process rather than
    // ToolOrdering regen).
    (void)apply_transform;

    if (!write(path)) {
        BOOST_LOG_TRIVIAL(error) << "[FLA-PP] write failed for " << path;
        return false;
    }
    return true;
}

} // namespace Slic3r
