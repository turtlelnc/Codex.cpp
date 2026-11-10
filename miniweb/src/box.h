#pragma once
#include "style.h"
#include <string>
#include <vector>

struct Node;
struct Image;

struct LineItem {
    enum Kind { Text, ImageItem, BoxItem, BreakItem } kind = Text;
    Node* node = nullptr;       // text node for text items, element for box/image items
    Node* link = nullptr;       // nearest <a href>
    const ComputedStyle* style = nullptr;
    std::string text;
    Image* image = nullptr;
    Node* box = nullptr;        // inline-block child
    float x = 0, y = 0, w = 0, h = 0;
    float baseline = 0;         // distance from item top to baseline
    float font_size = 16;
    Color color{0, 0, 0, 255};
    bool underline = false;
    bool line_through = false;
};

struct LineBox {
    float y = 0;
    float height = 0;
    float baseline = 0;
    float width = 0;
    std::vector<LineItem> items;
};

struct LayoutInfo {
    bool has_box = false;
    bool none = false;
    RectF border;               // relative to parent content origin
    float content_x = 0, content_y = 0;
    float content_w = 0, content_h = 0;
    float margin_top = 0, margin_right = 0, margin_bottom = 0, margin_left = 0;
    float padding_top = 0, padding_right = 0, padding_bottom = 0, padding_left = 0;
    float border_top = 0, border_right = 0, border_bottom = 0, border_left = 0;

    std::vector<LineBox> lines;
    std::vector<Node*> box_children;   // block/flex/inline-block/absolute children
    bool has_lines = false;

    float intrinsic_width = -1;        // <= 0 means unknown
    float image_w = 0, image_h = 0;
    bool is_image = false;
    Image* image = nullptr;

    float absolute_x = 0, absolute_y = 0; // filled by paint/hit-test helper
};
