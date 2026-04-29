#ifndef slic3r_GUI_IMSlider_hpp_
#define slic3r_GUI_IMSlider_hpp_

#include "TickCode.hpp"
#include <imgui/imgui.h>
#include <wx/slider.h>

#include <set>

class wxMenu;
struct IMGUI_API ImRect;

namespace Slic3r {

using namespace CustomGCode;
class PrintObject;
class Layer;

namespace GUI {

/* For exporting GCode in GCodeWriter is used XYZF_NUM(val) = PRECISION(val, 3) for XYZ values. 
 * So, let use same value as a permissible error for layer height.
 */
constexpr double epsilon() { return 0.0011; }

bool equivalent_areas(const double &bottom_area, const double &top_area);

// return true if color change was detected
bool check_color_change(PrintObject* object, size_t frst_layer_id, size_t layers_cnt, bool check_overhangs,
                        // what to do with detected color change
                        // and return true when detection have to be desturbed
                        std::function<bool(Layer*)> break_condition);

enum SelectedSlider {
    ssUndef = 0,
    ssLower = 1,
    ssHigher = 2
};

enum DrawMode
{
    dmRegular,
    dmSlaPrint,
    dmSequentialFffPrint,
    dmSequentialGCodeView,
};

enum LabelType
{
    ltHeightWithLayer,
    ltHeight,
    ltEstimatedTime,
};

class IMSlider
{
public:
    IMSlider(int lowerValue, int higherValue, int minValue, int maxValue, long style = wxSL_VERTICAL);

    bool init_texture();

    ~IMSlider() {}

    int    GetMinValue() const { return m_min_value; }
    int    GetMaxValue() const { return m_max_value; }
    double GetMinValueD() { return m_values.empty() ? 0. : m_values[m_min_value]; }
    double GetMaxValueD() { return m_values.empty() ? 0. : m_values[m_max_value]; }
    int    GetLowerValue() const { return m_lower_value; }
    int    GetHigherValue() const { return m_higher_value; }
    int    GetActiveValue() const;
    double GetLowerValueD() { return get_double_value(ssLower); }
    double GetHigherValueD() { return get_double_value(ssHigher); }
    SelectedSlider GetSelection() { return m_selection; }

    // Set low and high slider position. If the span is non-empty, disable the "one layer" mode.
    void SetLowerValue(const int lower_val);
    void SetHigherValue(const int higher_val);
    void SetSelectionSpan(const int lower_val, const int higher_val);
    void SetMaxValue(const int max_value);
    void SetKoefForLabels(const double koef) { m_label_koef = koef; }
    void SetSliderValues(const std::vector<double> &values);
    void SetSliderAlternateValues(const std::vector<double> &values) { m_alternate_values = values; }

    // xyz fork: Filament Lookahead viewer Phase 3 — slider tick coloring.
    //
    // Each entry is the slider index (0..max_value) where a lookahead
    // tower's base layer starts. The renderer draws a small colored
    // mark next to the slider track at each of these positions so the
    // user can see at a glance where the towers are. Phase 4 will
    // expand this with one extra mark per upper-stack; for now this is
    // base-layer marks only.
    //
    // extruder_id_hint is the tower's filament extruder (matches
    // GCodeProcessorResult::LookaheadTower::extruder_id) so the mark
    // can be tinted to the tower's filament color. -1 if unknown.
    struct LookaheadTowerMark {
        int slider_index{ 0 };
        int extruder_id_hint{ -1 };
    };
    void SetLookaheadTowerMarks(const std::vector<LookaheadTowerMark> &marks) { m_lookahead_tower_marks = marks; }

    // xyz fork: Filament Lookahead viewer Phase 4 — sub-tick model.
    //
    // The slider's value list grows so that each lookahead tower
    // contributes N entries (one per stack) instead of one. This lets
    // the user scrub within a tower instead of jumping over it. Each
    // slider index gets a parallel EntryMeta record:
    //
    //   - tower_id / stack_index: -1 for normal layers; (T, K) for the
    //     K-th stack of tower T.
    //   - viewer_layer_id: the libvgcode layer_id this slider position
    //     should resolve to when the viewer is asked to render the
    //     range. For normal layers this equals the libvgcode layer_id
    //     directly; for tower stacks it's the tower's base layer_id
    //     (since libvgcode buckets all stacks together — Phase 5 is
    //     where stack-by-stack filtering gets wired through a separate
    //     channel; until then sub-ticks are visually inert).
    struct EntryMeta {
        int8_t tower_id{ -1 };
        int8_t stack_index{ -1 };
        int8_t stack_count{ 0 };      // total stacks in this tower (for "K/M" tooltip display)
        int    viewer_layer_id{ -1 }; // libvgcode layer_id; -1 means "same as slider index"
    };
    void SetEntryMeta(const std::vector<EntryMeta> &meta) { m_entry_meta = meta; }
    int  ToViewerLayerId(int slider_index) const;

    Info GetTicksValues() const;
    void SetTicksValues(const Info &custom_gcode_per_print_z);
    void SetLayersTimes(const std::vector<float> &layers_times, float total_time);
    void SetLayersTimes(const std::vector<double> &layers_times);

    void SetDrawMode(bool is_sequential_print);
    void SetDrawMode(DrawMode mode) { m_draw_mode = mode; }
    //BBS
    void SetExtraStyle(long style) { m_extra_style = style; }
    void SetManipulationMode(Mode mode) { m_mode = mode; }
    Mode GetManipulationMode() const { return m_mode; }
    void SetModeAndOnlyExtruder(const bool is_one_extruder_printed_model, const int only_extruder, bool can_change_color);
    void SetExtruderColors(const std::vector<std::string> &extruder_colors);

    bool IsNewPrint();

    void set_render_as_disabled(bool value) { m_render_as_disabled = value; }
    bool is_rendering_as_disabled() const { return m_render_as_disabled; }

    bool is_horizontal() const { return m_style == wxSL_HORIZONTAL; }
    bool is_one_layer() const { return m_is_one_layer; }
    bool is_lower_at_min() const { return m_lower_value == m_min_value; }
    bool is_higher_at_max() const { return m_higher_value == m_max_value; }
    bool is_full_span() const { return this->is_lower_at_min() && this->is_higher_at_max(); }

    void UseDefaultColors(bool def_colors_on) { m_ticks.set_default_colors(def_colors_on); }

    void on_mouse_wheel(wxMouseEvent& evt);
    void post_ticks_changed_event(Type type = Unknown);
    bool check_ticks_changed_event(Type type);
    bool switch_one_layer_mode();
    void show_go_to_layer(bool show) { m_show_go_to_layer_dialog = show; }

    bool render(int canvas_width, int canvas_height);

    //BBS update scroll value changed
    bool is_dirty() { return m_dirty; }
    void set_as_dirty(bool dirty = true) { m_dirty = dirty; }
    bool is_need_post_tick_event() { return m_is_need_post_tick_changed_event; }
    void reset_post_tick_event(bool val = false) {
        m_is_need_post_tick_changed_event = val;
        m_tick_change_event_type = Type::Unknown;
    }
    Type get_post_tick_event_type() { return m_tick_change_event_type; }

    float m_scale = 1.0;
    void set_scale(float scale = 1.0);
    void on_change_color_mode(bool is_dark);
    void set_menu_enable(bool enable = true) { m_menu_enable = enable; }

protected:
    void add_custom_gcode(std::string custom_gcode);
    void add_code_as_tick(Type type, int selected_extruder = -1);
    void delete_tick(const TickCode& tick);
    void do_go_to_layer(size_t layer_number); //menu
    void correct_lower_value();
    void correct_higher_value();
    bool horizontal_slider(const char* str_id, int* v, int v_min, int v_max, const ImVec2& size, float scale = 1.0);
    void render_go_to_layer_dialog(); //menu
    void render_input_custom_gcode(std::string custom_gcode = ""); //menu
    void render_menu();
    void render_add_menu(); //menu
    void render_edit_menu(const TickCode& tick); //menu
    void draw_background_and_groove(const ImRect& bg_rect, const ImRect& groove);
    void draw_colored_band(const ImRect& groove, const ImRect& slideable_region);
    void draw_custom_label_block(const ImVec2 anchor, Type type);
    void draw_ticks(const ImRect& slideable_region);

    // xyz fork: Filament Lookahead viewer Phase 3 — render small colored
    // marks at every slider position where a lookahead tower's base layer
    // sits. The marks are drawn beside the slider track (opposite side
    // from custom-gcode ticks) so they don't conflict with any existing
    // overlay. Tinted using the tower's filament color when available;
    // falls back to a generic accent color otherwise. No-op when no
    // tower marks have been registered (i.e. lookahead inactive).
    void draw_lookahead_tower_marks(const ImRect& slideable_region);
    void draw_tick_on_mouse_position(const ImRect& slideable_region);
    void show_tooltip(const TickCode& tick); //menu
    void show_tooltip(const std::string tooltip); //menu
    bool vertical_slider(const char* str_id, int* higher_value, int* lower_value,
        std::string& higher_label, std::string& lower_label,
        int v_min, int v_max, const ImVec2& size,
        SelectedSlider& selection, bool one_layer_flag = false, float scale = 1.0f);
    bool is_wipe_tower_layer(int tick) const;

private:
    std::string get_label(int tick, LabelType label_type = ltHeightWithLayer);
    double get_double_value(const SelectedSlider& selection);
    int    get_tick_from_value(double value, bool force_lower_bound = false);
    float get_pos_from_value(int v_min, int v_max, int value, const ImRect& rect);
    int    get_tick_near_point(int v_min, int v_max, const ImVec2& pt, const ImRect& rect);

    std::string get_color_for_tool_change_tick(std::set<TickCode>::const_iterator it) const;
    // Get active extruders for tick.
    // Means one current extruder for not existing tick OR
    // 2 extruders - for existing tick (extruder before ToolChangeCode and extruder of current existing tick)
    // Use those values to disable selection of active extruders
    std::array<int, 2> get_active_extruders_for_tick(int tick) const;

    // Use those values to disable selection of active extruders
    bool m_is_dark = false;

    bool is_osx{false};
    int  m_min_value;
    int  m_max_value;
    int  m_lower_value;
    int  m_higher_value;
    int  m_one_layer_value; // ORCA
    bool m_dirty = false;

    bool m_render_as_disabled{ false };

    SelectedSlider m_selection;
    bool m_is_one_layer       = false;
    bool m_menu_enable        = true; //menu
    bool m_show_menu          = false; //menu
    bool m_show_custom_gcode_window = false; //menu
    bool m_show_go_to_layer_dialog = false; //menu
    bool m_force_mode_apply   = true;
    bool m_is_wipe_tower      = false; // This flag indicates that there is multiple extruder print with wipe tower
    bool m_is_spiral_vase     = false;

    /* BBS slider images */
    void *m_one_layer_on_id;
    void *m_one_layer_on_hover_id;
    void *m_one_layer_off_id;
    void *m_one_layer_off_hover_id;
    void* m_one_layer_on_light_id;
    void* m_one_layer_on_hover_light_id;
    void* m_one_layer_off_light_id;
    void* m_one_layer_off_hover_light_id;
    void* m_one_layer_on_dark_id;
    void* m_one_layer_on_hover_dark_id;
    void* m_one_layer_off_dark_id;
    void* m_one_layer_off_hover_dark_id;
    void *m_pause_icon_id;
    void *m_custom_icon_id;
    void *m_delete_icon_id;

    DrawMode            m_draw_mode = dmRegular;
    Mode                m_mode          = SingleExtruder;
    int                 m_only_extruder = -1;

    long                m_style;
    long                m_extra_style;
    float               m_label_koef{1.0};

    float                    m_zero_layer_height = 0.0f;
    std::vector<double>      m_values;
    TickCodeInfo             m_ticks;
    std::vector<double>      m_layers_times;
    std::vector<double>      m_layers_values;
    std::vector<std::string> m_extruder_colors;
    bool                     m_can_change_color;
    std::string              m_print_obj_idxs;
    bool                     m_is_need_post_tick_changed_event { false };
    Type                     m_tick_change_event_type;

    // xyz fork: Filament Lookahead viewer Phase 3 — tower-base mark
    // positions on the slider. Populated by GUI_Preview after each
    // gcode load; rendered by draw_lookahead_tower_marks().
    std::vector<LookaheadTowerMark> m_lookahead_tower_marks;

    // xyz fork: Filament Lookahead viewer Phase 4 — per-slider-index
    // metadata. Same length as m_values when populated; empty otherwise.
    // See `EntryMeta` documentation in the public section.
    std::vector<EntryMeta> m_entry_meta;

    std::vector<double> m_alternate_values;

    char m_custom_gcode[1024] = { 0 }; //menu
    char m_layer_number[64] = { 0 }; //menu
};

}

} // Slic3r


#endif // slic3r_GUI_IMSlider_hpp_
