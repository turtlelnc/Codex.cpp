#include "image.h"
#include <cmath>
#include "util.h"
#include "stb_image.h"
#include "svg.h"
#include <cstring>

std::shared_ptr<Image> ImageCache::get(const std::string& url) {
    auto it = cache_.find(url);
    if (it != cache_.end()) return it->second;
    auto img = std::make_shared<Image>();
    img->url = url;
    cache_[url] = img;
    if (url.empty() || !loader_) return img;
    LoadResult r = loader_->load_absolute(url);
    if (!r.ok) {
        util::warn("image load failed (%s): %s", url.c_str(), r.error.c_str());
        return img;
    }
    std::string low = util::to_lower(url);
    bool svg = util::ends_with(low, ".svg") || util::to_lower(r.content_type).find("svg") != std::string::npos;
    if (svg) {
        float iw = 0, ih = 0;
        svg_intrinsic_size(r.data, &iw, &ih);
        float s = iw <= 64 ? 3.0f : iw <= 256 ? 2.0f : 1.0f;
        auto rendered = render_svg(r.data, iw * s, ih * s, Color{0, 0, 0, 255});
        if (rendered && rendered->ok) {
            img->w = rendered->w;
            img->h = rendered->h;
            img->intrinsic_w = (int)std::lround(iw);
            img->intrinsic_h = (int)std::lround(ih);
            img->rgba = rendered->rgba;
            img->ok = true;
        }
        return img;
    }
    int w = 0, h = 0, n = 0;
    if(!stbi_info_from_memory(reinterpret_cast<const stbi_uc*>(r.data.data()),static_cast<int>(r.data.size()),&w,&h,&n)||w<=0||h<=0||w>4096||h>4096||static_cast<uint64_t>(w)*h>8*1024*1024){util::warn("image dimensions exceed preview limits");return img;}
    unsigned char* data = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(r.data.data()),
                                                (int)r.data.size(), &w, &h, &n, 4);
    if (!data || w <= 0 || h <= 0) {
        util::warn("image decode failed: %s", url.c_str());
        if (data) stbi_image_free(data);
        return img;
    }
    img->w = w;
    img->h = h;
    img->intrinsic_w = w;
    img->intrinsic_h = h;
    img->rgba.assign(data, data + (size_t)w * h * 4);
    img->ok = true;
    stbi_image_free(data);
    return img;
}
