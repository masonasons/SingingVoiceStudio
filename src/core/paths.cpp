#include "core/paths.h"

#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifndef SVS_SOURCE_DIR
#define SVS_SOURCE_DIR ""
#endif

namespace svs {

std::wstring widen(const std::string &s) {
#ifdef _WIN32
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), &w[0], n);
    return w;
#else
    return std::wstring(s.begin(), s.end());
#endif
}

std::string narrow(const std::wstring &w) {
#ifdef _WIN32
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), &s[0], n, nullptr, nullptr);
    return s;
#else
    return std::string(w.begin(), w.end());
#endif
}

static bool is_sep(char c) { return c == '/' || c == '\\'; }

std::string join_path(const std::string &a, const std::string &b) {
    if (a.empty()) return b;
    if (is_sep(a.back())) return a + b;
#ifdef _WIN32
    return a + "\\" + b;
#else
    return a + "/" + b;
#endif
}

std::string base_name(const std::string &path) {
    size_t i = path.find_last_of("/\\");
    return i == std::string::npos ? path : path.substr(i + 1);
}

static std::string parent(const std::string &path) {
    size_t i = path.find_last_of("/\\");
    return i == std::string::npos ? std::string() : path.substr(0, i);
}

std::string strip_extension(const std::string &path) {
    std::string b = base_name(path);
    size_t dot = b.find_last_of('.');
    if (dot == std::string::npos || dot == 0) return path;
    return path.substr(0, path.size() - (b.size() - dot));
}

std::string extension_lower(const std::string &path) {
    std::string b = base_name(path);
    size_t dot = b.find_last_of('.');
    if (dot == std::string::npos) return std::string();
    std::string e = b.substr(dot);
    for (char &c : e) c = char(tolower((unsigned char)c));
    return e;
}

bool file_exists(const std::string &path) {
#ifdef _WIN32
    DWORD a = GetFileAttributesW(widen(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
#endif
}

bool dir_exists(const std::string &path) {
#ifdef _WIN32
    DWORD a = GetFileAttributesW(widen(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

bool make_dirs(const std::string &path) {
    if (path.empty() || dir_exists(path)) return true;
    std::string up = parent(path);
    if (!up.empty() && up != path && !make_dirs(up)) return false;
#ifdef _WIN32
    return CreateDirectoryW(widen(path).c_str(), nullptr) || dir_exists(path);
#else
    return mkdir(path.c_str(), 0755) == 0 || dir_exists(path);
#endif
}

std::string exe_dir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(sizeof buf / sizeof buf[0]));
    return parent(narrow(std::wstring(buf, n)));
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return ".";
    buf[n] = 0;
    return parent(buf);
#endif
}

std::vector<std::string> data_roots() {
    std::vector<std::string> out;
    std::string exe = exe_dir();
    out.push_back(exe);
    if (!parent(exe).empty()) out.push_back(parent(exe));
    // $SVS_ONLY_BESIDE asks for the program's own folder alone, which is how a
    // packaged copy checks that it carries everything it needs
    const char *only = std::getenv("SVS_ONLY_BESIDE");
    if (only && *only) return {exe};
    if (*SVS_SOURCE_DIR) out.push_back(SVS_SOURCE_DIR);
    const char *env = std::getenv("SVS_DATA");
    if (env && *env) out.push_back(env);
    return out;
}

static bool exists_any(const std::string &p) { return file_exists(p) || dir_exists(p); }

std::string find_data(const std::string &relative) {
    auto roots = data_roots();
    for (const std::string &r : roots) {
        std::string p = join_path(r, relative);
        if (exists_any(p)) return p;
    }
    return join_path(roots.empty() ? "." : roots[0], relative);
}

bool data_exists(const std::string &relative) { return exists_any(find_data(relative)); }

std::string temp_dir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH + 1, buf);
    std::string t = narrow(std::wstring(buf, n));
    while (!t.empty() && is_sep(t.back())) t.pop_back();
    return t;
#else
    const char *t = std::getenv("TMPDIR");
    return t ? t : "/tmp";
#endif
}

std::string settings_dir() {
#ifdef _WIN32
    const wchar_t *app = _wgetenv(L"APPDATA");
    std::string base = app ? narrow(app) : exe_dir();
    return join_path(base, "Singing Voice Studio");
#else
    const char *x = std::getenv("XDG_CONFIG_HOME");
    std::string base = x ? x : join_path(std::getenv("HOME") ? std::getenv("HOME") : ".", ".config");
    return join_path(base, "singing-voice-studio");
#endif
}

bool read_file(const std::string &path, std::vector<unsigned char> *out) {
#ifdef _WIN32
    FILE *f = _wfopen(widen(path).c_str(), L"rb");
#else
    FILE *f = std::fopen(path.c_str(), "rb");
#endif
    if (!f) return false;
    out->clear();
    unsigned char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out->insert(out->end(), buf, buf + n);
    fclose(f);
    return true;
}

}  // namespace svs
