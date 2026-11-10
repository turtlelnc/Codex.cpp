#pragma once
#include "image.h"
#include "util.h"
#include <memory>
#include <string>

void svg_intrinsic_size(const std::string& svg, float* w, float* h);
std::shared_ptr<Image> render_svg(const std::string& svg, float target_w, float target_h, Color current_color);
