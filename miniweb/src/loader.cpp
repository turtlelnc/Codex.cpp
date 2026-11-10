#include "loader.h"
#include "util.h"
#include <filesystem>
#include <stdexcept>
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#endif
namespace {
constexpr size_t kMaxBody=8u*1024u*1024u;
std::filesystem::path access_root;
size_t resource_count=0,resource_bytes=0;
#ifndef _WIN32
int root_fd=-1;
#endif
std::string content_type_for(const std::string& path) {
    std::string p = util::to_lower(path);
    if (util::ends_with(p, ".html") || util::ends_with(p, ".htm")) return "text/html";
    if (util::ends_with(p, ".css")) return "text/css";
    if (util::ends_with(p, ".js") || util::ends_with(p, ".mjs")) return "text/javascript";
    if (util::ends_with(p, ".json")) return "application/json";
    if (util::ends_with(p, ".png")) return "image/png";
    if (util::ends_with(p, ".jpg") || util::ends_with(p, ".jpeg")) return "image/jpeg";
    if (util::ends_with(p, ".gif")) return "image/gif";
    if (util::ends_with(p, ".svg")) return "image/svg+xml";
    if (util::ends_with(p, ".webp")) return "image/webp";
    return "application/octet-stream";
}

}
void Loader::set_access_root(const std::string& root) { access_root=std::filesystem::canonical(std::filesystem::u8path(root)); resource_count=resource_bytes=0;
#ifndef _WIN32
 if(root_fd>=0)close(root_fd);root_fd=open(access_root.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(root_fd<0)throw std::runtime_error("Cannot pin preview root");
#endif
 }

Loader::Loader(std::string base_url) : base_url_(std::move(base_url)) {}

std::string Loader::resolve(const std::string& rel) const {
    if (rel.empty()) return base_url_;
    if (base_url_.empty()) return rel;
    return util::resolve_url(base_url_, rel);
}

LoadResult Loader::load(const std::string& url_or_relative) {
    return load_absolute(resolve(url_or_relative));
}

LoadResult Loader::load_absolute(const std::string& raw_url) {
    if(++resource_count>256 || resource_bytes>32*1024*1024)throw std::runtime_error("Preview resource budget exceeded");
    std::string url = util::trim(raw_url);
    if (url.empty()) {
        LoadResult r;
        r.error = "empty URL";
        return r;
    }
    std::string scheme = util::url_scheme(url);
    if (scheme.empty()) {
        url = util::path_to_file_url(url);
        scheme = "file";
    }
    if (scheme == "file") return load_file(url);
    if (scheme == "data") return load_data(url);
    LoadResult r;
    r.url = url;
    r.error = "unsupported or blocked URL scheme: " + scheme;
    return r;
}

LoadResult Loader::load_file(const std::string& url) {
    LoadResult r;
    r.url = url;
    std::string path = util::file_url_to_path(url);
    if (path.empty()) {
        r.error = "empty file path";
        return r;
    }
    std::error_code ec;auto resolved=std::filesystem::canonical(std::filesystem::u8path(path),ec);
    bool allowed=!ec&&!access_root.empty();auto a=access_root.begin(),b=resolved.begin();
    for(;allowed&&a!=access_root.end();++a,++b)if(b==resolved.end()||*a!=*b)allowed=false;
    for(const auto& part:resolved)if(part==".git"||part==".codex_cpp"||part==".codex-cpp")allowed=false;
    if(!allowed || !std::filesystem::is_regular_file(resolved,ec) || std::filesystem::hard_link_count(resolved,ec)!=1 || ec){r.error="Preview resource outside workspace, protected or linked";return r;}
    const auto size=std::filesystem::file_size(resolved,ec);if(ec||size>kMaxBody||resource_bytes+size>32*1024*1024){r.error="Preview resource too large";return r;}resource_bytes+=size;
    bool ok = false;std::string data;
#ifndef _WIN32
    int fd=dup(root_fd);if(fd<0){r.error="Cannot access preview root";return r;}
    const auto relative=resolved.lexically_relative(access_root);auto it=relative.begin();
    for(;it!=relative.end();++it){auto next=it;++next;const int opened=openat(fd,it->c_str(),O_RDONLY|O_NOFOLLOW|O_CLOEXEC|(next==relative.end()?0:O_DIRECTORY));close(fd);fd=opened;if(fd<0)break;}
    if(fd>=0){struct stat state{};if(fstat(fd,&state)==0&&S_ISREG(state.st_mode)&&state.st_nlink==1&&state.st_size>=0&&static_cast<uint64_t>(state.st_size)<=kMaxBody){char buffer[8192];ok=true;for(;;){auto n=read(fd,buffer,sizeof buffer);if(n==0)break;if(n<0||data.size()+n>kMaxBody){ok=false;break;}data.append(buffer,static_cast<size_t>(n));}}close(fd);}
#else
    data=util::read_file(resolved.u8string(),&ok);
#endif
    if (!ok) {
        r.error = "cannot read file: " + path;
        return r;
    }
    if (data.size() > kMaxBody) {
        r.error = "file too large";
        return r;
    }
    r.ok = true;
    r.data = std::move(data);
    r.content_type = content_type_for(path);
    r.status = 200;
    return r;
}

LoadResult Loader::load_data(const std::string& url) {
    LoadResult r;
    r.url = url;
    size_t comma = url.find(',');
    if (comma == std::string::npos) {
        r.error = "bad data URL";
        return r;
    }
    std::string meta = url.substr(5, comma - 5);
    std::string payload = url.substr(comma + 1);
    bool base64 = util::to_lower(meta).find(";base64") != std::string::npos;
    r.data = base64 ? util::base64_decode(payload) : util::url_decode(payload, true);
    r.content_type = meta.substr(0, meta.find(';'));
    if (r.content_type.empty()) r.content_type = "text/plain";
    if(r.data.size()>kMaxBody||resource_bytes+r.data.size()>32*1024*1024){r.data.clear();r.error="Preview data resource too large";return r;}resource_bytes+=r.data.size();
    r.ok = true;
    r.status = 200;
    return r;
}

