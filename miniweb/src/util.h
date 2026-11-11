// MiniWeb - small local-only browser engine
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct Color {
    uint8_t r = 0, g = 0, b = 0, a = 255;
    bool transparent() const { return a == 0; }
};

struct RectF {
    float x = 0, y = 0, w = 0, h = 0;
    float right() const { return x + w; }
    float bottom() const { return y + h; }
    bool contains(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

struct EdgesF {
    float top = 0, right = 0, bottom = 0, left = 0;
    float horizontal() const { return left + right; }
    float vertical() const { return top + bottom; }
};

namespace util {

std::string read_file(const std::string& path, bool* ok = nullptr);
bool write_file(const std::string& path, const void* data, size_t size);
bool file_exists(const std::string& path);
bool is_directory(const std::string& path);

std::string to_lower(std::string s);
std::string to_upper(std::string s);
std::string trim(const std::string& s);
std::string ltrim(const std::string& s);
std::string rtrim(const std::string& s);
bool starts_with(const std::string& s, const std::string& p);
bool ends_with(const std::string& s, const std::string& p);
bool contains_ci(const std::string& hay, const std::string& needle);
std::vector<std::string> split_ws(const std::string& s);
std::vector<std::string> split(const std::string& s, char c);
std::string join(const std::vector<std::string>& parts, const std::string& sep);
std::string replace_all(std::string s, const std::string& from, const std::string& to);

std::string html_unescape(const std::string& s);
std::string html_escape(const std::string& s);
std::string json_escape(const std::string& s);
std::string url_decode(const std::string& s, bool plus_is_space = false);
std::string url_encode_path(const std::string& s);
std::string base64_decode(const std::string& s);

std::string dirname(const std::string& path);
std::string basename(const std::string& path);
std::string path_join(const std::string& a, const std::string& b);
std::string normalize_path(const std::string& path);
std::string absolute_path(const std::string& path);
std::string current_dir();
std::string home_dir();

std::string resolve_url(const std::string& base, const std::string& rel);
std::string path_to_file_url(const std::string& path);
std::string file_url_to_path(const std::string& url);
bool is_file_url(const std::string& url);
bool is_loopback_url(const std::string& url);
bool url_allowed(const std::string& url);
std::string url_scheme(const std::string& url);

double now_ms();
void sleep_ms(int ms);
std::string format_float(float v, int decimals = 2);

Color parse_color(const std::string& s, bool* ok = nullptr);
const char* color_name(uint32_t rgb);

// UTF-8 helpers
std::vector<uint32_t> utf8_decode(const std::string& s);
std::string utf8_encode(uint32_t cp);
size_t utf8_next(const std::string& s, size_t i, uint32_t* cp);
bool is_cjk(uint32_t cp);
bool is_space_cp(uint32_t cp);

// Logging (stderr, respects MINIWEB_QUIET)
void log(const char* fmt, ...);
void warn(const char* fmt, ...);
void error(const char* fmt, ...);

} // namespace util
