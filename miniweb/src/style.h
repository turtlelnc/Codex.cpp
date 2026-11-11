#pragma once
#include "util.h"
#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

struct Length {
    enum Unit { Auto, Px, Percent, Em, Rem, Min, Max, Clamp } unit = Auto;
    float value = 0;
    std::vector<Length> arguments;
    float offset = 0; // px part of a simple calc(), e.g. calc(100% - 20px)
    static Length px(float v) { Length l; l.unit = Px; l.value = v; return l; }
    static Length percent(float v) { Length l; l.unit = Percent; l.value = v; return l; }
    static Length em(float v) { Length l; l.unit = Em; l.value = v; return l; }
    static Length rem(float v) { Length l; l.unit = Rem; l.value = v; return l; }
    bool is_auto() const { return unit == Auto; }
    bool is_zero() const { return unit == Px && value == 0 && offset == 0; }
};

inline Length length_auto() { return Length{}; }

template <typename T>
struct Edges {
    T top{}, right{}, bottom{}, left{};
    T& operator[](int i) { return i == 0 ? top : i == 1 ? right : i == 2 ? bottom : left; }
    const T& operator[](int i) const { return i == 0 ? top : i == 1 ? right : i == 2 ? bottom : left; }
};

enum class Display { Inline, Block, InlineBlock, Flex, Grid, Table, TableHeader, TableGroup, TableFooter, TableRow, TableCell, None };
enum class VerticalAlign { Baseline, Top, Middle, Bottom };
enum class TrackKind { Px, Percent, Fr, Auto, AutoFit };
struct GridTrack {
    TrackKind kind = TrackKind::Auto;
    float value = 0;
};
enum class Position { Static, Relative, Absolute, Fixed };
enum class TextAlign { Left, Right, Center, Justify };
enum class WhiteSpace { Normal, Pre, Nowrap, PreWrap };
enum class Overflow { Visible, Hidden, Auto, Scroll };
enum class FlexDirection { Row, RowReverse, Column, ColumnReverse };
enum class FlexWrap { NoWrap, Wrap, WrapReverse };
enum class Justify { FlexStart, FlexEnd, Center, SpaceBetween, SpaceAround, SpaceEvenly };
enum class AlignItems { Stretch, FlexStart, FlexEnd, Center, Baseline };
enum class BorderStyle { None, Solid, Dashed, Dotted, Double };
enum class FontStyle { Normal, Italic, Oblique };
enum class Visibility { Visible, Hidden, Collapse };
enum class ObjectFit { Fill, Contain, Cover, None, ScaleDown };
enum class BackgroundSize { Auto, Cover, Contain, Stretch };
enum class TextTransform { None, Uppercase, Lowercase, Capitalize };

struct BorderSide {
    float width = 3; // specified initial medium; used width is zero for none
    Color color{0, 0, 0, 255};
    bool current_color = true;
    BorderStyle style = BorderStyle::None;
    bool occupies_space() const { return width > 0 && style != BorderStyle::None; }
};

struct GradientStop {
    float pos = 0;
    Color color;
};
struct Gradient {
    bool enabled = false;
    float angle = 180.0f; // CSS convention: 0 = up, 90 = right, 180 = down
    std::vector<GradientStop> stops;
};
struct BoxShadow {
    float x = 0, y = 0, blur = 0, spread = 0;
    Color color{0, 0, 0, 50};
};

struct ComputedStyle {
    Display display = Display::Inline;
    Position position = Position::Static;

    Length width, height;
    Length min_width, min_height;
    Length max_width, max_height;
    Edges<Length> margin, padding;
    BorderSide border[4];
    float border_radius = 0;

    Color color{0, 0, 0, 255};
    Color background{0, 0, 0, 0};
    Gradient background_gradient;
    std::string background_image;
    BackgroundSize background_size = BackgroundSize::Auto;
    std::vector<BoxShadow> shadows;
    std::string font_family;
    float font_size = 16.0f;
    int font_weight = 400;
    FontStyle font_style = FontStyle::Normal;
    Length line_height; // Auto == normal
    TextAlign text_align = TextAlign::Left;
    WhiteSpace white_space = WhiteSpace::Normal;
    TextTransform text_transform = TextTransform::None;
    float letter_spacing = 0;
    bool underline = false;
    bool line_through = false;

    Visibility visibility = Visibility::Visible;
    float opacity = 1.0f;
    Overflow overflow = Overflow::Visible;
    ObjectFit object_fit = ObjectFit::Fill;

    FlexDirection flex_direction = FlexDirection::Row;
    FlexWrap flex_wrap = FlexWrap::NoWrap;
    Justify justify = Justify::FlexStart;
    AlignItems align_items = AlignItems::Stretch;
    float row_gap = 0, column_gap = 0;
    float flex_grow = 0, flex_shrink = 1;
    Length flex_basis;
    float aspect_ratio = 0;
    int order = 0;
    int grid_column_span = 1;
    bool table_fixed = false;
    float border_spacing_x = 2, border_spacing_y = 2;
    VerticalAlign vertical_align = VerticalAlign::Baseline;
    Length translate_x = Length::px(0), translate_y = Length::px(0);
    std::vector<GridTrack> grid_columns;
    std::vector<GridTrack> grid_rows;

    Length top, right, bottom, left;
    int z_index = 0;
    bool box_sizing_border = false;
    bool display_none = false;
    bool is_root = false;

    std::unordered_map<std::string, std::string> custom_props;

    bool is_block_level() const {
        return display == Display::Block || display == Display::Flex || display == Display::Grid ||
               display == Display::Table || display == Display::TableRow || display == Display::TableCell ||
               display == Display::TableGroup || display == Display::TableHeader || display == Display::TableFooter;
    }
    bool is_inline_level() const {
        return display == Display::Inline || display == Display::InlineBlock;
    }
};

// Inheritance is shared by DOM elements and anonymous layout boxes. Box styles,
// opacity, positioning and flex-item properties must not be copied from a parent.
inline ComputedStyle inherited_style(const ComputedStyle& parent) {
    ComputedStyle st;
    st.color = parent.color;
    st.font_family = parent.font_family;
    st.font_size = parent.font_size;
    st.font_weight = parent.font_weight;
    st.font_style = parent.font_style;
    st.line_height = parent.line_height;
    st.text_align = parent.text_align;
    st.white_space = parent.white_space;
    st.visibility = parent.visibility;
    st.letter_spacing = parent.letter_spacing;
    st.text_transform = parent.text_transform;
    st.custom_props = parent.custom_props;
    st.border_spacing_x = parent.border_spacing_x;
    st.border_spacing_y = parent.border_spacing_y;
    return st;
}

inline float resolve_length(const Length& l, float basis, float font_size, float root_size = 16.0f,
                            bool auto_is_zero = true) {
    switch (l.unit) {
        case Length::Px: return l.value + l.offset;
        case Length::Percent: return basis * l.value / 100.0f + l.offset;
        case Length::Em: return font_size * l.value + l.offset;
        case Length::Rem: return root_size * l.value + l.offset;
        case Length::Min: case Length::Max: case Length::Clamp: {
            if(l.arguments.empty())return 0;
            auto value=[&](size_t i){return resolve_length(l.arguments[i],basis,font_size,root_size,auto_is_zero);};
            if(l.unit==Length::Clamp&&l.arguments.size()==3)return std::max(value(0),std::min(value(1),value(2)));
            float result=value(0);for(size_t i=1;i<l.arguments.size();++i)result=l.unit==Length::Min?std::min(result,value(i)):std::max(result,value(i));return result;
        }
        case Length::Auto: return auto_is_zero ? 0.0f : -1.0f;
    }
    return 0;
}

inline float style_line_height(const ComputedStyle& s) {
    if (s.line_height.unit == Length::Auto) return std::max(s.font_size * 1.2f, s.font_size + 2.0f);
    if (s.line_height.unit == Length::Percent) return s.font_size * s.line_height.value / 100.0f;
    if (s.line_height.unit == Length::Em) return s.font_size * s.line_height.value;
    if (s.line_height.unit == Length::Rem) return 16.0f * s.line_height.value;
    return s.line_height.value;
}

inline float border_left(const ComputedStyle& s) {
    return s.border[3].occupies_space() ? s.border[3].width : 0.0f;
}
inline float border_right(const ComputedStyle& s) {
    return s.border[1].occupies_space() ? s.border[1].width : 0.0f;
}
inline float border_top(const ComputedStyle& s) {
    return s.border[0].occupies_space() ? s.border[0].width : 0.0f;
}
inline float border_bottom(const ComputedStyle& s) {
    return s.border[2].occupies_space() ? s.border[2].width : 0.0f;
}
inline float border_horizontal(const ComputedStyle& s) {
    return border_left(s) + border_right(s);
}
inline float border_vertical(const ComputedStyle& s) {
    return border_top(s) + border_bottom(s);
}
