#pragma once
#include "loader.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct Image {
    int w = 0;
    int h = 0;
    int intrinsic_w = 0;
    int intrinsic_h = 0;
    std::vector<uint8_t> rgba;
    bool ok = false;
    std::string url;
};

class ImageCache {
public:
    explicit ImageCache(Loader* loader = nullptr) : loader_(loader) {}
    void set_loader(Loader* loader) { loader_ = loader; }
    void clear() { cache_.clear(); }
    std::shared_ptr<Image> get(const std::string& url);

private:
    Loader* loader_ = nullptr;
    std::unordered_map<std::string, std::shared_ptr<Image>> cache_;
};
