// codex.cpp
// Single-file C++17 clean-room Codex-style coding agent runtime.
// Inspired by the public architecture of OpenAI Codex CLI, but not a line-by-line port.
//
// Goals preserved from the public design:
//   - Responses API function_call / function_call_output loop
//   - ToolRouter-style separation between model-visible specs and runtimes
//   - approval + sandbox policy layer
//   - session/turn/tool event JSONL rollout
//   - resumable local transcript
//   - shell/read/list/write/apply_patch tools
//   - JSON event output for automation
//
// Intentionally omitted: exact official TUI internals, plugins, multi-agent, WebSocket
// transport, telemetry, remote-control/app-server protocols.
//
// Build (macOS/Linux):
//   clang++ -std=c++17 -O2 -Wall -Wextra -pedantic codex.cpp -o codex-cpp
//
// Run:
//   OPENAI_API_KEY=... ./codex-cpp --root . "fix the failing tests"
//
// The only runtime dependency is the system `curl` executable.

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#if defined(_WIN32)
#include <direct.h>
#include <process.h>
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif
#include <vector>
#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;

#if !defined(_WIN32)
static volatile sig_atomic_t tui_interrupt_requested=0;
static void request_tui_interrupt(int) { tui_interrupt_requested=1; }
#endif

// ---------- utilities ----------

static std::string getenv_or(const char* key, const std::string& fallback = {}) {
    if (const char* v = std::getenv(key)) return v;
    return fallback;
}

static std::string trim(std::string s) {
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const auto b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static std::string json_escape(std::string_view s) {
    std::ostringstream o;
    for (unsigned char c : s) {
        switch (c) {
            case '"': o << "\\\""; break;
            case '\\': o << "\\\\"; break;
            case '\b': o << "\\b"; break;
            case '\f': o << "\\f"; break;
            case '\n': o << "\\n"; break;
            case '\r': o << "\\r"; break;
            case '\t': o << "\\t"; break;
            default:
                if (c < 0x20) {
                    static constexpr char hex[] = "0123456789abcdef";
                    o << "\\u00" << hex[(c >> 4) & 15] << hex[c & 15];
                } else o << static_cast<char>(c);
        }
    }
    return o.str();
}

static std::string base64_encode(std::string_view raw) {
    static const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;out.reserve((raw.size()+2)/3*4);
    for(size_t i=0;i<raw.size();i+=3) {
        const unsigned a=static_cast<unsigned char>(raw[i]);
        const unsigned b=i+1<raw.size()?static_cast<unsigned char>(raw[i+1]):0;
        const unsigned c=i+2<raw.size()?static_cast<unsigned char>(raw[i+2]):0;
        out+=alphabet[a>>2];out+=alphabet[((a&3)<<4)|(b>>4)];
        out+=i+1<raw.size()?alphabet[((b&15)<<2)|(c>>6)]:'=';
        out+=i+2<raw.size()?alphabet[c&63]:'=';
    }
    return out;
}

static std::optional<std::string> image_data_url(const fs::path& path,std::string& err) {
    std::error_code ec;const auto bytes=fs::file_size(path,ec);
    if(ec || bytes==0 || bytes>8'000'000) {err="image must be a readable file of 1–8 MB";return {};}
    std::ifstream in(path,std::ios::binary);if(!in) {err="cannot open image";return {};}
    std::string data(static_cast<size_t>(bytes),'\0');in.read(data.data(),static_cast<std::streamsize>(bytes));
    if(!in) {err="could not read image";return {};}
    std::string mime;
    if(data.size()>=8 && data.compare(0,8,"\x89PNG\r\n\x1a\n",8)==0) mime="image/png";
    else if(data.size()>=3 && static_cast<unsigned char>(data[0])==0xff &&
            static_cast<unsigned char>(data[1])==0xd8 && static_cast<unsigned char>(data[2])==0xff) mime="image/jpeg";
    else if(data.size()>=12 && data.compare(0,4,"RIFF")==0 && data.compare(8,4,"WEBP")==0) mime="image/webp";
    else if(data.size()>=6 && data.compare(0,3,"GIF")==0) mime="image/gif";
    else {err="supported images: PNG, JPEG, WebP, GIF";return {};}
    return "data:"+mime+";base64,"+base64_encode(data);
}

static std::string random_id(const char* prefix);

static std::string shell_quote(const std::string& s) {
#if defined(_WIN32)
    // Quote one argument for cmd.exe. This is intentionally conservative and is
    // primarily used for paths/headers generated by this program.
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '%') out += "%%";
        else out += c;
    }
    return out + "\"";
#else
    std::string out = "'";
    for (char c : s) out += (c == '\'') ? "'\\''" : std::string(1, c);
    return out + "'";
#endif
}

static std::string cd_prefix(const fs::path& root) {
#if defined(_WIN32)
    return "cd /d " + shell_quote(root.string()) + " && ";
#else
    return "cd " + shell_quote(root.string()) + " && ";
#endif
}

static fs::path temp_path_in(const fs::path& dir,const char* stem) {
    for(int attempt=0;attempt<10;++attempt) {
        fs::path p=dir/(std::string(stem)+"-"+random_id("tmp"));
#if !defined(_WIN32)
        int fd=open(p.c_str(),O_WRONLY|O_CREAT|O_EXCL,0600);
        if(fd>=0) {close(fd);return p;}
#else
        std::ofstream f(p,std::ios::binary|std::ios::app);
        if(f) return p;
#endif
    }
    return dir / (std::string(stem) + "-" + random_id("fallback"));
}

static fs::path temp_path(const char* stem) {
    std::error_code ec;
    fs::path dir = fs::temp_directory_path(ec);
    if (ec) dir = fs::current_path();
    return temp_path_in(dir,stem);
}

static std::string now_iso8601() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const std::time_t t = system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    std::ostringstream o;
    o << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return o.str();
}

static std::string random_id(const char* prefix) {
    static std::mt19937_64 rng{std::random_device{}()};
    std::ostringstream o;
    o << prefix << '-' << std::hex << rng() << rng();
    return o.str();
}

struct CommandResult {
    int exit_code = -1;
    std::string output;
};

static CommandResult run_capture(const std::string& command, size_t limit = 200000) {
    CommandResult r;
#if defined(_WIN32)
    FILE* pipe = _popen((command + " 2>&1").c_str(), "r");
#else
    FILE* pipe = popen((command + " 2>&1").c_str(), "r");
#endif
    if (!pipe) {
        r.output = std::string("popen failed: ") + std::strerror(errno);
        return r;
    }
    std::array<char, 4096> buf{};
    while (fgets(buf.data(), static_cast<int>(buf.size()), pipe)) {
        if (r.output.size() < limit) r.output.append(buf.data());
    }
    if (r.output.size() >= limit) {
        r.output.resize(limit);
        r.output += "\n[truncated]";
    }
#if defined(_WIN32)
    const int status = _pclose(pipe);
    r.exit_code = status;
#else
    const int status = pclose(pipe);
    if (WIFEXITED(status)) r.exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) r.exit_code = 128 + WTERMSIG(status);
#endif
    return r;
}

#if !defined(_WIN32)
static bool contains_plain_escape(const char* keys, ssize_t count) {
    for(ssize_t i=0;i<count;++i) {
        if(keys[i]!=27) continue;
        if(i+1<count) {
            if(keys[i+1]=='[' || keys[i+1]=='O') continue; // mouse, arrow, or function key
            return true;
        }
        fd_set fds;FD_ZERO(&fds);FD_SET(STDIN_FILENO,&fds);
        timeval timeout{0,40000};
        if(select(STDIN_FILENO+1,&fds,nullptr,nullptr,&timeout)>0) {
            char next=0;
            if(::read(STDIN_FILENO,&next,1)==1 && (next=='[' || next=='O')) return false;
        }
        return true;
    }
    return false;
}
#endif

static CommandResult run_capture_interruptible(const std::string& command, size_t limit,
                                               const std::function<void()>& on_tick,
                                               bool watch_escape) {
#if defined(_WIN32)
    (void)on_tick;(void)watch_escape;
    return run_capture(command,limit);
#else
    CommandResult result;
    int channels[2];
    if(::pipe(channels)!=0) {result.output="pipe failed";return result;}
    pid_t child=::fork();
    if(child==0) {
        ::setsid();
        ::close(channels[0]);
        ::dup2(channels[1],STDOUT_FILENO);
        ::dup2(channels[1],STDERR_FILENO);
        ::close(channels[1]);
        ::execl("/bin/sh","sh","-c",command.c_str(),static_cast<char*>(nullptr));
        _exit(127);
    }
    ::close(channels[1]);
    if(child<0) {::close(channels[0]);result.output="fork failed";return result;}
    termios saved{};
    bool raw=false;
    if(watch_escape && isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO,&saved)==0) {
        termios mode=saved;
        mode.c_lflag&=static_cast<tcflag_t>(~(ICANON|ECHO));
        mode.c_cc[VMIN]=0;mode.c_cc[VTIME]=0;
        raw=tcsetattr(STDIN_FILENO,TCSANOW,&mode)==0;
    }
    auto last_tick=std::chrono::steady_clock::now();
    bool interrupted=false;
    std::array<char,4096> buf{};
    for(;;) {
#if !defined(_WIN32)
        if(tui_interrupt_requested) {interrupted=true;break;}
#endif
        fd_set readers;FD_ZERO(&readers);FD_SET(channels[0],&readers);
        if(raw) FD_SET(STDIN_FILENO,&readers);
        timeval timeout{0,250000};
        int ready=select(std::max(channels[0],raw?STDIN_FILENO:channels[0])+1,
                         &readers,nullptr,nullptr,&timeout);
        if(ready>0) {
            if(raw && FD_ISSET(STDIN_FILENO,&readers)) {
                char keys[64];ssize_t n=::read(STDIN_FILENO,keys,sizeof(keys));
                if(n>0 && contains_plain_escape(keys,n)) {interrupted=true;break;}
            }
            if(FD_ISSET(channels[0],&readers)) {
                ssize_t n=::read(channels[0],buf.data(),buf.size());
                if(n==0) break;
                if(n>0 && result.output.size()<limit)
                    result.output.append(buf.data(),std::min<size_t>(static_cast<size_t>(n),limit-result.output.size()));
                if(n<0 && errno!=EINTR) break;
            }
        } else if(ready<0 && errno!=EINTR) break;
        auto now=std::chrono::steady_clock::now();
        if(on_tick && now-last_tick>=std::chrono::seconds(1)) {on_tick();last_tick=now;}
    }
    if(raw) tcsetattr(STDIN_FILENO,TCSANOW,&saved);
    if(interrupted) {
        if(::getpgid(child)==child) ::kill(-child,SIGTERM);
        else ::kill(child,SIGTERM);
    }
    ::close(channels[0]);
    int status=0;
    while(::waitpid(child,&status,0)<0 && errno==EINTR) {}
    if(interrupted) {result.exit_code=130;result.output="Interrupted by Esc";return result;}
    if(WIFEXITED(status)) result.exit_code=WEXITSTATUS(status);
    else if(WIFSIGNALED(status)) result.exit_code=128+WTERMSIG(status);
    if(result.output.size()>=limit) result.output+="\n[truncated]";
    return result;
#endif
}

static std::string slurp(const fs::path& p, size_t limit = 200000) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return "ERROR: cannot open " + p.string();
    std::string s(limit, '\0');
    f.read(s.data(), static_cast<std::streamsize>(limit));
    s.resize(static_cast<size_t>(f.gcount()));
    if (!f.eof()) s += "\n[truncated]";
    return s;
}

static bool write_all(const fs::path& p, const std::string& data, std::string& err) {
    std::error_code ec;
    if (!p.parent_path().empty()) fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) { err = "cannot write " + p.string(); return false; }
    f << data;
    if (!f) { err = "write failed " + p.string(); return false; }
    return true;
}

static std::string list_dir(const fs::path& p) {
    std::error_code ec;
    if (!fs::exists(p, ec)) return "ERROR: path does not exist: " + p.string();
    if (!fs::is_directory(p, ec)) return "ERROR: not a directory: " + p.string();
    std::vector<std::string> rows;
    for (const auto& e : fs::directory_iterator(p, ec)) {
        std::string n = e.path().filename().string();
        if (e.is_directory(ec)) n += '/';
        rows.push_back(std::move(n));
    }
    std::sort(rows.begin(), rows.end());
    std::ostringstream o;
    for (const auto& row : rows) o << row << '\n';
    return o.str();
}

static bool within_root(const fs::path& root, const fs::path& target) {
    std::error_code ec;
    const fs::path r = fs::weakly_canonical(root, ec);
    fs::path t;
    if (fs::exists(target, ec)) t = fs::weakly_canonical(target, ec);
    else t = fs::weakly_canonical(target.parent_path(), ec) / target.filename();
    if (ec) return false;
    auto ri = r.begin(), ti = t.begin();
    for (; ri != r.end(); ++ri, ++ti) if (ti == t.end() || *ri != *ti) return false;
    return true;
}

// A path-only sandbox can permit writes through a hardlink in the workspace
// whose other name lives outside it. Count all names of each inode in the
// workspace and fail closed when its link count exceeds the names we can see.
static std::optional<std::string> external_hardlink_risk(const fs::path& root) {
    std::error_code ec;
    if(!fs::is_directory(root,ec)) return "cannot inspect workspace";
#if defined(_WIN32)
    for(fs::recursive_directory_iterator it(root,ec),end;it!=end && !ec;it.increment(ec)) {
        auto status=it->symlink_status(ec);
        if(ec) break;
        if(fs::is_regular_file(status) && fs::hard_link_count(it->path(),ec)>1)
            return "hardlinked file: "+it->path().string();
    }
#else
    struct LinkGroup {std::uintmax_t seen=0,links=0;fs::path example;};
    std::map<std::pair<std::uintmax_t,std::uintmax_t>,LinkGroup> groups;
    for(fs::recursive_directory_iterator it(root,ec),end;it!=end && !ec;it.increment(ec)) {
        auto status=it->symlink_status(ec);
        if(ec) break;
        if(!fs::is_regular_file(status)) continue; // do not follow symlinks
        struct stat st{};
        if(::stat(it->path().c_str(),&st)!=0) return "cannot inspect file: "+it->path().string();
        if(st.st_nlink<=1) continue;
        auto& group=groups[{static_cast<std::uintmax_t>(st.st_dev),static_cast<std::uintmax_t>(st.st_ino)}];
        ++group.seen;group.links=static_cast<std::uintmax_t>(st.st_nlink);
        if(group.example.empty()) group.example=it->path();
    }
    if(!ec) for(const auto& [_,group]:groups)
        if(group.seen<group.links) return "external hardlink alias: "+group.example.string();
#endif
    if(ec) return "cannot completely inspect workspace: "+ec.message();
    return {};
}

// ---------- bounded JSON reader ----------
// Parse complete values before selecting keys: API envelopes may contain repeated
// names in unrelated nested objects and providers may return multiple calls.
struct Json {
    enum Kind { Null, String, Object, Array, Other } kind = Null;
    std::string value;
    std::map<std::string, Json> fields;
    std::vector<Json> items;
    const Json* get(const std::string& key) const {
        auto it = fields.find(key); return it == fields.end() ? nullptr : &it->second;
    }
    std::string str() const { return kind == String ? value : ""; }
};

struct JsonReader {
    const std::string& s; size_t i = 0; int depth = 0;
    void ws() { while (i < s.size() && (s[i]==' ' || s[i]=='\n' || s[i]=='\r' || s[i]=='\t')) ++i; }
    bool string(std::string& out) {
        if (i == s.size() || s[i++] != '"') return false;
        while (i < s.size()) {
            unsigned char c = static_cast<unsigned char>(s[i++]);
            if (c == '"') return true;
            if (c < 0x20) return false;
            if (c != '\\') { out += static_cast<char>(c); continue; }
            if (i == s.size()) return false;
            char e = s[i++];
            switch (e) {
                case '"': out += '"'; break; case '\\': out += '\\'; break;
                case '/': out += '/'; break; case 'b': out += '\b'; break;
                case 'f': out += '\f'; break; case 'n': out += '\n'; break;
                case 'r': out += '\r'; break; case 't': out += '\t'; break;
                case 'u': {
                    auto hex = [&]() -> int {
                        if (i+4>s.size()) return -1; int v=0;
                        for(int k=0;k<4;++k) { char h=s[i++]; v*=16;
                            if(h>='0'&&h<='9') v+=h-'0';
                            else if(h>='a'&&h<='f') v+=h-'a'+10;
                            else if(h>='A'&&h<='F') v+=h-'A'+10;
                            else return -1;
                        } return v;
                    };
                    int cp=hex(); if(cp<0) return false;
                    if(cp>=0xd800 && cp<=0xdbff) {
                        if(i+2>s.size() || s[i++]!='\\' || s[i++]!='u') return false;
                        int lo=hex(); if(lo<0xdc00 || lo>0xdfff) return false;
                        cp=0x10000+((cp-0xd800)<<10)+(lo-0xdc00);
                    } else if(cp>=0xdc00 && cp<=0xdfff) return false;
                    if(cp<0x80) out+=char(cp);
                    else if(cp<0x800) { out+=char(0xc0|(cp>>6)); out+=char(0x80|(cp&63)); }
                    else if(cp<0x10000) { out+=char(0xe0|(cp>>12)); out+=char(0x80|((cp>>6)&63)); out+=char(0x80|(cp&63)); }
                    else { out+=char(0xf0|(cp>>18)); out+=char(0x80|((cp>>12)&63)); out+=char(0x80|((cp>>6)&63)); out+=char(0x80|(cp&63)); }
                    break;
                }
                default: return false;
            }
        } return false;
    }
    bool parse(Json& v) {
        ws(); if (i>=s.size() || ++depth>64) return false;
        char c=s[i]; bool ok=false;
        if(c=='"') { v.kind=Json::String; ok=string(v.value); }
        else if(c=='{' || c=='[') {
            bool obj=c=='{'; v.kind=obj?Json::Object:Json::Array; ++i; ws();
            if(i<s.size() && s[i]==(obj?'}':']')) { ++i; ok=true; }
            else while(i<s.size()) {
                std::string key;
                if(obj) { if(!string(key)) break; ws(); if(i==s.size()||s[i++]!=':') break; }
                Json child; if(!parse(child)) break;
                if(obj) v.fields[key]=std::move(child); else v.items.push_back(std::move(child));
                ws(); if(i==s.size()) break;
                if(s[i]==(obj?'}':']')) { ++i; ok=true; break; }
                if(s[i++]!=',') break; ws();
            }
        } else {
            size_t start=i;
            while(i<s.size() && s[i]!=',' && s[i]!=']' && s[i]!='}' && s[i]!=' ' && s[i]!='\n' && s[i]!='\r') ++i;
            v.value=s.substr(start,i-start); v.kind=v.value=="null"?Json::Null:Json::Other;
            ok=v.value=="null" || v.value=="true" || v.value=="false" ||
               (!v.value.empty() && (v.value[0]=='-' || (v.value[0]>='0'&&v.value[0]<='9')));
        }
        --depth; return ok;
    }
    std::optional<Json> complete() { Json v; if(!parse(v)) return {}; ws(); if(i!=s.size()) return {}; return v; }
};
static std::optional<Json> parse_json(const std::string& s) { return JsonReader{s}.complete(); }
static std::string json_dump(const Json& j) {
    if(j.kind==Json::String) return "\"" + json_escape(j.value) + "\"";
    if(j.kind==Json::Null) return "null";
    if(j.kind==Json::Other) return j.value;
    if(j.kind==Json::Array) {
        std::string s="[";
        for(const auto& x:j.items) {if(s.size()>1)s+=',';s+=json_dump(x);}
        return s+"]";
    }
    std::string s="{";
    for(const auto& [k,v]:j.fields) {if(s.size()>1)s+=',';s+="\""+json_escape(k)+"\":"+json_dump(v);}
    return s+"}";
}
static const Json* at(const Json* x, const std::string& key) { return x?x->get(key):nullptr; }
static std::optional<std::string> json_string_field(const std::string& json, const std::string& key, size_t = 0) {
    auto v=parse_json(json); if(!v) return {}; auto field=v->get(key);
    if(!field || field->kind!=Json::String) return {}; return field->value;
}

// ChatGPT device authorization. Credentials belong to this program and are
// deliberately separate from ~/.codex/auth.json.
static constexpr const char* kAuthIssuer = "https://auth.openai.com";
static constexpr const char* kOAuthClient = "app_EMoamEEZ73f0CkXaXp7hrann";
static fs::path own_auth_path() {
    return fs::path(getenv_or("CODEX_CPP_HOME", getenv_or("HOME", getenv_or("USERPROFILE")) + "/.codex-cpp")) / "auth.json";
}
static std::string form_encode(const std::string& s) {
    static constexpr char hex[]="0123456789ABCDEF";
    std::string out;
    for(unsigned char c:s) {
        if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~') out+=char(c);
        else {out+='%'; out+=hex[c>>4]; out+=hex[c&15];}
    }
    return out;
}
static std::optional<std::string> base64url_decode(std::string s) {
    std::string out; int val=0,bits=-8;
    for(char c:s) {
        if(c=='=') break;
        int n=c>='A'&&c<='Z'?c-'A':c>='a'&&c<='z'?c-'a'+26:c>='0'&&c<='9'?c-'0'+52:c=='-'?62:c=='_'?63:-1;
        if(n<0) return {}; val=(val<<6)|n; bits+=6;
        if(bits>=0) {out+=char((val>>bits)&255);bits-=8;}
    }
    return out;
}
static std::optional<Json> jwt_claims(const std::string& token) {
    auto a=token.find('.'), b=token.find('.',a==std::string::npos?a:a+1);
    if(a==std::string::npos||b==std::string::npos) return {};
    auto decoded=base64url_decode(token.substr(a+1,b-a-1));
    return decoded?parse_json(*decoded):std::nullopt;
}
static std::string account_from_jwt(const std::string& token) {
    auto j=jwt_claims(token); if(!j) return {};
    auto nested=j->get("https://api.openai.com/auth");
    auto id=at(nested,"chatgpt_account_id");
    return id?id->str():"";
}
struct WebReply { long status=0; std::string body, error; };
static WebReply web_post(const std::string& url, const std::string& body,
                         const std::string& content_type, const std::vector<std::string>& headers={}) {
    WebReply result;
    fs::path req=temp_path("codex-auth-req"), out=temp_path("codex-auth-out"), hdr=temp_path("codex-auth-hdr");
    {
        std::ofstream f(req,std::ios::binary); if(!f) {result.error="cannot create request";return result;} f<<body;
        std::ofstream h(hdr,std::ios::binary); if(!h) {result.error="cannot create header";return result;}
        h<<"Content-Type: "<<content_type<<"\n";
        for(const auto& line:headers) h<<line<<"\n";
    }
#if !defined(_WIN32)
    fs::permissions(req,fs::perms::owner_read|fs::perms::owner_write,fs::perm_options::replace);
    fs::permissions(hdr,fs::perms::owner_read|fs::perms::owner_write,fs::perm_options::replace);
#endif
    const std::string cmd="curl -sS --connect-timeout 15 --max-time 300 --max-redirs 0 -o "+
        shell_quote(out.string())+" -w '%{http_code}' -H "+shell_quote("@"+hdr.string())+
        " --data-binary @"+shell_quote(req.string())+" "+shell_quote(url);
    auto r=run_capture(cmd,2000); result.body=slurp(out,8'000'000);
    if(r.exit_code!=0) result.error="curl failed: "+r.output;
    else {try {result.status=std::stol(trim(r.output));} catch(...) {result.error="invalid HTTP status";}}
    std::error_code ec;fs::remove(req,ec);fs::remove(out,ec);fs::remove(hdr,ec);
    return result;
}
struct OwnAuth { std::string access,refresh,id,account; long long expires=0; };
static bool save_auth(const OwnAuth& a,std::string& err) {
    fs::path p=own_auth_path(); std::error_code ec;fs::create_directories(p.parent_path(),ec);
    if(ec) {err="cannot create credential directory";return false;}
#if !defined(_WIN32)
    fs::permissions(p.parent_path(),fs::perms::owner_all,fs::perm_options::replace,ec);
#endif
    fs::path tmp=p;tmp+="."+random_id("tmp");
    std::string data="{\"access_token\":\""+json_escape(a.access)+"\",\"refresh_token\":\""+
        json_escape(a.refresh)+"\",\"id_token\":\""+json_escape(a.id)+"\",\"account_id\":\""+
        json_escape(a.account)+"\",\"expires_at\":"+std::to_string(a.expires)+"}";
    {std::ofstream f(tmp,std::ios::binary|std::ios::trunc); if(!f) {err="cannot save credentials";return false;}f<<data;}
#if !defined(_WIN32)
    fs::permissions(tmp,fs::perms::owner_read|fs::perms::owner_write,fs::perm_options::replace,ec);
#endif
    fs::rename(tmp,p,ec);
    if(ec) {fs::remove(tmp);err="cannot replace credential file";return false;}
    return true;
}
static std::optional<OwnAuth> load_auth() {
    auto p=own_auth_path(); std::error_code ec;
    if(!fs::is_regular_file(p,ec)) return {};
#if !defined(_WIN32)
    auto perms=fs::status(p,ec).permissions();
    if(ec || (perms&(fs::perms::group_all|fs::perms::others_all))!=fs::perms::none) return {};
#endif
    auto j=parse_json(slurp(p,200000)); if(!j) return {};
    OwnAuth a; auto get=[&](const char* key){auto v=j->get(key);return v?v->str():"";};
    a.access=get("access_token");a.refresh=get("refresh_token");a.id=get("id_token");a.account=get("account_id");
    if(auto e=j->get("expires_at")) {try{a.expires=std::stoll(e->value);}catch(...){}}
    return a.access.empty()||a.refresh.empty()?std::nullopt:std::optional<OwnAuth>(a);
}
static long long epoch_now() {return std::chrono::duration_cast<std::chrono::seconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();}
static bool receive_tokens(const std::string& raw, OwnAuth& a, std::string& err) {
    auto j=parse_json(raw); if(!j) {err="invalid OAuth token response";return false;}
    auto get=[&](const char* key){auto v=j->get(key);return v?v->str():"";};
    auto access=get("access_token"), refresh=get("refresh_token"), id=get("id_token");
    if(access.empty()) {err="OAuth response did not contain an access token";return false;}
    a.access=access;if(!refresh.empty()) a.refresh=refresh;if(!id.empty()) a.id=id;
    a.account=account_from_jwt(a.id);
    long long seconds=3600;
    if(auto e=j->get("expires_in")) {try {seconds=std::stoll(e->value);}catch(...){}}
    a.expires=epoch_now()+std::max<long long>(60,seconds);
    return true;
}
static bool refresh_auth(OwnAuth& a,std::string& err) {
    std::string body="grant_type=refresh_token&client_id="+form_encode(kOAuthClient)+
        "&refresh_token="+form_encode(a.refresh);
    auto r=web_post(std::string(kAuthIssuer)+"/oauth/token",body,"application/x-www-form-urlencoded");
    if(!r.error.empty()||r.status!=200) {err="token refresh failed (HTTP "+std::to_string(r.status)+") "+r.error;return false;}
    if(!receive_tokens(r.body,a,err)) return false;
    return save_auth(a,err);
}
static bool own_device_login(std::string& err) {
    auto r=web_post(std::string(kAuthIssuer)+"/api/accounts/deviceauth/usercode",
        "{\"client_id\":\""+std::string(kOAuthClient)+"\"}","application/json");
    if(!r.error.empty()||r.status!=200) {err="device authorization unavailable (HTTP "+std::to_string(r.status)+") "+r.error;return false;}
    auto j=parse_json(r.body);if(!j) {err="invalid device authorization response";return false;}
    auto get=[&](const char* key){auto v=j->get(key);return v?v->str():"";};
    std::string id=get("device_auth_id"), code=get("user_code");
    if(code.empty()) code=get("usercode");
    if(id.empty()||code.empty()) {err="device authorization response lacks code";return false;}
    int interval=5; if(auto v=j->get("interval")) {try{interval=std::stoi(v->value);}catch(...){try{interval=std::stoi(v->str());}catch(...){}}}
    interval=std::max(1,std::min(30,interval));
    std::cout<<"Open https://auth.openai.com/codex/device and enter: "<<code<<"\n"
             <<"Only approve this code if you started this login. Waiting up to 15 minutes...\n"<<std::flush;
    auto deadline=std::chrono::steady_clock::now()+std::chrono::minutes(15);
    while(std::chrono::steady_clock::now()<deadline) {
        auto poll=web_post(std::string(kAuthIssuer)+"/api/accounts/deviceauth/token",
            "{\"device_auth_id\":\""+json_escape(id)+"\",\"user_code\":\""+json_escape(code)+"\"}","application/json");
        if(!poll.error.empty()) {err=poll.error;return false;}
        if(poll.status==200) {
            auto data=parse_json(poll.body);if(!data) {err="invalid authorization response";return false;}
            auto field=[&](const char* k){auto v=data->get(k);return v?v->str():"";};
            std::string auth_code=field("authorization_code"), verifier=field("code_verifier");
            if(auth_code.empty()||verifier.empty()) {err="authorization response incomplete";return false;}
            std::string form="grant_type=authorization_code&client_id="+form_encode(kOAuthClient)+
                "&code="+form_encode(auth_code)+"&code_verifier="+form_encode(verifier)+
                "&redirect_uri="+form_encode(std::string(kAuthIssuer)+"/deviceauth/callback");
            auto token=web_post(std::string(kAuthIssuer)+"/oauth/token",form,"application/x-www-form-urlencoded");
            if(!token.error.empty()||token.status!=200) {err="token exchange failed (HTTP "+std::to_string(token.status)+") "+token.error;return false;}
            OwnAuth a;if(!receive_tokens(token.body,a,err)) return false;
            return save_auth(a,err);
        }
        if(poll.status!=403 && poll.status!=404) {err="authorization polling failed (HTTP "+std::to_string(poll.status)+")";return false;}
        std::this_thread::sleep_for(std::chrono::seconds(interval));
    }
    err="device authorization timed out";return false;
}
struct FunctionCall {
    std::string name, call_id, arguments;
};
struct ApiResponse {
    std::string raw, id;
    std::optional<std::string> text;
    std::vector<FunctionCall> calls;
};
static std::optional<ApiResponse> parse_api_response(const std::string& raw, bool chat, std::string& err) {
    auto doc=parse_json(raw); if(!doc || doc->kind!=Json::Object) { err="Invalid JSON API response"; return {}; }
    if(auto e=doc->get("error")) {
        auto message=at(e,"message"); err="API error: "+(message?message->str():"unknown"); return {};
    }
    ApiResponse out; out.raw=raw; if(auto id=doc->get("id")) out.id=id->str();
    auto parse_call=[&](const Json& node, bool nested) {
        const Json* fn=nested?node.get("function"): &node;
        if(!fn) return;
        auto n=fn->get("name"), a=fn->get("arguments"), id=node.get(nested?"id":"call_id");
        if(n && a && id && n->kind==Json::String && a->kind==Json::String && id->kind==Json::String)
            out.calls.push_back({n->value,id->value,a->value});
    };
    if(chat) {
        auto choices=doc->get("choices");
        if(choices && choices->kind==Json::Array && !choices->items.empty()) {
            auto message=choices->items.front().get("message");
            if(auto c=at(message,"content"); c && c->kind==Json::String) out.text=c->value;
            auto calls=at(message,"tool_calls");
            if(calls && calls->kind==Json::Array) for(const auto& call:calls->items) parse_call(call,true);
        }
    } else {
        auto output=doc->get("output");
        if(output && output->kind==Json::Array) for(const auto& item:output->items) {
            if(auto type=item.get("type"); type && type->str()=="function_call") parse_call(item,false);
            if(auto content=item.get("content"); content && content->kind==Json::Array)
                for(const auto& part:content->items)
                    if(auto type=part.get("type"); type && type->str()=="output_text")
                        if(auto t=part.get("text"); t && t->kind==Json::String)
                            out.text=out.text.value_or("")+t->value;
        }
    }
    if(!out.text && out.calls.empty()) { err="API response contained neither text nor tool calls: "+raw.substr(0,1000); return {}; }
    return out;
}

// ---------- policy ----------

enum class ApprovalPolicy { OnRequest, Never };
enum class SandboxMode { ReadOnly, WorkspaceWrite, DangerFullAccess };

static const char* sandbox_name(SandboxMode m) {
    switch (m) {
        case SandboxMode::ReadOnly: return "read-only";
        case SandboxMode::WorkspaceWrite: return "workspace-write";
        case SandboxMode::DangerFullAccess: return "danger-full-access";
    }
    return "unknown";
}

struct RuntimePolicy {
    ApprovalPolicy approval = ApprovalPolicy::OnRequest;
    SandboxMode sandbox = SandboxMode::WorkspaceWrite;

    bool can_write() const { return sandbox != SandboxMode::ReadOnly; }
    bool shell_allowed() const { return true; }
    bool needs_shell_approval() const {
        return approval == ApprovalPolicy::OnRequest && sandbox == SandboxMode::WorkspaceWrite;
    }
    bool needs_write_approval() const {
        return approval == ApprovalPolicy::OnRequest && sandbox != SandboxMode::DangerFullAccess;
    }
};

static bool approve(const std::string& action, bool default_yes = false) {
    std::cerr << "Approval required: " << action << (default_yes ? " [Y/n] " : " [y/N] ") << std::flush;
    std::string s; std::getline(std::cin, s); s = trim(s);
    if (s.empty()) return default_yes;
    return s == "y" || s == "Y" || s == "yes" || s == "YES";
}

// ---------- rollout/session ----------

enum class EventKind {
    SessionStarted, SessionResumed, TurnStarted, ModelOutput, ToolRequested,
    ApprovalRequested, ApprovalDecision, ToolStarted, ToolCompleted,
    TurnCompleted, Error
};

static const char* event_name(EventKind k) {
    switch (k) {
        case EventKind::SessionStarted: return "session.started";
        case EventKind::SessionResumed: return "session.resumed";
        case EventKind::TurnStarted: return "turn.started";
        case EventKind::ModelOutput: return "model.output";
        case EventKind::ToolRequested: return "tool.requested";
        case EventKind::ApprovalRequested: return "approval.requested";
        case EventKind::ApprovalDecision: return "approval.decision";
        case EventKind::ToolStarted: return "tool.started";
        case EventKind::ToolCompleted: return "tool.completed";
        case EventKind::TurnCompleted: return "turn.completed";
        case EventKind::Error: return "error";
    }
    return "unknown";
}

struct Session {
    fs::path root;
    fs::path dir;
    fs::path events_path;
    fs::path transcript_path;
    fs::path items_path;
    fs::path meta_path;
    std::string id;
    bool echo_json = false;
    std::string transcript;
    std::vector<std::string> items; // Responses input items, one JSON object per journal line

    size_t context_bytes() const {
        size_t n=0;
        for(const auto& item:items) {
            n+=item.size();size_t pos=0;
            while((pos=item.find("data:image/",pos))!=std::string::npos) {
                const size_t end=item.find('"',pos);
                if(end==std::string::npos) break;
                if(end-pos>1000) n-=end-pos-1000; // image bytes are not text tokens
                pos=end;
            }
        }
        return n;
    }
    bool record_item(const std::string& item) {
        auto json=parse_json(item);
        if(!json || json->kind!=Json::Object) return false;
        std::ofstream f(items_path,std::ios::app|std::ios::binary);
        if(!f || !(f<<item<<'\n')) return false;
        items.push_back(item); return true;
    }
    bool replace_items(const std::vector<std::string>& compacted) {
        const fs::path staged=items_path.string()+".new";
        {
            std::ofstream f(staged,std::ios::binary|std::ios::trunc);
            if(!f) return false;
            for(const auto& item:compacted) {
                auto json=parse_json(item);
                if(!json || json->kind!=Json::Object || !(f<<item<<'\n')) return false;
            }
            f.flush(); if(!f) return false;
        }
        std::error_code ec; fs::rename(staged,items_path,ec);
        if(ec) {fs::remove(staged,ec);return false;}
        items=compacted;return true;
    }

    void emit(EventKind kind, const std::string& payload = "{}") const {
        std::ostringstream line;
        line << "{\"timestamp\":\"" << json_escape(now_iso8601())
             << "\",\"session_id\":\"" << json_escape(id)
             << "\",\"type\":\"" << event_name(kind)
             << "\",\"payload\":" << payload << "}";
        std::ofstream f(events_path, std::ios::app | std::ios::binary);
        if (f) f << line.str() << '\n';
        if (echo_json) std::cout << line.str() << '\n';
    }

    void append_transcript(const std::string& text) {
        transcript += text;
        std::ofstream f(transcript_path, std::ios::binary | std::ios::trunc);
        if (f) f << transcript;
    }
};

static bool init_session(Session& s, const fs::path& root, bool echo_json,
                         const std::optional<std::string>& resume, std::string& err) {
    s.items.clear(); s.transcript.clear();
    s.root = root;
    s.dir = root / ".codex_cpp" / "sessions";
    std::error_code ec; fs::create_directories(s.dir, ec);
    if (ec) { err = "cannot create session dir: " + ec.message(); return false; }
    s.echo_json = echo_json;
    s.id = resume ? *resume : random_id("session");
    s.events_path = s.dir / (s.id + ".jsonl");
    s.transcript_path = s.dir / (s.id + ".transcript");
    s.items_path = s.dir / (s.id + ".items.jsonl");
    s.meta_path = s.dir / (s.id + ".meta.json");

    if (resume) {
        if(s.id.empty() || s.id=="." || s.id==".." ||
           s.id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-" )!=std::string::npos) {
            err="invalid session ID";return false;
        }
        if (!fs::exists(s.transcript_path)) { err = "session not found: " + s.id; return false; }
        s.transcript = slurp(s.transcript_path, 2'000'000);
        if(fs::exists(s.items_path)) {
            std::ifstream f(s.items_path,std::ios::binary); std::string line;
            while(std::getline(f,line)) {
                if(line.size()>12'000'000 || !parse_json(line)) {err="invalid session item journal";return false;}
                s.items.push_back(line);
            }
            if(!f.eof()) {err="could not read session item journal";return false;}
        } else if(!s.transcript.empty()) {
            // Upgrade old flat transcripts once; subsequent turns use typed items.
            if(!s.record_item("{\"role\":\"user\",\"content\":\""+json_escape(s.transcript)+"\"}")) {
                err="could not migrate session transcript";return false;
            }
        }
        s.emit(EventKind::SessionResumed, "{\"cwd\":\"" + json_escape(root.string()) + "\"}");
    } else {
        std::ofstream meta(s.meta_path, std::ios::binary | std::ios::trunc);
        if (meta) meta << "{\"id\":\"" << json_escape(s.id) << "\",\"created_at\":\""
                       << json_escape(now_iso8601()) << "\",\"cwd\":\"" << json_escape(root.string()) << "\"}\n";
        s.emit(EventKind::SessionStarted, "{\"cwd\":\"" + json_escape(root.string()) + "\"}");
    }
    return true;
}

static void list_sessions(const fs::path& root) {
    const fs::path dir = root / ".codex_cpp" / "sessions";
    std::error_code ec;
    if (!fs::exists(dir, ec)) return;
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.path().extension() == ".jsonl") files.push_back(e.path());
    std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) {
        std::error_code ec1, ec2;
        return fs::last_write_time(a, ec1) > fs::last_write_time(b, ec2);
    });
    for (const auto& p : files) std::cout << p.stem().string() << '\n';
}

static std::vector<std::string> session_ids(const fs::path& root) {
    const fs::path dir = root / ".codex_cpp" / "sessions";
    std::error_code ec;
    std::vector<fs::path> files;
    if (!fs::exists(dir, ec)) return {};
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.path().extension() == ".jsonl") files.push_back(e.path());
    std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) {
        std::error_code ea, eb;
        return fs::last_write_time(a, ea) > fs::last_write_time(b, eb);
    });
    std::vector<std::string> out;
    for (const auto& f : files) out.push_back(f.stem().string());
    return out;
}

static std::string command_arguments(const std::string& line) {
    const size_t p = line.find_first_of(" \t\r\n");
    return p == std::string::npos ? std::string() : trim(line.substr(p + 1));
}

static std::vector<std::string> split_words(const std::string& text) {
    std::istringstream in(text); std::vector<std::string> out; std::string word;
    while (in >> word) out.push_back(word);
    return out;
}

static std::string safe_terminal_text(std::string text) {
    text.erase(std::remove(text.begin(), text.end(), '\x1b'), text.end());
    text.erase(std::remove(text.begin(), text.end(), '\a'), text.end());
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    text.erase(std::remove(text.begin(), text.end(), '\n'), text.end());
    return text;
}

static bool write_session_name(const Session& s, const std::string& name, std::string& err) {
    std::ofstream meta(s.meta_path, std::ios::binary | std::ios::trunc);
    if (!meta) { err = "cannot update session metadata"; return false; }
    meta << "{\"id\":\"" << json_escape(s.id) << "\",\"updated_at\":\""
         << json_escape(now_iso8601()) << "\",\"cwd\":\"" << json_escape(s.root.string())
         << "\",\"name\":\"" << json_escape(name) << "\"}\n";
    return true;
}

static bool archive_session_files(const Session& s, std::string& err) {
    const fs::path archive = s.root / ".codex_cpp" / "archived_sessions";
    std::error_code ec; fs::create_directories(archive, ec);
    if (ec) { err = "cannot create archive directory: " + ec.message(); return false; }
    const fs::path official=s.dir/(s.id+".official-id");
    for (const fs::path& src : {s.events_path,s.transcript_path,s.items_path,s.meta_path,official}) {
        if (!fs::exists(src, ec)) continue;
        fs::rename(src, archive / src.filename(), ec);
        if (ec) { err = "cannot archive " + src.filename().string() + ": " + ec.message(); return false; }
    }
    return true;
}

static bool delete_session_files(const Session& s, std::string& err) {
    std::error_code ec;
    const fs::path official=s.dir/(s.id+".official-id");
    for (const fs::path& src : {s.events_path,s.transcript_path,s.items_path,s.meta_path,official}) {
        if (fs::exists(src, ec) && !fs::remove(src, ec)) { err = "cannot delete " + src.filename().string(); return false; }
        if (ec) { err = "cannot delete " + src.filename().string() + ": " + ec.message(); return false; }
    }
    return true;
}

static std::string clipboard_copy(const std::string& text) {
    const fs::path tmp = temp_path("codex-cpp-clipboard");
    { std::ofstream f(tmp, std::ios::binary); if (!f) return "cannot create clipboard temp file"; f << text; }
#if defined(_WIN32)
    const std::string cmd = "type " + shell_quote(tmp.string()) + " | clip";
#elif defined(__APPLE__)
    const std::string cmd = "pbcopy < " + shell_quote(tmp.string());
#else
    const std::string cmd = "(command -v wl-copy >/dev/null 2>&1 && wl-copy < " + shell_quote(tmp.string()) +
                            ") || (command -v xclip >/dev/null 2>&1 && xclip -selection clipboard < " + shell_quote(tmp.string()) + ")";
#endif
    CommandResult r = run_capture(cmd, 4096);
    std::error_code ec; fs::remove(tmp, ec);
    return r.exit_code == 0 ? std::string() : "clipboard command failed";
}

// ---------- tools ----------

struct ToolResult {
    int exit_code = 0;
    std::string output;
};

class ToolRuntime {
public:
    virtual ~ToolRuntime() = default;
    virtual std::string name() const = 0;
    virtual std::string spec_json() const = 0;
    virtual bool mutates() const { return false; }
    virtual bool is_shell() const { return false; }
    virtual ToolResult run(const std::string& args, const fs::path& root) = 0;
    virtual ToolResult run_with_ui(const std::string& args,const fs::path& root,
                                   const std::function<void()>&,bool) {return run(args,root);}
};

class ReadFileTool final : public ToolRuntime {
public:
    std::string name() const override { return "read_file"; }
    std::string spec_json() const override {
        return R"({"type":"function","name":"read_file","description":"Read a UTF-8/text file inside the workspace.","parameters":{"type":"object","properties":{"path":{"type":"string"}},"required":["path"],"additionalProperties":false},"strict":true})";
    }
    ToolResult run(const std::string& args, const fs::path& root) override {
        auto p = json_string_field(args, "path");
        if (!p) return {2, "ERROR: missing path"};
        fs::path t = root / *p;
        if (!within_root(root, t)) return {2, "ERROR: path escapes workspace"};
        return {0, slurp(t)};
    }
};

class ListDirTool final : public ToolRuntime {
public:
    std::string name() const override { return "list_dir"; }
    std::string spec_json() const override {
        return R"({"type":"function","name":"list_dir","description":"List one directory inside the workspace.","parameters":{"type":"object","properties":{"path":{"type":"string"}},"required":["path"],"additionalProperties":false},"strict":true})";
    }
    ToolResult run(const std::string& args, const fs::path& root) override {
        auto p = json_string_field(args, "path");
        if (!p) return {2, "ERROR: missing path"};
        fs::path t = root / *p;
        if (!within_root(root, t)) return {2, "ERROR: path escapes workspace"};
        return {0, list_dir(t)};
    }
};

static std::vector<std::pair<std::string,fs::path>> available_skills(const fs::path& root) {
    std::map<std::string,fs::path> found;
    const std::vector<fs::path> bases={fs::path(getenv_or("HOME"))/".codex"/"skills",root/".agents"/"skills"};
    for(const auto& base:bases) {
        std::error_code ec;
        if(!fs::is_directory(base,ec)) continue;
        for(const auto& entry:fs::directory_iterator(base,ec)) {
            const std::string name=entry.path().filename().string();
            if(name.empty() || name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")!=std::string::npos) continue;
            fs::path file=entry.path()/"SKILL.md";
            if(entry.is_directory(ec) && fs::is_regular_file(file,ec) && within_root(base,file)) found[name]=file;
        }
    }
    return {found.begin(),found.end()};
}

static std::string skills_manifest(const fs::path& root) {
    std::ostringstream out;
    for(const auto& [name,path]:available_skills(root)) {
        std::string body=slurp(path,4096),description;
        std::istringstream in(body);std::string line;
        while(std::getline(in,line)) if(line.rfind("description:",0)==0) {
            description=trim(line.substr(12));break;
        }
        out<<name<<": "<<description<<'\n';
    }
    return out.str();
}

class ReadSkillTool final : public ToolRuntime {
public:
    std::string name() const override {return "read_skill";}
    std::string spec_json() const override {
        return R"({"type":"function","name":"read_skill","description":"Read an available project or user skill by name. The skill list is in the session context.","parameters":{"type":"object","properties":{"name":{"type":"string"}},"required":["name"],"additionalProperties":false},"strict":true})";
    }
    ToolResult run(const std::string& args,const fs::path& root) override {
        auto wanted=json_string_field(args,"name");if(!wanted) return {2,"ERROR: missing name"};
        for(const auto& [name,path]:available_skills(root))
            if(name==*wanted) return {0,slurp(path,100000)};
        return {2,"ERROR: skill not found: "+*wanted};
    }
};

class WriteFileTool final : public ToolRuntime {
public:
    std::string name() const override { return "write_file"; }
    bool mutates() const override { return true; }
    std::string spec_json() const override {
        return R"({"type":"function","name":"write_file","description":"Replace a text file inside the workspace with complete contents.","parameters":{"type":"object","properties":{"path":{"type":"string"},"content":{"type":"string"}},"required":["path","content"],"additionalProperties":false},"strict":true})";
    }
    ToolResult run(const std::string& args, const fs::path& root) override {
        auto p = json_string_field(args, "path");
        auto c = json_string_field(args, "content");
        if (!p || !c) return {2, "ERROR: missing path/content"};
        fs::path t = root / *p;
        if (!within_root(root, t)) return {2, "ERROR: path escapes workspace"};
        std::error_code ec;
        if(fs::exists(t,ec) && fs::is_regular_file(t,ec) && fs::hard_link_count(t,ec)>1)
            return {126,"DENIED: hardlinked target may modify an external file"};
        if(ec) return {126,"DENIED: cannot inspect target: "+ec.message()};
        std::string err;
        if (!write_all(t, *c, err)) return {2, "ERROR: " + err};
        return {0, "OK: wrote " + std::to_string(c->size()) + " bytes to " + *p};
    }
};

// A prompt-level policy cannot restrict a shell. The launcher must establish an
// OS boundary, and must refuse restricted execution if that boundary is absent.
static std::optional<std::string> restricted_shell_command(const fs::path& root,
                                                            const std::string& command,
                                                            SandboxMode mode) {
    if(mode==SandboxMode::DangerFullAccess) return cd_prefix(root)+command;
    std::error_code ec;
    const fs::path canonical=fs::canonical(root,ec);
    if(ec) return {};
#if defined(__APPLE__)
    static const bool usable=[] {
        if(!fs::exists("/usr/bin/sandbox-exec")) return false;
        const std::string probe="/usr/bin/sandbox-exec -p "+
            shell_quote("(version 1)(deny default)(allow process*)(allow file-read*)(allow sysctl-read)(allow mach-lookup)(allow ipc-posix-shm)")+
            " /usr/bin/true";
        return run_capture(probe,1000).exit_code==0;
    }();
    if(!usable) return {};
    auto sbpl_quote=[](const std::string& s) {
        std::string out="\"";
        for(char c:s) {if(c=='\\'||c=='\"') out+='\\';out+=c;}
        return out+'"';
    };
    std::string profile="(version 1)(deny default)(allow process*)(allow file-read*)"
                        "(allow sysctl-read)(allow mach-lookup)(allow ipc-posix-shm)";
    if(mode==SandboxMode::WorkspaceWrite)
        profile+="(allow file-write* (subpath "+sbpl_quote(canonical.string())+"))";
    // No network permission is granted. The working directory remains available
    // for build outputs in workspace-write mode.
    const std::string setup=mode==SandboxMode::WorkspaceWrite
        ? "TMPDIR="+shell_quote(canonical.string())+"; export TMPDIR; " : "";
    return shell_quote("/usr/bin/sandbox-exec")+" -p "+shell_quote(profile)+
           " /bin/sh -c "+shell_quote(setup+cd_prefix(canonical)+command);
#elif defined(__linux__)
    static const bool usable=[] {
        if(!fs::exists("/usr/bin/bwrap")) return false;
        return run_capture("/usr/bin/bwrap --ro-bind / / --dev /dev --proc /proc --unshare-net --unshare-pid -- /bin/true",1000).exit_code==0;
    }();
    if(!usable) return {};
    std::string launch="/usr/bin/bwrap --ro-bind / / --dev /dev --proc /proc --unshare-net --unshare-pid --die-with-parent --new-session";
    if(!within_root("/tmp",canonical)) launch+=" --tmpfs /tmp";
    if(mode==SandboxMode::WorkspaceWrite)
        launch+=" --bind "+shell_quote(canonical.string())+" "+shell_quote(canonical.string());
    launch+=" -- /bin/sh -c "+shell_quote(cd_prefix(canonical)+command);
    return launch;
#else
    (void)command;(void)mode;
    return {};
#endif
}

class ShellTool final : public ToolRuntime {
public:
    std::string name() const override { return "shell"; }
    bool mutates() const override { return true; }
    bool is_shell() const override { return true; }
    std::string spec_json() const override {
        return R"({"type":"function","name":"shell","description":"Run a shell command from the workspace root. Use for search, build, tests, git, and project commands.","parameters":{"type":"object","properties":{"command":{"type":"string"}},"required":["command"],"additionalProperties":false},"strict":true})";
    }
    ToolResult run(const std::string& args, const fs::path& root) override {
        return run_interruptible(args,root,SandboxMode::WorkspaceWrite,{},false);
    }
    ToolResult run_interruptible(const std::string& args, const fs::path& root, SandboxMode mode,
                               const std::function<void()>& on_tick, bool watch_escape) {
        auto c = json_string_field(args, "command");
        if (!c) return {2, "ERROR: missing command"};
        if(mode==SandboxMode::WorkspaceWrite)
            if(auto risk=external_hardlink_risk(root)) return {126,"DENIED: "+*risk};
        auto command=restricted_shell_command(root,*c,mode);
        if(!command) return {126,"DENIED: OS shell sandbox unavailable; choose danger-full-access explicitly only if you trust the command"};
        auto r = run_capture_interruptible(*command,200000,on_tick,watch_escape);
        return {r.exit_code, "exit_code=" + std::to_string(r.exit_code) + "\n" + r.output};
    }
};

class ApplyPatchTool final : public ToolRuntime {
public:
    std::string name() const override { return "apply_patch"; }
    bool mutates() const override { return true; }
    std::string spec_json() const override {
        return R"({"type":"function","name":"apply_patch","description":"Apply a unified git patch to files inside the workspace.","parameters":{"type":"object","properties":{"patch":{"type":"string"}},"required":["patch"],"additionalProperties":false},"strict":true})";
    }
    ToolResult run(const std::string& args, const fs::path& root) override {
        return run_with_policy(args,root,SandboxMode::WorkspaceWrite);
    }
    ToolResult run_with_policy(const std::string& args, const fs::path& root, SandboxMode mode) {
        auto patch = json_string_field(args, "patch");
        if (!patch) return {2, "ERROR: missing patch"};
        if(mode!=SandboxMode::DangerFullAccess)
            if(auto risk=external_hardlink_risk(root)) return {126,"DENIED: "+*risk};
        const fs::path tmp = temp_path_in(root,".codex-cpp-patch");
        { std::ofstream f(tmp, std::ios::binary); if (!f) return {2, "ERROR: cannot create temp patch"}; f << *patch; }
        auto cmd=restricted_shell_command(root,"git apply --whitespace=nowarn "+shell_quote(tmp.string()),mode);
        if(!cmd) {std::error_code ec;fs::remove(tmp,ec);return {126,"DENIED: OS shell sandbox unavailable"};}
        auto r = run_capture(*cmd);
        std::error_code ec; fs::remove(tmp, ec);
        return {r.exit_code, "exit_code=" + std::to_string(r.exit_code) + "\n" + r.output};
    }
};

class ToolRouter {
    std::map<std::string, ToolRuntime*> by_name_;
    std::vector<std::unique_ptr<ToolRuntime>> owned_;
public:
    template<class T> void add() {
        auto p = std::make_unique<T>();
        by_name_[p->name()] = p.get();
        owned_.push_back(std::move(p));
    }
    void add_dynamic(std::unique_ptr<ToolRuntime> p) {
        if(by_name_.count(p->name())) return;
        by_name_[p->name()]=p.get();owned_.push_back(std::move(p));
    }
    ToolRuntime* find(const std::string& name) const {
        auto it = by_name_.find(name); return it == by_name_.end() ? nullptr : it->second;
    }
    std::string specs_json() const {
        std::ostringstream o; o << '[';
        for (size_t i = 0; i < owned_.size(); ++i) { if (i) o << ','; o << owned_[i]->spec_json(); }
        o << ']'; return o.str();
    }
    std::string chat_specs_json() const {
        // Responses API uses {type,name,description,parameters,...}; Chat
        // Completions nests the function fields below "function".
        std::ostringstream o; o << '[';
        for (size_t i = 0; i < owned_.size(); ++i) {
            if (i) o << ',';
            const std::string spec = owned_[i]->spec_json();
            auto name = json_string_field(spec, "name").value_or(owned_[i]->name());
            auto desc = json_string_field(spec, "description").value_or("");
            size_t pp = spec.find("\"parameters\":");
            std::string params = "{}";
            if (pp != std::string::npos) {
                pp += std::string("\"parameters\":").size();
                size_t i0 = pp; while(i0<spec.size() && (spec[i0]==' ' || spec[i0]=='\n')) ++i0;
                if (i0 < spec.size() && spec[i0] == '{') {
                    int depth = 0; bool in = false, esc = false; size_t j = i0;
                    for (; j < spec.size(); ++j) {
                        char c = spec[j];
                        if (in) { if (esc) esc=false; else if (c=='\\') esc=true; else if (c=='"') in=false; continue; }
                        if (c=='"') { in=true; continue; }
                        if (c=='{') ++depth; else if (c=='}' && --depth==0) { ++j; break; }
                    }
                    params = spec.substr(i0, j-i0);
                }
            }
            o << "{\"type\":\"function\",\"function\":{\"name\":\"" << json_escape(name)
              << "\",\"description\":\"" << json_escape(desc) << "\",\"parameters\":" << params << "}}";
        }
        o << ']'; return o.str();
    }
    ToolResult invoke(const FunctionCall& call, const fs::path& root,
                      const RuntimePolicy& policy, const Session& session,
                      const std::function<std::optional<bool>(const std::string&)>& approval_ui = {},
                      const std::function<void()>& on_tick = {}, bool watch_escape = false) const {
        ToolRuntime* tool = find(call.name);
        if (!tool) return {2, "ERROR: unknown tool: " + call.name};
        if(tool->is_shell() && policy.sandbox==SandboxMode::WorkspaceWrite)
            if(auto risk=external_hardlink_risk(root)) return {126,"DENIED: "+*risk};
        if (tool->is_shell() && policy.sandbox != SandboxMode::DangerFullAccess &&
            !restricted_shell_command(root,"true",policy.sandbox))
            return {126,"DENIED: OS shell sandbox unavailable; restricted shell cannot run here"};
        // The shell may run under the read-only OS sandbox for inspection.
        // Its mutating() marker is conservative for approval, not a reason to
        // suppress all shell commands before the OS policy sees them.
        if (tool->mutates() && !tool->is_shell() && !policy.can_write())
            return {126, "DENIED: mutation disabled by read-only policy"};

        bool need = tool->is_shell() ? policy.needs_shell_approval()
                                     : (tool->mutates() && policy.needs_write_approval());
        if (need) {
            session.emit(EventKind::ApprovalRequested, "{\"tool\":\"" + json_escape(call.name) + "\"}");
            const std::string approval_text = call.name + ": " + call.arguments;
            const bool ok = approval_ui ? approval_ui(approval_text).value_or(false) : approve(approval_text, false);
            session.emit(EventKind::ApprovalDecision, std::string("{\"approved\":") + (ok ? "true}" : "false}"));
            if (!ok) return {126, "DENIED by user"};
        }
        session.emit(EventKind::ToolStarted, "{\"tool\":\"" + json_escape(call.name) + "\",\"call_id\":\"" + json_escape(call.call_id) + "\"}");
        ToolResult r = tool->is_shell()
            ? static_cast<ShellTool*>(tool)->run_interruptible(call.arguments,root,policy.sandbox,on_tick,watch_escape)
            : call.name=="apply_patch"
                ? static_cast<ApplyPatchTool*>(tool)->run_with_policy(call.arguments,root,policy.sandbox)
                : tool->run_with_ui(call.arguments,root,on_tick,watch_escape);
        session.emit(EventKind::ToolCompleted, "{\"tool\":\"" + json_escape(call.name) + "\",\"call_id\":\"" + json_escape(call.call_id) + "\",\"exit_code\":" + std::to_string(r.exit_code) + "}");
        return r;
    }
};

// MCP stdio transport (2025-11-25 handshake). Only explicitly configured,
// user-owned servers are launched. stdout is reserved for JSON-RPC frames.
class McpServer {
public:
    std::string name;
#if !defined(_WIN32)
    pid_t pid=-1;
    int input=-1,output=-1;
#endif
    int next_id=1;
    std::string buffered;
    explicit McpServer(std::string n):name(std::move(n)){}
    ~McpServer(){stop();}
    void stop() {
#if !defined(_WIN32)
        if(input>=0) {::close(input);input=-1;}
        if(output>=0) {::close(output);output=-1;}
        if(pid>0) {
            if(::getpgid(pid)==pid) ::kill(-pid,SIGTERM); else ::kill(pid,SIGTERM);
            int status=0;bool reaped=false;
            for(int i=0;i<10;++i) {
                pid_t done=::waitpid(pid,&status,WNOHANG);
                if(done==pid || (done<0 && errno==ECHILD)) {reaped=true;break;}
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if(!reaped) {
                if(::getpgid(pid)==pid) ::kill(-pid,SIGKILL);else ::kill(pid,SIGKILL);
                while(::waitpid(pid,&status,0)<0 && errno==EINTR){}
            }
            pid=-1;
        }
#endif
    }
    bool start(const std::vector<std::string>& command,const fs::path& cwd,std::string& err) {
#if defined(_WIN32)
        (void)command;(void)cwd;err="MCP stdio requires macOS or Linux";return false;
#else
        if(command.empty() || command[0].empty()) {err="empty MCP command";return false;}
        int to_child[2],from_child[2];
        if(::pipe(to_child)!=0) {err="MCP pipe failed";return false;}
        if(::pipe(from_child)!=0) {::close(to_child[0]);::close(to_child[1]);err="MCP pipe failed";return false;}
        pid=::fork();
        if(pid==0) {
            ::setsid();::dup2(to_child[0],STDIN_FILENO);::dup2(from_child[1],STDOUT_FILENO);
            ::close(to_child[0]);::close(to_child[1]);::close(from_child[0]);::close(from_child[1]);
            int quiet=::open("/dev/null",O_WRONLY);
            if(quiet>=0) {::dup2(quiet,STDERR_FILENO);::close(quiet);}
            if(::chdir(cwd.c_str())!=0) _exit(127);
            std::vector<char*> argv;for(const auto& arg:command) argv.push_back(const_cast<char*>(arg.c_str()));
            argv.push_back(nullptr);::execvp(argv[0],argv.data());_exit(127);
        }
        ::close(to_child[0]);::close(from_child[1]);
        if(pid<0) {::close(to_child[1]);::close(from_child[0]);err="MCP fork failed";return false;}
        input=to_child[1];output=from_child[0];
        ::fcntl(input,F_SETFL,::fcntl(input,F_GETFL,0)|O_NONBLOCK);
        ::fcntl(output,F_SETFL,::fcntl(output,F_GETFL,0)|O_NONBLOCK);
        auto initialized=request("initialize","{\"protocolVersion\":\"2025-11-25\",\"capabilities\":{},\"clientInfo\":{\"name\":\"codex-cpp\",\"version\":\"1\"}}",err,10);
        if(!initialized) {stop();return false;}
        const Json* version=initialized->get("protocolVersion");
        if(!version || version->kind!=Json::String) {err="MCP initialize omitted protocolVersion";stop();return false;}
        if(version->value!="2025-11-25" && version->value!="2025-06-18" &&
           version->value!="2025-03-26") {
            err="unsupported MCP protocol version: "+version->value;stop();return false;
        }
        if(!send("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n",err,5)) {stop();return false;}
        return true;
#endif
    }
    bool send(const std::string& frame,std::string& err,int timeout_seconds) {
#if defined(_WIN32)
        (void)frame;(void)timeout_seconds;err="MCP unavailable";return false;
#else
        auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(timeout_seconds);
        size_t pos=0;
        while(pos<frame.size()) {
            if(std::chrono::steady_clock::now()>=deadline) {err="MCP write timed out";return false;}
            fd_set writable;FD_ZERO(&writable);FD_SET(input,&writable);timeval tv{0,250000};
            int ready=::select(input+1,nullptr,&writable,nullptr,&tv);
            if(ready<0 && errno!=EINTR) {err="MCP write failed";return false;}
            if(ready>0) {
                ssize_t n=::write(input,frame.data()+pos,frame.size()-pos);
                if(n>0) pos+=static_cast<size_t>(n);
                else if(n<0 && errno!=EAGAIN && errno!=EINTR) {err="MCP server closed input";return false;}
            }
        }
        return true;
#endif
    }
    std::optional<Json> request(const std::string& method,const std::string& params,std::string& err,
                                int timeout_seconds=120,const std::function<void()>& on_tick={},bool watch_escape=false) {
#if defined(_WIN32)
        (void)method;(void)params;(void)timeout_seconds;(void)on_tick;(void)watch_escape;
        err="MCP unavailable";return {};
#else
        if(input<0 || output<0) {err="MCP server is disconnected";return {};}
        int id=next_id++;
        std::string frame="{\"jsonrpc\":\"2.0\",\"id\":"+std::to_string(id)+
            ",\"method\":\""+json_escape(method)+"\",\"params\":"+params+"}\n";
        if(frame.size()>1'000'000) {err="MCP request too large";return {};}
        if(!send(frame,err,10)) return {};
        termios saved{};bool raw=false;
        if(watch_escape && isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO,&saved)==0) {
            termios t=saved;t.c_lflag&=static_cast<tcflag_t>(~(ICANON|ECHO));
            t.c_cc[VMIN]=0;t.c_cc[VTIME]=0;raw=tcsetattr(STDIN_FILENO,TCSANOW,&t)==0;
        }
        auto restore=[&]{if(raw) tcsetattr(STDIN_FILENO,TCSANOW,&saved);};
        auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(timeout_seconds);
        auto last_tick=std::chrono::steady_clock::now();
        for(;;) {
            size_t newline=buffered.find('\n');
            if(newline!=std::string::npos) {
                std::string line=buffered.substr(0,newline);buffered.erase(0,newline+1);
                auto msg=parse_json(line);if(!msg) {err="invalid MCP JSON-RPC frame";restore();return {};}
                const Json* mid=msg->get("id");
                if(!mid || mid->value!=std::to_string(id)) continue; // notifications are asynchronous
                restore();
                if(auto failure=msg->get("error")) {
                    auto detail=failure->get("message");err="MCP error: "+(detail?detail->str():"unknown");return {};
                }
                auto result=msg->get("result");
                if(!result || result->kind!=Json::Object) {err="MCP response has no result";return {};}
                return *result;
            }
            if(buffered.size()>1'000'000) {err="MCP response line too large";restore();return {};}
            if(std::chrono::steady_clock::now()>=deadline) {err="MCP request timed out";restore();return {};}
            fd_set readfds;FD_ZERO(&readfds);FD_SET(output,&readfds);
            if(raw) FD_SET(STDIN_FILENO,&readfds);
            timeval tv{0,250000};int ready=::select(std::max(output,raw?STDIN_FILENO:output)+1,&readfds,nullptr,nullptr,&tv);
            if(ready<0 && errno!=EINTR) {err="MCP transport failed";restore();return {};}
            if(ready>0) {
                if(raw && FD_ISSET(STDIN_FILENO,&readfds)) {
                    char keys[64];ssize_t n=::read(STDIN_FILENO,keys,sizeof(keys));
                    if(n>0 && std::find(keys,keys+n,char(27))!=keys+n) {
                        err="Interrupted by Esc";restore();stop();return {};
                    }
                }
                if(FD_ISSET(output,&readfds)) {
                    char bytes[4096];ssize_t n=::read(output,bytes,sizeof(bytes));
                    if(n==0) {err="MCP server closed output";restore();return {};}
                    if(n>0) buffered.append(bytes,static_cast<size_t>(n));
                    else if(errno!=EAGAIN && errno!=EINTR) {err="MCP read failed";restore();return {};}
                }
            }
            auto now=std::chrono::steady_clock::now();
            if(on_tick && now-last_tick>=std::chrono::seconds(1)) {on_tick();last_tick=now;}
        }
#endif
    }
};

class McpTool final : public ToolRuntime {
    std::shared_ptr<McpServer> server_;
    std::string name_,remote_,schema_;
public:
    McpTool(std::shared_ptr<McpServer> server,std::string name,std::string remote,
            std::string description,const Json& schema)
        :server_(std::move(server)),name_(std::move(name)),remote_(std::move(remote)) {
        schema_="{\"type\":\"function\",\"name\":\""+json_escape(name_)+
            "\",\"description\":\""+json_escape(description)+"\",\"parameters\":"+json_dump(schema)+"}";
    }
    std::string name() const override{return name_;}
    std::string spec_json() const override{return schema_;}
    bool mutates() const override{return true;} // unknown external tools need approval
    ToolResult run(const std::string& args,const fs::path& root) override {return run_with_ui(args,root,{},false);}
    ToolResult run_with_ui(const std::string& args,const fs::path&,
                           const std::function<void()>& on_tick,bool watch_escape) override {
        auto parsed=parse_json(args);
        if(!parsed || parsed->kind!=Json::Object) return {2,"ERROR: invalid MCP tool arguments"};
        std::string err;
        auto result=server_->request("tools/call","{\"name\":\""+json_escape(remote_)+
            "\",\"arguments\":"+json_dump(*parsed)+"}",err,120,on_tick,watch_escape);
        if(!result) return {err=="Interrupted by Esc"?130:2,"ERROR: "+err};
        std::ostringstream out;
        if(auto content=result->get("content");content && content->kind==Json::Array)
            for(const auto& part:content->items) {
                auto type=part.get("type"),text=part.get("text");
                if(type && type->str()=="text" && text && text->kind==Json::String) out<<text->value<<'\n';
                else if(type) out<<"[MCP content type: "<<type->str()<<"]\n";
            }
        if(auto structured=result->get("structuredContent")) out<<json_dump(*structured)<<'\n';
        const auto is_error=result->get("isError");
        return {is_error && is_error->value=="true"?1:0,out.str()};
    }
};

static std::string load_mcp_tools(const fs::path& config,const fs::path& root,ToolRouter& router,
                                  std::vector<std::shared_ptr<McpServer>>& servers) {
    if(config.empty() || !fs::exists(config)) return "";
    auto doc=parse_json(slurp(config,2'000'000));
    const Json* definitions=doc?doc->get("mcpServers"):nullptr;
    if(!definitions || definitions->kind!=Json::Object) return "MCP config needs a mcpServers object";
    std::ostringstream report;size_t count=0;
    for(const auto& [name,entry]:definitions->fields) {
        if(++count>16) {report<<"MCP server limit reached\n";break;}
        auto executable=entry.get("command"),args=entry.get("args");
        if(!executable || executable->kind!=Json::String || (args && args->kind!=Json::Array)) {
            report<<name<<": invalid command/args\n";continue;
        }
        std::vector<std::string> command={executable->value};bool valid=true;
        if(args) for(const auto& a:args->items) {
            if(a.kind!=Json::String) {valid=false;break;} command.push_back(a.value);
        }
        if(!valid || command.size()>64) {report<<name<<": invalid arguments\n";continue;}
        auto server=std::make_shared<McpServer>(name);std::string err;
        if(!server->start(command,root,err)) {report<<name<<": "<<err<<'\n';continue;}
        servers.push_back(server);
        std::optional<std::string> cursor;size_t total=0;
        for(int page=0;page<20;++page) {
            auto result=server->request("tools/list",cursor?"{\"cursor\":\""+json_escape(*cursor)+"\"}":"{}",err,10);
            if(!result) {report<<name<<": "<<err<<'\n';break;}
            const Json* tools=result->get("tools");if(!tools || tools->kind!=Json::Array) {report<<name<<": invalid tools/list\n";break;}
            for(const auto& tool:tools->items) {
                auto remote=tool.get("name"),description=tool.get("description"),schema=tool.get("inputSchema");
                if(!remote || remote->kind!=Json::String || !schema || schema->kind!=Json::Object) continue;
                std::string label="mcp__"+name+"__"+remote->value;
                if(label.size()>64 || label.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")!=std::string::npos) continue;
                if(router.find(label)) continue;
                router.add_dynamic(std::make_unique<McpTool>(server,label,remote->value,
                    description?description->str():"MCP tool",*schema));++total;
            }
            auto next=result->get("nextCursor");
            if(!next || next->kind!=Json::String || next->value.empty()) break;
            cursor=next->value;
        }
        report<<name<<": "<<total<<" tools\n";
    }
    return report.str();
}

// ---------- API client ----------

enum class ApiStyle { Responses, ChatCompletions };

static const char* api_style_name(ApiStyle s) {
    return s == ApiStyle::Responses ? "responses" : "chat";
}

struct ApiClient {
    std::string api_key;
    std::string base_url = "https://api.openai.com";
    std::string model = "gpt-5.6-sol";
    std::string reasoning_effort;
    ApiStyle style = ApiStyle::Responses;
    bool no_api_key = false;
    bool chatgpt_auth = false;

    std::optional<std::string> post_codex(const std::string& body,std::string& err,
        const std::function<void(const std::string&)>& on_delta = {},
        const std::function<void()>& on_tick = {}, bool watch_escape = false) const {
        auto a=load_auth(); if(!a) {err="Not signed in. Run --provider codex --login.";return {};}
        if(a->expires<=epoch_now()+90 && !refresh_auth(*a,err)) return {};
        auto request=[&]() {
            WebReply result;
            const fs::path req=temp_path("codex-stream-req"), hdr=temp_path("codex-stream-hdr"),
                           stderr_path=temp_path("codex-stream-stderr");
            {
                std::ofstream f(req,std::ios::binary);f<<body;
                std::ofstream h(hdr,std::ios::binary);
                h<<"Authorization: Bearer "<<a->access<<"\nAccept: text/event-stream\nContent-Type: application/json\n";
                if(!a->account.empty()) h<<"ChatGPT-Account-ID: "<<a->account<<"\n";
            }
            const std::string marker="__CODEX_HTTP_"+random_id("status")+"__:";
            std::string cmd="curl -sS -N --connect-timeout 20 --max-time 300 --max-redirs 0"+
                std::string(" -w ")+shell_quote("\n"+marker+"%{http_code}\n")+
                " -H "+shell_quote("@"+hdr.string())+" --data-binary @"+shell_quote(req.string())+
                " "+shell_quote("https://chatgpt.com/backend-api/codex/responses")+
                " 2>"+shell_quote(stderr_path.string());
#if defined(_WIN32)
            FILE* pipe=_popen(cmd.c_str(),"r");
#else
            int channels[2]={-1,-1};
            pid_t child=-1;
            if(::pipe(channels)==0) {
                child=::fork();
                if(child==0) {
                    ::setsid(); // own process group, so Esc stops the shell and curl
                    ::close(channels[0]);
                    ::dup2(channels[1],STDOUT_FILENO);
                    ::close(channels[1]);
                    ::execl("/bin/sh","sh","-c",cmd.c_str(),static_cast<char*>(nullptr));
                    _exit(127);
                }
                ::close(channels[1]);
                if(child<0) {::close(channels[0]);channels[0]=-1;}
            }
#endif
#if defined(_WIN32)
            if(!pipe) result.error="could not start curl";
#else
            if(child<0 || channels[0]<0) result.error="could not start curl";
#endif
            else {
                std::array<char,4096> buf{};std::string line;
                auto consume_line=[&]() {
                    if(line.rfind(marker,0)==0) {
                        try{result.status=std::stol(line.substr(marker.size()));}catch(...){}
                    } else {
                        if(result.body.size()+line.size()<=8'000'000) result.body+=line;
                        else result.error="Codex stream exceeded 8 MB";
                        if(result.error.empty() && line.rfind("data: ",0)==0 && on_delta) {
                            auto event=parse_json(line.substr(6));
                            if(event && at(&*event,"type") && at(&*event,"type")->str()=="response.output_text.delta") {
                                auto delta=event->get("delta");
                                if(delta && delta->kind==Json::String) on_delta(delta->value);
                            }
                        }
                    }
                    line.clear();
                };
                auto consume_bytes=[&](const char* data,size_t count) {
                    for(size_t i=0;i<count;++i) {
                        line+=data[i];
                        if(data[i]=='\n') consume_line();
                        else if(line.size()>1'000'000) {
                            result.error="Codex stream line exceeded 1 MB";
                            line.clear();
                        }
                    }
                };
#if defined(_WIN32)
                while(fgets(buf.data(),static_cast<int>(buf.size()),pipe))
                    consume_bytes(buf.data(),std::strlen(buf.data()));
#else
                termios saved_terminal{};
                bool raw_input=false;
                if(watch_escape && isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO,&saved_terminal)==0) {
                    termios raw=saved_terminal;
                    raw.c_lflag &= static_cast<tcflag_t>(~(ICANON|ECHO));
                    raw.c_cc[VMIN]=0;raw.c_cc[VTIME]=0;
                    raw_input=tcsetattr(STDIN_FILENO,TCSANOW,&raw)==0;
                }
                auto last_tick=std::chrono::steady_clock::now();
                const int fd=channels[0];
                bool interrupted=false;
                for(;;) {
#if !defined(_WIN32)
                    if(tui_interrupt_requested) {interrupted=true;result.error="Interrupted by Ctrl+C";break;}
#endif
                    fd_set readers;FD_ZERO(&readers);FD_SET(fd,&readers);
                    if(raw_input) FD_SET(STDIN_FILENO,&readers);
                    timeval timeout{0,250000};
                    int ready=select(std::max(fd,raw_input?STDIN_FILENO:fd)+1,&readers,nullptr,nullptr,&timeout);
                    if(ready>0) {
                        if(raw_input && FD_ISSET(STDIN_FILENO,&readers)) {
                            char keys[64];ssize_t count=::read(STDIN_FILENO,keys,sizeof(keys));
                            if(count>0 && contains_plain_escape(keys,count)) {
                                interrupted=true;result.error="Interrupted by Esc";break;
                            }
                        }
                        if(FD_ISSET(fd,&readers)) {
                            ssize_t n=::read(fd,buf.data(),buf.size());
                            if(n>0) consume_bytes(buf.data(),static_cast<size_t>(n));
                            else if(n==0) break;
                            else if(errno!=EINTR) {result.error="stream read failed";break;}
                        }
                    } else if(ready<0 && errno!=EINTR) {result.error="stream wait failed";break;}
                    auto now=std::chrono::steady_clock::now();
                    if(on_tick && now-last_tick>=std::chrono::seconds(1)) {
                        on_tick();last_tick=now;
                    }
                }
                if(raw_input) tcsetattr(STDIN_FILENO,TCSANOW,&saved_terminal);
                if(interrupted) {
                    // The forked shell has its own session. Kill its whole group,
                    // including curl, before waiting so pclose-like hangs cannot occur.
                    if(::getpgid(child)==child) ::kill(-child,SIGTERM);
                    else ::kill(child,SIGTERM);
                }
                ::close(fd);
#endif
                if(!line.empty() && result.error.empty()) consume_line();
#if defined(_WIN32)
                int status=_pclose(pipe);
                if(status!=0 && result.error.empty()) result.error="curl failed: "+trim(slurp(stderr_path,2000));
#else
                int status=0;
                while(::waitpid(child,&status,0)<0 && errno==EINTR) {}
                if((!WIFEXITED(status)||WEXITSTATUS(status)!=0) && result.error.empty())
                    result.error="curl failed: "+trim(slurp(stderr_path,2000));
#endif
            }
            std::error_code ec;fs::remove(req,ec);fs::remove(hdr,ec);fs::remove(stderr_path,ec);
            return result;
        };
        auto r=request();
        if(r.error=="Interrupted by Esc") {err=r.error;return {};}
        if(r.status==401 && refresh_auth(*a,err)) r=request();
        if(!r.error.empty() || r.status!=200) {
            err="Codex backend request failed (HTTP "+std::to_string(r.status)+"): "+r.error;
            // Avoid placing backend error bodies in logs: they may contain tokens.
            return {};
        }
        return r.body;
    }

    std::optional<ApiResponse> parse_codex_stream(const std::string& raw,std::string& err) const {
        std::map<std::string,Json> done;
        std::string id; bool completed=false;
        std::istringstream lines(raw); std::string line;
        while(std::getline(lines,line)) {
            if(line.rfind("data: ",0)!=0) continue;
            auto evt=parse_json(line.substr(6));if(!evt) continue;
            auto type=evt->get("type"); if(!type) continue;
            if(type->str()=="response.output_item.done") {
                auto item=evt->get("item");if(!item) continue;
                auto idx=evt->get("output_index");
                auto key=idx?idx->value:std::to_string(done.size());
                done[key]=*item;
            } else if(type->str()=="response.completed") {
                completed=true;
                if(auto resp=evt->get("response")) {
                    if(auto x=resp->get("id")) id=x->str();
                    if(auto out=resp->get("output");out&&out->kind==Json::Array)
                        for(size_t i=0;i<out->items.size();++i) done[std::to_string(i)]=out->items[i];
                }
            } else if(type->str()=="response.failed") {
                err="Codex backend reported a failed response";return {};
            }
        }
        if(!completed) {err="Codex stream ended before response.completed";return {};}
        std::string aggregate="{\"id\":\""+json_escape(id)+"\",\"output\":[";
        bool first=true;
        for(const auto& [_,item]:done) {if(!first) aggregate+=',';first=false;aggregate+=json_dump(item);}
        aggregate+="]}";
        return parse_api_response(aggregate,false,err);
    }

    std::optional<std::string> post(const std::string& path, const std::string& body,
                                    std::string& err) const {
        if (chatgpt_auth) {
            err = "internal routing error: ChatGPT login request reached API Key transport";
            return std::nullopt;
        }
        const fs::path req = temp_path("codex-cpp-request");
        const fs::path out = temp_path("codex-cpp-response");
        const fs::path header = temp_path("codex-cpp-header");
        { std::ofstream f(req, std::ios::binary); if (!f) { err = "cannot create request temp file"; return std::nullopt; } f << body; }

        std::string cmd = "curl -sS --fail-with-body --connect-timeout 20 --max-time 300 -o " + shell_quote(out.string());
        if (!no_api_key) {
            if (api_key.empty()) { std::error_code ec; fs::remove(req, ec); err = "API key is not set"; return std::nullopt; }
            { std::ofstream f(header, std::ios::binary); f << "Authorization: Bearer " << api_key << "\n"; }
#if !defined(_WIN32)
            fs::permissions(header, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
#endif
            cmd += " -H " + shell_quote("@" + header.string());
        }
        cmd += " -H " + shell_quote("Content-Type: application/json") +
               " --data-binary @" + shell_quote(req.string()) + " " +
               shell_quote(base_url + path);
        CommandResult cr = run_capture_interruptible(cmd, 100000, {}, false);
        const std::string raw = slurp(out, 2'000'000);
        std::error_code ec; fs::remove(req, ec); fs::remove(out, ec); fs::remove(header, ec);
        if (cr.exit_code != 0) {
            err = "HTTP request failed (curl " + std::to_string(cr.exit_code) + "): " + cr.output;
            if (!raw.empty()) err += "\n" + raw;
            return std::nullopt;
        }
        return raw;
    }

    std::optional<ApiResponse> call_text_only(const std::string& instructions,
                                              const std::string& text,
                                              std::string& err) const {
        std::string body;
        std::string path;
        if (style == ApiStyle::Responses) {
            body = "{\"model\":\"" + json_escape(model) +
                   "\",\"instructions\":\"" + json_escape(instructions) +
                   "\",\"input\":\"" + json_escape(text) + "\"";
            if (!reasoning_effort.empty())
                body += ",\"reasoning\":{\"effort\":\"" + json_escape(reasoning_effort) + "\"}";
            body += "}";
            path = "/v1/responses";
        } else {
            body = "{\"model\":\"" + json_escape(model) +
                   "\",\"messages\":[{\"role\":\"system\",\"content\":\"" + json_escape(instructions) +
                   "\"},{\"role\":\"user\",\"content\":\"" + json_escape(text) + "\"}],\"stream\":false}";
            path = "/chat/completions";
        }
        if(chatgpt_auth) {
            body="{\"model\":\""+json_escape(model)+"\",\"instructions\":\""+json_escape(instructions)+
                "\",\"input\":[{\"role\":\"user\",\"content\":\""+json_escape(text)+
                "\"}],\"stream\":true,\"store\":false}";
            auto stream=post_codex(body,err);return stream?parse_codex_stream(*stream,err):std::nullopt;
        }
        auto raw=post(path,body,err); if(!raw) return std::nullopt;
        auto a=parse_api_response(*raw,style==ApiStyle::ChatCompletions,err);
        if(!a || !a->text) { if(err.empty()) err="Text-only response missing text"; return std::nullopt; }
        return a;
    }

    std::optional<ApiResponse> call_responses(const std::string& instructions,
                                    const std::string& input_json,
                                    const std::string& tools_json,
                                    std::string& err,
                                    const std::optional<std::string>& previous_response_id = std::nullopt,
                                    const std::function<void(const std::string&)>& on_delta = {},
                                    const std::function<void()>& on_tick = {}, bool watch_escape = false) const {
        if(chatgpt_auth) {
            std::string body="{\"model\":\""+json_escape(model)+"\",\"instructions\":\""+
                json_escape(instructions)+"\",\"input\":"+input_json+",\"tools\":"+tools_json+
                ",\"tool_choice\":\"auto\",\"parallel_tool_calls\":false,\"stream\":true,\"store\":false";
            if(!reasoning_effort.empty()) body+=",\"reasoning\":{\"effort\":\""+json_escape(reasoning_effort)+"\"}";
            body+="}";
            auto stream=post_codex(body,err,on_delta,on_tick,watch_escape);return stream?parse_codex_stream(*stream,err):std::nullopt;
        }
        std::string body = "{\"model\":\"" + json_escape(model) +
            "\",\"instructions\":\"" + json_escape(instructions) +
            "\",\"input\":" + input_json +
            ",\"tools\":" + tools_json +
            ",\"tool_choice\":\"auto\",\"parallel_tool_calls\":false";
        if (!reasoning_effort.empty())
            body += ",\"reasoning\":{\"effort\":\"" + json_escape(reasoning_effort) + "\"}";
        if (previous_response_id && !previous_response_id->empty())
            body += ",\"previous_response_id\":\"" + json_escape(*previous_response_id) + "\"";
        body += "}";
        auto raw = post("/v1/responses", body, err);
        if (!raw) return std::nullopt;
        return parse_api_response(*raw,false,err);
    }

    std::optional<ApiResponse> call_chat(const std::string& messages_json,
                                         const std::string& tools_json,
                                         std::string& err) const {
        if (chatgpt_auth) {
            err = "ChatGPT login requires the Responses backend";
            return std::nullopt;
        }
        std::string body = "{\"model\":\"" + json_escape(model) +
            "\",\"messages\":" + messages_json +
            ",\"tools\":" + tools_json + ",\"tool_choice\":\"auto\",\"stream\":false}";
        auto raw = post("/chat/completions", body, err);
        if (!raw) return std::nullopt;
        return parse_api_response(*raw,true,err);
    }
};

static const char* kBaseInstructions = R"PROMPT(You are a coding agent running in a terminal-based coding assistant.
Work on the user's task until it is resolved. Inspect relevant project files before editing.
Use the provided structured tools for filesystem and shell work; never claim a command ran when it did not.
Keep edits focused, preserve existing conventions, and validate changes with relevant build or tests when practical.
Treat workspace instructions as scoped project guidance. Direct user instructions take precedence.
Communicate progress briefly during longer work and finish with a concise, useful result.)PROMPT";

static std::string xml_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char ch : s) {
        switch (ch) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default: out += ch; break;
        }
    }
    return out;
}

static std::optional<std::string> load_project_instructions(const fs::path& root) {
    // Codex prefers AGENTS.override.md over AGENTS.md in the same directory.
    // This single-workspace runtime currently has one active cwd, so the root
    // is also the active instruction scope.
    for (const char* name : {"AGENTS.override.md", "AGENTS.md"}) {
        const fs::path path = root / name;
        std::error_code ec;
        if (!fs::is_regular_file(path, ec)) continue;
        std::string text = slurp(path, 32 * 1024);
        if (!trim(text).empty()) return text;
    }
    return std::nullopt;
}

static std::string render_environment_context(const fs::path& root, const RuntimePolicy& policy) {
    std::ostringstream out;
    out << "<environment_context>\n";
    out << "  <cwd>" << xml_escape(root.string()) << "</cwd>\n";
    out << "  <filesystem>\n";
    out << "    <workspace_roots><root>" << xml_escape(root.string()) << "</root></workspace_roots>\n";
    if (policy.sandbox == SandboxMode::DangerFullAccess) {
        out << "    <permission_profile type=\"disabled\"><file_system type=\"unrestricted\" /></permission_profile>\n";
    } else if (policy.sandbox == SandboxMode::ReadOnly) {
        out << "    <permission_profile type=\"managed\"><file_system type=\"restricted\"><entry access=\"read\"><path>"
            << xml_escape(root.string())
            << "</path></entry></file_system></permission_profile>\n";
    } else {
        out << "    <permission_profile type=\"managed\"><file_system type=\"restricted\"><entry access=\"write\"><path>"
            << xml_escape(root.string())
            << "</path></entry></file_system></permission_profile>\n";
    }
    out << "  </filesystem>\n";
    out << "</environment_context>";
    return out.str();
}

static std::string render_permissions_instructions(const RuntimePolicy& policy) {
    std::ostringstream out;
    out << "# Sandbox and approvals\n";
    if (policy.sandbox == SandboxMode::ReadOnly) {
        out << "The workspace is read-only. Do not write files or run mutating shell commands. ";
    } else if (policy.sandbox == SandboxMode::WorkspaceWrite) {
        out << "You may read and write inside the workspace. Do not access paths outside it. ";
    } else {
        out << "Filesystem access is unrestricted by this client policy. ";
    }
    if (policy.approval == ApprovalPolicy::OnRequest && policy.sandbox != SandboxMode::DangerFullAccess) {
        out << "Potentially destructive or escalated actions may require user approval.";
    } else {
        out << "The client will not request interactive approvals.";
    }
    return out.str();
}

static std::string render_model_instructions(const ApiClient& api) {
    std::ostringstream out;
    out << "# Model/runtime context\n";
    out << "Provider: " << api_style_name(api.style) << "; model: " << api.model << ".\n";
    if (!api.reasoning_effort.empty()) out << "Configured reasoning effort: " << api.reasoning_effort << ".\n";
    if (api.style == ApiStyle::ChatCompletions) {
        out << "Tool calls use the Chat Completions tool_calls protocol. Keep tool arguments valid JSON.";
    } else {
        out << "Tool calls use the Responses function_call protocol. Keep tool arguments valid JSON.";
    }
    return out.str();
}

struct PromptBundle {
    std::string developer_instructions;
    std::string contextual_user_prefix;
};

struct PromptBuilder {
    const fs::path& root;
    const RuntimePolicy& policy;
    const ApiClient& api;
    bool plan_mode = false;
    std::string personality;
    std::string goal;

    PromptBundle build() const {
        PromptBundle bundle;
        std::ostringstream developer;
        developer << kBaseInstructions << "\n\n";
        developer << render_model_instructions(api) << "\n\n";
        developer << render_permissions_instructions(policy);
        if (plan_mode) {
            developer << "\n\n# Plan mode\nAnalyze the task and produce an implementation plan. Do not make mutating tool calls.";
        }
        if (!personality.empty()) {
            developer << "\n\n# Communication style\n" << personality;
        }
        bundle.developer_instructions = developer.str();

        std::ostringstream contextual;
        if (auto agents = load_project_instructions(root)) {
            contextual << "# AGENTS.md instructions for " << root.string()
                       << "\n\n<INSTRUCTIONS>\n" << *agents << "\n</INSTRUCTIONS>\n\n";
        }
        if(auto skills=skills_manifest(root); !skills.empty())
            contextual << "# Available skills\n" << skills
                       << "Use read_skill with the exact name when a skill applies.\n\n";
        contextual << render_environment_context(root, policy);
        if (!goal.empty()) contextual << "\n\n<current_goal>" << xml_escape(goal) << "</current_goal>";
        bundle.contextual_user_prefix = contextual.str();
        return bundle;
    }
};

static std::string format_tool_history(const FunctionCall& c, const ToolResult& r) {
    std::ostringstream o;
    o << "\n[assistant function_call]\nname=" << c.name << "\ncall_id=" << c.call_id
      << "\narguments=" << c.arguments << "\n[function_call_output]\n"
      << r.output << "\n[/function_call_output]\n";
    return o.str();
}

// ---------- terminal UI ----------

enum class UiMode { Auto, Tui, Plain };


static bool terminal_is_tty() {
#if defined(_WIN32)
    return _isatty(_fileno(stdin)) && _isatty(_fileno(stdout));
#else
    return isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
#endif
}

static bool enable_and_detect_vt() {
    if (!terminal_is_tty()) return false;
#if defined(_WIN32)
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out == INVALID_HANDLE_VALUE || out == nullptr) return false;
    DWORD mode = 0;
    if (!GetConsoleMode(out, &mode)) return false;
    if (!(mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
        if (!SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) return false;
    }
    return true;
#else
    const std::string term = getenv_or("TERM");
    if (term.empty() || term == "dumb") return false;
    return true;
#endif
}

static size_t terminal_columns() {
#if defined(_WIN32)
    CONSOLE_SCREEN_BUFFER_INFO info{};
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(out, &info)) {
        const int width = info.srWindow.Right - info.srWindow.Left + 1;
        if (width > 0) return static_cast<size_t>(width);
    }
#else
    winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return static_cast<size_t>(ws.ws_col);
    if (const char* cols = std::getenv("COLUMNS")) {
        try { const int n = std::stoi(cols); if (n > 0) return static_cast<size_t>(n); } catch (...) {}
    }
#endif
    return 80;
}

static size_t terminal_rows() {
#if defined(_WIN32)
    CONSOLE_SCREEN_BUFFER_INFO info{};
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(out, &info)) {
        const int height = info.srWindow.Bottom - info.srWindow.Top + 1;
        if (height > 10) return static_cast<size_t>(height);
    }
#else
    winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 10) return ws.ws_row;
    if (const char* rows = std::getenv("LINES")) {
        try { const int n = std::stoi(rows); if (n > 10) return static_cast<size_t>(n); } catch (...) {}
    }
#endif
    return 32;
}


struct SlashCommandDef {
    const char* name;
    const char* description;
    bool implemented;
};

static const std::vector<SlashCommandDef>& slash_commands() {
    static const std::vector<SlashCommandDef> kCommands = {
        {"/model", "choose what model and reasoning effort to use", true},
        {"/login", "sign in with ChatGPT device code", true},
        {"/auth", "show ChatGPT login status", true},
        {"/ide", "include current selection, open files, and other context from your IDE", false},
        {"/permissions", "choose what Codex is allowed to do", true},
        {"/keymap", "remap TUI shortcuts", false},
        {"/vim", "toggle Vim mode for the composer", false},
        {"/experimental", "toggle experimental features", false},
        {"/approve", "approve one retry of a recent auto-review denial", false},
        {"/skills", "list available project and user skills", true},
        {"/import", "import setup, this project, and recent chats from Claude Code", false},
        {"/hooks", "view and manage lifecycle hooks", false},
        {"/review", "review my current changes and find issues", true},
        {"/rename", "rename the current thread", true},
        {"/new", "start a new chat during a conversation", true},
        {"/archive", "archive this session and exit", true},
        {"/delete", "permanently delete this session and exit", true},
        {"/resume", "resume a saved chat", true},
        {"/fork", "fork the current chat", true},
        {"/app", "continue this session in the Desktop app", false},
        {"/init", "create an AGENTS.md file with instructions for Codex", true},
        {"/compact", "summarize conversation to prevent hitting the context limit", true},
        {"/plan", "switch to Plan mode", true},
        {"/goal", "set or view the goal for a long-running task", true},
        {"/agent", "switch the active agent thread", false},
        {"/side", "start a side conversation in an ephemeral fork", true},
        {"/copy", "copy last response as markdown", true},
        {"/raw", "toggle raw scrollback mode for copy-friendly terminal selection", false},
        {"/diff", "show git diff (including untracked files)", true},
        {"/mention", "mention a file", true},
        {"/image", "attach a PNG, JPEG, WebP or GIF to the next turn", true},
        {"/status", "show current session configuration and token usage", true},
        {"/usage", "view account usage or use a usage limit reset", false},
        {"/title", "configure which items appear in the terminal title", true},
        {"/statusline", "configure which items appear in the status line", true},
        {"/theme", "choose a syntax highlighting theme", false},
        {"/pets", "choose or hide the terminal pet", false},
        {"/mcp", "list connected local MCP stdio servers and tools", true},
        {"/plugins", "browse plugins", false},
        {"/logout", "log out of Codex", true},
        {"/exit", "exit Codex", true},
        {"/feedback", "send logs to maintainers", false},
        {"/ps", "list background terminals", true},
        {"/stop", "stop all background terminals", true},
        {"/clear", "clear the terminal and start a new chat", true},
        {"/personality", "choose a communication style for Codex", true},
        {"/subagents", "switch the active agent thread", false},
        {"/memories", "configure memory use and generation", false},
        // C++ port helpers. Hidden from the default popup but still accepted.
        {"/help", "show available commands", true},
        {"/sessions", "list local saved sessions", true},
        {"/history", "show input history", true},
        {"/quit", "exit Codex", true},
    };
    return kCommands;
}

static const SlashCommandDef* find_slash_command(const std::string& name) {
    for (const auto& command : slash_commands()) if (name == command.name) return &command;
    return nullptr;
}

static std::string slash_command_token(const std::string& input) {
    if (input.empty() || input.front() != '/') return {};
    const size_t end = input.find_first_of(" \t\r\n");
    return input.substr(0, end == std::string::npos ? input.size() : end);
}

static std::vector<const SlashCommandDef*> matching_slash_commands(const std::string& input) {
    std::vector<const SlashCommandDef*> out;
    // Popup is valid only for a command token that starts at byte/column zero.
    // Once whitespace/newline appears, the user is entering arguments, not filtering commands.
    if (input.empty() || input.front() != '/' || input.find_first_of(" \t\r\n") != std::string::npos) return out;
    const std::string filter = slash_command_token(input);
    if (filter.empty()) return out;
    for (const auto& command : slash_commands()) {
        const std::string name = command.name;
        if (name == "/help" || name == "/sessions" || name == "/history" || name == "/quit") continue;
        if (name.rfind(filter, 0) == 0) out.push_back(&command);
    }
    return out;
}

class TuiRenderer {
public:
    enum class CellKind { User, Assistant, Tool, Output, Patch, Error, System, SessionSummary, Notice };
    struct Cell { CellKind kind; std::string title; std::string body; };

    TuiRenderer(const fs::path& root, std::string provider, std::string model, std::string sandbox)
        : root_(root.string()), provider_(std::move(provider)), model_(std::move(model)), sandbox_(std::move(sandbox)) {
        std::cout << "\x1b[?1049h\x1b[?2004h\x1b[?1000h\x1b[?1006h\x1b[?25l\x1b[2J\x1b[H" << std::flush;
        active_ = true;
        cells_.push_back({CellKind::SessionSummary, "Codex C++", ""});
        cells_.push_back({CellKind::Notice, "", "Tip: Type /help for commands, or start by describing a coding task."});
        render();
    }
    ~TuiRenderer() {
        if (active_) std::cout << "\x1b[?25h\x1b[?1006l\x1b[?1000l\x1b[?2004l\x1b[?1049l" << std::flush;
    }
    TuiRenderer(const TuiRenderer&) = delete;
    TuiRenderer& operator=(const TuiRenderer&) = delete;

    void add_user(const std::string& text) { add_cell(CellKind::User, "", text); }
    void add_tool(const std::string& name, const std::string& args = {}) {
        std::string label;
        if (name == "shell") label = "Ran shell command";
        else if (name == "read_file") label = "Read file";
        else if (name == "list_dir") label = "Explored directory";
        else if (name == "write_file") label = "Wrote file";
        else if (name == "apply_patch") label = "Applied patch";
        else label = name;
        add_cell(CellKind::Tool, label, compact_args(args));
    }
    void add_tool_output(const std::string& text) {
        std::string shown = text;
        if (shown.size() > 5000) shown = shown.substr(0, 5000) + "\n[output truncated]";
        add_cell(CellKind::Output, "", shown);
    }
    void add_diff(const std::string& patch) { add_cell(CellKind::Patch, "Edited files", patch); }
    void add_assistant(const std::string& text) { add_cell(CellKind::Assistant, "", text); }
    void add_error(const std::string& text) { add_cell(CellKind::Error, "Error", text); }
    void add_system(const std::string& text) { add_cell(CellKind::System, "", text); }
    void clear_history() { cells_.clear(); scroll_offset_=0; render(); }
    void reset_transcript() {
        cells_.clear(); scroll_offset_=0;
        cells_.push_back({CellKind::SessionSummary, "Codex C++", ""});
        cells_.push_back({CellKind::Notice, "", "Tip: Type /help for commands, or start by describing a coding task."});
        render();
    }
    void set_model(std::string model, std::string effort = {}) { model_=std::move(model); effort_=std::move(effort); render(); }
    void set_sandbox(std::string sandbox) { sandbox_=std::move(sandbox); render(); }
    void set_thread_name(std::string name) { thread_name_=std::move(name); render(); }
    void set_statusline_items(const std::vector<std::string>& items) {
        footer_model_=footer_cwd_=footer_thread_=footer_sandbox_=false;
        for(const auto& item:items){
            if(item=="model") footer_model_=true; else if(item=="cwd"||item=="directory") footer_cwd_=true;
            else if(item=="thread"||item=="name") footer_thread_=true; else if(item=="sandbox"||item=="permissions") footer_sandbox_=true;
        }
        render();
    }

    int choose(const std::string& title, const std::vector<std::string>& options, size_t selected=0) {
        if (options.empty()) return -1;
        overlay_active_=true; overlay_title_=title; overlay_options_=options;
        overlay_choice_=static_cast<int>(std::min(selected, options.size()-1)); render();
#if !defined(_WIN32)
        termios oldt{}; bool raw_ok=tcgetattr(STDIN_FILENO,&oldt)==0;
        if(raw_ok){ termios raw=oldt; raw.c_lflag&=static_cast<tcflag_t>(~(ICANON|ECHO)); raw.c_iflag&=static_cast<tcflag_t>(~(IXON|ICRNL)); tcsetattr(STDIN_FILENO,TCSANOW,&raw); }
#endif
        int result=-1;
        for(;;){
            int k=read_key();
#if !defined(_WIN32)
            if(tui_interrupt_requested) {result=-1;break;}
#endif
            if(k==27){ result=-1; break; }
            if(k==KEY_UP){ overlay_choice_=(overlay_choice_+static_cast<int>(options.size())-1)%static_cast<int>(options.size()); render(); continue; }
            if(k==KEY_DOWN){ overlay_choice_=(overlay_choice_+1)%static_cast<int>(options.size()); render(); continue; }
            if(k>='1' && k<='9'){ int n=k-'1'; if(n<static_cast<int>(options.size())){ result=n; break; } }
            if(k=='\r'||k=='\n'){ result=overlay_choice_; break; }
        }
#if !defined(_WIN32)
        if(raw_ok) tcsetattr(STDIN_FILENO,TCSANOW,&oldt);
#endif
        overlay_active_=false; overlay_options_.clear(); overlay_body_.clear(); render(); return result;
    }

    bool approval(const std::string& text) {
        overlay_body_=text;
        const int choice=choose("Would you like to run this command?", {"Yes, proceed", "No, cancel"}, 0);
        overlay_body_.clear();
        return choice==0;
    }

    void render(const std::string& footer = "") {
        status_ = footer;
        if (!footer.empty() && (footer.find("working") != std::string::npos || footer.find("running") != std::string::npos)) {
            if (!working_) { working_=true; working_started_=std::chrono::steady_clock::now(); }
        } else if (footer.empty()) {
            working_=false;
        }
        draw_diff(build_frame());
    }

    void append_assistant_delta(const std::string& delta) {
        if(delta.empty()) return;
        if(!assistant_stream_active_) {
            add_cell(CellKind::Assistant,"","");
            assistant_stream_index_=cells_.size()-1;
            assistant_stream_active_=true;
        }
        cells_[assistant_stream_index_].body+=delta;
        draw_diff(build_frame());
    }

    void finish_assistant_stream(const std::string& text) {
        if(assistant_stream_active_) cells_[assistant_stream_index_].body=text;
        else if(!text.empty()) add_cell(CellKind::Assistant,"",text);
        assistant_stream_active_=false;
        working_=false;
        status_.clear();
        draw_diff(build_frame());
    }

    void stream_assistant(const std::string& text) { finish_assistant_stream(text); }

    std::string read_line(std::vector<std::string>& history) {
#if defined(_WIN32)
        std::cout << "\x1b[?25h" << std::flush;
        std::string line; std::getline(std::cin,line); std::cout << "\x1b[?25l" << std::flush; return line;
#else
        termios oldt{};
        if (tcgetattr(STDIN_FILENO, &oldt) != 0) { std::string line; std::getline(std::cin,line); return line; }
        termios raw=oldt; raw.c_lflag &= static_cast<tcflag_t>(~(ICANON|ECHO)); raw.c_iflag &= static_cast<tcflag_t>(~(IXON|ICRNL));
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        std::string line; size_t cursor=0, hist=history.size(); bool popup=false;
        composer_=""; composer_cursor_=0; show_commands_=false; command_popup_selected_=0; working_=false; render();
        for (;;) {
            int k=read_key();
#if !defined(_WIN32)
            if(tui_interrupt_requested) {line.clear();break;}
#endif
            if(k==KEY_IGNORED) continue;
            if(k==KEY_PASTE) {
                // Treat the whole bracketed paste as text. In particular, its
                // newlines must not submit the prompt or select a slash item.
                line.insert(cursor,pasted_text_);
                cursor+=pasted_text_.size();
                pasted_text_.clear();
                popup=false;
                show_commands_=false;
                composer_=line;composer_cursor_=cursor;render();
                continue;
            }
            // Enter submits. Ctrl+J inserts a real newline. Enhanced terminals may
            // report Shift+Enter as KEY_NEWLINE as well.
            if (k=='\r') {
                auto matches = matching_slash_commands(line);
                if (!matches.empty()) {
                    command_popup_selected_ = std::min(command_popup_selected_, matches.size()-1);
                    line = matches[command_popup_selected_]->name;
                    cursor = line.size();
                    // A selected bare slash command is accepted immediately, matching Codex's popup behavior.
                    break;
                }
                if (!line.empty()) break; else continue;
            }
            if (k=='\n' || k==KEY_NEWLINE) { line.insert(cursor, 1, '\n'); ++cursor; }
            else if (k==3) { line.clear(); break; }
            else if (k==27) { if (popup) { popup=false; show_commands_=false; render(); continue; } line.clear(); break; }
            else if (k==127 || k==8) {
                if (cursor>0) { size_t prev=prev_utf8_boundary(line,cursor); line.erase(prev,cursor-prev); cursor=prev; }
            }
            else if (k==KEY_DELETE) {
                if (cursor<line.size()) { size_t next=next_utf8_boundary(line,cursor); line.erase(cursor,next-cursor); }
            }
            else if (k==KEY_LEFT) { if(cursor>0) cursor=prev_utf8_boundary(line,cursor); }
            else if (k==KEY_RIGHT) { if(cursor<line.size()) cursor=next_utf8_boundary(line,cursor); }
            else if (k==KEY_HOME) cursor=line_start_boundary(line,cursor);
            else if (k==KEY_END) cursor=line_end_boundary(line,cursor);
            else if (k==KEY_UP) {
                auto matches = matching_slash_commands(line);
                if (!matches.empty()) {
                    if (command_popup_selected_ == 0) command_popup_selected_ = matches.size()-1;
                    else --command_popup_selected_;
                } else {
                    const size_t text_width=current_composer_text_width();
                    if (!move_cursor_vertical(line,cursor,-1,text_width) && !history.empty()) {
                        if(hist>0)--hist; line=history[hist]; cursor=line.size();
                    }
                }
            }
            else if (k==KEY_DOWN) {
                auto matches = matching_slash_commands(line);
                if (!matches.empty()) {
                    command_popup_selected_ = (command_popup_selected_ + 1) % matches.size();
                } else {
                    const size_t text_width=current_composer_text_width();
                    if (!move_cursor_vertical(line,cursor,+1,text_width) && !history.empty()) {
                        if(hist<history.size())++hist; line=hist<history.size()?history[hist]:""; cursor=line.size();
                    }
                }
            }
            else if (k==KEY_PGUP) { scroll_offset_ += 4; }
            else if (k==KEY_PGDN) { scroll_offset_ = scroll_offset_>4?scroll_offset_-4:0; }
            else if (k==KEY_SCROLL_UP) { ++scroll_offset_; }
            else if (k==KEY_SCROLL_DOWN) { if(scroll_offset_) --scroll_offset_; }
            else if (k>=32 && k<=255) {
                // Terminal input is a byte stream.  For non-ASCII text (notably IME
                // commits on macOS) consume the entire UTF-8 code point before
                // mutating/re-rendering the composer. Rendering after each byte of a
                // multi-byte character produces transient invalid UTF-8 and visible
                // blank/flicker artifacts during composition commit.
                if (k < 0x80) {
                    line.insert(line.begin()+static_cast<long>(cursor), static_cast<char>(k));
                    ++cursor;
                } else {
                    const unsigned char first = static_cast<unsigned char>(k);
                    int expected = 1;
                    if ((first & 0xE0) == 0xC0) expected = 2;
                    else if ((first & 0xF0) == 0xE0) expected = 3;
                    else if ((first & 0xF8) == 0xF0) expected = 4;
                    std::string cp;
                    cp.push_back(static_cast<char>(first));
#if !defined(_WIN32)
                    for (int i=1; i<expected; ++i) {
                        unsigned char cont=0;
                        if (::read(STDIN_FILENO,&cont,1)!=1) break;
                        if ((cont & 0xC0) != 0x80) break;
                        cp.push_back(static_cast<char>(cont));
                    }
#endif
                    line.insert(cursor, cp);
                    cursor += cp.size();
                }
            }
            // Derive popup visibility from the current composer contents on every key.
            // It is never allowed to survive as stale UI state.
            auto matches = matching_slash_commands(line);
            popup = !matches.empty();
            if (!popup) command_popup_selected_ = 0;
            else if (command_popup_selected_ >= matches.size()) command_popup_selected_ = matches.size()-1;
            show_commands_=popup; composer_=line; composer_cursor_=cursor; render();
        }
        tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
        composer_.clear(); composer_cursor_=0; show_commands_=false; render();
        return line;
#endif
    }

    void prompt() { working_=false; status_.clear(); render(); }

private:
    static constexpr int KEY_LEFT=1001, KEY_RIGHT=1002, KEY_UP=1003, KEY_DOWN=1004,
                         KEY_HOME=1005, KEY_END=1006, KEY_PGUP=1007, KEY_PGDN=1008,
                         KEY_DELETE=1009, KEY_NEWLINE=1010, KEY_PASTE=1011,
                         KEY_IGNORED=1012, KEY_SCROLL_UP=1013, KEY_SCROLL_DOWN=1014;

    std::string pasted_text_;

    int read_key() {
#if defined(_WIN32)
        return std::cin.get();
#else
        unsigned char c=0; if (::read(STDIN_FILENO,&c,1)!=1) return -1;
        if (c!=27) return c;
        unsigned char a=0; if(::read(STDIN_FILENO,&a,1)!=1) return 27;
        // Option/Alt+Enter is treated as newline on terminals that encode it as ESC CR/LF.
        if (a=='\r' || a=='\n') return KEY_NEWLINE;
        if(a!='[') return 27;
        std::string csi;
        for(int i=0;i<32;++i) {
            unsigned char b=0;
            if(::read(STDIN_FILENO,&b,1)!=1) return KEY_IGNORED;
            csi.push_back(static_cast<char>(b));
            if(b<0x40 || b>0x7e) continue;
            if(csi=="A")return KEY_UP; if(csi=="B")return KEY_DOWN;
            if(csi=="C")return KEY_RIGHT; if(csi=="D")return KEY_LEFT;
            if(csi=="H"||csi=="1~")return KEY_HOME;
            if(csi=="F"||csi=="4~")return KEY_END;
            if(csi=="3~")return KEY_DELETE;
            if(csi=="5~")return KEY_PGUP;
            if(csi=="6~")return KEY_PGDN;
            if(csi=="13;2u")return KEY_NEWLINE;
            if(csi.rfind("<64;",0)==0 && csi.back()=='M')return KEY_SCROLL_UP;
            if(csi.rfind("<65;",0)==0 && csi.back()=='M')return KEY_SCROLL_DOWN;
            if(csi!="200~")return KEY_IGNORED;
            pasted_text_.clear();
            const std::string end="\x1b[201~";
            std::string pending;
            for(;;) {
                unsigned char ch=0;
                if(::read(STDIN_FILENO,&ch,1)!=1) return KEY_IGNORED;
                pending.push_back(static_cast<char>(ch));
                if(pending==end) break;
                while(!pending.empty() && end.compare(0,pending.size(),pending)!=0) {
                    if(pasted_text_.size()<1000000) pasted_text_.push_back(pending.front());
                    pending.erase(0,1);
                }
            }
            std::string normalized;
            normalized.reserve(pasted_text_.size());
            for(size_t j=0;j<pasted_text_.size();++j) {
                char ch=pasted_text_[j];
                if(ch=='\r') {normalized+='\n';if(j+1<pasted_text_.size() && pasted_text_[j+1]=='\n') ++j;}
                else if(ch=='\n' || static_cast<unsigned char>(ch)>=32) normalized+=ch;
            }
            pasted_text_=std::move(normalized);
            return KEY_PASTE;
        }
        return KEY_IGNORED;
#endif
    }

    static size_t prev_utf8_boundary(const std::string& s, size_t pos) {
        if (pos==0) return 0; size_t p=std::min(pos,s.size())-1;
        while(p>0 && (static_cast<unsigned char>(s[p])&0xc0)==0x80) --p;
        return p;
    }
    static size_t next_utf8_boundary(const std::string& s, size_t pos) {
        if(pos>=s.size()) return s.size(); size_t p=pos+1;
        while(p<s.size() && (static_cast<unsigned char>(s[p])&0xc0)==0x80) ++p;
        return p;
    }
    static size_t line_start_boundary(const std::string& s, size_t pos) {
        pos=std::min(pos,s.size()); size_t p=s.rfind('\n',pos==0?0:pos-1); return p==std::string::npos?0:p+1;
    }
    static size_t line_end_boundary(const std::string& s, size_t pos) {
        size_t p=s.find('\n',std::min(pos,s.size())); return p==std::string::npos?s.size():p;
    }
    struct VisualBoundary { size_t byte=0,row=0,col=0; };
    static std::vector<VisualBoundary> visual_boundaries(const std::string& s, size_t width) {
        width=std::max<size_t>(1,width); std::vector<VisualBoundary> out; size_t row=0,col=0;
        out.push_back({0,0,0});
        for(size_t i=0;i<s.size();) {
            if(s[i]=='\n') { ++i; ++row; col=0; out.push_back({i,row,col}); continue; }
            size_t next=i+1; uint32_t cp=decode_utf8(s,i,next); size_t cw=static_cast<size_t>(std::max(0,codepoint_width(cp)));
            if(cw>0 && col>0 && col+cw>width) { ++row; col=0; out.push_back({i,row,col}); }
            col+=cw; i=next; out.push_back({i,row,col});
        }
        return out;
    }
    static bool move_cursor_vertical(const std::string& s, size_t& cursor, int delta, size_t width) {
        auto bs=visual_boundaries(s,width); if(bs.empty()) return false;
        size_t idx=0; for(size_t i=0;i<bs.size();++i) if(bs[i].byte<=cursor) idx=i; else break;
        const size_t row=bs[idx].row,col=bs[idx].col;
        if(delta<0 && row==0) return false;
        const size_t target=delta<0?row-1:row+1;
        bool found=false; size_t best=cursor,bestdist=static_cast<size_t>(-1);
        for(const auto& b:bs) if(b.row==target) { size_t d=b.col>col?b.col-col:col-b.col; if(!found||d<bestdist){found=true;best=b.byte;bestdist=d;} }
        if(!found) return false; cursor=best; return true;
    }
    size_t current_composer_text_width() const {
        const size_t cols=std::max<size_t>(1,terminal_columns()), left_margin=2;
        const size_t composer_width=cols>left_margin*2?cols-left_margin*2:cols;
        return composer_width>4?composer_width-4:1;
    }
    static bool ansi_sequence_at(const std::string& s, size_t i, size_t& end) {
        if (i >= s.size() || static_cast<unsigned char>(s[i]) != 0x1b) return false;
        if (i + 1 >= s.size()) { end = i + 1; return true; }
        if (s[i + 1] == '[') {
            size_t j = i + 2;
            while (j < s.size()) {
                unsigned char c = static_cast<unsigned char>(s[j++]);
                if (c >= 0x40 && c <= 0x7e) { end = j; return true; }
            }
            end = s.size(); return true;
        }
        if (s[i + 1] == ']') {
            size_t j = i + 2;
            while (j < s.size()) {
                if (s[j] == '\a') { end = j + 1; return true; }
                if (static_cast<unsigned char>(s[j]) == 0x1b && j + 1 < s.size() && s[j + 1] == '\\') {
                    end = j + 2; return true;
                }
                ++j;
            }
            end = s.size(); return true;
        }
        end = std::min(s.size(), i + 2); return true;
    }

    static uint32_t decode_utf8(const std::string& s, size_t i, size_t& next) {
        const unsigned char c0 = static_cast<unsigned char>(s[i]);
        if (c0 < 0x80) { next = i + 1; return c0; }
        auto cont=[&](size_t k){ return k < s.size() && (static_cast<unsigned char>(s[k]) & 0xc0) == 0x80; };
        if ((c0 & 0xe0) == 0xc0 && cont(i+1)) {
            next=i+2; return ((c0&0x1f)<<6) | (static_cast<unsigned char>(s[i+1])&0x3f);
        }
        if ((c0 & 0xf0) == 0xe0 && cont(i+1) && cont(i+2)) {
            next=i+3; return ((c0&0x0f)<<12) | ((static_cast<unsigned char>(s[i+1])&0x3f)<<6) | (static_cast<unsigned char>(s[i+2])&0x3f);
        }
        if ((c0 & 0xf8) == 0xf0 && cont(i+1) && cont(i+2) && cont(i+3)) {
            next=i+4; return ((c0&0x07)<<18) | ((static_cast<unsigned char>(s[i+1])&0x3f)<<12) |
                             ((static_cast<unsigned char>(s[i+2])&0x3f)<<6) | (static_cast<unsigned char>(s[i+3])&0x3f);
        }
        next=i+1; return 0xfffd;
    }

    static int codepoint_width(uint32_t cp) {
        if (cp == 0) return 0;
        if (cp < 0x20 || (cp >= 0x7f && cp < 0xa0)) return 0;
        if ((cp >= 0x0300 && cp <= 0x036f) || (cp >= 0x1ab0 && cp <= 0x1aff) ||
            (cp >= 0x1dc0 && cp <= 0x1dff) || (cp >= 0x20d0 && cp <= 0x20ff) ||
            (cp >= 0xfe00 && cp <= 0xfe0f) || (cp >= 0xfe20 && cp <= 0xfe2f) ||
            (cp >= 0xe0100 && cp <= 0xe01ef)) return 0;
        if ((cp >= 0x1100 && cp <= 0x115f) || cp == 0x2329 || cp == 0x232a ||
            (cp >= 0x2e80 && cp <= 0xa4cf && cp != 0x303f) ||
            (cp >= 0xac00 && cp <= 0xd7a3) || (cp >= 0xf900 && cp <= 0xfaff) ||
            (cp >= 0xfe10 && cp <= 0xfe19) || (cp >= 0xfe30 && cp <= 0xfe6f) ||
            (cp >= 0xff00 && cp <= 0xff60) || (cp >= 0xffe0 && cp <= 0xffe6) ||
            (cp >= 0x1f300 && cp <= 0x1faff) || (cp >= 0x20000 && cp <= 0x3fffd)) return 2;
        return 1;
    }

    static size_t display_width(const std::string& s) {
        size_t cols=0;
        for(size_t i=0;i<s.size();) {
            size_t end=0;
            if(ansi_sequence_at(s,i,end)) { i=end; continue; }
            size_t next=i+1; const uint32_t cp=decode_utf8(s,i,next);
            cols += static_cast<size_t>(std::max(0, codepoint_width(cp))); i=next;
        }
        return cols;
    }

    static std::string truncate_display(const std::string& s, size_t width) {
        std::string out; size_t cols=0; bool saw_ansi=false;
        for(size_t i=0;i<s.size();) {
            size_t end=0;
            if(ansi_sequence_at(s,i,end)) { out.append(s,i,end-i); saw_ansi=true; i=end; continue; }
            size_t next=i+1; const uint32_t cp=decode_utf8(s,i,next);
            const size_t cw=static_cast<size_t>(std::max(0, codepoint_width(cp)));
            if(cols+cw>width) break;
            out.append(s,i,next-i); cols+=cw; i=next;
        }
        if(saw_ansi && out.find("\x1b[")!=std::string::npos) out += "\x1b[0m";
        return out;
    }

    static std::string fit_display(const std::string& s, size_t width) {
        std::string out=truncate_display(s,width);
        const size_t used=display_width(out);
        if(used<width) out.append(width-used,' ');
        return out;
    }

    static std::vector<std::string> wrap(const std::string& text, size_t width, size_t max_lines=9999) {
        std::vector<std::string> out; if(width==0)return out; std::istringstream in(text); std::string line;
        while(std::getline(in,line) && out.size()<max_lines) {
            if(line.empty()){out.push_back("");continue;}
            while(display_width(line)>width && out.size()<max_lines) {
                std::string part=truncate_display(line,width);
                if(part.empty()) break;
                out.push_back(part);
                size_t consumed=0, cols=0;
                while(consumed<line.size()) {
                    size_t end=0;
                    if(ansi_sequence_at(line,consumed,end)) { consumed=end; continue; }
                    size_t next=consumed+1; uint32_t cp=decode_utf8(line,consumed,next);
                    size_t cw=static_cast<size_t>(std::max(0,codepoint_width(cp)));
                    if(cols+cw>width) break;
                    cols+=cw; consumed=next;
                }
                line.erase(0,consumed);
            }
            if(out.size()<max_lines && !line.empty())out.push_back(line);
        }
        return out;
    }
    static std::vector<std::string> wrap_words_display(const std::string& text, size_t width) {
        std::vector<std::string> out;
        if (width == 0) return out;
        std::istringstream in(text);
        std::string word, line;
        auto flush_line = [&]() {
            if (!line.empty()) { out.push_back(line); line.clear(); }
        };
        while (in >> word) {
            if (display_width(word) > width) {
                flush_line();
                auto pieces = wrap(word, width);
                out.insert(out.end(), pieces.begin(), pieces.end());
                continue;
            }
            if (line.empty()) {
                line = word;
            } else if (display_width(line) + 1 + display_width(word) <= width) {
                line += " " + word;
            } else {
                flush_line();
                line = word;
            }
        }
        flush_line();
        if (out.empty()) out.push_back("");
        return out;
    }
    static std::string markdown_inline(const std::string& source) {
        std::string out;
        bool bold=false,italic=false,code=false;
        auto reset=[&]() {
            out+="\x1b[0m";
            if(code) out+="\x1b[38;5;223;48;5;237m";
            else if(bold && italic) out+="\x1b[1;3m";
            else if(bold) out+="\x1b[1m";
            else if(italic) out+="\x1b[3m";
        };
        for(size_t i=0;i<source.size();) {
            if(source[i]=='\x1b' || (static_cast<unsigned char>(source[i])<32 && source[i]!='\t')) {++i;continue;}
            if(source[i]=='\\' && i+1<source.size()) {out+=source[i+1];i+=2;continue;}
            if(source[i]=='`') {code=!code;reset();++i;continue;}
            if(!code && source.compare(i,2,"**")==0) {bold=!bold;reset();i+=2;continue;}
            if(!code && source[i]=='*') {italic=!italic;reset();++i;continue;}
            if(!code && source[i]=='[') {
                const size_t close=source.find("](",i+1);
                const size_t end=close==std::string::npos?std::string::npos:source.find(')',close+2);
                if(end!=std::string::npos && end-close<300) {
                    out+="\x1b[4;36m"+source.substr(i+1,close-i-1)+"\x1b[0m";
                    reset();
                    const std::string url=source.substr(close+2,end-close-2);
                    if(!url.empty()) out+=" \x1b[2m("+url+")\x1b[0m";
                    reset();i=end+1;continue;
                }
            }
            out+=source[i++];
        }
        if(bold||italic||code) out+="\x1b[0m";
        return out;
    }

    static std::vector<std::string> table_cells(const std::string& line) {
        std::string s=trim(line);
        if(!s.empty() && s.front()=='|') s.erase(0,1);
        if(!s.empty() && s.back()=='|') s.pop_back();
        std::vector<std::string> cells;
        std::string cell;
        for(size_t i=0;i<s.size();++i) {
            if(s[i]=='\\' && i+1<s.size() && s[i+1]=='|') {cell+='|';++i;}
            else if(s[i]=='|') {cells.push_back(trim(cell));cell.clear();}
            else cell+=s[i];
        }
        cells.push_back(trim(cell));
        return cells;
    }
    static bool table_separator(const std::string& line, size_t columns) {
        const auto cells=table_cells(line);
        if(cells.size()!=columns) return false;
        for(const auto& cell:cells) {
            size_t dashes=0;
            for(char c:cell) {
                if(c=='-') ++dashes;
                else if(c!=':' && c!=' ') return false;
            }
            if(dashes<3) return false;
        }
        return true;
    }
    static std::vector<std::string> markdown_lines(const std::string& text, size_t width) {
        std::vector<std::string> source;
        std::istringstream input(text);std::string line;
        while(std::getline(input,line)) {
            std::string safe;
            for(unsigned char ch:line) if(ch>=32 || ch=='\t') safe+=static_cast<char>(ch);
            source.push_back(std::move(safe));
        }
        if(source.empty()) source.push_back("");
        std::vector<std::string> out;
        auto append=[&](const std::string& styled,size_t indent=0) {
            const size_t available=width>indent?width-indent:1;
            auto parts=wrap(styled,available);
            if(parts.empty()) {out.push_back("");return;}
            for(auto& part:parts) out.push_back(std::string(indent,' ')+part);
        };
        bool fenced=false;
        for(size_t i=0;i<source.size();) {
            std::string raw=source[i],t=trim(raw);
            if(t.rfind("```",0)==0 || t.rfind("~~~",0)==0) {
                fenced=!fenced;
                if(fenced) {
                    if(!out.empty() && !out.back().empty()) out.push_back("");
                    const std::string lang=trim(t.substr(3));
                    if(!lang.empty()) append("\x1b[2m  "+lang+"\x1b[0m");
                } else out.push_back("");
                ++i;continue;
            }
            if(fenced) {
                append("\x1b[2m│ \x1b[0m\x1b[38;5;223m"+raw+"\x1b[0m",2);
                ++i;continue;
            }
            if(i+1<source.size() && raw.find('|')!=std::string::npos) {
                auto head=table_cells(raw);
                if(head.size()>=2 && head.size()<=12 && table_separator(source[i+1],head.size())) {
                    size_t end=i+2;
                    std::vector<std::vector<std::string>> rows;
                    while(end<source.size() && source[end].find('|')!=std::string::npos) {
                        auto cells=table_cells(source[end]);
                        if(cells.size()!=head.size()) break;
                        rows.push_back(std::move(cells));++end;
                    }
                    std::vector<size_t> widths(head.size(),1);
                    for(size_t c=0;c<head.size();++c) widths[c]=std::max(widths[c],display_width(markdown_inline(head[c])));
                    for(const auto& row:rows) for(size_t c=0;c<row.size();++c)
                        widths[c]=std::max(widths[c],display_width(markdown_inline(row[c])));
                    size_t total=1;for(size_t w:widths) total+=w+3;
                    const bool grid_possible=width>=1+7*head.size(); // four text columns per cell
                    if(grid_possible && total>width) {
                        const auto natural=widths;
                        const size_t budget=width-1-3*head.size();
                        size_t used=0;
                        for(size_t c=0;c<widths.size();++c) {widths[c]=std::min<size_t>(natural[c],4);used+=widths[c];}
                        while(used<budget) {
                            size_t best=widths.size();double score=-1;
                            for(size_t c=0;c<widths.size();++c) if(widths[c]<natural[c]) {
                                const double demand=static_cast<double>(natural[c])/static_cast<double>(widths[c]+1);
                                if(demand>score) {score=demand;best=c;}
                            }
                            if(best==widths.size()) break;
                            ++widths[best];++used;
                        }
                    }
                    if(grid_possible) {
                        auto border=[&](const std::string& left,const std::string& join,const std::string& right) {
                            std::string s=left;
                            for(size_t c=0;c<widths.size();++c) {
                                for(size_t k=0;k<widths[c]+2;++k) s+="─";
                                s+=c+1<widths.size()?join:right;
                            }
                            return "\x1b[2m"+s+"\x1b[0m";
                        };
                        auto row_lines=[&](const std::vector<std::string>& cells,bool header) {
                            std::vector<std::vector<std::string>> pieces(cells.size());size_t height=1;
                            for(size_t c=0;c<cells.size();++c) {
                                pieces[c]=wrap(markdown_inline(cells[c]),widths[c]);
                                height=std::max(height,pieces[c].size());
                            }
                            for(size_t r=0;r<height;++r) {
                                std::string s="\x1b[2m│\x1b[0m";
                                for(size_t c=0;c<cells.size();++c) {
                                    std::string part=r<pieces[c].size()?pieces[c][r]:"";
                                    if(header) part="\x1b[1;36m"+part+"\x1b[0m";
                                    s+=" "+fit_display(part,widths[c])+" \x1b[2m│\x1b[0m";
                                }
                                out.push_back(std::move(s));
                            }
                        };
                        out.push_back(border("┌","┬","┐"));row_lines(head,true);
                        out.push_back(border("├","┼","┤"));
                        for(const auto& row:rows) row_lines(row,false);
                        out.push_back(border("└","┴","┘"));
                    } else {
                        // Extremely narrow terminals cannot show four columns per cell.
                        for(size_t r=0;r<rows.size();++r) {
                            if(r) out.push_back("");
                            for(size_t c=0;c<head.size();++c)
                                append("\x1b[1;36m"+head[c]+"\x1b[0m: "+markdown_inline(rows[r][c]),2);
                        }
                    }
                    i=end;continue;
                }
            }
            if(t.empty()) {out.push_back("");++i;continue;}
            size_t hashes=0;while(hashes<t.size() && t[hashes]=='#')++hashes;
            if(hashes>0 && hashes<=6 && hashes<t.size() && t[hashes]==' ') {
                if(!out.empty() && !out.back().empty()) out.push_back("");
                append("\x1b[1;38;5;81m"+markdown_inline(trim(t.substr(hashes+1)))+"\x1b[0m");
            } else if(t.rfind("> ",0)==0) {
                append("\x1b[2m│ \x1b[0m"+markdown_inline(t.substr(2)),2);
            } else if(t.rfind("- ",0)==0 || t.rfind("* ",0)==0) {
                append("\x1b[38;5;81m• \x1b[0m"+markdown_inline(t.substr(2)),2);
            } else {
                append(markdown_inline(raw));
            }
            ++i;
        }
        return out;
    }
    static std::string compact_args(const std::string& args) {
        std::string s=args;
        for(char& c:s) if(c=='\n'||c=='\r'||c=='\t') c=' ';
        if(s.size()>160) s=s.substr(0,157)+"...";
        return s;
    }
    static std::string style_patch_line(const std::string& line) {
        if(line.empty()) return line;
        if(line[0]=='+') return "\x1b[32m"+line+"\x1b[0m";
        if(line[0]=='-') return "\x1b[31m"+line+"\x1b[0m";
        if(line.rfind("@@",0)==0) return "\x1b[36m"+line+"\x1b[0m";
        return line;
    }

    void add_cell(CellKind kind, const std::string& title, const std::string& body) {
        cells_.push_back({kind,title,body}); if(cells_.size()>500)cells_.erase(cells_.begin(),cells_.begin()+100); scroll_offset_=0;
    }

    std::vector<std::string> cell_lines(const Cell& c, size_t width) const {
        std::vector<std::string> out;
        const size_t body_width = width > 4 ? width - 4 : width;
        auto body=wrap(c.body, body_width, c.kind==CellKind::Output?10:80);
        switch(c.kind) {
            case CellKind::User: {
                if(body.empty()) body.push_back("");
                out.push_back("");
                for(size_t i=0;i<body.size();++i) out.push_back((i==0?"\x1b[38;5;81;1m› \x1b[0m":"  ")+body[i]);
                break;
            }
            case CellKind::Assistant: {
                body=markdown_lines(c.body,body_width);
                out.push_back("");
                out.push_back("\x1b[38;5;81;1mCodex\x1b[0m");
                for(auto& l:body) out.push_back("  "+l);
                out.push_back("");
                break;
            }
            case CellKind::Tool: {
                std::string head="\x1b[38;5;214m• \x1b[0m\x1b[38;5;223;1m"+c.title+"\x1b[0m";
                if(!c.body.empty()) head += " \x1b[2m· "+c.body+"\x1b[0m";
                out.push_back(head);
                break;
            }
            case CellKind::Output: {
                for(size_t i=0;i<body.size();++i) out.push_back(std::string(i==0?"\x1b[2m  └ \x1b[0m":"\x1b[2m    \x1b[0m")+"\x1b[2m"+body[i]+"\x1b[0m");
                break;
            }
            case CellKind::Patch: {
                out.push_back("\x1b[38;5;214m• \x1b[0m\x1b[1m"+c.title+"\x1b[0m");
                for(auto& l:body) out.push_back("\x1b[2m  └ \x1b[0m"+style_patch_line(l));
                break;
            }
            case CellKind::Error: {
                out.push_back("\x1b[31;1m✘ "+c.title+"\x1b[0m");
                for(auto& l:body) out.push_back("\x1b[2m  └ \x1b[0m"+l);
                break;
            }
            case CellKind::System: {
                for(auto& l:body) out.push_back("\x1b[2m"+l+"\x1b[0m");
                break;
            }
            case CellKind::SessionSummary: {
                const size_t card_w = std::min<size_t>(width > 4 ? width - 2 : width, 64);
                std::string horiz;
                for(size_t i=0;i<(card_w>2?card_w-2:0);++i) horiz += "─";
                const std::string top = "┌" + horiz + "┐";
                const std::string bot = "└" + horiz + "┘";
                auto fit=[&](const std::string& x){ const size_t inner=card_w>4?card_w-4:0; return "│ "+fit_display(x,inner)+" │"; };
                out.push_back("\x1b[38;5;81m"+top+"\x1b[0m");
                out.push_back("\x1b[38;5;81m"+fit("›_  " + c.title)+"\x1b[0m");
                out.push_back(fit(""));
                out.push_back(fit("model:      " + model_));
                out.push_back(fit("directory:  " + root_));
                out.push_back("\x1b[38;5;81m"+bot+"\x1b[0m");
                out.push_back("");
                break;
            }
            case CellKind::Notice: {
                if(body.empty()) break;
                out.push_back(body.front());
                for(size_t i=1;i<body.size();++i) out.push_back("  "+body[i]);
                out.push_back("");
                break;
            }
        }
        return out;
    }

    std::vector<std::string> content_lines(size_t width, size_t budget) const {
        std::vector<std::string> lines;
        for(const auto& c:cells_) {
            auto chunk=cell_lines(c,width);
            lines.insert(lines.end(),chunk.begin(),chunk.end());
        }
        const size_t max_offset=lines.size()>budget?lines.size()-budget:0;
        size_t end=lines.size()-std::min(scroll_offset_,max_offset);
        size_t begin=end>budget?end-budget:0;
        return std::vector<std::string>(lines.begin()+static_cast<long>(begin), lines.begin()+static_cast<long>(end));
    }

    std::string status_line() const {
        if (working_) {
            const auto elapsed=std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now()-working_started_).count();
            std::ostringstream ss; ss << "\x1b[38;5;214m• \x1b[0mWorking (" << elapsed << "s • esc to interrupt)";
            return ss.str();
        }
        if(!status_.empty()) return "\x1b[2m"+status_+"\x1b[0m";
        return {};
    }

    std::string footer_line() const {
        std::string cwd=root_;
        const char* home=std::getenv("HOME");
        if(home) {
            std::string h(home);
            if(cwd.rfind(h,0)==0) cwd="~"+cwd.substr(h.size());
        }
        std::vector<std::string> parts;
        if(footer_model_) {
            std::string m="\x1b[38;5;223m"+model_;
            if(!effort_.empty()) m+=" "+effort_;
            m+="\x1b[0m"; parts.push_back(std::move(m));
        }
        if(footer_thread_ && !thread_name_.empty()) parts.push_back("\x1b[2m"+thread_name_+"\x1b[0m");
        if(footer_sandbox_) parts.push_back("\x1b[2m"+sandbox_+"\x1b[0m");
        if(footer_cwd_) parts.push_back("\x1b[38;5;151m"+cwd+"\x1b[0m");
        std::ostringstream ss;
        for(size_t i=0;i<parts.size();++i){ if(i) ss<<"\x1b[2m · \x1b[0m"; ss<<parts[i]; }
        return ss.str();
    }

    std::vector<std::string> build_frame() const {
        const size_t cols=std::max<size_t>(1,terminal_columns()), rows=std::max<size_t>(1,terminal_rows());
        std::vector<std::string> f(rows,"");
        const bool has_status = working_ || !status_.empty();
        const size_t left_margin=2;
        const size_t composer_width = cols > left_margin * 2 ? cols - left_margin * 2 : cols;
        const size_t text_width = composer_width > 4 ? composer_width - 4 : 1;
        const auto composer_all_lines = composer_.empty() ? std::vector<std::string>{std::string()} : wrap(composer_, text_width);
        const size_t wrapped_rows = std::max<size_t>(1, composer_all_lines.size());
        const size_t composer_h = std::min<size_t>(6, std::max<size_t>(3, wrapped_rows + 2));
        const size_t bottom_h = (has_status ? 1 : 0) + composer_h + 2;
        const size_t panel_width=cols>left_margin*2?cols-left_margin*2:cols;
        const auto approval_lines=overlay_active_?wrap(overlay_body_,panel_width>6?panel_width-6:panel_width,4):std::vector<std::string>{};
        const size_t panel_wanted=overlay_active_?4+approval_lines.size()+overlay_options_.size():0;
        const size_t panel_h=rows>bottom_h+1?std::min(panel_wanted,rows-bottom_h-1):0;
        const size_t content_h=rows>bottom_h+panel_h?rows-bottom_h-panel_h:0;
        auto content=content_lines(cols>left_margin*2?cols-left_margin*2:cols,content_h);
        size_t y=0;
        for(auto& l:content) if(y<content_h) f[y++]=std::string(left_margin,' ')+l;

        const size_t base=rows-bottom_h;
        if(has_status) f[base]=std::string(left_margin,' ')+status_line();

        const size_t composer_row=base+(has_status?1:0);
        const size_t footer_row=composer_row+composer_h;
        const size_t hint_row=footer_row+1;
        const std::string placeholder="Use /help to list available commands";
        std::vector<std::string> composer_lines = composer_.empty() ? std::vector<std::string>{placeholder} : wrap(composer_, text_width);
        if(composer_lines.empty()) composer_lines.push_back("");
        auto vb=visual_boundaries(composer_,text_width); size_t cursor_visual_row=0;
        for(const auto& b:vb) if(b.byte<=std::min(composer_cursor_,composer_.size())) cursor_visual_row=b.row; else break;
        const size_t inner_rows=composer_h-2;
        const size_t composer_scroll=cursor_visual_row>=inner_rows?cursor_visual_row-inner_rows+1:0;
        for(size_t r=0;r<composer_h;++r) {
            std::string plain;
            if(r>=1 && r<=inner_rows) {
                const size_t li=composer_scroll+(r-1);
                if(li<composer_lines.size()) plain=(li==0?"› ":"  ")+composer_lines[li];
            }
            plain=fit_display(plain, composer_width);
            const std::string fg = composer_.empty() ? "\x1b[38;5;245m" : "\x1b[97m";
            f[composer_row+r]=std::string(left_margin,' ')+"\x1b[48;5;237m"+fg+plain+"\x1b[0m";
        }
        f[footer_row]=std::string(left_margin+2,' ')+footer_line();
        f[hint_row]=std::string(left_margin+2,' ')+"\x1b[2menter to send · ctrl+j newline · ↑↓ history/navigation\x1b[0m";

        {
            // Derive popup contents from the composer on every frame. Each command item can
            // occupy multiple terminal rows because its description may wrap. Layout and
            // scrolling therefore operate in visual rows, not in command-count units.
            auto matches = matching_slash_commands(composer_);
            if (!matches.empty()) {
                const size_t selected = std::min(command_popup_selected_, matches.size()-1);
                const size_t popup_width = composer_width;
                const size_t prefix_width = 2; // "› " / "  "
                size_t command_col = 0;
                for (const auto* command : matches)
                    command_col = std::max(command_col, display_width(command->name));
                // Keep enough room for useful description text even with unusually long names.
                if (popup_width > 24)
                    command_col = std::min(command_col, popup_width / 3);
                const size_t gap_width = 2;
                const size_t desc_width = popup_width > prefix_width + command_col + gap_width
                    ? popup_width - prefix_width - command_col - gap_width
                    : 1;

                struct PopupItemLines { std::vector<std::string> lines; };
                std::vector<PopupItemLines> rendered;
                rendered.reserve(matches.size());
                for (size_t idx=0; idx<matches.size(); ++idx) {
                    const auto* command = matches[idx];
                    const bool is_selected = idx == selected;
                    auto desc_lines = wrap_words_display(command->description, desc_width);
                    if (desc_lines.empty()) desc_lines.push_back("");
                    PopupItemLines item;
                    item.lines.reserve(desc_lines.size());
                    for (size_t j=0; j<desc_lines.size(); ++j) {
                        const std::string marker = (j==0 && is_selected) ? "\x1b[96m› " : "  ";
                        const std::string name_style = is_selected ? "\x1b[96m\x1b[1m" : "\x1b[97m";
                        const std::string desc_style = is_selected ? "\x1b[96m" : "\x1b[2m";
                        std::string line = marker;
                        if (j==0) line += name_style + fit_display(command->name, command_col) + "\x1b[0m";
                        else line += std::string(command_col, ' ');
                        line += std::string(gap_width, ' ') + desc_style + desc_lines[j] + "\x1b[0m";
                        item.lines.push_back(std::move(line));
                    }
                    rendered.push_back(std::move(item));
                }

                // Reserve at most 12 visual rows above the composer. Pick the earliest item
                // that still allows the selected item to remain fully visible.
                const size_t max_popup_rows = std::min<size_t>(12, composer_row);
                size_t first = selected;
                size_t used = rendered[selected].lines.size();
                while (first > 0 && used + rendered[first-1].lines.size() <= max_popup_rows) {
                    --first;
                    used += rendered[first].lines.size();
                }
                size_t last = selected + 1;
                while (last < rendered.size() && used + rendered[last].lines.size() <= max_popup_rows) {
                    used += rendered[last].lines.size();
                    ++last;
                }
                // If there is still space after filling downward, pull more items from above.
                while (first > 0 && used + rendered[first-1].lines.size() <= max_popup_rows) {
                    --first;
                    used += rendered[first].lines.size();
                }

                const size_t py = composer_row > used ? composer_row - used : 0;
                size_t row = py;
                for (size_t idx=first; idx<last && row<composer_row; ++idx) {
                    for (const auto& line : rendered[idx].lines) {
                        if (row >= composer_row) break;
                        f[row++] = std::string(left_margin,' ') + truncate_display(line, popup_width);
                    }
                }
            }
        }

        if(overlay_active_ && panel_h>0) {
            const size_t oy=content_h;
            auto put=[&](size_t row,const std::string& txt){
                if(row>=panel_h) return;
                f[oy+row]=std::string(left_margin,' ')+"\x1b[48;5;236m"+
                    fit_display("  "+txt,panel_width)+"\x1b[0m";
            };
            for(size_t row=0;row<panel_h;++row) put(row,"");
            put(0,"\x1b[38;5;214;1m"+overlay_title_+"\x1b[0m");
            for(size_t i=0;i<approval_lines.size() && i+2<panel_h;++i) put(i+2,approval_lines[i]);
            const size_t options_y=panel_h>overlay_options_.size()+1?panel_h-overlay_options_.size()-1:1;
            const std::vector<std::string> defaults={"Yes, proceed","No, cancel"};
            const auto& opts=overlay_options_.empty()?defaults:overlay_options_;
            for(size_t i=0;i<opts.size() && options_y+i<panel_h;++i){
                const std::string line=(static_cast<int>(i)==overlay_choice_?"\x1b[1m› ":"  ")+std::to_string(i+1)+". "+opts[i]+"\x1b[0m";
                put(options_y+i,line);
            }
        }
        return f;
    }

    void draw_diff(const std::vector<std::string>& frame) {
        // A frame row must never be allowed to auto-wrap. Auto-wrap is destructive in a TUI:
        // a styled composer row that is even one column wider than the real terminal spills its
        // background into the footer/history row below. Clip every physical row to the *actual*
        // terminal width before diffing or drawing.
        const size_t cols=std::max<size_t>(1,terminal_columns());
        std::vector<std::string> clipped=frame;
        for(auto& line:clipped) line=truncate_display(line,cols);
        // Always reset SGR before erasing/redrawing a physical row.
        if(previous_frame_.size()!=clipped.size()) { std::cout<<"\x1b[0m\x1b[2J"; previous_frame_.assign(clipped.size(),""); }
        for(size_t i=0;i<clipped.size();++i) if(previous_frame_[i]!=clipped[i])
            std::cout<<"\x1b["<<(i+1)<<";1H\x1b[0m\x1b[2K"<<clipped[i]<<"\x1b[0m";
        previous_frame_=clipped;
        const size_t rows=clipped.size();
        const bool has_status = working_ || !status_.empty();
        const size_t left_margin=2;
        const size_t composer_width = cols > left_margin * 2 ? cols - left_margin * 2 : cols;
        const size_t text_width = composer_width > 4 ? composer_width - 4 : 1;
        const auto composer_all_lines = composer_.empty() ? std::vector<std::string>{std::string()} : wrap(composer_, text_width);
        const size_t wrapped_rows = std::max<size_t>(1, composer_all_lines.size());
        const size_t composer_h = std::min<size_t>(6, std::max<size_t>(3, wrapped_rows + 2));
        const size_t bottom_h = (has_status ? 1 : 0) + composer_h + 2;
        const size_t base=rows-bottom_h;
        const size_t composer_row=base+(has_status?1:0);
        auto vb=visual_boundaries(composer_,text_width); size_t cursor_row=0,cursor_col=0;
        for(const auto& b:vb) if(b.byte<=std::min(composer_cursor_,composer_.size())) {cursor_row=b.row;cursor_col=b.col;} else break;
        const size_t inner_rows=composer_h-2;
        const size_t composer_scroll=cursor_row>=inner_rows?cursor_row-inner_rows+1:0;
        const size_t screen_row=composer_row+1+(cursor_row-composer_scroll);
        const size_t screen_col=left_margin+3+cursor_col;
        std::cout<<"\x1b["<<(screen_row+1)<<";"<<screen_col<<"H\x1b[?25h"<<std::flush;
    }

    bool active_=false, overlay_active_=false, show_commands_=false, working_=false, assistant_stream_active_=false;
    size_t assistant_stream_index_=0;
    bool footer_model_=true, footer_cwd_=true, footer_thread_=true, footer_sandbox_=false;
    size_t command_popup_selected_=0;
    int overlay_choice_=0; size_t scroll_offset_=0, composer_cursor_=0;
    std::chrono::steady_clock::time_point working_started_=std::chrono::steady_clock::now();
    std::string root_,provider_,model_,effort_,sandbox_,thread_name_,status_,composer_,overlay_title_,overlay_body_;
    std::vector<std::string> overlay_options_;
    std::vector<Cell> cells_; std::vector<std::string> previous_frame_;
};

// ---------- CLI ----------

struct Options {
    fs::path root = fs::current_path();
    fs::path mcp_config;
    RuntimePolicy policy;
    bool json = false;
    bool list_sessions = false;
    bool self_test = false;
    bool interactive = false;
    UiMode ui_mode = UiMode::Auto;
    bool no_api_key = false;
    int max_steps = 64;
    ApiStyle api_style = ApiStyle::Responses;
    std::string provider = "codex";
    std::string base_url;
    std::string model;
    std::string reasoning_effort;
    std::string api_key_env;
    bool login = false;
    bool device_auth = false;
    std::optional<std::string> resume;
    std::vector<std::string> prompt;
    std::vector<fs::path> images;
};

static void print_help() {
    std::cout << R"HELP(codex-cpp - single-file C++ Codex-style coding agent

Usage:
  codex-cpp [options] [prompt...]
  codex-cpp -i [options]

Provider/API options:
  --provider codex|openai|deepseek|custom
                                default: codex; use openai explicitly for API Key billing
  --login                       sign in to ChatGPT with device authorization
  --device-auth                 alias for device authorization with --login
  --api responses|chat          protocol backend
  --base-url URL                provider root URL
  --model MODEL                 model name
  --reasoning EFFORT            low|medium|high|xhigh (Responses API)
  --api-key-env NAME            read key from this environment variable
  --no-api-key                  for trusted local endpoints such as LM Studio

UI:
  -i, --interactive             interactive mode
  --ui auto|tui|plain           auto uses TUI when VT/TTY is available, else plain

Agent options:
  --root DIR                    workspace (default: current directory)
  --resume SESSION_ID           resume a saved structured session
  --list-sessions               print known local session IDs
  --json                        echo rollout events as JSONL
  --auto                        never request approval
  --approval on-request|never
  --sandbox read-only|workspace-write|danger-full-access
  --read-only                   alias for --sandbox read-only
  --danger-full-access          alias; also implies --approval never
  --max-steps N                 agent step limit (default: 64)
  --mcp-config PATH             trusted MCP stdio server configuration JSON
  --image PATH                  attach an image to the next prompt (repeatable)
  --self-test                   run parser/router/session smoke tests without API
  -h, --help

Provider presets:
  codex     built-in ChatGPT login and Codex Responses backend
  openai    responses, https://api.openai.com, OPENAI_API_KEY
  deepseek  chat,      https://api.deepseek.com, DEEPSEEK_API_KEY (default model: deepseek-flash)
  custom    values supplied by --api/--base-url/--model/--api-key-env

Interactive commands:
  /help  /status  /sessions  /clear  /quit

Security note:
  Restricted shell commands require macOS sandbox-exec or a working Linux bwrap.
  If the OS sandbox is unavailable, shell is denied unless danger-full-access
  was explicitly selected. Network access from restricted shell is blocked.
  ChatGPT subscriptions use --provider codex and this program's own login.
  MCP servers execute with your user permissions when you configure them.
  Default MCP config: ~/.codex-cpp/mcp.json (if present).
  Example: {"mcpServers":{"demo":{"command":"python3","args":["/absolute/server.py"]}}}
)HELP";
}

static std::optional<Options> parse_options(int argc, char** argv, std::string& err) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") { print_help(); std::exit(0); }
        if (a == "--root" && i + 1 < argc) { o.root = argv[++i]; continue; }
        if (a == "--mcp-config" && i + 1 < argc) { o.mcp_config = argv[++i]; continue; }
        if (a == "--image" && i + 1 < argc) { o.images.emplace_back(argv[++i]); continue; }
        if (a == "--resume" && i + 1 < argc) { o.resume = argv[++i]; continue; }
        if (a == "--list-sessions") { o.list_sessions = true; continue; }
        if (a == "--json") { o.json = true; continue; }
        if (a == "--self-test") { o.self_test = true; continue; }
        if (a == "-i" || a == "--interactive") { o.interactive = true; continue; }
        if (a == "--ui" && i + 1 < argc) {
            std::string v = argv[++i];
            if (v == "auto") o.ui_mode = UiMode::Auto;
            else if (v == "tui") o.ui_mode = UiMode::Tui;
            else if (v == "plain") o.ui_mode = UiMode::Plain;
            else { err = "invalid --ui: " + v; return std::nullopt; }
            continue;
        }
        if (a == "--no-api-key") { o.no_api_key = true; continue; }
        if (a == "--login") { o.login = true; continue; }
        if (a == "--device-auth") { o.device_auth = true; continue; }
        if (a == "--provider" && i + 1 < argc) { o.provider = argv[++i]; continue; }
        if (a == "--api" && i + 1 < argc) {
            std::string v = argv[++i];
            if (v == "responses") o.api_style = ApiStyle::Responses;
            else if (v == "chat") o.api_style = ApiStyle::ChatCompletions;
            else { err = "invalid --api: " + v; return std::nullopt; }
            continue;
        }
        if (a == "--base-url" && i + 1 < argc) { o.base_url = argv[++i]; continue; }
        if (a == "--model" && i + 1 < argc) { o.model = argv[++i]; continue; }
        if (a == "--reasoning" && i + 1 < argc) { o.reasoning_effort = argv[++i]; continue; }
        if (a == "--api-key-env" && i + 1 < argc) { o.api_key_env = argv[++i]; continue; }
        if (a == "--auto") { o.policy.approval = ApprovalPolicy::Never; continue; }
        if (a == "--read-only") { o.policy.sandbox = SandboxMode::ReadOnly; continue; }
        if (a == "--danger-full-access") { o.policy.sandbox = SandboxMode::DangerFullAccess; o.policy.approval = ApprovalPolicy::Never; continue; }
        if (a == "--approval" && i + 1 < argc) {
            std::string v = argv[++i];
            if (v == "on-request") o.policy.approval = ApprovalPolicy::OnRequest;
            else if (v == "never") o.policy.approval = ApprovalPolicy::Never;
            else { err = "invalid --approval: " + v; return std::nullopt; }
            continue;
        }
        if (a == "--sandbox" && i + 1 < argc) {
            std::string v = argv[++i];
            if (v == "read-only") o.policy.sandbox = SandboxMode::ReadOnly;
            else if (v == "workspace-write") o.policy.sandbox = SandboxMode::WorkspaceWrite;
            else if (v == "danger-full-access") o.policy.sandbox = SandboxMode::DangerFullAccess;
            else { err = "invalid --sandbox: " + v; return std::nullopt; }
            continue;
        }
        if (a == "--max-steps" && i + 1 < argc) {
            try { o.max_steps = std::stoi(argv[++i]); }
            catch (...) { err = "invalid --max-steps"; return std::nullopt; }
            if (o.max_steps < 1 || o.max_steps > 512) { err = "--max-steps must be 1..512"; return std::nullopt; }
            continue;
        }
        o.prompt.push_back(a);
    }
    return o;
}

static bool self_test(const fs::path& root) {
    int failed = 0;
    std::error_code root_ec; fs::create_directories(root,root_ec);
    auto check = [&](bool ok, const char* what) {
        std::cout << (ok ? "PASS " : "FAIL ") << what << '\n'; if (!ok) ++failed;
    };

    const std::string sample = R"({"id":"resp_123","output":[{"type":"function_call","call_id":"call_7","name":"read_file","arguments":"{\"path\":\"a.txt\"}"}]})";
    std::string parse_err;
    auto parsed_response=parse_api_response(sample,false,parse_err);
    check(parsed_response && parsed_response->calls.size()==1 && parsed_response->calls[0].name=="read_file" &&
          parsed_response->calls[0].call_id=="call_7" && json_string_field(parsed_response->calls[0].arguments,"path")==std::optional<std::string>("a.txt"), "Responses function_call parser");
    check(parsed_response && parsed_response->id=="resp_123", "response id parser");
    const std::string chat_sample = R"({"id":"chat_1","choices":[{"message":{"role":"assistant","content":null,"tool_calls":[{"id":"tool_1","type":"function","function":{"name":"read_file","arguments":"{\"path\":\"a.txt\"}"}}]}}]})";
    auto parsed_chat=parse_api_response(chat_sample,true,parse_err);
    check(parsed_chat && parsed_chat->calls.size()==1 && parsed_chat->calls[0].call_id=="tool_1", "Chat Completions tool_call parser");
    const std::string many=R"({"id":"r","metadata":{"name":"decoy"},"output":[{"type":"function_call","name":"read_file","call_id":"a","arguments":"{\"path\":\"a\"}"},{"type":"function_call","name":"list_dir","call_id":"b","arguments":"{\"path\":\".\"}"}]})";
    auto parsed_many=parse_api_response(many,false,parse_err);
    check(parsed_many && parsed_many->calls.size()==2 && parsed_many->calls[1].name=="list_dir", "multiple scoped tool calls");
    ApiClient stream_client;stream_client.chatgpt_auth=true;
    auto streamed=stream_client.parse_codex_stream(
        "data: {\"type\":\"response.output_item.done\",\"output_index\":0,\"item\":{\"type\":\"function_call\",\"call_id\":\"c1\",\"name\":\"read_file\",\"arguments\":\"{\\\"path\\\":\\\"a.txt\\\"}\"}}\n\n"
        "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"r1\",\"output\":null}}\n\n",parse_err);
    check(streamed && streamed->calls.size()==1 && streamed->calls[0].call_id=="c1","Codex SSE tool result");
    check(!stream_client.parse_codex_stream("data: {\"type\":\"response.in_progress\"}\n",parse_err),
          "incomplete Codex SSE rejected");
    check(json_string_field(R"({"outer":{"path":"decoy"},"path":"right"})","path")==std::optional<std::string>("right"), "JSON fields stay in scope");

    ToolRouter router; router.add<ReadFileTool>(); router.add<ListDirTool>(); router.add<ReadSkillTool>(); router.add<WriteFileTool>(); router.add<ShellTool>(); router.add<ApplyPatchTool>();
    check(router.find("shell") != nullptr && router.find("nope") == nullptr, "ToolRouter lookup");
    check(router.specs_json().find("\"read_file\"") != std::string::npos, "tool schema generation");
    check(router.chat_specs_json().find("\"function\"") != std::string::npos, "chat tool schema generation");

    fs::path troot = root / ".codex_cpp" / "selftest";
    std::error_code ec; fs::create_directories(troot, ec);
    std::string err; write_all(troot / "a.txt", "hello\n", err);
    RuntimePolicy p; p.sandbox = SandboxMode::WorkspaceWrite; p.approval = ApprovalPolicy::Never;
    Session dummy; dummy.root = troot; dummy.dir = troot; dummy.id = "selftest"; dummy.events_path = troot / "events.jsonl";
    FunctionCall read{"read_file", "c1", R"({"path":"a.txt"})"};
    auto rr = router.invoke(read, troot, p, dummy);
    check(rr.exit_code == 0 && rr.output == "hello\n", "read_file runtime");
    FunctionCall wr{"write_file", "c2", R"({"path":"b.txt","content":"world\n"})"};
    auto rw = router.invoke(wr, troot, p, dummy);
    check(rw.exit_code == 0 && slurp(troot / "b.txt") == "world\n", "write_file runtime");
    FunctionCall escape{"write_file", "c3", R"({"path":"../outside.txt","content":"no"})"};
    check(router.invoke(escape,troot,p,dummy).exit_code!=0, "workspace file path guard");
    FunctionCall shell{"shell", "c4", R"({"command":"touch denied.txt"})"};
    const auto sandboxed=restricted_shell_command(troot,"true",p.sandbox);
    auto sandbox_shell=router.invoke(shell,troot,p,dummy);
    check((sandboxed ? sandbox_shell.exit_code==0 && fs::exists(troot/"denied.txt")
                     : sandbox_shell.exit_code==126 && !fs::exists(troot/"denied.txt")),
          "automatic shell runs only under an OS sandbox");
    if(sandboxed) {
        FunctionCall escape_shell{"shell","c5",R"({"command":"touch ../shell-outside.txt"})"};
        check(router.invoke(escape_shell,troot,p,dummy).exit_code!=0 &&
              !fs::exists(troot.parent_path()/"shell-outside.txt"),"sandbox rejects writes outside root");
    }
    RuntimePolicy plan=p; plan.sandbox=SandboxMode::ReadOnly;
    check(router.invoke(wr,troot,plan,dummy).exit_code==126, "read-only execution guard");
    if(restricted_shell_command(troot,"true",SandboxMode::ReadOnly)) {
        FunctionCall inspect{"shell","c6",R"({"command":"printf inspection"})"};
        auto inspected=router.invoke(inspect,troot,plan,dummy);
        check(inspected.exit_code==0 && inspected.output.find("inspection")!=std::string::npos,
              "read-only shell can inspect");
        FunctionCall cannot_write{"shell","c7",R"({"command":"touch read-only-denied.txt"})"};
        check(router.invoke(cannot_write,troot,plan,dummy).exit_code!=0 &&
              !fs::exists(troot/"read-only-denied.txt"),"read-only shell denies writes");
    }
    RuntimePolicy unrestricted=p;unrestricted.sandbox=SandboxMode::DangerFullAccess;
    FunctionCall cwd_shell{"shell","c8",R"({"command":"pwd"})"};
    check(router.invoke(cwd_shell,troot,unrestricted,dummy).exit_code==0,
          "explicit unrestricted shell still runs");
    fs::path guard_file=troot.parent_path()/"hardlink-guard.txt";
    write_all(guard_file,"guard",err);
    fs::path alias=troot/"hardlink-guard.txt";
    fs::create_hard_link(guard_file,alias,ec);
    if(!ec) {
        FunctionCall attack{"shell","hardlink",R"({"command":"printf escaped > hardlink-guard.txt"})"};
        auto denied=router.invoke(attack,troot,p,dummy);
        check(denied.exit_code==126 && denied.output.find("external hardlink alias")!=std::string::npos &&
              slurp(guard_file)=="guard","external hardlink shell refused before execution");
        FunctionCall direct_write{"write_file","hardlink-write",R"({"path":"hardlink-guard.txt","content":"escaped"})"};
        check(router.invoke(direct_write,troot,p,dummy).exit_code==126 && slurp(guard_file)=="guard",
              "hardlinked write_file target refused");
        FunctionCall patch{"apply_patch","hardlink-patch",R"({"patch":""})"};
        check(router.invoke(patch,troot,p,dummy).exit_code==126 && slurp(guard_file)=="guard",
              "hardlinked apply_patch workspace refused");
        fs::remove(alias,ec);
    } else check(false,"create hardlink test fixture");
    fs::remove(guard_file,ec);

    Session s; check(init_session(s, troot, false, std::nullopt, err), "session creation");
    s.append_transcript("abc");
    const std::string u="{\"role\":\"user\",\"content\":\"hello\"}";
    const std::string a="{\"role\":\"assistant\",\"content\":\"world\"}";
    check(s.record_item(u) && s.record_item(a),"structured item journal append");
    Session s2; check(init_session(s2, troot, false, s.id, err) && s2.transcript == "abc" &&
                      s2.items==std::vector<std::string>({u,a}), "structured session resume");
    check(!init_session(s2,troot,false,std::string("../escape"),err),"invalid session ID rejected");
    check(s.replace_items({u}) && init_session(s2,troot,false,s.id,err) && s2.items.size()==1,
          "atomic item journal replacement");
    check(matching_slash_commands("").empty() && matching_slash_commands("hello /model").empty(), "slash popup plain-text guard");
    auto slash_model = matching_slash_commands("/mo");
    check(!slash_model.empty() && std::string(slash_model.front()->name) == "/model", "slash popup prefix matching");
    check(matching_slash_commands("/model high").empty(), "slash popup closes for arguments");

    write_all(troot / "AGENTS.md", "Project rule: run focused tests.\n", err);
    write_all(troot / ".agents" / "skills" / "demo" / "SKILL.md",
              "---\nname: demo\ndescription: Small test skill\n---\nInstructions.\n",err);
    check(skills_manifest(troot).find("demo: Small test skill")!=std::string::npos,
          "project skill discovery");
    check(router.invoke({"read_skill","c6",R"({"name":"demo"})"},troot,p,dummy).output.find("Instructions.")!=std::string::npos,
          "skill read by name");
    ApiClient prompt_api; prompt_api.model = "test-model"; prompt_api.style = ApiStyle::Responses;
    PromptBuilder prompt_builder{troot, p, prompt_api, false, "concise", "finish the test"};
    PromptBundle prompt_bundle = prompt_builder.build();
    check(prompt_bundle.developer_instructions.find("Sandbox and approvals") != std::string::npos &&
          prompt_bundle.developer_instructions.find("test-model") != std::string::npos,
          "prompt developer layers");
    check(prompt_bundle.contextual_user_prefix.find("Project rule: run focused tests.") != std::string::npos &&
          prompt_bundle.contextual_user_prefix.find("<environment_context>") != std::string::npos &&
          prompt_bundle.contextual_user_prefix.find(troot.string()) != std::string::npos,
          "prompt AGENTS and environment context");
    RuntimePolicy ro = p; ro.sandbox = SandboxMode::ReadOnly; ro.approval = ApprovalPolicy::OnRequest;
    PromptBuilder ro_builder{troot, ro, prompt_api, false, "", ""};
    check(ro_builder.build().developer_instructions.find("workspace is read-only") != std::string::npos,
          "prompt read-only permissions");

    std::cout << (failed ? "SELF-TEST FAILED\n" : "SELF-TEST PASSED\n");
    return failed == 0;
}

static std::string join_json_array(const std::vector<std::string>& xs) {
    std::ostringstream o; o << '[';
    for (size_t i = 0; i < xs.size(); ++i) { if (i) o << ','; o << xs[i]; }
    o << ']'; return o.str();
}

static std::vector<std::string> chat_history(const Session& session,
                                              const std::string& instructions) {
    std::vector<std::string> messages={"{\"role\":\"system\",\"content\":\""+json_escape(instructions)+"\"}"};
    std::vector<std::string> calls;
    auto flush_calls=[&]() {
        if(!calls.empty()) {
            messages.push_back("{\"role\":\"assistant\",\"content\":null,\"tool_calls\":"+join_json_array(calls)+"}");
            calls.clear();
        }
    };
    for(const auto& item:session.items) {
        auto j=parse_json(item); if(!j) continue;
        const auto type=at(&*j,"type"),role=at(&*j,"role");
        if(type && type->str()=="function_call") {
            const auto name=at(&*j,"name"),id=at(&*j,"call_id"),args=at(&*j,"arguments");
            if(name&&id&&args) calls.push_back("{\"id\":\""+json_escape(id->str())+
                "\",\"type\":\"function\",\"function\":{\"name\":\""+json_escape(name->str())+
                "\",\"arguments\":\""+json_escape(args->str())+"\"}}");
        } else if(type && type->str()=="function_call_output") {
            flush_calls();const auto id=at(&*j,"call_id"),out=at(&*j,"output");
            if(id&&out) messages.push_back("{\"role\":\"tool\",\"tool_call_id\":\""+
                json_escape(id->str())+"\",\"content\":\""+json_escape(out->str())+"\"}");
        } else if(role && (role->str()=="user" || role->str()=="assistant")) {
            flush_calls();
            auto content=j->get("content");
            if(role->str()=="user" && content && content->kind==Json::Array) {
                std::vector<std::string> parts;
                for(const auto& part:content->items) {
                    const auto type=part.get("type"),value=part.get("text"),url=part.get("image_url");
                    if(type && type->str()=="input_text" && value)
                        parts.push_back("{\"type\":\"text\",\"text\":\""+json_escape(value->str())+"\"}");
                    else if(type && type->str()=="input_image" && url)
                        parts.push_back("{\"type\":\"image_url\",\"image_url\":{\"url\":\""+json_escape(url->str())+"\"}}");
                }
                messages.push_back("{\"role\":\"user\",\"content\":"+join_json_array(parts)+"}");
            } else messages.push_back(item);
        }
    }
    flush_calls();return messages;
}

static bool compact_session(Session& session, const ApiClient& api, std::string& err) {
    if(session.items.empty()) return true;
    std::string summary;
    std::vector<std::string> recent_images;
    // Summarize every piece in order, so the earliest decisions are not silently
    // dropped when the history reaches the context budget.
    std::string chunk;
    auto summarize=[&]() {
        if(chunk.empty()) return true;
        auto response=api.call_text_only(
            "Summarize this coding session for continuation. Preserve user constraints, files changed, tests, unresolved work, and exact decisions. The first part is a prior summary; incorporate new events in order.",
            "Previous summary:\n"+summary+"\n\nNew events:\n"+chunk,err);
        if(!response || !response->text || trim(*response->text).empty()) {
            if(err.empty()) err="context compaction failed";
            return false;
        }
        summary=trim(*response->text);chunk.clear();return true;
    };
    for(const auto& item:session.items) {
        std::string summarized=item;
        if(auto doc=parse_json(item)) {
            if(auto content=doc->get("content");content && content->kind==Json::Array) {
                bool has_image=false;
                for(auto& part:doc->fields["content"].items) {
                    auto kind=part.get("type"),url=part.get("image_url");
                    if(kind && kind->str()=="input_image" && url) {
                        part.fields["image_url"].value="[image retained separately]";
                        has_image=true;
                    }
                }
                if(has_image) {
                    recent_images.push_back(item);
                    if(recent_images.size()>2) recent_images.erase(recent_images.begin());
                    summarized=json_dump(*doc);
                }
            }
        }
        if(chunk.size()+summarized.size()>100000 && !summarize()) return false;
        chunk+=summarized+'\n';
    }
    if(!summarize()) return false;
    const std::string compacted="{\"role\":\"user\",\"content\":\"[prior conversation summary]\\n"+
        json_escape(summary)+"\"}";
    std::vector<std::string> replacement={compacted};
    replacement.insert(replacement.end(),recent_images.begin(),recent_images.end());
    if(!session.replace_items(replacement)) {err="could not save compacted session";return false;}
    session.append_transcript("\n[context compacted]\n"+summary+"\n");
    return true;
}

static void print_plain_banner(const Options& opt, const ApiClient& api) {
    std::cout << "codex-cpp  plain terminal mode\n"
              << "workspace: " << opt.root << "\n"
              << "provider:  " << opt.provider << " / " << api_style_name(api.style) << " / " << api.model << "\n"
              << "sandbox:   " << sandbox_name(opt.policy.sandbox) << "\n"
              << "type /help for commands\n\n";
}

int main(int argc, char** argv) {
#if !defined(_WIN32)
    ::signal(SIGPIPE,SIG_IGN); // a terminated MCP stdio peer must return an error
#endif
    std::string err;
    auto parsed = parse_options(argc, argv, err);
    if (!parsed) { std::cerr << err << '\n'; return 2; }
    Options opt = *parsed;

    // Provider presets are deliberately small. Any OpenAI-compatible endpoint can
    // be configured explicitly with --provider custom.
    if (opt.provider == "codex") {
        if (opt.model.empty()) opt.model = "gpt-5.6-sol";
        opt.api_style = ApiStyle::Responses;
    } else if (opt.provider == "openai") {
        if (opt.base_url.empty()) opt.base_url = getenv_or("OPENAI_BASE_URL", "https://api.openai.com");
        if (opt.model.empty()) opt.model = getenv_or("OPENAI_MODEL", "gpt-5.6-sol");
        if (opt.api_key_env.empty()) opt.api_key_env = "OPENAI_API_KEY";
        // Explicit --api still wins only when provider=custom; OpenAI preset uses Responses.
        opt.api_style = ApiStyle::Responses;
    } else if (opt.provider == "deepseek") {
        if (opt.base_url.empty()) opt.base_url = getenv_or("DEEPSEEK_BASE_URL", "https://api.deepseek.com");
        if (opt.model.empty()) opt.model = getenv_or("DEEPSEEK_MODEL", "deepseek-flash");
        if (opt.api_key_env.empty()) opt.api_key_env = "DEEPSEEK_API_KEY";
        opt.api_style = ApiStyle::ChatCompletions;
    } else if (opt.provider == "custom") {
        if (opt.base_url.empty()) opt.base_url = getenv_or("CODEX_CPP_BASE_URL");
        if (opt.model.empty()) opt.model = getenv_or("CODEX_CPP_MODEL");
        if (opt.api_key_env.empty()) opt.api_key_env = getenv_or("CODEX_CPP_API_KEY_ENV", "OPENAI_API_KEY");
        if (opt.base_url.empty() || opt.model.empty()) {
            std::cerr << "custom provider requires --base-url and --model (or CODEX_CPP_* env vars)\n"; return 2;
        }
    } else {
        std::cerr << "invalid --provider: " << opt.provider << '\n'; return 2;
    }

    std::error_code ec;
    opt.root = fs::absolute(opt.root, ec);
    if (ec || !fs::exists(opt.root)) { std::cerr << "workspace does not exist: " << opt.root << '\n'; return 2; }
    if (opt.list_sessions) { list_sessions(opt.root); return 0; }
    if (opt.self_test) return self_test(opt.root) ? 0 : 1;
    if (opt.login) {
        if (opt.provider != "codex") { std::cerr << "--login requires --provider codex\n"; return 2; }
        return own_device_login(err) ? (std::cout<<"Signed in.\n",0) : (std::cerr<<err<<'\n',3);
    }

    Session session;
    if (!init_session(session, opt.root, opt.json, opt.resume, err)) { std::cerr << err << '\n'; return 2; }

    ToolRouter router;
    router.add<ReadFileTool>(); router.add<ListDirTool>(); router.add<ReadSkillTool>(); router.add<WriteFileTool>(); router.add<ShellTool>(); router.add<ApplyPatchTool>();
    std::vector<std::shared_ptr<McpServer>> mcp_servers;
    fs::path mcp_config=opt.mcp_config.empty()
        ? fs::path(getenv_or("CODEX_CPP_HOME",getenv_or("HOME")+"/.codex-cpp"))/"mcp.json"
        : opt.mcp_config;
    const std::string mcp_report=load_mcp_tools(mcp_config,opt.root,router,mcp_servers);
    if(!mcp_report.empty() && mcp_servers.empty() && fs::exists(mcp_config))
        std::cerr<<"[mcp] "<<mcp_report;

    ApiClient api;
    api.api_key = opt.no_api_key ? "" : getenv_or(opt.api_key_env.c_str());
    api.base_url = opt.base_url;
    api.model = opt.model;
    api.reasoning_effort = opt.reasoning_effort;
    api.style = opt.api_style;
    api.no_api_key = opt.no_api_key;
    api.chatgpt_auth = opt.provider == "codex";
    while (!api.base_url.empty() && api.base_url.back() == '/') api.base_url.pop_back();

    const bool interactive = opt.interactive || opt.prompt.empty();
    bool use_tui = false;
    if (interactive && !opt.json) {
        const bool vt = enable_and_detect_vt();
        if (opt.ui_mode == UiMode::Tui && !vt) {
            std::cerr << "[ui] TUI requested but this terminal has no usable VT support; falling back to plain mode.\n";
        }
        use_tui = (opt.ui_mode == UiMode::Tui && vt) || (opt.ui_mode == UiMode::Auto && vt);
    }
    std::optional<TuiRenderer> tui;
#if !defined(_WIN32)
    if(use_tui) {
        struct sigaction action{};
        action.sa_handler=request_tui_interrupt;
        sigemptyset(&action.sa_mask);
        action.sa_flags=0; // wake blocking terminal reads for orderly cleanup
        sigaction(SIGINT,&action,nullptr);
        sigaction(SIGTERM,&action,nullptr);
    }
#endif
    if (interactive && !opt.json && use_tui) {
        tui.emplace(opt.root, opt.provider, api.model, sandbox_name(opt.policy.sandbox));
        tui->set_model(api.model, api.reasoning_effort);
        tui->render(" /help  /status  /sessions  /clear  /quit");
    } else if (interactive && !opt.json) {
        print_plain_banner(opt, api);
    }

    std::string first_prompt;
    if (!opt.prompt.empty()) {
        for (size_t i = 0; i < opt.prompt.size(); ++i) { if (i) first_prompt += ' '; first_prompt += opt.prompt[i]; }
    }

    bool plan_mode = false;
    std::string goal;
    std::string personality;
    std::string last_assistant;
    std::vector<std::pair<std::string,std::string>> pending_mentions;
    std::vector<std::pair<std::string,std::string>> pending_images;
    if(opt.images.size()>4) {std::cerr<<"Maximum four images per turn.\n";return 2;}
    for(const auto& image:opt.images) {
        const fs::path file=image.is_absolute()?image:opt.root/image;
        auto url=image_data_url(file,err);
        if(!url) {std::cerr<<"image "<<file<<": "<<err<<'\n';return 2;}
        pending_images.emplace_back(file.string(),std::move(*url));
    }

    auto run_turn = [&](const std::string& raw_prompt, bool review_only = false) -> int {
        std::string user_prompt = trim(raw_prompt);
        if (user_prompt.empty()) return 0;
        if (!pending_mentions.empty()) {
            user_prompt += "\n\nMentioned file context:";
            for (const auto& [path, content] : pending_mentions)
                user_prompt += "\n\n--- " + path + " ---\n" + content;
            pending_mentions.clear();
        }
        if(session.context_bytes()>350000) {
            if(tui) tui->render("compacting context...");
            if(!compact_session(session,api,err)) {
                if(tui) {tui->add_error(err);tui->render();}
                else std::cerr<<"[error] "<<err<<'\n';
                return 3;
            }
        }
        if (tui) { tui->add_user(trim(raw_prompt)); tui->render("working..."); }
        std::ostringstream start;
        start << "Workspace root: " << opt.root.string() << "\n"
              << "Sandbox policy: " << sandbox_name(opt.policy.sandbox) << "\n"
              << "User task:\n" << user_prompt << "\n";
        if (session.transcript.empty()) session.append_transcript(start.str());
        else session.append_transcript("\n[resumed user turn]\n" + user_prompt + "\n");
        session.emit(EventKind::TurnStarted, "{\"task\":\"" + json_escape(user_prompt) + "\",\"sandbox\":\"" + sandbox_name(opt.policy.sandbox) + "\"}");

        RuntimePolicy turn_policy=opt.policy;
        if(plan_mode || review_only) turn_policy.sandbox=SandboxMode::ReadOnly;
        PromptBuilder prompt_builder{opt.root, turn_policy, api, plan_mode, personality, goal};
        PromptBundle prompt_bundle = prompt_builder.build();
        std::string turn_instructions = prompt_bundle.developer_instructions;
        if(review_only) turn_instructions+="\n\n# Review\nInspect changes and report actionable issues with file locations. Do not edit files.";
        const std::string text_content=prompt_bundle.contextual_user_prefix+"\n\n"+user_prompt;
        std::string user_item;
        if(pending_images.empty()) user_item="{\"role\":\"user\",\"content\":\""+json_escape(text_content)+"\"}";
        else {
            user_item="{\"role\":\"user\",\"content\":[{\"type\":\"input_text\",\"text\":\""+
                json_escape(text_content)+"\"}";
            for(const auto& [path,url]:pending_images) {
                (void)path;
                user_item+=",{\"type\":\"input_image\",\"image_url\":\""+json_escape(url)+"\"}";
            }
            user_item+="]}";
        }
        if(!session.record_item(user_item)) {
            err="could not save user message";
            if(tui) {tui->add_error(err);tui->render();}
            else std::cerr<<"[error] "<<err<<'\n';
            return 3;
        }
        pending_images.clear();

        for (int step = 1; step <= opt.max_steps; ++step) {
#if !defined(_WIN32)
            if(tui_interrupt_requested) return 130;
#endif
            if(session.context_bytes()>450000 && !compact_session(session,api,err)) {
                if(tui) {tui->add_error(err);tui->render();}
                else std::cerr<<"[error] "<<err<<'\n';
                return 3;
            }
            std::optional<ApiResponse> response;
            std::string streamed_text;
            auto show_delta=[&](const std::string& delta) {
                streamed_text+=delta;
                if(opt.json) return;
                if(tui) tui->append_assistant_delta(delta);
                else {
                    if(streamed_text.size()==delta.size()) std::cout<<"[assistant]\n";
                    std::cout<<delta<<std::flush;
                }
            };
            auto update_elapsed=[&]() {if(tui) tui->render("working...");};
            if (api.style == ApiStyle::Responses) {
                const std::string input_json=join_json_array(session.items);
                response = api.call_responses(turn_instructions, input_json, router.specs_json(), err,
                                              std::nullopt, show_delta, update_elapsed, bool(tui));
            } else {
                response = api.call_chat(join_json_array(chat_history(session,turn_instructions)),router.chat_specs_json(),err);
            }
#if !defined(_WIN32)
            if(tui_interrupt_requested) return 130;
#endif
            if (!response) {
                if(!opt.json && !tui && !streamed_text.empty()) std::cout<<'\n';
                if(err=="Interrupted by Esc") {
                    session.append_transcript("\n[turn interrupted by user]\n");
                    session.emit(EventKind::TurnCompleted,"{\"status\":\"interrupted\"}");
                    if(tui) {
                        if(!streamed_text.empty()) tui->finish_assistant_stream(streamed_text);
                        tui->add_system("Interrupted.");
                        tui->render();
                    } else if(!opt.json) std::cout<<"[interrupted]\n";
                    return 0;
                }
                session.emit(EventKind::Error, "{\"message\":\"" + json_escape(err) + "\"}");
                if (tui) {
                    tui->add_error(err);
                    tui->render();
                } else {
                    std::cerr << "[error] " << err << '\n';
                }
                return 3;
            }
            session.emit(EventKind::ModelOutput, "{\"step\":" + std::to_string(step) + ",\"response_id\":\"" + json_escape(response->id) + "\"}");

            if (!response->calls.empty()) {
                if(!streamed_text.empty() && !opt.json) {
                    if(tui) tui->finish_assistant_stream(streamed_text);
                    else std::cout<<'\n';
                }
                if(response->text && !response->text->empty())
                    session.record_item("{\"role\":\"assistant\",\"content\":\""+json_escape(*response->text)+"\"}");
                for(const auto& c:response->calls)
                    if(!session.record_item("{\"type\":\"function_call\",\"call_id\":\""+json_escape(c.call_id)+
                        "\",\"name\":\""+json_escape(c.name)+"\",\"arguments\":\""+json_escape(c.arguments)+"\"}")) {
                        err="could not save tool call";return 3;
                    }
                for(const FunctionCall& c:response->calls) {
                    session.emit(EventKind::ToolRequested, "{\"step\":" + std::to_string(step) + ",\"tool\":\"" + json_escape(c.name) + "\",\"call_id\":\"" + json_escape(c.call_id) + "\"}");
                    if (!opt.json) { if (tui) { tui->add_tool(c.name, c.arguments); if (c.name == "apply_patch") { auto p = json_string_field(c.arguments, "patch"); if (p) tui->add_diff(*p); } tui->render("running tool..."); } else std::cout << "[tool " << step << "] " << c.name << '\n'; }
                    ToolResult r = router.invoke(c, opt.root, turn_policy, session,
                        tui ? std::function<std::optional<bool>(const std::string&)>([&](const std::string& msg)->std::optional<bool>{ return tui->approval(msg); })
                            : std::function<std::optional<bool>(const std::string&)>{},
                        [&](){if(tui) tui->render("running tool...");},bool(tui));
#if !defined(_WIN32)
                    if(tui_interrupt_requested) return 130;
#endif
                    if(r.exit_code==130 && r.output.find("Interrupted by Esc")!=std::string::npos) {
                        session.append_transcript(format_tool_history(c,r)+"\n[turn interrupted by user]\n");
                        session.record_item("{\"type\":\"function_call_output\",\"call_id\":\""+
                            json_escape(c.call_id)+"\",\"output\":\"Interrupted by user\"}");
                        bool remaining=false;
                        for(const auto& later:response->calls) {
                            if(&later==&c) {remaining=true;continue;}
                            if(remaining) session.record_item("{\"type\":\"function_call_output\",\"call_id\":\""+
                                json_escape(later.call_id)+"\",\"output\":\"Cancelled after interruption\"}");
                        }
                        session.emit(EventKind::TurnCompleted,"{\"status\":\"interrupted\"}");
                        if(tui) {tui->add_system("Interrupted.");tui->render();}
                        else if(!opt.json) std::cout<<"[interrupted]\n";
                        return 0;
                    }
                    if (!opt.json && !r.output.empty()) {
                        std::string shown = r.output.size() > 1000 ? r.output.substr(0, 1000) + "\n[output truncated in UI]" : r.output;
                        if (tui) { tui->add_tool_output(shown); tui->render("working..."); }
                        else { std::cout << shown; if (shown.back() != '\n') std::cout << '\n'; }
                    }
                    session.append_transcript(format_tool_history(c, r));
                    if(!session.record_item("{\"type\":\"function_call_output\",\"call_id\":\""+
                        json_escape(c.call_id)+"\",\"output\":\""+json_escape(r.output)+"\"}")) {
                        err="could not save tool result";return 3;
                    }
                }
                continue;
            }

            if (response->text && !trim(*response->text).empty()) {
                const std::string answer = trim(*response->text);
                last_assistant = answer;
                session.append_transcript("\n[assistant final]\n" + answer + "\n");
                if(!session.record_item("{\"role\":\"assistant\",\"content\":\""+json_escape(answer)+"\"}")) {
                    err="could not save assistant response";return 3;
                }
                session.emit(EventKind::TurnCompleted, "{\"status\":\"completed\",\"steps\":" + std::to_string(step) + "}");
                if (!opt.json) {
                    if (tui) tui->stream_assistant(answer);
                    else if(streamed_text.empty()) std::cout << "[assistant]\n" << answer << '\n';
                    else {
                        if(trim(streamed_text)!=answer) std::cout<<"\n[assistant final]\n"<<answer;
                        std::cout<<'\n';
                    }
                } else std::cout << answer << '\n';
                return 0;
            }
            session.append_transcript("\n[system]\nModel returned neither text nor a function call.\n");
        }
        session.emit(EventKind::TurnCompleted, "{\"status\":\"max_steps\"}");
        if (tui) {
            tui->add_error("stopped after maximum agent steps");
            tui->render();
        } else {
            std::cerr << "[error] stopped after maximum agent steps\n";
        }
        return 4;
    };

    if (!first_prompt.empty()) {
        int rc = run_turn(first_prompt);
#if !defined(_WIN32)
        if(tui_interrupt_requested) return 130;
#endif
        if (!interactive || rc != 0) return rc;
    }
    if (!interactive) return 0;

    std::vector<std::string> input_history;
    for (;;) {
        std::string line;
        if (!opt.json && tui) line = tui->read_line(input_history);
        else {
            if (!opt.json) std::cout << "codex> " << std::flush;
            if (!std::getline(std::cin, line)) break;
        }
#if !defined(_WIN32)
        if(tui_interrupt_requested) return 130;
#endif
        line = trim(line);
        if (line.empty()) continue;
        if (input_history.empty() || input_history.back() != line) input_history.push_back(line);
        const std::string token = slash_command_token(line);
        const std::string args = command_arguments(line);
        auto say = [&](const std::string& text, bool is_error=false) {
            if (tui) { if (is_error) tui->add_error(text); else tui->add_system(text); tui->render(); }
            else { std::cout << text; if (text.empty() || text.back()!='\n') std::cout << '\n'; }
        };
        auto fresh_session = [&]() -> bool {
            Session next; std::string local_err;
            if (!init_session(next, opt.root, opt.json, std::nullopt, local_err)) { say(local_err,true); return false; }
            session = std::move(next); last_assistant.clear(); pending_mentions.clear();pending_images.clear();
            if (tui) { tui->set_thread_name(""); tui->reset_transcript(); }
            return true;
        };

        if (token == "/quit" || token == "/exit") break;
        if (token == "/help") {
            std::ostringstream h;
            for (const auto& command : slash_commands()) {
                const std::string name = command.name;
                if (name == "/sessions" || name == "/history" || name == "/quit") continue;
                h << name << "  " << command.description;
                if (!command.implemented) h << "  [requires unsupported Codex capability]";
                h << '\n';
            }
            say(h.str()); continue;
        }
        if (token == "/history") {
            std::ostringstream h; for (size_t i=0;i<input_history.size();++i) h << (i+1) << ": " << input_history[i] << "\n";
            say(h.str().empty()?"No input history.":h.str()); continue;
        }
        if (token == "/sessions") {
            auto ids=session_ids(opt.root); std::ostringstream out;
            for(const auto& id:ids) out << (id==session.id?"› ":"  ") << id << '\n';
            say(out.str().empty()?"No saved sessions.":out.str()); continue;
        }
        if (token == "/status") {
            std::ostringstream st;
            st << "session: " << session.id << "\n"
               << "workspace: " << opt.root.string() << "\n"
               << "provider: " << opt.provider << " (" << api_style_name(api.style) << ")\n"
               << "model: " << api.model;
            if(!api.reasoning_effort.empty()) st << " " << api.reasoning_effort;
            st << "\nsandbox: " << sandbox_name(opt.policy.sandbox)
               << "\napproval: " << (opt.policy.approval==ApprovalPolicy::Never?"never":"on-request")
               << "\nplan mode: " << (plan_mode?"on":"off")
               << "\ntranscript: " << session.transcript.size() << " bytes"
               << "\ncontext: " << session.context_bytes() << " bytes / " << session.items.size() << " items"
               << "\nshell sandbox: " << (opt.policy.sandbox==SandboxMode::DangerFullAccess
                       ? "disabled by danger-full-access"
                       : restricted_shell_command(opt.root,"true",opt.policy.sandbox)
                         ? "available" : "unavailable (restricted shell denied)");
            if(!goal.empty()) st << "\ngoal: " << goal;
            if(!personality.empty()) st << "\npersonality: " << personality;
            say(st.str()); continue;
        }
        if(token=="/skills") {
            const auto list=skills_manifest(opt.root);
            say(list.empty()?"No skills found in .agents/skills or ~/.codex/skills.":list);
            continue;
        }
        if(token=="/mcp") {
            say(mcp_report.empty()?"No MCP servers configured. Use --mcp-config PATH or ~/.codex-cpp/mcp.json.":mcp_report);
            continue;
        }
        if (token == "/login" || token == "/auth") {
            if (opt.provider != "codex") { say("Use --provider codex for ChatGPT subscription sign-in.",true); continue; }
            if(token=="/login") {
                if(!args.empty() && args!="device") {say("Usage: /login [device]",true);continue;}
                if(tui) tui->render("waiting for device authorization...");
                std::string login_error;
                bool ok=own_device_login(login_error);
                say(ok?"Signed in.":login_error,!ok);
            } else {
                auto auth=load_auth();
                say(auth?"Signed in to ChatGPT (local credentials).":"Not signed in.",!auth);
            }
            continue;
        }
        if (token == "/model") {
            std::string model=api.model, effort=api.reasoning_effort;
            auto words=split_words(args);
            const std::set<std::string> efforts={"low","medium","high","xhigh","none"};
            if(words.empty()) {
                if(tui) {
                    std::vector<std::string> options;
                    for(const auto& e:std::vector<std::string>{"low","medium","high","xhigh","none"})
                        options.push_back(api.model + (e=="none"?" (default)":" "+e));
                    int c=tui->choose("Choose reasoning effort for " + api.model, options, 0);
                    if(c<0) continue;
                    effort=std::vector<std::string>{"low","medium","high","xhigh","none"}[static_cast<size_t>(c)];
                } else { say("Usage: /model MODEL [low|medium|high|xhigh|none] or /model EFFORT"); continue; }
            } else if(efforts.count(words[0])) effort=words[0];
            else { model=words[0]; if(words.size()>1) effort=words[1]; }
            if(!effort.empty() && !efforts.count(effort)) { say("Invalid reasoning effort: "+effort,true); continue; }
            if(effort=="none") effort.clear();
            api.model=model; api.reasoning_effort=effort; opt.model=model; opt.reasoning_effort=effort;
            if(tui) tui->set_model(model,effort);
            say("Model set to " + model + (effort.empty()?"":" ("+effort+")")); continue;
        }
        if (token == "/permissions") {
            std::string mode=args;
            if(mode.empty() && tui) {
                int c=tui->choose("Choose permissions", {"read-only","workspace-write","danger-full-access"},
                                  opt.policy.sandbox==SandboxMode::ReadOnly?0:opt.policy.sandbox==SandboxMode::WorkspaceWrite?1:2);
                if(c<0) continue; mode=std::vector<std::string>{"read-only","workspace-write","danger-full-access"}[static_cast<size_t>(c)];
            }
            if(mode.empty()){ say("Usage: /permissions read-only|workspace-write|danger-full-access"); continue; }
            if(mode=="read-only") { opt.policy.sandbox=SandboxMode::ReadOnly; opt.policy.approval=ApprovalPolicy::OnRequest; }
            else if(mode=="workspace-write") { opt.policy.sandbox=SandboxMode::WorkspaceWrite; opt.policy.approval=ApprovalPolicy::OnRequest; }
            else if(mode=="danger-full-access") { opt.policy.sandbox=SandboxMode::DangerFullAccess; opt.policy.approval=ApprovalPolicy::Never; }
            else { say("Unknown permission profile: "+mode,true); continue; }
            if(tui) tui->set_sandbox(sandbox_name(opt.policy.sandbox));
            say("Permissions set to " + mode); continue;
        }
        if (token == "/personality") {
            std::string value=args;
            if(value.empty() && tui){ int c=tui->choose("Choose communication style", {"concise","balanced","detailed"},1); if(c<0) continue; value=std::vector<std::string>{"concise","balanced","detailed"}[static_cast<size_t>(c)]; }
            if(value.empty()){ say("Usage: /personality concise|balanced|detailed"); continue; }
            personality=value; say("Personality set to " + personality); continue;
        }
        if (token == "/plan") {
            if(args=="on") plan_mode=true; else if(args=="off") plan_mode=false; else plan_mode=!plan_mode;
            say(std::string("Plan mode ")+(plan_mode?"enabled":"disabled")); continue;
        }
        if (token == "/goal") {
            if(args.empty()) say(goal.empty()?"No goal set.":"Current goal: "+goal);
            else if(args=="clear") { goal.clear(); say("Goal cleared."); }
            else { goal=args; say("Goal set: "+goal); }
            continue;
        }
        if (token == "/rename") {
            if(args.empty()){ say("Usage: /rename NAME"); continue; }
            std::string local_err; if(!write_session_name(session,args,local_err)){ say(local_err,true); continue; }
            if(tui) tui->set_thread_name(args); say("Thread renamed to " + args); continue;
        }
        if (token == "/new" || token == "/clear") {
            if(fresh_session()) say("Started a new chat: " + session.id); continue;
        }
        if (token == "/resume") {
            std::string id=args;
            if(id.empty() && tui){ auto ids=session_ids(opt.root); if(ids.empty()){say("No saved sessions.");continue;} int c=tui->choose("Resume a saved chat",ids,0); if(c<0)continue; id=ids[static_cast<size_t>(c)]; }
            if(id.empty()){ say("Usage: /resume SESSION_ID"); continue; }
            Session resumed; std::string local_err;
            if(!init_session(resumed,opt.root,opt.json,id,local_err)){ say(local_err,true); continue; }
            session=std::move(resumed); last_assistant.clear(); pending_mentions.clear();pending_images.clear();
            if(tui){ tui->set_thread_name(""); tui->reset_transcript(); }
            say("Resumed " + id + " (" + std::to_string(session.transcript.size()) + " transcript bytes)"); continue;
        }
        if (token == "/fork" || token == "/side") {
            const std::string old=session.id; const std::string prior=session.transcript;
            const auto prior_items=session.items;
            Session forked; std::string local_err;
            if(!init_session(forked,opt.root,opt.json,std::nullopt,local_err)){ say(local_err,true); continue; }
            if(!forked.replace_items(prior_items)) {say("Could not fork session items.",true);continue;}
            forked.append_transcript(prior + (token=="/side"?"\n[ephemeral side conversation fork]\n":"\n[forked session]\n"));
            session=std::move(forked);
            if(tui){ tui->set_thread_name(token=="/side"?"side":"fork"); tui->reset_transcript(); }
            say(std::string(token=="/side"?"Started side conversation ":"Forked ") + old + " → " + session.id); continue;
        }
        if (token == "/archive") {
            std::string local_err; if(!archive_session_files(session,local_err)){ say(local_err,true); continue; }
            say("Archived session " + session.id); break;
        }
        if (token == "/delete") {
            bool ok=false;
            if(tui) ok=tui->choose("Permanently delete this session?", {"Cancel","Delete permanently"},0)==1;
            else ok=approve("permanently delete session "+session.id,false);
            if(!ok){ say("Delete cancelled."); continue; }
            std::string local_err; if(!delete_session_files(session,local_err)){ say(local_err,true); continue; }
            say("Deleted session " + session.id); break;
        }
        if (token == "/init") {
            const fs::path agents=opt.root/"AGENTS.md"; std::error_code ec;
            if(fs::exists(agents,ec)){ say("AGENTS.md already exists."); continue; }
            std::ofstream f(agents); if(!f){ say("Cannot create AGENTS.md",true); continue; }
            f << "# AGENTS.md\n\n## Project instructions\n\n- Inspect existing code before editing.\n- Keep changes focused on the requested task.\n- Run relevant build/tests after edits.\n- Preserve existing project conventions.\n";
            say("Created " + agents.string()); continue;
        }
        if (token == "/mention") {
            if(args.empty()){ say("Usage: /mention PATH"); continue; }
            fs::path target=fs::path(args); if(target.is_relative()) target=opt.root/target;
            if(!within_root(opt.root,target)){ say("Mention path escapes workspace.",true); continue; }
            std::error_code ec; if(!fs::is_regular_file(target,ec)){ say("Not a regular file: "+target.string(),true); continue; }
            std::string content=slurp(target,120000); pending_mentions.emplace_back(fs::relative(target,opt.root,ec).string(),content);
            say("Attached " + pending_mentions.back().first + " to the next turn."); continue;
        }
        if(token=="/image") {
            if(args.empty()) {say("Usage: /image PATH",true);continue;}
            if(pending_images.size()>=4) {say("Maximum four images per turn.",true);continue;}
            fs::path target=args;if(target.is_relative()) target=opt.root/target;
            std::string image_error;auto url=image_data_url(target,image_error);
            if(!url) {say("Cannot attach image: "+image_error,true);continue;}
            pending_images.emplace_back(target.string(),std::move(*url));
            say("Attached image to the next turn: "+target.string());continue;
        }
        if (token == "/diff") {
            const std::string cmd="cd " + shell_quote(opt.root.string()) + " && git diff --no-ext-diff -- .; printf '\\n-- status --\\n'; git status --short";
            CommandResult r=run_capture(cmd,120000); say(r.output.empty()?"No git diff.":r.output, r.exit_code!=0); continue;
        }
        if (token == "/copy") {
            if(last_assistant.empty()){ say("There is no assistant response to copy.",true); continue; }
            const std::string e=clipboard_copy(last_assistant); if(!e.empty()) say(e,true); else say("Copied last response as markdown."); continue;
        }
        if (token == "/review") {
            std::string focus=args.empty()?"Review the current workspace changes. Inspect the git diff, identify correctness or regression risks, and report concrete findings. Do not edit files unless explicitly asked.":"Review the current workspace changes with this focus: "+args;
            (void)run_turn(focus,true); continue;
        }
        if (token == "/compact") {
            if(session.items.empty()){ say("Nothing to compact."); continue; }
            if(tui) tui->render("working...");
            if(!compact_session(session,api,err)) say(err.empty()?"Compaction failed.":err,true);
            else say("Conversation compacted to " + std::to_string(session.context_bytes()) + " context bytes.");
            continue;
        }
        if (token == "/title") {
            const std::string title=args.empty()?"Codex C++":safe_terminal_text(args);
            std::cout << "\x1b]0;" << title << "\x07" << std::flush; say("Terminal title set to " + title); continue;
        }
        if (token == "/statusline") {
            if(args.empty()){ say("Usage: /statusline model,cwd[,thread,sandbox]"); continue; }
            std::string normalized=args; std::replace(normalized.begin(),normalized.end(),',',' '); auto items=split_words(normalized);
            if(tui) tui->set_statusline_items(items); say("Status line updated."); continue;
        }
        if (token == "/logout") {
            if (opt.provider == "codex") {
                std::error_code ec; fs::remove(own_auth_path(),ec);
                say(ec?"Could not remove local credentials.":"Signed out (local credentials removed).",bool(ec));
                continue;
            }
            api.api_key.clear();
#if defined(_WIN32)
            if(!opt.api_key_env.empty()) _putenv_s(opt.api_key_env.c_str(), "");
#else
            if(!opt.api_key_env.empty()) unsetenv(opt.api_key_env.c_str());
#endif
            say("Cleared API credential from this process. Environment changes outside this process are unchanged."); continue;
        }
        if (token == "/ps") { say("No background terminals are managed by this single-file runtime."); continue; }
        if (token == "/stop") { say("No background terminals to stop."); continue; }
        if (!token.empty()) {
            const SlashCommandDef* command=find_slash_command(token);
            const std::string message=command
                ? (std::string(command->name)+" requires a Codex capability that this standalone C++ runtime does not currently provide.")
                : ("Unknown command: "+token);
            say(message,true); continue;
        }
        int rc = run_turn(line);
#if !defined(_WIN32)
        if(tui_interrupt_requested) return 130;
#endif
        if (rc != 0) {
            if (tui) {
                tui->add_error("turn failed with code " + std::to_string(rc));
                tui->render();
            } else {
                std::cout << "turn failed with code " << rc << '\n';
            }
        }
    }
    return 0;
}
