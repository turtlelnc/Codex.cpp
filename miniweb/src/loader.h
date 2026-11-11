#pragma once
#include <string>

struct LoadResult {
    bool ok = false;
    std::string url;
    std::string data;
    std::string content_type;
    std::string error;
    int status = 0;
};

class Loader {
public:
    static void set_access_root(const std::string& root);
    explicit Loader(std::string base_url = {});
    void set_base_url(const std::string& url) { base_url_ = url; }
    const std::string& base_url() const { return base_url_; }
    std::string resolve(const std::string& rel) const;
    LoadResult load(const std::string& url_or_relative);
    LoadResult load_absolute(const std::string& url);

private:
    std::string base_url_;

    LoadResult load_file(const std::string& url);
    LoadResult load_data(const std::string& url);
};
