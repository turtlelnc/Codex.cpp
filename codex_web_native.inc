// C++17 local/hosted web runtime. UI and Agent JSON protocol are shared.
#include <mutex>
#include <condition_variable>
#include <deque>
#include <atomic>
#include <csignal>
#include <cwctype>
#include <regex>
#include <git2.h>
#include "codex_resources.h"
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <bcrypt.h>
#include <sddl.h>
#include <shellapi.h>
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Bcrypt.lib")
#else
#include <dirent.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <spawn.h>
#include <sys/file.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif
extern char **environ;
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif
namespace native_web {
using J = Json;
void require(bool condition,const std::string& message);
// Shared normalization for optional browser origins and TUI server URLs.
std::string browser_origin(std::string value) {
  while(!value.empty()&&value.back()=='/')value.pop_back();
  require(value.size()<=2048&&value.find_first_of("@?#\\") == std::string::npos&&
    std::none_of(value.begin(),value.end(),[](unsigned char c){return c<=32||c==127;}),"Use an HTTP(S) origin without path or credentials");
  auto separator=value.find("://");
  require(separator!=std::string::npos&&value.find('/',separator+3)==std::string::npos,"Use an HTTP(S) origin without a path");
  std::unique_ptr<CURLU,decltype(&curl_url_cleanup)> url(curl_url(),curl_url_cleanup);
  require(url&&curl_url_set(url.get(),CURLUPART_URL,value.c_str(),0)==CURLUE_OK,"Invalid server URL");
  auto part=[&](CURLUPart key){char* bytes=nullptr;if(curl_url_get(url.get(),key,&bytes,0)!=CURLUE_OK)return std::string{};
    std::string result(bytes);curl_free(bytes);return result;};
  auto scheme=part(CURLUPART_SCHEME),name=part(CURLUPART_HOST),port=part(CURLUPART_PORT);
  require((scheme=="http"||scheme=="https")&&!name.empty(),"Use an HTTP or HTTPS origin");
  require(port.empty()||std::stoi(port)>0,"URL port must be 1..65535");
  for(char& c:name)c=char(std::tolower(static_cast<unsigned char>(c)));
  if((scheme=="http"&&port=="80")||(scheme=="https"&&port=="443"))port.clear();
  return scheme+"://"+name+(port.empty()?"":":"+port);
}
bool ipv4_address(const std::string& address) {
  std::istringstream input(address);std::string octet;int count=0;
  while(std::getline(input,octet,'.')){
    if(octet.empty()||octet.size()>3||(octet.size()>1&&octet.front()=='0')||
      !std::all_of(octet.begin(),octet.end(),[](unsigned char c){return std::isdigit(c);})||std::stoi(octet)>255)return false;
    ++count;
  }
  return count==4&&!address.empty()&&address.back()!='.';
}
J obj(std::initializer_list<std::pair<const std::string, J>> fields = {}) {
  J j;
  j.kind = J::Object;
  j.fields = fields;
  return j;
}
J arr(std::vector<J> items = {}) {
  J j;
  j.kind = J::Array;
  j.items = std::move(items);
  return j;
}
J str(std::string s) {
  J j;
  j.kind = J::String;
  j.value = std::move(s);
  return j;
}
J boolean(bool b) {
  J j;
  j.kind = J::Other;
  j.value = b ? "true" : "false";
  return j;
}
J number(long long n) {
  J j;
  j.kind = J::Other;
  j.value = std::to_string(n);
  return j;
}
std::string text(const J &j, const std::string &k, std::string fallback = "") {
  auto p = j.get(k);
  return p && p->kind == J::String ? p->value : fallback;
}
bool flag(const J &j, const std::string &k) {
  auto p = j.get(k);
  return p && p->kind == J::Other && p->value == "true";
}
J field(const J &j, const std::string &k, J fallback = J{}) {
  auto p = j.get(k);
  return p ? *p : fallback;
}
#include "codex_git.inc"
void require(bool good, const std::string &why) {
  if (!good)
    throw std::runtime_error(why);
}
#include "codex_git_server.inc"
std::string entropy() {
  unsigned char bytes[32];
#if defined(_WIN32)
  require(BCryptGenRandom(nullptr, bytes, sizeof bytes, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0,
          "Random generator unavailable");
#else
  int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  require(fd >= 0, "Random generator unavailable");
  size_t count = 0;
  while (count < sizeof bytes) {
    auto n = ::read(fd, bytes + count, sizeof bytes - count);
    if (n <= 0) {
      ::close(fd);
      throw std::runtime_error("Random generator failed");
    }
    count += static_cast<size_t>(n);
  }
  ::close(fd);
#endif
  std::string out;
  for (auto b : bytes) {
    out += "0123456789abcdef"[b >> 4];
    out += "0123456789abcdef"[b & 15];
  }
  return out;
}
bool same(const std::string &a, const std::string &b) {
  unsigned difference = static_cast<unsigned>(a.size() ^ b.size());
  for (size_t i = 0; i < a.size(); ++i)
    difference |= static_cast<unsigned char>(a[i]) ^ (i < b.size() ? static_cast<unsigned char>(b[i]) : 0u);
  return difference == 0;
}
// SHA-256 is used for optimistic editor versions and credential fingerprints.
std::string digest(std::string data) {
  static constexpr uint32_t k[] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
      0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
      0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
      0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
      0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
      0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
      0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
      0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
  uint32_t h[] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                  0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  uint64_t bits = static_cast<uint64_t>(data.size()) * 8;
  data += '\x80';
  while (data.size() % 64 != 56)
    data += '\0';
  for (int i = 7; i >= 0; --i)
    data += static_cast<char>(bits >> (i * 8));
  auto rr = [](uint32_t v, int n) { return (v >> n) | (v << (32 - n)); };
  for (size_t off = 0; off < data.size(); off += 64) {
    uint32_t w[64]{};
    for (int i = 0; i < 16; ++i)
      for (int j = 0; j < 4; ++j)
        w[i] = (w[i] << 8) | static_cast<unsigned char>(data[off + static_cast<size_t>(4 * i + j)]);
    for (int i = 16; i < 64; ++i)
      w[i] = w[i - 16] + (rr(w[i - 15], 7) ^ rr(w[i - 15], 18) ^ (w[i - 15] >> 3)) + w[i - 7] +
             (rr(w[i - 2], 17) ^ rr(w[i - 2], 19) ^ (w[i - 2] >> 10));
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], v = h[7];
    for (int i = 0; i < 64; ++i) {
      uint32_t t = v + (rr(e, 6) ^ rr(e, 11) ^ rr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i],
               u = (rr(a, 2) ^ rr(a, 13) ^ rr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
      v = g;
      g = f;
      f = e;
      e = d + t;
      d = c;
      c = b;
      b = a;
      a = t + u;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += v;
  }
  std::ostringstream out;
  for (auto v : h)
    out << std::hex << std::setw(8) << std::setfill('0') << v;
  return out.str();
}
bool identifier(const std::string &s) {
  return !s.empty() && s.size() <= 128 && std::all_of(s.begin(), s.end(), [](unsigned char c) {
    return std::isalnum(c) || c == '_' || c == '-';
  });
}
bool model_name(const std::string &s) {
  return s.size() <= 200 && std::all_of(s.begin(), s.end(), [](unsigned char c) {
           return std::isalnum(c) || std::string("._:/-").find(static_cast<char>(c)) != std::string::npos;
         });
}
bool valid_utf8(const std::string &data) {
  for (size_t i = 0; i < data.size();) {
    unsigned char a = static_cast<unsigned char>(data[i]);
    size_t n = a < 128                  ? 1
               : a >= 0xc2 && a <= 0xdf ? 2
               : a >= 0xe0 && a <= 0xef ? 3
               : a >= 0xf0 && a <= 0xf4 ? 4
                                        : 0;
    if (!n || i + n > data.size())
      return false;
    for (size_t j = 1; j < n; ++j) {
      auto c = static_cast<unsigned char>(data[i + j]);
      if (c < 0x80 || c > 0xbf)
        return false;
    }
    if (n >= 3) {
      auto c = static_cast<unsigned char>(data[i + 1]);
      if ((a == 0xe0 && c < 0xa0) || (a == 0xed && c >= 0xa0) || (a == 0xf0 && c < 0x90) ||
          (a == 0xf4 && c >= 0x90))
        return false;
    }
    i += n;
  }
  return true;
}

void protect(const fs::path &path) {
#if defined(_WIN32)
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  require(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;FA;;;OW)(A;;FA;;;SY)",
                                                               SDDL_REVISION_1, &descriptor, nullptr) != 0,
          "Cannot create private ACL");
  bool good = SetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                               descriptor) != 0;
  LocalFree(descriptor);
  require(good, "Cannot protect private state");
#else
  (void)path;
#endif
}
void private_dir(const fs::path &path) {
  std::error_code ec;
  fs::create_directories(path, ec);
  require(!ec && !fs::is_symlink(path) && fs::is_directory(path), "Unsafe service directory");
  protect(path);
#if !defined(_WIN32)
  struct stat info{};
  require(::lstat(path.c_str(), &info) == 0 && info.st_uid == getuid(),
          "Service directory must belong to current user");
  require(::chmod(path.c_str(), 0700) == 0, "Cannot protect service directory");
#endif
}
std::string read_bytes(const fs::path &path, size_t limit = 16 * 1024 * 1024) {
#if defined(_WIN32)
  HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  require(handle != INVALID_HANDLE_VALUE, "Cannot read file");
  BY_HANDLE_FILE_INFORMATION info{};
  bool good = GetFileType(handle) == FILE_TYPE_DISK && GetFileInformationByHandle(handle, &info) &&
              !(info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) &&
              info.nNumberOfLinks == 1 && info.nFileSizeHigh == 0 && info.nFileSizeLow <= limit;
  if (!good) {
    CloseHandle(handle);
    throw std::runtime_error("Missing, linked or oversized file");
  }
  std::string output(info.nFileSizeLow, '\0');
  DWORD n = 0;
  good =
      ReadFile(handle, output.data(), static_cast<DWORD>(output.size()), &n, nullptr) && n == output.size();
  CloseHandle(handle);
  require(good, "Cannot read file");
  return output;
#else
  int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  require(fd >= 0, "Cannot read file");
  struct stat info{};
  if (fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_nlink != 1 || info.st_size < 0 ||
      static_cast<uint64_t>(info.st_size) > limit) {
    ::close(fd);
    throw std::runtime_error("Missing, linked or oversized file");
  }
  std::string out;
  char buffer[8192];
  while (true) {
    auto n = ::read(fd, buffer, sizeof buffer);
    if (!n)
      break;
    if (n < 0) {
      ::close(fd);
      throw std::runtime_error("Cannot read file");
    }
    out.append(buffer, static_cast<size_t>(n));
    if (out.size() > limit) {
      ::close(fd);
      throw std::runtime_error("File exceeds limit");
    }
  }
  ::close(fd);
  return out;
#endif
}

J read_json(const fs::path &path) {
  if (!fs::exists(path))
    return obj();
  auto parsed = parse_json(read_bytes(path));
  require(parsed && parsed->kind == J::Object, "Invalid saved state");
  return *parsed;
}
void atomic_save(const fs::path &path, const J &value) {
  auto temporary = path;
  temporary += "." + entropy();
  auto data = json_dump(value);
#if defined(_WIN32)
  struct RemovePartial {fs::path path;~RemovePartial(){std::error_code ignored;fs::remove(path,ignored);}} remove_partial{temporary};
  std::ofstream stream(temporary, std::ios::binary);
  stream << data;
  stream.flush();
  require(bool(stream),"Cannot write state file");
  stream.close();
  require(bool(stream),"Cannot close state file");
  protect(temporary);
  require(MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) !=
              0,
          "Cannot save state");
#else
  int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  require(fd >= 0, "Cannot create state file");
  size_t offset = 0;
  while (offset < data.size()) {
    auto n = ::write(fd, data.data() + offset, data.size() - offset);
    if (n <= 0) {
      ::close(fd);
      fs::remove(temporary);
      throw std::runtime_error("Cannot save state");
    }
    offset += static_cast<size_t>(n);
  }
  if(::fsync(fd)!=0){::close(fd);fs::remove(temporary);throw std::runtime_error("Cannot sync state file");}
  ::close(fd);
  require(::rename(temporary.c_str(), path.c_str()) == 0, "Cannot replace state");
  int directory = ::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  require(directory >= 0, "Cannot sync state directory");
  int synced = ::fsync(directory);
  ::close(directory);
  require(synced == 0, "Cannot sync state directory");
#endif
}
// All workspace path components are opened without following links on POSIX.
struct Workspace {
  fs::path root;
  explicit Workspace(fs::path p) : root(fs::canonical(p)) {}
  static bool blocked(const fs::path &p) {
    for (const auto &part : p) {
      auto name = part.generic_u8string();
      for (char &c : name)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (name == "..")
        return true;
#if defined(_WIN32)
      if (name.find(':') != std::string::npos)
        return true;
      while (!name.empty() && (name.back() == '.' || name.back() == ' '))
        name.pop_back();
#endif
      if (name == ".git" || name == ".codex_cpp" || name == ".codex-cpp")
        return true;
    }
    return false;
  }
#if !defined(_WIN32)
  int open_file(const std::string &name, int mode, bool internal = false) {
    fs::path relative(name);
    require(!relative.empty() && !relative.is_absolute() && (internal || !blocked(relative)),
            "Workspace path denied");
    int fd = ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    require(fd >= 0, "Cannot open workspace");
    std::vector<std::string> parts;
    for (const auto &p : relative)
      parts.push_back(p.u8string());
    for (size_t i = 0; i < parts.size(); ++i) {
      if (parts[i] == ".." || parts[i] == "." || parts[i].empty()) {
        ::close(fd);
        throw std::runtime_error("Workspace path denied");
      }
      int next = ::openat(fd, parts[i].c_str(),
                          i + 1 == parts.size() ? mode | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC
                                                : O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      ::close(fd);
      fd = next;
      if (fd < 0)
        throw std::runtime_error("Missing or linked workspace file");
    }
    return fd;
  }
#endif
#if defined(_WIN32)
  HANDLE checked_handle(const std::string &name, DWORD access, DWORD share, bool internal = false) {
    auto relative = fs::u8path(name);
    require(!relative.empty() && !relative.is_absolute() && !relative.has_root_name() &&
                (internal || !blocked(relative)),
            "Workspace path denied");
    for (const auto &part : relative)
      require(part != "..", "Workspace path denied");
    HANDLE folder = CreateFileW(
        root.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    require(folder != INVALID_HANDLE_VALUE, "Cannot open workspace");
    std::vector<wchar_t> root_name(32768);
    DWORD root_length = GetFinalPathNameByHandleW(folder, root_name.data(),
                                                  static_cast<DWORD>(root_name.size()), FILE_NAME_NORMALIZED);
    CloseHandle(folder);
    require(root_length && root_length < root_name.size(), "Cannot inspect workspace");
    HANDLE file = CreateFileW((root / relative).c_str(), access, share, nullptr, OPEN_EXISTING,
                              FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    require(file != INVALID_HANDLE_VALUE, "Cannot open workspace file");
    BY_HANDLE_FILE_INFORMATION info{};
    std::vector<wchar_t> file_name(32768);
    DWORD file_length = GetFinalPathNameByHandleW(file, file_name.data(),
                                                  static_cast<DWORD>(file_name.size()), FILE_NAME_NORMALIZED);
    std::wstring prefix(root_name.data(), root_length),
        target(file_name.data(), file_length < file_name.size() ? file_length : 0);
    for (auto &c : prefix)
      c = std::towlower(c);
    for (auto &c : target)
      c = std::towlower(c);
    while (!prefix.empty() && prefix.back() == L'\\')
      prefix.pop_back();
    prefix += L'\\';
    bool good = file_length && file_length < file_name.size() && target.rfind(prefix, 0) == 0 &&
                GetFileType(file) == FILE_TYPE_DISK && GetFileInformationByHandle(file, &info) &&
                !(info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) &&
                info.nNumberOfLinks == 1 && info.nFileSizeHigh == 0 && info.nFileSizeLow <= 16 * 1024 * 1024;
    if (!good) {
      CloseHandle(file);
      throw std::runtime_error("Linked, external or oversized workspace file denied");
    }
    return file;
  }
#endif
  std::string read(const std::string &name, bool internal = false) {
#if defined(_WIN32)
    HANDLE handle = checked_handle(name, GENERIC_READ, FILE_SHARE_READ, internal);
    DWORD size = GetFileSize(handle, nullptr), n = 0;
    std::string output(size, '\0');
    bool good = ReadFile(handle, output.data(), size, &n, nullptr) && n == size;
    CloseHandle(handle);
    require(good, "Cannot read workspace file");
    return output;
#else
    int fd = open_file(name, O_RDONLY, internal);
    struct stat st{};
    if (::fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_nlink != 1 || st.st_size > 16 * 1024 * 1024) {
      ::close(fd);
      throw std::runtime_error("Not a regular, private-sized file");
    }
    std::string out;
    char bytes[8192];
    while (true) {
      auto n = ::read(fd, bytes, sizeof bytes);
      if (n < 0) {
        ::close(fd);
        throw std::runtime_error("Cannot read file");
      }
      if (!n)
        break;
      out.append(bytes, static_cast<size_t>(n));
      if (out.size() > 16 * 1024 * 1024) {
        ::close(fd);
        throw std::runtime_error("File exceeds limit");
      }
    }
    ::close(fd);
    return out;
#endif
  }
  std::string untracked_image(const std::string &name) {
    auto relative = fs::u8path(name);
    require(!relative.empty() && !relative.is_absolute() && !relative.has_root_name() && !blocked(relative),
            "Untracked path denied");
#if !defined(_WIN32)
    int parent = ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    require(parent >= 0, "Cannot inspect workspace");
    std::vector<std::string> parts;
    for (const auto &part : relative)
      parts.push_back(part.u8string());
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
      int next = ::openat(parent, parts[i].c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      ::close(parent);
      parent = next;
      require(parent >= 0, "Linked untracked parent denied");
    }
    struct stat info{};
    if (::fstatat(parent, parts.back().c_str(), &info, AT_SYMLINK_NOFOLLOW)) {
      ::close(parent);
      throw std::runtime_error("Cannot inspect untracked file");
    }
    std::string kind = std::to_string(info.st_mode) + ":";
    if (S_ISLNK(info.st_mode)) {
      char target[16385];
      auto n = ::readlinkat(parent, parts.back().c_str(), target, sizeof target);
      ::close(parent);
      require(n >= 0 && static_cast<size_t>(n) < sizeof target, "Cannot review symlink target");
      return kind + std::string(target, static_cast<size_t>(n));
    }
    ::close(parent);
    require(S_ISREG(info.st_mode) && info.st_nlink == 1, "Untracked file cannot be safely reviewed");
    return kind + read(name);
#else
    return std::to_string(static_cast<unsigned>(fs::status(root / relative).permissions())) + ":" +
           read(name);
#endif
  }
  J files() {
    std::vector<J> names;
    size_t visited = 0;
#if !defined(_WIN32)
    int root_fd = ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    require(root_fd >= 0, "Cannot open workspace");
    std::function<void(int, const std::string &, int)> visit = [&](int fd, const std::string &prefix,
                                                                   int depth) {
      DIR *dir = fdopendir(fd);
      if (!dir) {
        ::close(fd);
        return;
      }
      std::vector<std::string> entries;
      while (auto entry = readdir(dir)) {
        std::string name = entry->d_name;
        if (name != "." && name != "..")
          entries.push_back(name);
      }
      std::sort(entries.begin(), entries.end());
      for (auto &name : entries) {
        if (++visited > 10000 || names.size() >= 5000)
          break;
        auto relative = prefix + name;
        if (blocked(fs::path(relative)))
          continue;
        struct stat info{};
        if (fstatat(dirfd(dir), name.c_str(), &info, AT_SYMLINK_NOFOLLOW))
          continue;
        if (S_ISDIR(info.st_mode) && depth < 64) {
          int child = openat(dirfd(dir), name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
          if (child >= 0)
            visit(child, relative + "/", depth + 1);
        } else if (S_ISREG(info.st_mode) && info.st_nlink == 1)
          names.push_back(str(relative));
      }
      closedir(dir);
    };
    visit(root_fd, "", 0);
#else
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         it != end && !ec; it.increment(ec)) {
      if (++visited > 10000 || names.size() >= 5000)
        break;
      auto rel = it->path().lexically_relative(root);
      auto status = it->symlink_status(ec);
      if (ec)
        break;
      auto attributes = GetFileAttributesW(it->path().c_str());
      if (fs::is_symlink(status) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) || blocked(rel) ||
          it.depth() > 64) {
        it.disable_recursion_pending();
        continue;
      }
      if (fs::is_regular_file(status) && fs::hard_link_count(it->path(), ec) == 1)
        names.push_back(str(rel.generic_u8string()));
    }
#endif
    return obj({{"files", arr(names)}});
  }

  J write(const std::string &path, const std::string &data, const std::string &version) {
    require(data.size() <= 1024 * 1024 && data.find('\0') == std::string::npos && valid_utf8(data),
            "Editor accepts text up to 1 MiB");
#if defined(_WIN32)
    HANDLE handle = checked_handle(path, GENERIC_READ | GENERIC_WRITE, 0);
    require(handle != INVALID_HANDLE_VALUE, "File busy or unavailable");
    BY_HANDLE_FILE_INFORMATION info{};
    bool good = GetFileInformationByHandle(handle, &info) &&
                !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && info.nNumberOfLinks == 1;
    DWORD n = 0;
    std::string before(static_cast<size_t>(GetFileSize(handle, nullptr)), '\0');
    good = good && ReadFile(handle, before.data(), static_cast<DWORD>(before.size()), &n, nullptr) &&
           digest(before) == version;
    SetFilePointer(handle, 0, nullptr, FILE_BEGIN);
    good = good && WriteFile(handle, data.data(), static_cast<DWORD>(data.size()), &n, nullptr) &&
           n == data.size() && SetEndOfFile(handle);
    CloseHandle(handle);
    require(good, "Editor save refused");
#else
    int fd = open_file(path, O_RDWR);
    struct stat st{};
    bool good = ::fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_nlink == 1 && st.st_size <= 1024 * 1024;
    std::string before;
    char buf[8192];
    if (good) {
      ssize_t n;
      while ((n = ::read(fd, buf, sizeof buf)) > 0)
        before.append(buf, static_cast<size_t>(n));
      good = n == 0 && digest(before) == version;
    }
    if (!good) {
      ::close(fd);
      throw std::runtime_error("File changed; reload before saving");
    }
    size_t offset = 0;
    while (offset < data.size()) {
      auto n = ::pwrite(fd, data.data() + offset, data.size() - offset, static_cast<off_t>(offset));
      if (n <= 0)
        break;
      offset += static_cast<size_t>(n);
    }
    good = offset == data.size() && ::ftruncate(fd, static_cast<off_t>(data.size())) == 0;
    ::fsync(fd);
    ::close(fd);
    require(good, "Editor write failed; inspect file before retrying");
#endif
    return obj({{"saved", boolean(true)}, {"version", str(digest(data))}});
  }
};
struct Child {
#if defined(_WIN32)
  HANDLE process = nullptr, input = nullptr, output = nullptr, job = nullptr;
#else
  pid_t pid = -1;
  int input = -1, output = -1;
#endif
  int status = -1;
  bool ended = false;
  ~Child() {
    terminate();
  }
  void start(std::vector<std::string> args, const std::map<std::string, std::string> &extra = {}) {
    require(!args.empty(), "Missing subprocess command");
    std::map<std::string, std::string> vars;
#if defined(_WIN32)
    LPWCH raw = GetEnvironmentStringsW();
    for (auto p = raw; *p; p += wcslen(p) + 1) {
      std::wstring item(p);
      auto n = item.find(L'=');
      if (n != std::wstring::npos && n)
        vars[fs::path(item.substr(0, n)).u8string()] = fs::path(item.substr(n + 1)).u8string();
    }
    FreeEnvironmentStringsW(raw);
#else
    for (char **p = environ; *p; ++p) {
      std::string item(*p);
      auto n = item.find('=');
      if (n != std::string::npos)
        vars[item.substr(0, n)] = item.substr(n + 1);
    }
#endif
    for (auto &[key, value] : extra)
      vars[key] = value;
    vars["GIT_TERMINAL_PROMPT"] = "0";
#if defined(_WIN32)
    SECURITY_ATTRIBUTES inherit{sizeof inherit, nullptr, TRUE};
    HANDLE ci = nullptr, co = nullptr;
    require(CreatePipe(&ci, &input, &inherit, 65536) && CreatePipe(&output, &co, &inherit, 65536),
            "Cannot create child pipes");
    SetHandleInformation(input, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(output, HANDLE_FLAG_INHERIT, 0);
    std::wstring line;
    for (auto &arg : args) {
      if (!line.empty())
        line += L' ';
      line += L'"';
      size_t slashes = 0;
      for (wchar_t ch : fs::u8path(arg).wstring()) {
        if (ch == L'\\') {
          ++slashes;
          continue;
        }
        line.append(ch == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        line += ch;
        slashes = 0;
      }
      line.append(slashes * 2, L'\\');
      line += L'"';
    }
    std::wstring environment;
    for (auto &[key, value] : vars) {
      environment += fs::u8path(key + "=" + value).wstring();
      environment += L'\0';
    }
    environment += L'\0';
    HANDLE quiet = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                               OPEN_EXISTING, 0, nullptr);
    require(quiet != INVALID_HANDLE_VALUE, "Cannot redirect child diagnostics");
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof startup;
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = ci;
    startup.StartupInfo.hStdOutput = co;
    startup.StartupInfo.hStdError = quiet;
    PROCESS_INFORMATION pi{};
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<unsigned char> storage(bytes);
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    require(InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &bytes) != 0,
            "Cannot isolate child handles");
    HANDLE inherited[] = {ci, co, quiet};
    bool good = UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                          inherited, sizeof inherited, nullptr, nullptr) != 0;
    job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    good = good && job &&
           SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits) &&
           CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE,
                          CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW |
                              EXTENDED_STARTUPINFO_PRESENT,
                          environment.data(), nullptr, &startup.StartupInfo, &pi);
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    CloseHandle(ci);
    CloseHandle(co);
    CloseHandle(quiet);
    require(good, "Cannot start native child");
    process = pi.hProcess;
    require(AssignProcessToJobObject(job, process) != 0, "Cannot assign child lifetime group");
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

#else
    int in[2], out[2];
    require(::pipe(in) == 0, "Cannot create child input");
    if (::pipe(out) != 0) {
      ::close(in[0]);
      ::close(in[1]);
      throw std::runtime_error("Cannot create child output");
    }
    for (int fd : {in[0], in[1], out[0], out[1]})
      ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    std::vector<std::string> environment;
    for (auto &[key, value] : vars)
      environment.push_back(key + "=" + value);
    std::vector<char *> av, ev;
    for (auto &a : args)
      av.push_back(a.data());
    av.push_back(nullptr);
    for (auto &e : environment)
      ev.push_back(e.data());
    ev.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, in[0], 0);
    posix_spawn_file_actions_adddup2(&actions, out[1], 1);
    posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
    for (int fd : {in[0], in[1], out[0], out[1]})
      posix_spawn_file_actions_addclose(&actions, fd);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attr, 0);
    int result = posix_spawnp(&pid, args[0].c_str(), &actions, &attr, av.data(), ev.data());
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    ::close(in[0]);
    ::close(out[1]);
    if (result) {
      ::close(in[1]);
      ::close(out[0]);
      throw std::runtime_error("Cannot start native child");
    }
    input = in[1];
    output = out[0];
    ::fcntl(output, F_SETFL, ::fcntl(output, F_GETFL, 0) | O_NONBLOCK);
#endif
  }
  bool running() {
    if (ended)
      return false;
#if defined(_WIN32)
    DWORD code = 0;
    GetExitCodeProcess(process, &code);
    if (code == STILL_ACTIVE)
      return true;
    status = static_cast<int>(code);
#else
    int result = 0;
    auto got = waitpid(pid, &result, WNOHANG);
    if (got == 0)
      return true;
    if (got < 0) {
      status = 128;
    } else
      status = WIFEXITED(result) ? WEXITSTATUS(result) : 128 + WTERMSIG(result);
#endif
    ended = true;
    return false;
  }
  std::string receive() {
    char data[8192];
#if defined(_WIN32)
    DWORD available = 0, n = 0;
    if (!PeekNamedPipe(output, nullptr, 0, nullptr, &available, nullptr) || !available)
      return {};
    if (!ReadFile(output, data, std::min<DWORD>(available, sizeof data), &n, nullptr))
      return {};
    return std::string(data, n);
#else
    auto n = ::read(output, data, sizeof data);
    return n > 0 ? std::string(data, static_cast<size_t>(n)) : "";
#endif
  }
  void send(const std::string &data) {
#if defined(_WIN32)
    DWORD n = 0;
    require(WriteFile(input, data.data(), static_cast<DWORD>(data.size()), &n, nullptr) && n == data.size(),
            "Agent input closed");
#else
    require(::write(input, data.data(), data.size()) == static_cast<ssize_t>(data.size()),
            "Agent input closed");
#endif
  }
  void terminate() {
#if defined(_WIN32)
    if (process) {
      if (!ended)
        TerminateJobObject(job, 130);
      WaitForSingleObject(process, 5000);
      CloseHandle(process);
      process = nullptr;
    }
    if (job) {
      CloseHandle(job);
      job = nullptr;
    }
    if (input) {
      CloseHandle(input);
      input = nullptr;
    }
    if (output) {
      CloseHandle(output);
      output = nullptr;
    }
#else
    if (pid > 0 && !ended) {
      ::kill(-pid, SIGKILL);
      int code = 0;
      ::waitpid(pid, &code, 0);
      ended = true;
      status = 130;
    }
    if (input >= 0) {
      ::close(input);
      input = -1;
    }
    if (output >= 0) {
      ::close(output);
      output = -1;
    }
#endif
  }
};
std::string command(std::vector<std::string> args, int timeout = 30,
                    const std::map<std::string, std::string> &env = {}) {
  Child child;
  child.start(std::move(args), env);
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout);
  std::string out;
  while (true) {
    auto part = child.receive();
    out += part;
    require(out.size() <= 20 * 1024 * 1024, "Subprocess output too large");
    if (!child.running()) {
      while (!(part = child.receive()).empty())
        out += part;
      break;
    }
    require(std::chrono::steady_clock::now() < deadline, "Subprocess timed out");
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  require(child.status == 0, "Command failed; inspect workspace and configuration");
  return out;
}
// Project stored model context into user-facing text without changing model input.
static std::string visible_user_message(const std::string &value) {
  const std::string environment_end = "</environment_context>";
  const bool contextual = value.rfind("# Available skills\n", 0) == 0 ||
                          value.rfind("<environment_context>\n", 0) == 0;
  if (contextual) {
    auto end = value.find(environment_end);
    if (end != std::string::npos) {
      end += environment_end.size();
      if (value.compare(end, std::string("\n\n<current_goal>").size(), "\n\n<current_goal>") == 0) {
        auto goal_end = value.find("</current_goal>", end + std::string("\n\n<current_goal>").size());
        if (goal_end == std::string::npos) return value;
        end = goal_end + std::string("</current_goal>").size();
      }
      if (value.compare(end, 2, "\n\n") == 0) return value.substr(end + 2);
    }
  }
  // Older sessions used the workspace/sandbox text wrapper.
  if (value.rfind("Workspace root:", 0) == 0) {
    const std::string marker = "\nUser task:\n";
    auto task = value.find(marker);
    if (task != std::string::npos) return value.substr(task + marker.size());
  }
  return value;
}
struct App {
  Workspace workspace;
  fs::path binary, home, auth_home, state_path, registry_path;
  std::recursive_mutex mutex;
  std::thread worker;
  std::unique_ptr<Child> child;
  bool busy = false, stopping = false;
  std::string sid, error, draft, reasoning_draft, permission = "workspace", active = "codex", pending_id;
  J pending, events = arr(), messages = arr(), submissions = obj(),
             registry = obj({{"providers", arr()}, {"active", str("codex")}});
  Options options;
  std::map<std::string, std::string> secrets;
  std::string display_model, journal_key;
  J journal_cache = arr();
  std::map<std::string, std::string> title_cache;
  App(fs::path root, fs::path self, fs::path state, Options initial, bool explicit_options,
      bool isolated_credentials = false)
      : workspace(root), binary(self), home(state), options(std::move(initial)) {
    private_dir(home);
    auth_home = isolated_credentials ? home / "agent-home" : home;
    private_dir(auth_home);
    state_path = home / ("web-" + digest(workspace.root.generic_u8string()).substr(0, 24) + ".json");
    registry_path = home / "web-providers.json";
    auto saved = read_json(state_path);
    sid = text(saved, "session_id");
    require(sid.empty() || identifier(sid), "Invalid saved session");
    messages = field(saved, "messages", arr());
    submissions = field(saved, "submissions", obj());
    if (flag(saved, "busy"))
      error = "Previous task was interrupted. Inspect files before submitting again.";
    if (fs::exists(registry_path))
      registry = read_json(registry_path);
    if (!registry.get("providers") || registry.get("providers")->kind != J::Array)
      registry = obj({{"providers", arr()}, {"active", str("codex")}});
    if (explicit_options) {
      active = "cli";
      display_model = options.model;
      std::string why;
      require(configure_provider(options, why), why);
    } else
      use_provider(text(registry, "active", "codex"), false);
  }
  ~App() {
    {
      std::lock_guard<std::recursive_mutex> lock(mutex);
      stopping = true;
      if (child)
        child->terminate();
    }
    finish();
  }
  void finish() {
    if (worker.joinable())
      worker.join();
  }
  void save() {
    atomic_save(state_path, obj({{"session_id", str(sid)},
                                 {"messages", messages},
                                 {"submissions", submissions},
                                 {"busy", boolean(busy)}}));
  }
  J provider(const std::string &id) {
    if (id == "codex")
      return obj({{"id", str(id)}, {"name", str("Codex")}, {"models", arr()}, {"builtin", boolean(true)}});
    if (id == "cli")
      return obj({{"id", str(id)},
                  {"name", str("命令行：" + options.provider)},
                  {"models", arr()},
                  {"builtin", boolean(true)}});
    for (auto &p : registry.fields["providers"].items)
      if (text(p, "id") == id) {
        auto out = p;
        out.fields.erase("key");
        out.fields["has_key"] = boolean(!text(p, "key").empty());
        return out;
      }
    throw std::runtime_error("Provider not found");
  }
  void use_provider(const std::string &id, bool persist = true) {
    require(!busy, "Wait for current task to finish");
    Options next;
    next.root = workspace.root;
    std::map<std::string, std::string> keys;
    display_model.clear();
    if (id != "codex") {
      J p;
      for (auto &item : registry.fields["providers"].items)
        if (text(item, "id") == id)
          p = item;
      require(p.kind == J::Object, "Provider not found");
      next.provider = "custom";
      next.base_url = text(p, "base_url");
      next.api_style = text(p, "api") == "chat" ? ApiStyle::ChatCompletions : ApiStyle::Responses;
      next.api_key_env = "MY_API_KEY";
      next.no_api_key = text(p, "key").empty();
      if (!next.no_api_key)
        keys["MY_API_KEY"] = text(p, "key");
      auto models = field(p, "models", arr());
      if (!models.items.empty())
        display_model = models.items[0].str();
      next.model = display_model.empty() ? "default" : display_model;
    }
    std::string why;
    require(configure_provider(next, why), why);
    options = next;
    secrets = std::move(keys);
    active = id;
    if (persist) {
      registry.fields["active"] = str(id);
      atomic_save(registry_path, registry);
    }
  }
  std::vector<std::string> agent_args() {
    std::vector<std::string> args = {binary.u8string(), "--root",         workspace.root.u8string(),
                                     "--provider",      options.provider, "--model",
                                     options.model};
    for (auto it = penguin_manifests.rbegin(); it != penguin_manifests.rend(); ++it) {
      const auto frozen = penguin_worker_manifests.find(*it);
      if(frozen == penguin_worker_manifests.end())args.insert(args.begin()+1,{"--penguin",*it});
      else args.insert(args.begin()+1,{"--penguin-reviewed",frozen->second,penguin_reviews.at(*it)});
    }
    if (!options.base_url.empty()) {
      args.insert(args.end(), {"--base-url", options.base_url, "--api",
                               options.api_style == ApiStyle::Responses ? "responses" : "chat"});
    }
    if (options.no_api_key)
      args.push_back("--no-api-key");
    else if (!options.api_key_env.empty())
      args.insert(args.end(), {"--api-key-env", options.api_key_env});
    if (!options.reasoning_effort.empty())
      args.insert(args.end(), {"--reasoning", options.reasoning_effort});
    return args;
  }
  J journal() {
    std::vector<J> rows;
    if (sid.empty())
      return arr();
    std::string cache_key;
    std::error_code cache_error;
    auto file = workspace.root / ".codex_cpp/sessions" / (sid + ".items.jsonl");
    auto stamp = fs::last_write_time(file, cache_error);
    if (!cache_error) {
      // libc++ may use a 128-bit filesystem clock representation. Split the
      // duration before formatting, preserving subsecond precision without
      // overflowing nanoseconds for clocks with an older epoch (e.g. Windows).
      auto elapsed = stamp.time_since_epoch();
      auto seconds = std::chrono::duration_cast<std::chrono::seconds>(elapsed);
      auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed - seconds);
      cache_key = sid + ":" + std::to_string(seconds.count()) + ":" + std::to_string(nanos.count()) +
                  ":" + std::to_string(fs::file_size(file, cache_error));
      if (!cache_error && cache_key == journal_key)
        return journal_cache;
    }
    std::string source;
    try {
      source = workspace.read(".codex_cpp/sessions/" + sid + ".items.jsonl", true);
    } catch (...) {
      return arr();
    }
    std::istringstream in(source);
    std::string line;
    std::map<std::string, size_t> calls;
    while (std::getline(in, line)) {
      auto item = parse_json(line);
      if (!item)
        continue;
      auto role = text(*item, "role"), kind = text(*item, "type");
      if (kind == "codex_reasoning") {
        rows.push_back(obj({{"k", str("reasoning")}, {"text", field(*item, "text")}}));
      } else if (role == "user" || role == "assistant") {
        auto content = field(*item, "content");
        std::string value;
        if (content.kind == J::String)
          value = content.str();
        else
          for (auto &p : content.items)
            value += text(p, "text");
        if (role == "user") value = visible_user_message(value);
        if (!value.empty())
          rows.push_back(obj({{"k", str(role)}, {"text", str(value)}}));
      } else if (kind == "function_call") {
        calls[text(*item, "call_id")] = rows.size();
        rows.push_back(obj({{"k", str("tool")},
                            {"id", field(*item, "call_id")},
                            {"name", field(*item, "name")},
                            {"args", str(text(*item, "arguments").substr(0, 30000))},
                            {"args_truncated", boolean(text(*item, "arguments").size() > 30000)},
                            {"output", J{}}}));
      } else if (kind == "function_call_output") {
        auto found = calls.find(text(*item, "call_id"));
        if (found != calls.end()) {
          auto output = text(*item, "output");
          rows[found->second].fields["output"] = str(output.substr(0, 12000));
          rows[found->second].fields["output_truncated"] = boolean(output.size() > 12000);
        }
      }
    }
    if (rows.size() > 400)
      rows.erase(rows.begin(), rows.end() - 400);
    journal_key = cache_key;
    journal_cache = arr(rows);
    return journal_cache;
  }
  J sessions() {
    std::vector<J> result;
    std::error_code ec;
    auto directory = workspace.root / ".codex_cpp/sessions";
    if (fs::is_symlink(directory) || fs::is_symlink(directory.parent_path()))
      return arr();
    for (fs::directory_iterator it(directory, ec), end; it != end && !ec; it.increment(ec)) {
      auto id = it->path().stem().u8string();
      if (!identifier(id) || it->path().extension() != ".jsonl" || it->is_symlink() || !it->is_regular_file())
        continue;
      std::string title = id;
      try {
        auto metadata = parse_json(workspace.read(".codex_cpp/sessions/" + id + ".meta.json", true));
        if (metadata)
          title = text(*metadata, "name", id);
      } catch (...) {
      }
      if (title.empty() || title == id) {
        auto found = title_cache.find(id);
        if (found != title_cache.end())
          title = found->second;
        else
          try {
            std::istringstream input(workspace.read(".codex_cpp/sessions/" + id + ".items.jsonl", true));
            std::string line;
            while (std::getline(input, line)) {
              auto item = parse_json(line);
              if (!item || text(*item, "role") != "user")
                continue;
              auto content = field(*item, "content");
              std::string value = content.str();
              if (content.kind == J::Array)
                for (auto &part : content.items)
                  value += text(part, "text");
              value = visible_user_message(value);
              title = value.substr(0, value.find('\n')).substr(0, 72);
              if (!title.empty())
                title_cache[id] = title;
              break;
            }
          } catch (...) {
          }
      }
      result.push_back(obj(
          {{"id", str(id)},
           {"title", str(title)},
           {"selected", boolean(id == sid)},
           {"updated", number(static_cast<long long>(it->last_write_time().time_since_epoch().count()))}}));
      if (result.size() >= 100)
        break;
    }
    std::sort(result.begin(), result.end(), [](const J &a, const J &b) {
      return std::stoll(a.fields.at("updated").value) > std::stoll(b.fields.at("updated").value);
    });
    return arr(result);
  }
  J snapshot() {
    return obj({{"session_id", sid.empty() ? J{} : str(sid)},
                {"busy", boolean(busy)},
                {"timeline", journal()},
                {"draft", str(draft)},
                {"reasoning_draft", str(reasoning_draft)},
                {"messages", messages},
                {"events", events},
                {"pending", pending},
                {"error", str(error)},
                {"model", str(display_model)},
                {"reasoning", str(options.reasoning_effort)},
                {"permission", str(permission)},
                {"provider", provider(active)},
                {"sessions", sessions()}});
  }
  void start(const std::string &prompt, const std::string &rid, bool login = false) {
    require(!busy, "A task is already running");
    if (!login) {
      require(!trim(prompt).empty() && prompt.size() <= 64000 && prompt.find('\0') == std::string::npos,
              "Message must contain 1..64000 bytes");
      require(identifier(rid), "request_id required");
    }
    if (worker.joinable())
      worker.join();
    busy = true;
    stopping = false;
    error.clear();
    events = arr();
    pending = J{};
    draft.clear();
      reasoning_draft.clear();
    if (!login) {
      submissions.fields[rid] = str(digest(prompt));
      while (submissions.fields.size() > 1000)
        submissions.fields.erase(submissions.fields.begin());
      messages.items.push_back(obj({{"role", str("user")}, {"content", str(prompt)}}));
    }
    save();
    auto args = agent_args();
    auto environment = secrets;
    environment["CODEX_CPP_HOME"] = auth_home.u8string();
    if (login)
      args = {binary.u8string(), "--provider", "codex", "--login"};
    else {
      args.insert(args.end(), {"--json", "--sandbox",
                               permission == "full"       ? "danger-full-access"
                               : permission == "readonly" ? "read-only"
                                                          : "workspace-write",
                               "--approval", permission == "full" ? "never" : "on-request"});
      if (!sid.empty())
        args.insert(args.end(), {"--resume", sid});
      args.insert(args.end(), {"--", prompt});
    }
#if defined(__linux__)
    args.insert(args.begin() + 1, {"--native-child", std::to_string(::getpid())});
#endif
    worker = std::thread([this, args, environment, login]() {
      try {
        auto local = std::make_unique<Child>();
        local->start(args, environment);
        {
          std::lock_guard<std::recursive_mutex> lock(mutex);
          child = std::move(local);
        }
        std::string buffer;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(30),
             approval_deadline = deadline;
        while (true) {
          std::string bytes;
          bool running;
          {
            std::lock_guard<std::recursive_mutex> lock(mutex);
            bytes = child->receive();
            running = child->running();
            if (stopping) {
              child->terminate();
              running = false;
            }
            if (!pending_id.empty() && std::chrono::steady_clock::now() > approval_deadline) {
              child->send("n\n");
              pending_id.clear();
              pending = J{};
            }
          }
          if (!bytes.empty()) {
            buffer += bytes;
            require(buffer.size() <= 2 * 1024 * 1024, "Agent frame exceeds limit");
            size_t newline;
            while ((newline = buffer.find('\n')) != std::string::npos) {
              auto line = buffer.substr(0, newline);
              buffer.erase(0, newline + 1);
              std::lock_guard<std::recursive_mutex> lock(mutex);
              if (login) {
                events.items.push_back(
                    obj({{"type", str("login.notice")}, {"payload", obj({{"text", str(line)}})}}));
                continue;
              }
              auto event = parse_json(line);
              if (!event || event->kind != J::Object)
                continue;
              auto id = text(*event, "session_id");
              if (identifier(id) && id.rfind("session-", 0) == 0)
                sid = id;
              auto kind = text(*event, "type");
              auto payload = field(*event, "payload", obj());
              if (kind == "model.reasoning.delta")
                reasoning_draft += text(payload, "text");
              if (kind == "model.delta")
                draft += text(payload, "text");
              if (kind == "model.output") { draft.clear(); reasoning_draft.clear(); }
              if (kind == "approval.requested") {
                pending = payload;
                pending.fields["tool_call_id"] = field(payload, "call_id");
                pending_id = entropy();
                pending.fields["call_id"] = str(pending_id);
                approval_deadline = std::chrono::steady_clock::now() + std::chrono::minutes(10);
              }
              if (kind == "approval.decision") {
                pending = J{};
                pending_id.clear();
              }
              events.items.push_back(*event);
              if (events.items.size() > 100)
                events.items.erase(events.items.begin());
            }
          }
          if (!running) {
            if (bytes.empty())
              break;
          }
          require(std::chrono::steady_clock::now() < deadline, "Agent task timed out");
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        std::lock_guard<std::recursive_mutex> lock(mutex);
        if (stopping)
          error = "Task stopped";
        else if (child->status)
          error = "Agent failed (exit " + std::to_string(child->status) +
                  "). Check provider credentials or sandbox availability.";
      } catch (const std::exception &e) {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        error = e.what();
      }
      std::lock_guard<std::recursive_mutex> lock(mutex);
      if (child) {
        child->terminate();
        child.reset();
      }
      busy = false;
      pending = J{};
      pending_id.clear();
      draft.clear();
      reasoning_draft.clear();
      auto rows = journal();
      messages = arr();
      for (auto &row : rows.items)
        if (text(row, "k") == "user" || text(row, "k") == "assistant")
          messages.items.push_back(obj({{"role", field(row, "k")}, {"content", field(row, "text")}}));
      try {
        save();
      } catch (...) {
        error = "Cannot persist completed task state";
      }
    });
  }
  J rpc(const J &request) {
    std::unique_lock<std::recursive_mutex> lock(mutex);
    auto op = text(request, "op");
    if (op == "list")
      return obj({{"projects", arr({str("local")})}});
    if (op == "snapshot")
      return snapshot();
    if (op == "tool_log") {
      require(!sid.empty() && text(request, "session_id") == sid, "Session changed; reopen the tool log");
      auto call_id = text(request, "call_id");
      require(!call_id.empty() && call_id.size() <= 256, "Invalid tool call ID");
      // Read only the current App's journal, preserving hosted scope/membership checks.
      std::istringstream input(workspace.read(".codex_cpp/sessions/" + sid + ".items.jsonl", true));
      std::string line;
      J result = obj();
      bool found = false;
      while (std::getline(input, line)) {
        auto item = parse_json(line);
        if (!item || text(*item, "call_id") != call_id) continue;
        auto kind = text(*item, "type");
        if (kind == "function_call") {
          result = obj({{"name", field(*item, "name")}, {"arguments", field(*item, "arguments")}, {"output", J{}}});
          found = true;
        } else if (kind == "function_call_output" && found) result.fields["output"] = field(*item, "output");
      }
      require(found, "Tool call not found in the current session");
      return result;
    }
    if (op == "status")
      return obj({{"workspace", str(workspace.root.u8string())}, {"mode", str("local")}});
    if (op == "send") {
      auto rid = text(request, "request_id"), prompt = text(request, "text");
      auto existing = submissions.get(rid);
      if (existing) {
        require(existing->str() == digest(prompt), "request_id already used for another message");
        return obj({{"submitted", boolean(true)}, {"duplicate", boolean(true)}});
      }
      require(options.provider != "custom" || !display_model.empty(), "Select a model before sending");
      start(prompt, rid);
      return obj({{"submitted", boolean(true)}});
    }
    if (op == "login") {
      start("", "", true);
      return obj({{"started", boolean(true)}});
    }
    if (op == "stop") {
      stopping = true;
      if (child)
        child->terminate();
      return obj({{"stopped", boolean(true)}});
    }
    if (op == "approve") {
      auto value = request.get("allow");
      require(value && value->kind == J::Other && (value->value == "true" || value->value == "false"),
              "Explicit approval decision required");
      require(child && !pending_id.empty() && same(text(request, "approval"), pending_id),
              "Approval is stale");
      child->send(flag(request, "allow") ? "y\n" : "n\n");
      pending = J{};
      pending_id.clear();
      return obj({{"recorded", boolean(true)}});
    }
    if (op == "models") {
      auto args = agent_args();
      args.push_back("--list-models");
#if defined(__linux__)
      args.insert(args.begin() + 1, {"--native-child", std::to_string(::getpid())});
#endif
      auto environment = secrets;
      environment["CODEX_CPP_HOME"] = auth_home.u8string();
      lock.unlock();
      try {
        auto raw = command(args, 25, environment);
        auto models = parse_json(raw);
        require(models && models->kind == J::Array, "Invalid model list");
        return obj({{"models", *models}, {"error", str("")}});
      } catch (const std::exception &e) {
        return obj({{"models", arr()},
                    {"error",
                     str(std::string("Model list unavailable. Check provider, credentials and OS sandbox: ") +
                         e.what())}});
      }
    }

    if (op == "files")
      return workspace.files();
    if (op == "read") {
      auto data = workspace.read(text(request, "path"));
      require(data.size() <= 1024 * 1024, "File exceeds editor limit");
      bool editable = data.find('\0') == std::string::npos && valid_utf8(data);
      auto shown = data;
      if (!valid_utf8(shown)) {
        shown.clear();
        for (unsigned char c : data)
          shown += c < 128 ? std::string(1, static_cast<char>(c)) : std::string("\xef\xbf\xbd");
      }
      return obj({{"text", str(shown)}, {"version", str(digest(data))}, {"editable", boolean(editable)}});
    }
    if (op == "write") {
      auto value = request.get("text");
      require(value && value->kind == J::String, "Text is required");
      return workspace.write(text(request, "path"), value->str(), text(request, "base"));
    }
    if (op == "diff") {
      return git_review(workspace.root);
    }
    if (op == "providers") {
      std::vector<J> result = {provider("codex")};
      if (active == "cli")
        result.push_back(provider("cli"));
      for (auto &p : registry.fields["providers"].items)
        result.push_back(provider(text(p, "id")));
      return obj({{"active", str(active)}, {"providers", arr(result)}});
    }
    if (op == "provider_save") {
      auto id = text(request, "id");
      J previous;
      for (auto &p : registry.fields["providers"].items)
        if (text(p, "id") == id)
          previous = p;
      require(id.empty() || previous.kind == J::Object, "Provider not found");
      if (id.empty())
        id = "p" + entropy().substr(0, 8);
      require(active != id || !busy, "Wait for task completion");
      auto name = trim(text(request, "name")), url = text(request, "base_url"), api = text(request, "api");
      require(!name.empty() && name.size() <= 240, "Provider name is required");
      require((url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0) && url.size() <= 2048 &&
                  url.find_first_of("@?#\r\n\t ") == std::string::npos && url.find("://") + 3 < url.size(),
              "Use an HTTP(S) URL without embedded credentials or query");
      require(api == "chat" || api == "responses", "Invalid API style");
      while (!url.empty() && url.back() == '/')
        url.pop_back();
      auto key = text(request, "key", text(previous, "key"));
      if (key.empty() && !flag(request, "clear_key"))
        key = text(previous, "key");
      if (flag(request, "clear_key"))
        key.clear();
      require(key.size() <= 4096 &&
                  std::none_of(key.begin(), key.end(), [](unsigned char c) { return c < 32 || c == 127; }),
              "Invalid API key");
      auto raw = field(request, "models", arr());
      std::vector<J> models;
      if (raw.kind == J::String) {
        std::string list = raw.str();
        for (char &c : list)
          if (c == ',' || c == ';' || c == '\n' || c == '\r' || c == '\t')
            c = ' ';
        std::istringstream input(list);
        std::string model;
        while (input >> model) {
          require(model_name(model), "Invalid model name");
          models.push_back(str(model));
        }
      } else {
        require(raw.kind == J::Array, "Invalid models list");
        for (auto &m : raw.items) {
          require(m.kind == J::String && model_name(m.str()), "Invalid model name");
          if (!m.str().empty())
            models.push_back(m);
        }
      }
      require(models.size() <= 50, "At most 50 models per provider");
      J cleaned = obj({{"id", str(id)},
                       {"name", str(name)},
                       {"base_url", str(url)},
                       {"api", str(api)},
                       {"models", arr(models)},
                       {"key", str(key)}});
      bool replaced = false;
      for (auto &p : registry.fields["providers"].items)
        if (text(p, "id") == id) {
          p = cleaned;
          replaced = true;
        }
      if (!replaced) {
        require(registry.fields["providers"].items.size() < 30, "Provider limit reached");
        registry.fields["providers"].items.push_back(cleaned);
      }
      atomic_save(registry_path, registry);
      if (active == id)
        use_provider(id);
      return obj({{"id", str(id)}});
    }
    require(!busy, "Wait for task completion");
    if (op == "provider_use") {
      use_provider(text(request, "id"));
      return obj({{"active", str(active)}});
    }
    if (op == "provider_delete") {
      auto id = text(request, "id");
      require(id != "codex" && id != "cli", "Cannot delete a built-in provider");
      provider(id);
      if (active == id)
        use_provider("codex");
      auto &list = registry.fields["providers"].items;
      list.erase(std::remove_if(list.begin(), list.end(), [&](const J &p) { return text(p, "id") == id; }),
                 list.end());
      atomic_save(registry_path, registry);
      return obj({{"deleted", boolean(true)}});
    }
    if (op == "session") {
      auto value = field(request, "session_id");
      auto id = value.kind == J::Null ? "" : value.str();
      require(id.empty() || identifier(id), "Invalid session ID");
      if (!id.empty())
        workspace.read(".codex_cpp/sessions/" + id + ".items.jsonl", true);
      sid = id;
      messages = arr();
      events = arr();
      draft.clear();
      reasoning_draft.clear();
      error.clear();
      save();
      return obj({{"selected", boolean(true)}});
    }
    if (op == "model") {
      auto id = text(request, "model");
      require(model_name(id), "Invalid model name");
      if (display_model != id)
        options.reasoning_effort.clear();
      display_model = id;
      options.model = id;
      if (options.model.empty()) {
        std::string why;
        if (options.provider == "custom")
          options.model = "default";
        else
          require(configure_provider(options, why), why);
      }
      return obj({{"updated", boolean(true)}});
    }
    if (op == "reasoning") {
      auto effort = text(request, "effort");
      require(std::set<std::string>{"", "none", "minimal", "low", "medium", "high", "xhigh", "max", "ultra"}
                  .count(effort),
              "Invalid reasoning effort");
      options.reasoning_effort = effort == "none" ? "" : effort;
      return obj({{"updated", boolean(true)}});
    }
    if (op == "permission") {
      auto value = text(request, "permission");
      require(value == "workspace" || value == "readonly" || value == "full", "Invalid execution permission");
      permission = value;
      return obj({{"updated", boolean(true)}});
    }
    throw std::runtime_error("Unknown web operation");
  }
};
#if defined(_WIN32)
using Socket = SOCKET;
constexpr Socket invalid_socket = INVALID_SOCKET;
void close_socket(Socket fd) {
  closesocket(fd);
}
#else
using Socket = int;
constexpr Socket invalid_socket = -1;
void close_socket(Socket fd) {
  ::close(fd);
}
#endif
struct Request {
  std::string method, path, body;
  std::map<std::string, std::string> headers;
};
std::string lower(std::string value) {
  for (char &c : value)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return value;
}
bool send_all(Socket fd, const std::string &value) {
  size_t offset = 0;
  while (offset < value.size()) {
    auto n = ::send(fd, value.data() + offset,
                    static_cast<int>(std::min<size_t>(value.size() - offset, 65536)), 0);
    if (n <= 0)
      return false;
    offset += static_cast<size_t>(n);
  }
  return true;
}
void reply(Socket fd, int code, const std::string &body,
           const std::string &mime = "application/json; charset=utf-8", const std::string &cookie = "") {
  auto header = "HTTP/1.1 " + std::to_string(code) + " Response\r\nContent-Type: " + mime +
                "\r\nContent-Length: " + std::to_string(body.size()) +
                "\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: "
                "nosniff\r\nReferrer-Policy: no-referrer\r\nContent-Security-Policy: default-src 'self'; "
                "script-src 'self'; style-src 'self'; connect-src 'self'; img-src 'self' data:; "
                "frame-ancestors 'none'; base-uri 'none'; form-action 'self'\r\n";
  if (!cookie.empty())
    header += "Set-Cookie: " + cookie + "\r\n";
  send_all(fd, header + "\r\n" + body);
}
void response(Socket fd, int code, const J &data, const std::string &cookie = "") {
  reply(fd, code, json_dump(data), "application/json; charset=utf-8", cookie);
}
static std::atomic<bool> stopping_service{false};
Request request(Socket fd) {
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
  auto bounded_receive=[&](char* bytes,size_t size){
    for(;;){
      require(!stopping_service,"Service is stopping");
      require(std::chrono::steady_clock::now()<deadline,"HTTP request deadline exceeded");
      fd_set readers;FD_ZERO(&readers);FD_SET(fd,&readers);timeval tick{0,100000};
      const int ready=::select(static_cast<int>(fd)+1,&readers,nullptr,nullptr,&tick);
      if(ready>0)break;
      require(ready>=0,"Cannot wait for HTTP request");
    }
    return ::recv(fd,bytes,static_cast<int>(size),0);
  };
#if defined(_WIN32)
  DWORD timeout = 10000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char *>(&timeout), sizeof timeout);
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char *>(&timeout), sizeof timeout);
#else
  timeval timeout{10, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
#endif
  std::string data;
  char buffer[8192];
  size_t end;
  while ((end = data.find("\r\n\r\n")) == std::string::npos) {
    auto n = bounded_receive(buffer, sizeof buffer);
    require(n > 0, "Incomplete HTTP request");
    data.append(buffer, static_cast<size_t>(n));
    require(data.size() <= 65536, "HTTP headers too large");
  }
  Request result;
  std::istringstream in(data.substr(0, end));
  std::string version, line;
  in >> result.method >> result.path >> version;
  require(version == "HTTP/1.1" || version == "HTTP/1.0", "Invalid HTTP version");
  std::getline(in, line);
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    auto pos = line.find(':');
    require(pos != std::string::npos, "Invalid HTTP header");
    auto name = lower(line.substr(0, pos));
    require(!result.headers.count(name), "Duplicate HTTP header");
    result.headers[name] = trim(line.substr(pos + 1));
  }
  require(!result.headers.count("transfer-encoding"), "Transfer encoding unsupported");
  size_t length = 0;
  if (result.headers.count("content-length")) {
    auto value = result.headers["content-length"];
    require(!value.empty() && value.size() <= 8 &&
                std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isdigit(c); }),
            "Invalid content length");
    length = static_cast<size_t>(std::stoul(value));
  }
  require(length <= 2 * 1024 * 1024, "HTTP body too large");
  result.body = data.substr(end + 4);
  while (result.body.size() < length) {
    auto n = bounded_receive(buffer, sizeof buffer);
    require(n > 0, "Incomplete HTTP body");
    result.body.append(buffer, static_cast<size_t>(n));
  }
  require(result.body.size() == length, "Unexpected bytes after HTTP body");
  auto query = result.path.find('?');
  if (query != std::string::npos)
    result.path.resize(query);
  return result;
}
struct WebSession {
  std::string user, csrf;
  std::time_t expires = 0;
};

static_assert(std::atomic<bool>::is_always_lock_free, "Signal flag must be lock-free");
void stop_service(int) {
  stopping_service = 1;
}
struct Service {
  fs::path binary, web, home;
  std::string origin, host = "127.0.0.1", authority;
  int port = 8765;
  bool direct_access = false;
  std::unique_ptr<App> local;
  std::recursive_mutex mutex;
  std::map<std::string, WebSession> sessions;
  std::map<std::string, std::pair<std::time_t, int>> failures;
  virtual ~Service() = default;
  virtual bool hosted() const {
    return false;
  }
  virtual bool login(const std::string &, const std::string &) {
    return false;
  }
  virtual J dispatch(const std::string &, const J &message) {
    return local->rpc(message);
  }
  virtual std::string mode() const {
    return "local";
  }
  virtual void before_listen() {}
  virtual void begin_shutdown() {
    if (local) {
      std::lock_guard<std::recursive_mutex> lock(local->mutex);
      local->stopping = true;
      if (local->child)
        local->child->terminate();
    }
  }
  std::string cookie_name() const {return (hosted()?"codex_session_":"codex_local_session_")+std::to_string(port);}
  std::string cookie(const std::string &token) {
    return cookie_name()+"=" + token + "; Path=/; HttpOnly; SameSite=Strict; Max-Age=28800" +
           (origin.rfind("https://", 0) == 0 ? std::string("; Secure") : "");
  }
  std::pair<std::string, WebSession> authenticate(const Request &req) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    auto raw = req.headers.count("cookie") ? req.headers.at("cookie") : "";
    std::istringstream stream(raw);
    std::string part, token;
    while (std::getline(stream, part, ';')) {
      part = trim(part);
      auto prefix=cookie_name()+"=";
      if (part.rfind(prefix, 0) == 0)
        token = part.substr(prefix.size());
    }
    auto found = sessions.find(token);
    if (found != sessions.end() && found->second.expires > std::time(nullptr))
      return *found;
    return {};
  }
  std::pair<std::string, WebSession> issue(const std::string &user) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    auto now = std::time(nullptr);
    for (auto it = sessions.begin(); it != sessions.end();)
      if (it->second.expires <= now)
        it = sessions.erase(it);
      else
        ++it;
    require(sessions.size() < 2048, "Too many active sessions");
    auto token = entropy();
    WebSession item{user, entropy(), now + 28800};
    sessions[token] = item;
    return {token, item};
  }
  void handle(Socket fd) {
    try {
      auto req = request(fd);
      auto allowed_origin=origin;
      if(direct_access){
        auto incoming=req.headers["host"];auto colon=incoming.rfind(':');
        auto address=colon==std::string::npos?incoming:incoming.substr(0,colon);
        auto incoming_port=colon==std::string::npos?"80":incoming.substr(colon+1);
        require(incoming_port==std::to_string(port),"Host port refused");
        bool local_name=address=="localhost"&&(host=="0.0.0.0"||host.rfind("127.",0)==0);
        require(local_name||(ipv4_address(address)&&(host=="0.0.0.0"||address==host)),"Direct mode requires a listener IP; configure --origin for DNS names");
        allowed_origin=browser_origin("http://"+incoming);
      }else require(req.headers["host"] == authority, "Host refused");
      if (req.headers.count("origin"))
        require(req.headers["origin"] == allowed_origin, "Origin refused");
      if (req.path.rfind("/api/", 0) == 0 && req.headers.count("sec-fetch-site"))
        require(req.headers["sec-fetch-site"] == "same-origin" || req.headers["sec-fetch-site"] == "none",
                "Cross-site request refused");
      if (req.method == "GET" && (req.path == "/" || req.path == "/app.js" || req.path == "/style.css" || req.path == "/penguin.js")) {
        auto name = req.path == "/" ? "index.html" : req.path.substr(1);
        auto mime = req.path == "/"         ? "text/html; charset=utf-8"
                    : (req.path == "/app.js" || req.path == "/penguin.js") ? "text/javascript; charset=utf-8"
                                            : "text/css; charset=utf-8";
        reply(fd, 200, web.empty()?std::string(codex_resource(name)):read_bytes(web / name), mime);
        return;
      }
      auto authenticated = authenticate(req);
      if (req.method == "GET" && req.path == "/api/me") {
        bool fresh = false;
        if (authenticated.first.empty() && !hosted()) {
          authenticated = issue("local");
          fresh = true;
        }
        if (authenticated.first.empty()) {
          response(fd, 401, obj({{"error", str("Login required")}, {"mode", str(mode())}}));
          return;
        }
        auto &user = authenticated.second;
        response(fd, 200,
                 obj({{"username", str(user.user)},
                      {"csrf", str(user.csrf)},
                      {"expires", number(user.expires)},
                      {"penguins", penguin_web_catalog()},
                      {"mode", str(mode())}}),
                 fresh ? cookie(authenticated.first) : "");
        return;
      }
      if (req.method == "GET" && req.path == "/api/penguins") {
        require(!authenticated.first.empty(), "Login required");
        response(fd, 200, *parse_json(penguin_inspect())); return;
      }
      if (req.method == "GET" && req.path.rfind("/penguin/", 0) == 0) {
        require(!authenticated.first.empty(), "Login required");
        for (const auto& module : penguin_web_modules) {
          const auto base = "/penguin/" + module.id + "/";
          std::string asset;
          if (req.path == base + "entry.js") asset = module.entry;
          if (req.path == base + "style.css") asset = module.style;
          if (asset.empty()) continue;
          auto path = module.root / fs::u8path(asset); std::error_code ec;
          require(within_root(module.root, path) && fs::is_regular_file(path, ec) && !ec && fs::file_size(path, ec) <= 4 * 1024 * 1024 && !ec, "Invalid Penguin asset");
          reply(fd, 200, read_bytes(path), req.path == base + "entry.js" ? "text/javascript; charset=utf-8" : "text/css; charset=utf-8"); return;
        }
        throw std::runtime_error("Penguin asset not found");
      }
      require(req.method == "POST", "Unknown route");
      require(lower(req.headers["content-type"]).rfind("application/json", 0) == 0,
              "Expected application/json");
      auto parsed = parse_json(req.body);
      require(parsed && parsed->kind == J::Object, "Expected JSON object");
      auto &body = *parsed;
      if (req.path == "/api/login") {
        require(hosted(), "Local service uses same-origin sessions");
        auto name = text(body, "username"), password = text(body, "password");
        require(name.size() <= 128 && password.size() <= 4096, "Invalid login");
        {
          std::lock_guard<std::recursive_mutex> lock(mutex);
          auto now = std::time(nullptr);
          for (auto it = failures.begin(); it != failures.end();)
            if (now - it->second.first >= 60)
              it = failures.erase(it);
            else
              ++it;
          require(failures.size() < 4096, "Login capacity reached");
          auto &entry = failures[name];
          if (now - entry.first >= 60)
            entry = {now, 0};
          require(entry.second++ < 10, "Too many login attempts; wait one minute");
        }
        require(login(name, password), "Invalid login");
        auto created = issue(name);
        response(fd, 200, obj({{"ok", boolean(true)}}), cookie(created.first));
        return;
      }
      if (authenticated.first.empty()) {
        response(fd, 401, obj({{"error", str("Login required")}}));
        return;
      }
      require(same(req.headers["x-csrf-token"], authenticated.second.csrf), "CSRF token required");
      if (req.path == "/api/logout") {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        sessions.erase(authenticated.first);
        response(fd, 200, obj({{"ok", boolean(true)}}),
                 cookie_name()+"=; HttpOnly; SameSite=Strict; Path=/; Max-Age=0");
        return;
      }
      require(req.path == "/api/rpc", "Unknown route");
      response(fd, 200, obj({{"result", dispatch(authenticated.second.user, body)}}));
    } catch (const std::exception &e) {
      response(fd, 400, obj({{"error", str(e.what())}}));
    }
  }
  void listen() {
    before_listen();
#if defined(_WIN32)
    WSADATA data{};
    require(WSAStartup(MAKEWORD(2, 2), &data) == 0, "Winsock initialization failed");
#endif
    Socket listener = ::socket(AF_INET, SOCK_STREAM, 0);
    require(listener != invalid_socket, "Cannot create web socket");
#if !defined(_WIN32)
    ::fcntl(listener, F_SETFD, FD_CLOEXEC);
#endif
    int yes = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&yes), sizeof yes);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port));
    require(inet_pton(AF_INET, host.c_str(), &address.sin_addr) == 1, "Use an IPv4 bind address");
    if (::bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof address) != 0) {
      close_socket(listener);
      throw std::runtime_error("Cannot bind web port");
    }
    require(::listen(listener, 32) == 0, "Cannot listen on web port");
    std::mutex queue_mutex;
    std::condition_variable ready;
    std::deque<Socket> queue;
    bool stopped = false;
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i)
      threads.emplace_back([&]() {
        while (true) {
          Socket fd;
          {
            std::unique_lock<std::mutex> lock(queue_mutex);
            ready.wait(lock, [&] { return stopped || !queue.empty(); });
            if (stopped && queue.empty())
              return;
            fd = queue.front();
            queue.pop_front();
          }
          handle(fd);
          close_socket(fd);
        }
      });
    ::signal(SIGINT, stop_service);
    ::signal(SIGTERM, stop_service);
    std::cout << "Codex.cpp native " << mode() << " web: " << origin << "\nNo Python runtime required.\n"
              << std::flush;
    if(hosted())std::cout<<"Listening on "<<host<<":"<<port<<(direct_access?" (direct HTTP; connect using the server IP)":" (configured access origin)")<<"\n"<<std::flush;
    while (!stopping_service) {
      fd_set waiting;
      FD_ZERO(&waiting);
      FD_SET(listener, &waiting);
      timeval delay{0, 200000};
      int selected = ::select(static_cast<int>(listener) + 1, &waiting, nullptr, nullptr, &delay);
      if (selected <= 0)
        continue;
      Socket fd = ::accept(listener, nullptr, nullptr);
      if (fd == invalid_socket)
        continue;
#if !defined(_WIN32)
      ::fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif
      {
        std::lock_guard<std::mutex> lock(queue_mutex);
        if (queue.size() >= 32) {
          close_socket(fd);
          continue;
        }
        queue.push_back(fd);
      }
      ready.notify_one();
    }
    begin_shutdown();
    {
      std::lock_guard<std::mutex> lock(queue_mutex);
      stopped = true;
    }
    ready.notify_all();
    for (auto &thread : threads)
      thread.join();
    close_socket(listener);
  }
};
fs::path executable(const char *argv0) {
  std::error_code ec;
#if defined(_WIN32)
  std::vector<wchar_t> path(32768);
  auto size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  require(size && size < path.size(), "Cannot locate executable");
  return fs::path(std::wstring(path.data(), size));
#elif defined(__APPLE__)
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::vector<char> path(size);
  require(_NSGetExecutablePath(path.data(), &size) == 0, "Cannot locate executable");
  return fs::canonical(path.data());
#else
  auto path = fs::read_symlink("/proc/self/exe", ec);
  if (!ec)
    return path;
  return fs::canonical(argv0);
#endif
}
#include "codex_agent_git.inc"
#include "codex_web_multi.inc"
#include "codex_web_team.inc"
void child_mode(int &argc, char **&argv, std::vector<char *> &storage) {
#if defined(__linux__)
  if (argc > 2 && std::string(argv[1]) == "--native-child") {
    pid_t parent = static_cast<pid_t>(std::stol(argv[2]));
    require(parent > 1 && ::prctl(PR_SET_PDEATHSIG, SIGKILL) == 0 && ::getppid() == parent,
            "Native parent is gone");
    storage.push_back(argv[0]);
    for (int i = 3; i < argc; ++i)
      storage.push_back(argv[i]);
    argc = static_cast<int>(storage.size());
    storage.push_back(nullptr);
    argv = storage.data();
  }
#else
  (void)argc;
  (void)argv;
  (void)storage;
#endif
}
int main(int argc, char **argv) {
  try {
    std::vector<char*> penguin_argument_storage;
#if defined(_WIN32)
    int wide_count = 0;
    auto wide_args = CommandLineToArgvW(GetCommandLineW(), &wide_count);
    require(wide_args != nullptr, "Cannot read Windows command line");
    std::vector<std::string> utf8_arguments;
    for (int i = 0; i < wide_count; ++i)
      utf8_arguments.push_back(fs::path(wide_args[i]).u8string());
    LocalFree(wide_args);
    std::vector<char *> native_pointers;
    for (auto &value : utf8_arguments)
      native_pointers.push_back(value.data());
    native_pointers.push_back(nullptr);
    argc = wide_count;
    argv = native_pointers.data();
#endif
    penguin_arguments(argc, argv, penguin_argument_storage);
    std::string override_root;
    std::string operation = argc > 1 ? argv[1] : "web", host, origin, add_user, revoke_user;
    int port = 8765;
    auto self = executable(argv[0]);
    fs::path home = own_auth_path().parent_path(), state, assets;
    std::vector<std::string> arguments = {argv[0]};
    bool explicit_options = false;
    for (int i = 2; i < argc; ++i) {
      std::string arg = argv[i];
      if (arg == "--help" || arg == "-h") {
        std::cout << "Native web: codex-cpp web [--root DIR] [--port 8765] [provider options]\nHosted web: "
                     "codex-cpp serve --state-dir DIR [--host 0.0.0.0] [--port 8765] [--origin URL]\nAccounts: "
                     "codex-cpp serve --state-dir DIR --add-user USER\nNo Python runtime required. WebUI "
                     "is embedded; --web-dir DIR overrides it.\n";
        return 0;
      }
      if (arg == "--port" || arg == "--host" || arg == "--origin" || arg == "--state-dir" ||
          arg == "--web-dir" || arg == "--add-user" || arg == "--revoke-user") {
        require(i + 1 < argc, "Missing option value");
        std::string value = argv[++i];
        if (arg == "--port")
          port = std::stoi(value);
        else if (arg == "--host")
          host = value;
        else if (arg == "--origin")
          origin = value;
        else if (arg == "--state-dir")
          state = fs::absolute(fs::u8path(value));
        else if (arg == "--web-dir")
          assets = fs::absolute(fs::u8path(value));
        else if (arg == "--add-user")
          add_user = value;
        else
          revoke_user = value;
        continue;
      }
      require(std::set<std::string>{"--root", "--provider", "--model", "--base-url", "--api", "--api-key-env",
                                    "--no-api-key", "--reasoning"}
                  .count(arg),
              "Unsupported web option; execution policy is chosen in the page");
      if (arg != "--root")
        explicit_options = true;
      arguments.push_back(arg);
      if (arg != "--no-api-key") {
        require(i + 1 < argc, "Missing option value");
        if (arg == "--root")
          override_root = argv[i + 1];
        arguments.push_back(argv[++i]);
      }
    }
    std::vector<char *> pointers;
    for (auto &arg : arguments)
      pointers.push_back(arg.data());
    std::string why;
    auto parsed = parse_options(static_cast<int>(pointers.size()), pointers.data(), why);
    require(parsed.has_value(), why);
    require(parsed->prompt.empty(), "Web service takes provider options, not a prompt");
    auto opt = *parsed;
    if (!override_root.empty())
      opt.root = fs::u8path(override_root);
    opt.root = fs::canonical(opt.root);
#if !defined(_WIN32)
    require(getuid() != 0, "Run as an ordinary account, never root");
    ::signal(SIGPIPE, SIG_IGN);
#endif
    require(port >= 1 && port <= 65535, "Port must be 1..65535");
    if(host.empty())host=operation=="serve"&&origin.empty()?"0.0.0.0":"127.0.0.1";
    require(ipv4_address(host),"Use a numeric IPv4 listen address");
    std::unique_ptr<Service> service;
    if (operation == "serve") {
      require(!explicit_options, "Configure hosted providers per project or personal context in the page");
      require(!state.empty(), "Hosted mode requires --state-dir");
      auto hosted = std::make_unique<HostedService>(state, opt);
      if (!add_user.empty()) {
        std::cout << "Account: " << add_user
                  << "\nLogin token (save privately): " << hosted->add_user(add_user) << '\n';
        return 0;
      }
      if (!revoke_user.empty()) {
        require(hosted->database.fields["users"].fields.erase(revoke_user) > 0, "Account not found");
        hosted->persist();
        std::cout << "Account revoked\n";
        return 0;
      }
      hosted->direct_access=origin.empty();
      service = std::move(hosted);
    } else {
      require(host == "127.0.0.1", "Local mode only binds loopback");
      service = std::make_unique<LocalService>();
      service->home = home;
      service->local = std::make_unique<App>(opt.root, self, home, opt, explicit_options);
    }
    execution.initialize(opt.root);
    service->binary = self;
    service->web = assets;
    service->host = host;
    service->port = port;
    if (origin.empty())origin = "http://"+(host=="0.0.0.0"?std::string("127.0.0.1"):host)+":"+std::to_string(port);
    origin=browser_origin(origin);
    auto start = origin.find("://");
    require(start != std::string::npos, "Invalid browser origin");
    service->authority = origin.substr(start + 3);
    require(!service->authority.empty() &&
                service->authority.find_first_of("/@?#\r\n\t ") == std::string::npos,
            "Use an origin without path or credentials");
    service->origin = origin;
    require(assets.empty() || (fs::is_regular_file(assets / "index.html") && fs::is_regular_file(assets / "app.js") &&
                fs::is_regular_file(assets / "style.css")), "Incomplete --web-dir assets");
    service->listen();
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "Web service: " << e.what() << '\n';
    return 2;
  }
}
} // namespace native_web

static ToolResult run_libgit_tool(const std::string &tool,const std::string &arguments,const fs::path &root,bool write,SandboxMode mode,const std::function<void()> &tick,bool watch){return native_web::run_agent_tool(tool,arguments,root,write,mode,tick,watch);}
