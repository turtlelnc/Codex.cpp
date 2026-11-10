#pragma once
#include "box.h"
#include "dom.h"
#include "font.h"
#include "image.h"
#include <vector>

struct LayoutContext {
    Document* doc = nullptr;
    FontManager* fonts = nullptr;
    ImageCache* images = nullptr;
    float viewport_w = 1280;
    float viewport_h = 800;
};

void layout_document(Document& doc, LayoutContext& ctx);
float document_content_height(const Document& doc);
void paint_document(class Canvas& canvas, Document& doc, FontManager& fonts, ImageCache* images = nullptr);
