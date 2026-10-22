#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>
#include <system_error>

namespace fs = std::filesystem;

namespace color {
constexpr const char* red = "\x1b[31m";
constexpr const char* green = "\x1b[32m";
constexpr const char* cyan = "\x1b[36m";
constexpr const char* reset = "\x1b[0m";
}  

namespace {

constexpr const char* kVersion = "3.1.0-WIN";

constexpr int kExitUsage = 64;
constexpr int kExitNotFound = 66;
constexpr int kExitInternal = 70;
constexpr unsigned long long kMaxConvertBytes = 64ULL * 1024 * 1024 * 1024;
constexpr unsigned long long kMaxConvertEntries = 2000000;
constexpr unsigned long long kMaxExtractBytes = 4ULL * 1024 * 1024 * 1024;
constexpr unsigned long long kMaxExtractEntries = 500000;

bool g_colorOut = false;
bool g_colorErr = false;

void printUsage();
int run7z(const std::vector<std::wstring>& args);

std::string narrow(const std::wstring& value) {
    if (value.empty()) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                           static_cast<int>(value.size()), nullptr, 0,
                                           nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<size_t>(needed), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, value.data(),
                            static_cast<int>(value.size()), out.data(), needed,
                            nullptr, nullptr) != needed) return {};
    return out;
}

std::wstring widen(const std::string& value) {
    if (value.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                           static_cast<int>(value.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out(static_cast<size_t>(needed), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), out.data(), needed) != needed)
        return {};
    return out;
}

bool isDirectory(const fs::path& p) {
    std::error_code ec;
    return fs::is_directory(p, ec) && !ec;
}

bool isRegularFile(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec) && !ec;
}

bool validExtension(const std::string& ext) {
    return !ext.empty() && std::all_of(ext.begin(), ext.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}

fs::path resolveSplitArchive(fs::path p) {
    if (isRegularFile(p)) return p;
    std::wstring name = p.filename().wstring();
    std::transform(name.begin(), name.end(), name.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    if (name.size() >= 4 && name.compare(name.size() - 4, 4, L".001") == 0)
        return p;
    fs::path first = p;
    first += L".001";
    if (isRegularFile(first)) return first;
    return p;
}

std::string sanitize(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    for (unsigned char c : s)
        r += (c < 0x20 || c == 0x7F) ? '?' : static_cast<char>(c);
    return r;
}

void printErr(const std::string& m) {
    std::cerr << (g_colorErr ? color::red : "") << "[ERROR] " << sanitize(m)
              << (g_colorErr ? color::reset : "") << '\n';
}

void printInfo(const char* col, const std::string& m) {
    std::cout << (g_colorOut ? col : "") << "[INFO] " << sanitize(m)
              << (g_colorOut ? color::reset : "") << '\n';
}

std::string toUtf8(const wchar_t* w) {
    if (!w) return {};

    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w, -1,
                                nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};

    std::string s(static_cast<size_t>(n), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w, -1,
                            s.data(), n, nullptr, nullptr) <= 0)
        return {};

    s.resize(static_cast<size_t>(n - 1));
    return s;
}

bool validLevel(const std::string& l) {
    return l.size() == 1 && l[0] >= '0' && l[0] <= '9';
}

bool prepSource(const std::string& src) {
    fs::path p = fs::u8path(src);
    if (!isDirectory(p)) {
        printErr("Source directory not found: " + src);
        return false;
    }
    printInfo(color::green,
              "Using source directory: " + narrow(fs::absolute(p).wstring()));
    return true;
}

bool confirm(const std::string& question) {
    std::cout << question << " [y/N]: " << std::flush;
    std::string line;
    if (!std::getline(std::cin, line)) return false;
    return line == "y" || line == "Y" || line == "yes";
}

std::string fmtBytes(unsigned long long b) {
    const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double v = static_cast<double>(b);
    int i = 0;
    while (v >= 1024.0 && i < 4) {
        v /= 1024.0;
        ++i;
    }
    char buf[64];
    if (i == 0)
        std::snprintf(buf, sizeof buf, "%llu B", b);
    else
        std::snprintf(buf, sizeof buf, "%.2f %s", v, units[i]);
    return buf;
}

std::wstring lowerW(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

bool allDigits(const std::string& s) {
    if (s.empty() || s.size() > 20) return false;
    unsigned long long value = 0;
    const auto result = std::from_chars(s.data(), s.data() + s.size(), value);
    return result.ec == std::errc{} && result.ptr == s.data() + s.size();
}

std::wstring quoteArg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
    std::wstring r = L"\"";
    size_t bs = 0;
    for (wchar_t c : a) {
        if (c == L'\\') {
            ++bs;
        } else if (c == L'"') {
            r.append(bs * 2 + 1, L'\\');
            r += L'"';
            bs = 0;
        } else {
            r.append(bs, L'\\');
            r += c;
            bs = 0;
        }
    }
    r.append(bs * 2, L'\\');
    r += L'"';
    return r;
}

fs::path exePath() {
    wchar_t b[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, b, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return fs::path(b);
}

std::wstring envVar(const wchar_t* name) {
    DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (n == 0) return {};
    std::vector<wchar_t> buf(n, L'\0');
    DWORD got = GetEnvironmentVariableW(name, buf.data(), n);
    if (got == 0 || got >= n) return {};
    return std::wstring(buf.data(), got);
}

fs::path find7zExe() {
    const fs::path self = exePath();
    std::vector<fs::path> candidates;
    if (!self.empty()) candidates.push_back(self.parent_path() / L"7z.exe");
    for (const wchar_t* env : {L"ProgramFiles", L"ProgramFiles(x86)"}) {
        std::wstring base = envVar(env);
        if (!base.empty()) candidates.push_back(fs::path(base) / L"7-Zip" / L"7z.exe");
    }
    for (const auto& c : candidates) {
        std::error_code ec;
        if (!fs::is_regular_file(c, ec)) continue;
        if (!self.empty() && fs::equivalent(c, self, ec)) continue;
        return c;
    }
    return {};
}

bool capture7z(const std::vector<std::wstring>& args, int& code, std::string& out,
               size_t maxBytes = 512u * 1024u * 1024u) {
    fs::path exe = find7zExe();
    if (exe.empty()) {
        printErr("7z.exe not found (looked next to this program and in Program Files).");
        return false;
    }
    std::wstring cmd = quoteArg(exe.wstring());
    for (const auto& a : args) cmd += L" " + quoteArg(a);

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        printErr("Could not create a pipe.");
        return false;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> line(cmd.begin(), cmd.end());
    line.push_back(L'\0');

    const BOOL ok = CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(rd);
        printErr("Could not start 7z.exe.");
        return false;
    }

    char buf[4096];
    DWORD got = 0;
    bool readOk = true;
    bool tooBig = false;
    while (true) {
        if (!ReadFile(rd, buf, sizeof buf, &got, nullptr)) {
            if (GetLastError() != ERROR_BROKEN_PIPE) readOk = false;
            break;
        }
        if (got == 0) break;
        out.append(buf, got);
        if (out.size() > maxBytes) {
            tooBig = true;
            readOk = false;
            break;
        }
    }
    CloseHandle(rd);

    if (!readOk) TerminateProcess(pi.hProcess, 1);
    const DWORD waited = WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 1;
    const BOOL gotCode = GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (!readOk || waited != WAIT_OBJECT_0 || !gotCode) {
        printErr(tooBig ? "The archive listing is too large to process."
                        : "Failed while reading output from 7z.exe.");
        return false;
    }
    code = static_cast<int>(exitCode);
    return true;
}

struct Settings {
    bool debug = false;
    char overwrite = 'u';
    bool pathAsked = false;
};

fs::path dataDir() {
    std::wstring appdata = envVar(L"APPDATA");
    fs::path base;
    if (!appdata.empty()) {
        base = appdata;
    } else {
        std::error_code ec;
        base = fs::temp_directory_path(ec);
    }
    fs::path d = base / L"8Ext";
    std::error_code ec;
    fs::create_directories(d, ec);
    return d;
}

Settings loadSettings() {
    Settings st;
    std::ifstream f(dataDir() / L"settings.ini", std::ios::binary);
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == "debug=1") {
            st.debug = true;
        } else if (line == "pathAsked=1") {
            st.pathAsked = true;
        } else if (line.size() == 11 && line.rfind("overwrite=", 0) == 0) {
            char c = line[10];
            if (c == 'a' || c == 's' || c == 't' || c == 'u') st.overwrite = c;
        }
    }
    return st;
}

bool saveSettings(const Settings& st) {
    std::ofstream f(dataDir() / L"settings.ini", std::ios::binary | std::ios::trunc);
    f << "debug=" << (st.debug ? 1 : 0) << "\n"
      << "overwrite=" << st.overwrite << "\n"
      << "pathAsked=" << (st.pathAsked ? 1 : 0) << "\n";
    return static_cast<bool>(f);
}

std::string nowStamp(bool compact) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char b[40];
    if (compact)
        std::snprintf(b, sizeof b, "%04d%02d%02d_%02d%02d%02d", t.wYear, t.wMonth,
                      t.wDay, t.wHour, t.wMinute, t.wSecond);
    else
        std::snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d:%02d", t.wYear, t.wMonth,
                      t.wDay, t.wHour, t.wMinute, t.wSecond);
    return b;
}

std::string lowerA(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string cleanLog(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    for (unsigned char c : s)
        r += ((c < 0x20 && c != '\n' && c != '\r' && c != '\t') || c == 0x7F)
                 ? '?'
                 : static_cast<char>(c);
    return r;
}

const char* overwriteText(char m) {
    switch (m) {
        case 'a': return "existing files are overwritten";
        case 's': return "existing files are skipped";
        case 't': return "existing files are renamed";
        default: return "new files are renamed on a name collision";
    }
}

const char* exitMeaning(int c) {
    switch (c) {
        case 0: return "success";
        case 1: return "warning (non-fatal)";
        case 2: return "fatal error";
        case 7: return "command-line error";
        case 8: return "not enough memory";
        case 255: return "stopped by the user";
        default: return "unknown";
    }
}

std::vector<std::string> splitTabs(const std::string& s) {
    std::vector<std::string> parts;
    size_t pos = 0;
    while (true) {
        size_t t = s.find('\t', pos);
        if (t == std::string::npos) {
            parts.push_back(s.substr(pos));
            break;
        }
        parts.push_back(s.substr(pos, t - pos));
        pos = t + 1;
    }
    return parts;
}

std::vector<std::string> readHistory() {
    std::vector<std::string> lines;
    std::ifstream f(dataDir() / L"history.tsv", std::ios::binary);
    std::string l;
    while (std::getline(f, l)) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        if (!l.empty()) lines.push_back(l);
    }
    return lines;
}

void recordHistory(const fs::path& archive, const fs::path& dest, int code,
                   unsigned long long bytes) {
    fs::path file = dataDir() / L"history.tsv";
    {
        std::ofstream f(file, std::ios::binary | std::ios::app);
        if (!f) return;
        f << nowStamp(false) << '\t' << code << '\t' << bytes << '\t'
          << sanitize(narrow(fs::absolute(archive).wstring())) << '\t'
          << sanitize(narrow(fs::absolute(dest).wstring())) << '\n';
    }
    std::error_code ec;
    auto size = fs::file_size(file, ec);
    if (ec || size <= 1024 * 1024) return;
    std::vector<std::string> lines = readHistory();
    if (lines.size() > 1000) lines.erase(lines.begin(), lines.end() - 1000);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    for (const auto& l : lines) out << l << '\n';
}

bool parseSize(const std::string& in, unsigned long long& out) {
    size_t i = 0;
    std::string num;
    while (i < in.size() && (std::isdigit(static_cast<unsigned char>(in[i])) || in[i] == '.'))
        num += in[i++];
    std::string unit = in.substr(i);
    std::transform(unit.begin(), unit.end(), unit.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

    static const struct {
        const char* name;
        int shift;
    } units[] = {{"B", 0}, {"KB", 10}, {"MB", 20}, {"GB", 30},
                 {"TB", 40}, {"PB", 50}, {"EB", 60}};
    int shift = -1;
    for (const auto& u : units)
        if (unit == u.name) shift = u.shift;

    if (shift < 0 || num.empty() || num.size() > 30 || num == "." ||
        std::count(num.begin(), num.end(), '.') > 1)
        return false;
    long double v = 0;
    try {
        v = std::stold(num);
    } catch (...) {
        return false;
    }
    long double r = std::ldexp(v, shift);
    if (!(r >= 1.0L) || r >= 18446744073709551616.0L) return false;
    out = static_cast<unsigned long long>(r);
    return true;
}

std::wstring trimSlashes(std::wstring s) {
    while (!s.empty() && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    return lowerW(s);
}

bool dirInList(const std::wstring& list, const fs::path& dir) {
    const std::wstring want = trimSlashes(dir.wstring());
    size_t pos = 0;
    while (pos <= list.size()) {
        size_t semi = list.find(L';', pos);
        std::wstring item =
            list.substr(pos, semi == std::wstring::npos ? std::wstring::npos : semi - pos);
        if (!item.empty() && trimSlashes(item) == want) return true;
        if (semi == std::wstring::npos) break;
        pos = semi + 1;
    }
    return false;
}

bool editUserPath(const fs::path& dir, bool add, bool& changed) {
    changed = false;
    if (dir.wstring().find(L';') != std::wstring::npos) {
        printErr("This program's folder contains a ';' and cannot be put on PATH.");
        return false;
    }
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0,
                      KEY_QUERY_VALUE | KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        printErr("Could not open the user environment in the registry.");
        return false;
    }

    DWORD type = REG_EXPAND_SZ, size = 0;
    std::wstring value;
    LSTATUS q = RegQueryValueExW(key, L"Path", nullptr, &type, nullptr, &size);
    if (q == ERROR_SUCCESS) {
        if (type != REG_SZ && type != REG_EXPAND_SZ) {
            RegCloseKey(key);
            printErr("The user PATH has an unexpected type; leaving it untouched.");
            return false;
        }
        std::vector<wchar_t> buf(size / sizeof(wchar_t) + 1, L'\0');
        DWORD size2 = size;
        if (RegQueryValueExW(key, L"Path", nullptr, nullptr,
                             reinterpret_cast<LPBYTE>(buf.data()), &size2) != ERROR_SUCCESS) {
            RegCloseKey(key);
            printErr("Could not read the user PATH.");
            return false;
        }
        value.assign(buf.data());
    } else if (q == ERROR_FILE_NOT_FOUND) {
        type = REG_EXPAND_SZ;
    } else {
        RegCloseKey(key);
        printErr("Could not read the user PATH.");
        return false;
    }

    const bool present = dirInList(value, dir);
    std::wstring updated = value;
    if (add && !present) {
        if (!updated.empty() && updated.back() != L';') updated += L';';
        updated += dir.wstring();
    } else if (!add && present) {
        const std::wstring want = trimSlashes(dir.wstring());
        std::wstring rebuilt;
        size_t pos = 0;
        while (pos <= value.size()) {
            size_t semi = value.find(L';', pos);
            std::wstring item = value.substr(
                pos, semi == std::wstring::npos ? std::wstring::npos : semi - pos);
            if (!item.empty() && trimSlashes(item) != want) {
                if (!rebuilt.empty()) rebuilt += L';';
                rebuilt += item;
            }
            if (semi == std::wstring::npos) break;
            pos = semi + 1;
        }
        updated = rebuilt;
    } else {
        RegCloseKey(key);
        return true;
    }

    LSTATUS w = RegSetValueExW(key, L"Path", 0, type,
                               reinterpret_cast<const BYTE*>(updated.c_str()),
                               static_cast<DWORD>((updated.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    if (w != ERROR_SUCCESS) {
        printErr("Could not write the user PATH.");
        return false;
    }
    changed = true;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                        reinterpret_cast<LPARAM>(L"Environment"), SMTO_ABORTIFHUNG, 5000,
                        nullptr);
    return true;
}

bool launchedByDoubleClick() {
    DWORD pids[2];
    return GetConsoleProcessList(pids, 2) == 1;
}

enum class Fmt { Unknown, Sevenz, Zip, Tar, TarGz, TarBz2, TarXz, Gz, Bz2, Xz, Rar };

bool endsWithW(const std::wstring& s, const wchar_t* suf) {
    const size_t len = std::wcslen(suf);
    return s.size() >= len && s.compare(s.size() - len, len, suf) == 0;
}

Fmt detectFormat(const fs::path& p) {
    std::wstring n = lowerW(p.filename().wstring());
    if (endsWithW(n, L".001")) n.erase(n.size() - 4);
    if (endsWithW(n, L".tar.gz") || endsWithW(n, L".tgz")) return Fmt::TarGz;
    if (endsWithW(n, L".tar.bz2") || endsWithW(n, L".tbz2") || endsWithW(n, L".tbz"))
        return Fmt::TarBz2;
    if (endsWithW(n, L".tar.xz") || endsWithW(n, L".txz")) return Fmt::TarXz;
    if (endsWithW(n, L".tar")) return Fmt::Tar;
    if (endsWithW(n, L".gz")) return Fmt::Gz;
    if (endsWithW(n, L".bz2")) return Fmt::Bz2;
    if (endsWithW(n, L".xz")) return Fmt::Xz;
    if (endsWithW(n, L".7z")) return Fmt::Sevenz;
    if (endsWithW(n, L".zip")) return Fmt::Zip;
    if (endsWithW(n, L".rar")) return Fmt::Rar;
    return Fmt::Unknown;
}

Fmt parseFormatName(std::string s) {
    s = lowerA(s);
    if (s == "7z") return Fmt::Sevenz;
    if (s == "zip") return Fmt::Zip;
    if (s == "tar") return Fmt::Tar;
    if (s == "tar.gz" || s == "tgz") return Fmt::TarGz;
    if (s == "tar.bz2" || s == "tbz2" || s == "tbz") return Fmt::TarBz2;
    if (s == "tar.xz" || s == "txz") return Fmt::TarXz;
    if (s == "gz") return Fmt::Gz;
    if (s == "bz2") return Fmt::Bz2;
    if (s == "xz") return Fmt::Xz;
    if (s == "rar") return Fmt::Rar;
    return Fmt::Unknown;
}

const char* fmtLabel(Fmt f) {
    switch (f) {
        case Fmt::Sevenz: return "7z";
        case Fmt::Zip: return "zip";
        case Fmt::Tar: return "tar";
        case Fmt::TarGz: return "tar.gz";
        case Fmt::TarBz2: return "tar.bz2";
        case Fmt::TarXz: return "tar.xz";
        case Fmt::Gz: return "gz";
        case Fmt::Bz2: return "bz2";
        case Fmt::Xz: return "xz";
        case Fmt::Rar: return "rar";
        default: return "unknown";
    }
}

const wchar_t* extFor(Fmt f) {
    switch (f) {
        case Fmt::Sevenz: return L".7z";
        case Fmt::Zip: return L".zip";
        case Fmt::Tar: return L".tar";
        case Fmt::TarGz: return L".tar.gz";
        case Fmt::TarBz2: return L".tar.bz2";
        case Fmt::TarXz: return L".tar.xz";
        case Fmt::Rar: return L".rar";
        default: return L"";
    }
}

bool isCreatable(Fmt f) {
    return f == Fmt::Sevenz || f == Fmt::Zip || f == Fmt::Tar || f == Fmt::TarGz ||
           f == Fmt::TarBz2 || f == Fmt::TarXz || f == Fmt::Rar;
}

bool isTarWrapped(Fmt f) { return f == Fmt::TarGz || f == Fmt::TarBz2 || f == Fmt::TarXz; }

std::wstring stripArchiveExt(const fs::path& p) {
    std::wstring n = p.filename().wstring();
    std::wstring l = lowerW(n);
    auto cut = [&](const wchar_t* suf) {
        const size_t len = std::wcslen(suf);
        if (l.size() > len && l.compare(l.size() - len, len, suf) == 0) {
            n.erase(n.size() - len);
            l.erase(l.size() - len);
            return true;
        }
        return false;
    };
    cut(L".001");
    static const wchar_t* suffixes[] = {L".tar.gz", L".tar.bz2", L".tar.xz", L".tgz",
                                        L".tbz2",   L".tbz",     L".txz",    L".tar",
                                        L".7z",     L".zip",     L".rar",    L".gz",
                                        L".bz2",    L".xz"};
    for (const wchar_t* suf : suffixes)
        if (cut(suf)) break;
    return n;
}

std::wstring rarLevel(const std::string& level) {
    static const int map[] = {0, 1, 1, 2, 2, 3, 3, 4, 4, 5};
    int n = level.empty() ? 5 : level[0] - '0';
    if (n < 0 || n > 9) n = 5;
    return std::to_wstring(map[n]);
}

struct ScopedDir {
    fs::path path;
    bool create() {
        static unsigned seq = 0;
        std::error_code ec;
        fs::path root = fs::temp_directory_path(ec);
        if (ec) return false;
        path = root / (L"a7z_convert_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
                       std::to_wstring(GetTickCount64()) + L"_" + std::to_wstring(++seq));
        return fs::create_directory(path, ec) && !ec;
    }
    ~ScopedDir() {
        if (path.empty()) return;
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

fs::path findRarExe() {
    const fs::path self = exePath();
    std::vector<fs::path> candidates;
    if (!self.empty()) candidates.push_back(self.parent_path() / L"rar.exe");
    for (const wchar_t* env : {L"ProgramFiles", L"ProgramFiles(x86)"}) {
        std::wstring base = envVar(env);
        if (!base.empty()) candidates.push_back(fs::path(base) / L"WinRAR" / L"Rar.exe");
    }
    for (const auto& c : candidates) {
        std::error_code ec;
        if (fs::is_regular_file(c, ec)) return c;
    }
    return {};
}

int runTool(const fs::path& exe, const std::vector<std::wstring>& args,
            const fs::path& workDir) {
    std::wstring cmd = quoteArg(exe.wstring());
    for (const auto& a : args) cmd += L" " + quoteArg(a);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> line(cmd.begin(), cmd.end());
    line.push_back(L'\0');
    if (!CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        workDir.empty() ? nullptr : workDir.c_str(), &si, &pi)) {
        printErr("Could not start " + narrow(exe.filename().wstring()));
        return -1;
    }
    const DWORD waited = WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    const BOOL gotCode = GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (waited != WAIT_OBJECT_0 || !gotCode) {
        printErr("Could not obtain the external tool's exit code.");
        return -1;
    }
    return static_cast<int>(code);
}

int run7z(const std::vector<std::wstring>& args) {
    const fs::path exe = find7zExe();
    if (exe.empty()) {
        printErr("7z.exe not found (looked next to this program and in Program Files).");
        return kExitNotFound;
    }
    std::wstring cmd = quoteArg(exe.wstring());
    for (const auto& arg : args) cmd += L" " + quoteArg(arg);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(L'\0');
    if (!CreateProcessW(exe.c_str(), mutableCmd.data(), nullptr, nullptr, TRUE, 0,
                        nullptr, nullptr, &si, &pi)) {
        printErr("Could not start 7z.exe.");
        return kExitInternal;
    }
    const DWORD waitResult = WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 1;
    const BOOL gotExitCode = GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (waitResult != WAIT_OBJECT_0 || !gotExitCode) {
        printErr("Could not obtain the 7-Zip exit code.");
        return kExitInternal;
    }
    return static_cast<int>(exitCode);
}

void printUsage() {
    std::cout << "8Ext " << kVersion << "\n\n"
              << "Core commands:\n"
              << "  -extract ARCHIVE [OUTPUT_DIR]\n"
              << "  -test ARCHIVE\n"
              << "  -info ARCHIVE\n"
              << "  -formats\n"
              << "  -uncompressed size ARCHIVE\n"
              << "  -convert SOURCE_FORMAT -as_TARGET ARCHIVE\n"
              << "  -compress-file gz|bz2|xz FILE [LEVEL 0-9]\n"
              << "  -recompress OUTPUT_ARCHIVE [LEVEL 0-9] [SOURCE_DIR]\n"
              << "  -encrypt OUTPUT.7z [LEVEL 0-9] [SOURCE_DIR]\n"
              << "  -rar-repair ARCHIVE.rar\n"
              << "  -extractionHistory  -temp  -ver\n"
              << "  -addpath  -removepath\n"
              << "Settings/debug commands:\n"
              << "  -7zSettings -enable_debugging|-disable_debugging\n"
              << "  -debugging_options -extractSizelimit LIMIT -compress ARCHIVE\n"
              << "  -debugging_options -ao{a|s|t|u}\n"
              << "  -advanced_options -creatediag7zExtraction -extract ARCHIVE\n"
              << "  -advanced_options -showBytesExtracted -compress ARCHIVE\n"
              << "  -block-EXT -from-extracting ARCHIVE OUTPUT_DIR\n";
}

struct ConsoleSetup {
    HANDLE h[2]{};
    DWORD old[2]{};
    bool changed[2]{};

    ConsoleSetup() {
        const DWORD ids[2] = {STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
        const bool noColor = std::getenv("NO_COLOR") != nullptr;
        for (int i = 0; i < 2; ++i) {
            h[i] = GetStdHandle(ids[i]);
            DWORD m = 0;
            const bool isConsole =
                h[i] && h[i] != INVALID_HANDLE_VALUE && GetConsoleMode(h[i], &m);
            if (isConsole && !noColor &&
                SetConsoleMode(h[i], m | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
                old[i] = m;
                changed[i] = true;
                (i == 0 ? g_colorOut : g_colorErr) = true;
            }
        }
    }
    ~ConsoleSetup() {
        for (int i = 0; i < 2; ++i)
            if (changed[i]) SetConsoleMode(h[i], old[i]);
    }
};

}

bool scanArchive(const fs::path& archive, int& code, unsigned long long& total,
                 unsigned long long& entries, unsigned long long& folders,
                 bool& overflow, std::vector<std::string>* header = nullptr) {
    std::string out;
    total = entries = folders = 0;
    overflow = false;
    if (!capture7z({L"l", L"-slt", L"--", archive.wstring()}, code, out)) return false;
    if (code > 1) return true;

    bool inEntries = false;
    size_t pos = 0;
    while (pos <= out.size()) {
        size_t nl = out.find('\n', pos);
        std::string line =
            out.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = (nl == std::string::npos) ? out.size() + 1 : nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (line == "----------") {
            inEntries = true;
            continue;
        }
        if (!inEntries) {
            if (header && header->size() < 64 && line.find(" = ") != std::string::npos)
                header->push_back(line);
            continue;
        }

        if (line.rfind("Path = ", 0) == 0) {
            ++entries;
        } else if (line == "Folder = +") {
            ++folders;
        } else if (line.rfind("Size = ", 0) == 0) {
            std::string n = line.substr(7);
            if (allDigits(n)) {
                unsigned long long v = std::stoull(n);
                if (v > std::numeric_limits<unsigned long long>::max() - total)
                    overflow = true;
                else
                    total += v;
            }
        }
    }
    if (folders > entries) folders = entries;
    return true;
}

bool preflightExpansion(const fs::path& archive, const char* purpose,
                        unsigned long long maxBytes = kMaxExtractBytes) {
    int code = 0;
    unsigned long long total = 0, entries = 0, folders = 0;
    bool overflow = false;
    if (!scanArchive(archive, code, total, entries, folders, overflow)) {
        printErr(std::string("Could not inspect the archive before ") + purpose + ". Refusing to proceed safely.");
        return false;
    }
    if (overflow) {
        printErr(std::string("The reported expanded size overflowed 64-bit accounting before ") +
                 purpose + ". Refusing to proceed.");
        return false;
    }
    if (code > 1) {
        printInfo(color::cyan, std::string("Could not verify the expanded size before ") +
                                purpose + ". This can happen with encrypted or damaged archives.");
        return confirm("Continue without a verified size estimate? This is riskier");
    }
    if (total > maxBytes) {
        printErr(std::string("Refusing ") + purpose + ": the archive reports " + fmtBytes(total) +
                 " expanded, exceeding the configured limit of " + fmtBytes(maxBytes) + ".");
        return false;
    }
    if (entries > kMaxExtractEntries) {
        printErr(std::string("Refusing ") + purpose + ": the archive reports " +
                 std::to_string(entries) + " entries, exceeding the default 500,000-entry limit.");
        return false;
    }
    return true;
}

int listArchive(const char* archiveArg) {
    fs::path archive = resolveSplitArchive(fs::u8path(archiveArg));
    if (!isRegularFile(archive)) {
        printErr(std::string("Archive not found: ") + archiveArg);
        return kExitNotFound;
    }

    int code = 0;
    unsigned long long total = 0, entries = 0, folders = 0;
    bool overflow = false;
    if (!scanArchive(archive, code, total, entries, folders, overflow))
        return kExitInternal;
    if (code > 1) {
        printErr("7-Zip could not read the archive (exit code " + std::to_string(code) +
                 "). It may be corrupt, or its file names may be encrypted.");
        return code;
    }
    if (overflow) {
        printErr("The archive reports a total size beyond 64 bits; refusing to print a "
                 "wrong total.");
        return kExitInternal;
    }

    printInfo(color::green, "Archive: " + narrow(archive.filename().wstring()));
    printInfo(color::green, "Files: " + std::to_string(entries - folders) +
                            ", folders: " + std::to_string(folders));
    printInfo(color::green, "Total uncompressed size: " + std::to_string(total) +
                            " bytes (" + fmtBytes(total) + ")");

    std::error_code ec;
    if (archive.extension() != L".001") {
        auto packed = fs::file_size(archive, ec);
        if (!ec) {
            std::string msg = "Archive file size: " + fmtBytes(packed);
            if (total > 0) {
                char pct[32];
                std::snprintf(pct, sizeof pct, "%.1f",
                              100.0 * static_cast<double>(packed) /
                                  static_cast<double>(total));
                msg += " (" + std::string(pct) + "% of uncompressed)";
            }
            printInfo(color::green, msg);
        }
    }
    if (code == 1)
        printInfo(color::cyan, "7-Zip reported warnings; the totals may be incomplete.");
    return 0;
}

int packTree(const fs::path& src, const fs::path& out, Fmt fmt, const std::string& level,
             bool encrypt) {
    std::error_code existsEc;
    if (fs::exists(out, existsEc) || existsEc) {
        printErr(existsEc ? "Could not check whether the output path exists."
                          : "Output already exists, refusing to overwrite: " + narrow(out.wstring()));
        return kExitUsage;
    }
    const std::wstring lv = widen(level);
    const std::wstring star = (src / L"*").wstring();
    int rc = 0;

    if (fmt == Fmt::Sevenz || fmt == Fmt::Zip || fmt == Fmt::Tar) {
        std::vector<std::wstring> a = {L"a"};
        if (fmt == Fmt::Sevenz) {
            a.push_back(L"-t7z");
            a.push_back(L"-mx=" + lv);
            if (encrypt) {
                a.push_back(L"-mhe=on");
                a.push_back(L"-p");
                printInfo(color::cyan,
                          "7-Zip will now ask for a password (AES-256, file names encrypted).");
            }
        } else if (fmt == Fmt::Zip) {
            a.push_back(L"-tzip");
            a.push_back(L"-mcu=on");
            a.push_back(L"-mx=" + lv);
        } else {
            a.push_back(L"-ttar");
        }
        a.push_back(L"--");
        a.push_back(out.wstring());
        a.push_back(star);
        rc = static_cast<int>(run7z(a));
    } else if (isTarWrapped(fmt)) {
        ScopedDir stage;
        if (!stage.create()) {
            printErr("Could not create a temporary directory.");
            return kExitInternal;
        }
        fs::path tarFile = stage.path / (stripArchiveExt(out) + L".tar");
        rc = static_cast<int>(run7z({L"a", L"-ttar", L"--", tarFile.wstring(), star}));
        if (rc <= 1) {
            const wchar_t* type =
                fmt == Fmt::TarGz ? L"gzip" : (fmt == Fmt::TarBz2 ? L"bzip2" : L"xz");
            rc = static_cast<int>(run7z({L"a", std::wstring(L"-t") + type, L"-mx=" + lv, L"--",
                                         out.wstring(), tarFile.wstring()}));
        }
    } else {
        fs::path rar = findRarExe();
        if (rar.empty()) {
            printErr("Creating .rar files needs WinRAR's Rar.exe, which was not found.");
            return kExitNotFound;
        }
        std::vector<std::wstring> a = {L"a", L"-r", L"-ep1", L"-ma5", L"-rr",
                                       L"-m" + rarLevel(level)};
        if (encrypt) {
            a.push_back(L"-hp");
            printInfo(color::cyan,
                      "WinRAR will ask for a password (AES-256, file names encrypted).");
        }
        a.push_back(L"--");
        a.push_back(out.wstring());
        a.push_back(star);
        rc = runTool(rar, a, fs::path());
    }

    std::error_code ec;
    if (rc > 1 || rc < 0) {
        fs::remove(out, ec);
        printErr("Archiving failed (exit code " + std::to_string(rc) + ").");
        return rc < 0 ? kExitInternal : rc;
    }
    if (rc == 1) printInfo(color::cyan, "The tool reported warnings; check the archive.");
    if (!isRegularFile(out)) {
        printErr("The archiver reported completion but the output archive is missing.");
        return kExitInternal;
    }

    auto size = fs::file_size(out, ec);
    printInfo(color::green, "Created " + narrow(out.wstring()) +
                            (ec ? "" : " (" + fmtBytes(size) + ")"));
    return rc;
}

int packDirectory(const char* outArg, const std::string& level,
                  const std::string& source, bool encrypt) {
    fs::path out = fs::absolute(fs::u8path(outArg));
    fs::path src = fs::absolute(fs::u8path(source));

    const Fmt fmt = detectFormat(out);
    if (!isCreatable(fmt)) {
        printErr("Output must end in .7z, .zip, .tar, .tar.gz/.tgz, .tar.bz2/.tbz2, "
                 ".tar.xz/.txz or .rar.");
        return kExitUsage;
    }
    if (encrypt && fmt != Fmt::Sevenz && fmt != Fmt::Rar) {
        printErr("Encryption is only supported for .7z and .rar outputs.");
        return kExitUsage;
    }
    if (fmt == Fmt::Rar && findRarExe().empty()) {
        printErr("Creating .rar files needs WinRAR's Rar.exe, which was not found. "
                 "7-Zip can only read RAR.");
        return kExitNotFound;
    }

    std::error_code ec;
    if (!fs::is_directory(out.parent_path(), ec)) {
        printErr("Output folder does not exist: " + narrow(out.parent_path().wstring()));
        return kExitNotFound;
    }
    if (!fs::is_directory(src, ec)) {
        printErr("Source directory not found: " + narrow(src.wstring()));
        return kExitNotFound;
    }
    fs::directory_iterator first(src, ec);
    if (ec) {
        printErr("Could not read the source directory: " + ec.message());
        return kExitInternal;
    }
    if (first == fs::directory_iterator()) {
        printErr("Source directory is empty: " + narrow(src.wstring()));
        return kExitUsage;
    }
    fs::path rel = out.lexically_normal().lexically_relative(src.lexically_normal());
    if (!rel.empty() && *rel.begin() != L"..") {
        printErr("The output archive must not be inside the source directory.");
        return kExitUsage;
    }
    return packTree(src, out, fmt, level, encrypt);
}

int recompress(const char* outArg, const std::string& level, const std::string& source) {
    return packDirectory(outArg, level, source, false);
}

int encryptArchive(const char* outArg, const std::string& level,
                   const std::string& source) {
    return packDirectory(outArg, level, source, true);
}

int showTempStatus() {
    std::error_code ec;
    fs::path tmp = fs::temp_directory_path(ec);
    if (ec) {
        printErr("Could not determine the temp directory: " + ec.message());
        return kExitInternal;
    }

    unsigned long long files = 0, dirs = 0, bytes = 0;
    fs::recursive_directory_iterator it(
        tmp, fs::directory_options::skip_permission_denied, ec);
    const fs::recursive_directory_iterator end;
    while (!ec && it != end) {
        std::error_code e2;
        if (it->is_directory(e2)) {
            ++dirs;
        } else if (it->is_regular_file(e2)) {
            ++files;
            std::uintmax_t sz = it->file_size(e2);
            if (!e2) bytes += sz;
        }
        it.increment(ec);
    }
    const bool partial = static_cast<bool>(ec);
    const std::string partialWhy = ec.message();

    unsigned long long leftovers = 0;
    ec.clear();
    for (fs::directory_iterator d(tmp, ec), dend; !ec && d != dend; d.increment(ec)) {
        std::error_code e2;
        if (d->is_directory(e2) &&
            d->path().filename().wstring().rfind(L"a7z_convert_", 0) == 0)
            ++leftovers;
    }

    printInfo(color::green, "Temp directory: " + narrow(tmp.wstring()));
    printInfo(color::green, "Contents: " + std::to_string(files) + " files, " +
                            std::to_string(dirs) + " folders, " + fmtBytes(bytes));
    if (partial)
        printInfo(color::cyan, "The scan stopped early (" + partialWhy +
                               "); the totals above are partial.");

    ec.clear();
    fs::space_info sp = fs::space(tmp, ec);
    if (!ec)
        printInfo(color::green, "Free space on this drive: " + fmtBytes(sp.available) +
                                " of " + fmtBytes(sp.capacity));

    if (leftovers > 0)
        printInfo(color::cyan, std::to_string(leftovers) +
                               " leftover a7z_convert_* folder(s) from an interrupted "
                               "conversion can be deleted safely.");
    return 0;
}

int extractAny(const fs::path& archive, const fs::path& outDir, char ow,
               const std::wstring& exclude = L"",
               unsigned long long maxBytes = kMaxExtractBytes) {
    if (!preflightExpansion(archive, "extracting", maxBytes)) return kExitUsage;
    const std::wstring aoSwitch = L"-ao" + std::wstring(1, static_cast<wchar_t>(ow));
    std::vector<std::wstring> a = {L"x", aoSwitch, L"-o" + outDir.wstring()};
    if (!exclude.empty()) {
        a.push_back(L"-r");
        a.push_back(exclude);
    }
    if (!isTarWrapped(detectFormat(archive))) {
        a.push_back(L"--");
        a.push_back(archive.wstring());
        return static_cast<int>(run7z(a));
    }

    ScopedDir stage;
    if (!stage.create()) {
        printErr("Could not create a temporary directory.");
        return kExitInternal;
    }
    int rc = static_cast<int>(
        run7z({L"x", L"-aoa", L"-o" + stage.path.wstring(), L"--", archive.wstring()}));
    if (rc > 1) return rc;

    fs::path inner;
    int count = 0;
    std::error_code ec;
    for (fs::directory_iterator it(stage.path, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec)) {
            inner = it->path();
            ++count;
        }
    }
    if (count != 1) {
        printErr("Expected one tar file inside the compressed archive but found " +
                 std::to_string(count) + ".");
        return kExitInternal;
    }
    if (!preflightExpansion(inner, "extracting the inner tar", maxBytes)) return kExitUsage;
    printInfo(color::green, "Unpacking the inner tar.");
    a.push_back(L"--");
    a.push_back(inner.wstring());
    return static_cast<int>(run7z(a));
}

bool requireDebug() {
    if (loadSettings().debug) return true;
    printErr("Debugging mode is off. Turn it on with: -7zSettings -enable_debugging");
    return false;
}

fs::path extractDirFor(fs::path archive) {
    fs::path abs = fs::absolute(archive);
    return abs.parent_path() / (stripArchiveExt(abs) + L"_extracted");
}

int setDebugging(bool on) {
    Settings st = loadSettings();
    st.debug = on;
    if (!saveSettings(st)) {
        printErr("Could not save the settings.");
        return kExitInternal;
    }
    printInfo(color::green, on ? "Debugging mode is now ON." : "Debugging mode is now OFF.");
    return 0;
}

int setOverwriteMode(char mode) {
    if (mode != 'a' && mode != 's' && mode != 't' && mode != 'u') {
        printErr("Overwrite mode must be -aoa, -aos, -aot or -aou.");
        return kExitUsage;
    }
    Settings st = loadSettings();
    st.overwrite = mode;
    if (!saveSettings(st)) {
        printErr("Could not save the settings.");
        return kExitInternal;
    }
    printInfo(color::green, std::string("Overwrite mode set to -ao") + mode + ": " +
                            overwriteText(mode) + ".");
    if (mode == 'a')
        printInfo(color::cyan, "Warning: existing files will be overwritten without asking.");
    return 0;
}

int pathCommand(bool add) {
    fs::path dir = exePath().parent_path();
    if (dir.empty()) {
        printErr("Could not determine this program's folder.");
        return kExitInternal;
    }
    bool changed = false;
    if (!editUserPath(dir, add, changed)) return kExitInternal;
    if (!changed) {
        printInfo(color::cyan, add ? "Already on your user PATH." : "Not on your user PATH; nothing to remove.");
        return 0;
    }
    printInfo(color::green, std::string(add ? "Added " : "Removed ") + narrow(dir.wstring()) +
                            (add ? " to" : " from") +
                            " your user PATH. Open a new terminal for it to take effect.");
    return 0;
}

int showHistory() {
    std::vector<std::string> lines = readHistory();
    if (lines.empty()) {
        printInfo(color::cyan, "No extraction history yet.");
        return 0;
    }
    const size_t start = lines.size() > 50 ? lines.size() - 50 : 0;
    for (size_t i = start; i < lines.size(); ++i) {
        std::vector<std::string> f = splitTabs(lines[i]);
        if (f.size() < 5) continue;
        unsigned long long bytes = allDigits(f[2]) ? std::stoull(f[2]) : 0;
        printInfo(color::green, f[0] + "  exit " + f[1] + "  " +
                                (bytes ? fmtBytes(bytes) : std::string("size unknown")) +
                                "  " + f[3] + "  ->  " + f[4]);
    }
    printInfo(color::cyan, "Showing the last " + std::to_string(lines.size() - start) +
                           " of " + std::to_string(lines.size()) + " entries. File: " +
                           narrow((dataDir() / L"history.tsv").wstring()));
    return 0;
}

int diagExtract(const char* archiveArg) {
    fs::path archive = fs::absolute(resolveSplitArchive(fs::u8path(archiveArg)));
    if (!isRegularFile(archive)) {
        printErr(std::string("Archive not found: ") + archiveArg);
        return kExitNotFound;
    }
    if (!preflightExpansion(archive, "diagnostic extraction")) return kExitUsage;
    const Settings st = loadSettings();
    fs::path outDir = extractDirFor(archive);
    std::error_code ec;
    fs::create_directories(outDir, ec);
    if (ec || !fs::is_directory(outDir, ec)) {
        printErr("Could not create the output directory: " + narrow(outDir.wstring()));
        return kExitInternal;
    }

    int scanCode = 0;
    unsigned long long total = 0, entries = 0, folders = 0;
    bool overflow = false;
    const bool scanned =
        scanArchive(archive, scanCode, total, entries, folders, overflow) &&
        scanCode <= 1 && !overflow;

    printInfo(color::green, "Extracting to " + narrow(outDir.wstring()) + " (" +
                            overwriteText(st.overwrite) + ").");
    const ULONGLONG t0 = GetTickCount64();
    int code = 0;
    std::string out;
    if (!capture7z({L"x", L"-ao" + std::wstring(1, static_cast<wchar_t>(st.overwrite)),
                    L"-bb1", L"-bt", L"-o" + outDir.wstring(), L"--", archive.wstring()},
                   code, out))
        return kExitInternal;
    const ULONGLONG ms = GetTickCount64() - t0;

    fs::path diag = outDir.parent_path() /
                    (stripArchiveExt(archive) + L"_diag_" + widen(nowStamp(true)) + L".txt");
    std::ofstream f(diag, std::ios::binary);
    if (!f) {
        printErr("Could not write the diagnostic file: " + narrow(diag.wstring()));
        return kExitInternal;
    }
    std::error_code sec;
    auto packed = fs::file_size(archive, sec);
    f << "8Ext extraction diagnostic\n"
      << "Wrapper version: " << kVersion << "\n"
      << "Date: " << nowStamp(false) << "\n"
      << "Archive: " << sanitize(narrow(archive.wstring())) << "\n"
      << "Archive size on disk: " << (sec ? std::string("unknown") : fmtBytes(packed)) << "\n";
    if (scanned)
        f << "Declared contents: " << (entries - folders) << " files, " << folders
          << " folders, " << total << " bytes uncompressed\n";
    else
        f << "Declared contents: not available (encrypted names or unreadable listing)\n";
    f << "Output folder: " << sanitize(narrow(outDir.wstring())) << "\n"
      << "Overwrite mode: -ao" << st.overwrite << " (" << overwriteText(st.overwrite) << ")\n"
      << "Elapsed: " << ms << " ms\n"
      << "7-Zip exit code: " << code << " (" << exitMeaning(code) << ")\n"
      << "\n---- 7-Zip output ----\n"
      << cleanLog(out) << "\n";
    f.close();

    recordHistory(archive, outDir, code, scanned ? total : 0);
    printInfo(code > 1 ? color::red : color::green,
              std::string("Finished with 7-Zip exit code ") + std::to_string(code) + " (" +
                  exitMeaning(code) + ") in " + std::to_string(ms) + " ms.");
    printInfo(color::green, "Diagnostic file: " + narrow(diag.wstring()));
    return code;
}

int extractWithLimit(const std::string& limitText, const char* archiveArg) {
    unsigned long long limit = 0;
    if (!parseSize(limitText, limit)) {
        printErr("Invalid limit. Use a number plus B, KB, MB, GB, TB, PB or EB "
                 "(binary units), for example 500MB or 1.5GB.");
        return kExitUsage;
    }
    fs::path archive = fs::absolute(resolveSplitArchive(fs::u8path(archiveArg)));
    if (!isRegularFile(archive)) {
        printErr(std::string("Archive not found: ") + archiveArg);
        return kExitNotFound;
    }

    int scanCode = 0;
    unsigned long long total = 0, entries = 0, folders = 0;
    bool overflow = false;
    if (!scanArchive(archive, scanCode, total, entries, folders, overflow))
        return kExitInternal;
    if (scanCode > 1 || overflow) {
        printErr("The archive's size cannot be verified, so the limit cannot be enforced. "
                 "Nothing was extracted.");
        return kExitUsage;
    }
    if (total > limit) {
        printErr("The archive expands to " + fmtBytes(total) + ", which exceeds the limit of " +
                 fmtBytes(limit) + ". Nothing was extracted.");
        return kExitUsage;
    }

    const Settings st = loadSettings();
    fs::path outDir = extractDirFor(archive);
    std::error_code ec;
    fs::create_directories(outDir, ec);
    if (ec || !fs::is_directory(outDir, ec)) {
        printErr("Could not create the output directory: " + narrow(outDir.wstring()));
        return kExitInternal;
    }
    fs::space_info sp = fs::space(outDir, ec);
    if (!ec && sp.available < total) {
        printErr("Not enough free space: need " + fmtBytes(total) + ", have " +
                 fmtBytes(sp.available) + ".");
        return kExitUsage;
    }

    printInfo(color::green, "Archive expands to " + fmtBytes(total) + " (limit " +
                            fmtBytes(limit) + "). Extracting to " + narrow(outDir.wstring()) +
                            " (" + overwriteText(st.overwrite) + ").");
    const int rc = extractAny(archive, outDir, st.overwrite, L"", limit);
    recordHistory(archive, outDir, rc, total);
    return rc;
}

int showBytesExtracted(const char* archiveArg) {
    fs::path archive = fs::absolute(resolveSplitArchive(fs::u8path(archiveArg)));
    if (!isRegularFile(archive)) {
        printErr(std::string("Archive not found: ") + archiveArg);
        return kExitNotFound;
    }
    const std::string key = lowerA(sanitize(narrow(archive.wstring())));

    unsigned long long runs = 0, bytes = 0;
    for (const auto& line : readHistory()) {
        std::vector<std::string> f = splitTabs(line);
        if (f.size() < 5 || lowerA(f[3]) != key) continue;
        if (std::atoi(f[1].c_str()) > 1) continue;
        ++runs;
        if (allDigits(f[2])) bytes += std::stoull(f[2]);
    }
    printInfo(color::green, "Recorded successful extractions of this archive: " +
                            std::to_string(runs) + ", " + std::to_string(bytes) +
                            " bytes (" + fmtBytes(bytes) + ") in total.");

    int code = 0;
    unsigned long long total = 0, entries = 0, folders = 0;
    bool overflow = false;
    if (scanArchive(archive, code, total, entries, folders, overflow) && code <= 1 &&
        !overflow) {
        std::error_code ec;
        auto packed = fs::file_size(archive, ec);
        std::string msg = "Archive contents: " + fmtBytes(total) + " uncompressed";
        if (!ec) {
            msg += ", " + fmtBytes(packed) + " compressed";
            if (total > 0) {
                char pct[32];
                std::snprintf(pct, sizeof pct, "%.1f",
                              100.0 * static_cast<double>(packed) / static_cast<double>(total));
                msg += " (" + std::string(pct) + "%)";
            }
        }
        printInfo(color::green, msg);
    } else {
        printInfo(color::cyan, "The archive's current size could not be read.");
    }
    return 0;
}

int convertArchive(const std::string& srcName, const std::string& targetName,
                   const char* archiveArg) {
    const Fmt target = parseFormatName(targetName);
    if (!isCreatable(target)) {
        printErr("Unknown target format. Use zip, 7z, tar, tar.gz, tar.bz2, tar.xz or rar.");
        return kExitUsage;
    }
    fs::path archive = fs::absolute(resolveSplitArchive(fs::u8path(archiveArg)));
    if (!isRegularFile(archive)) {
        printErr(std::string("Archive not found: ") + archiveArg);
        return kExitNotFound;
    }
    if (srcName != "auto") {
        const Fmt src = parseFormatName(srcName);
        if (src == Fmt::Unknown) {
            printErr("Unknown source format: " + srcName);
            return kExitUsage;
        }
        if (src != detectFormat(archive)) {
            printErr("The archive's extension does not match the source format '" + srcName +
                     "'. Use 'auto' to skip this check.");
            return kExitUsage;
        }
    }
    if (target == Fmt::Rar && findRarExe().empty()) {
        printErr("Creating .rar files needs WinRAR's Rar.exe, which was not found.");
        return kExitNotFound;
    }

    fs::path outPath = archive.parent_path() / (stripArchiveExt(archive) + extFor(target));
    if (lowerW(outPath.lexically_normal().wstring()) ==
        lowerW(archive.lexically_normal().wstring())) {
        printErr("The source and target formats are the same; use -recompress instead.");
        return kExitUsage;
    }
    if (fs::exists(outPath)) {
        printErr("Output already exists, refusing to overwrite: " + narrow(outPath.wstring()));
        return kExitUsage;
    }

    int scanCode = 0;
    unsigned long long total = 0, entries = 0, folders = 0;
    bool overflow = false;
    if (!scanArchive(archive, scanCode, total, entries, folders, overflow))
        return kExitInternal;
    if (scanCode > 1) {
        printInfo(color::cyan,
                  "Could not pre-check the archive size; extraction will require explicit "
                  "confirmation if the size remains unverifiable.");
    } else {
        if (overflow || total > kMaxConvertBytes || entries > kMaxConvertEntries) {
            printErr("The archive exceeds the conversion limits (" +
                     fmtBytes(kMaxConvertBytes) + ", " +
                     std::to_string(kMaxConvertEntries) + " entries).");
            return kExitUsage;
        }
        std::error_code sec;
        fs::path tmpRoot = fs::temp_directory_path(sec);
        if (!sec) {
            fs::space_info sp = fs::space(tmpRoot, sec);
            if (!sec && sp.available < total) {
                printErr("Not enough free space in the temp directory: need " +
                         fmtBytes(total) + ", have " + fmtBytes(sp.available) + ".");
                return kExitUsage;
            }
        }
        sec.clear();
        fs::space_info zp = fs::space(outPath.parent_path(), sec);
        if (!sec && zp.available < total) {
            printErr("Not enough free space for the output: up to " + fmtBytes(total) +
                     " may be needed, have " + fmtBytes(zp.available) + ".");
            return kExitUsage;
        }
    }

    ScopedDir tmp;
    if (!tmp.create()) {
        printErr("Could not create a unique temporary directory.");
        return kExitInternal;
    }

    printInfo(color::green, "Extracting " + narrow(archive.wstring()) +
                            " to a temporary directory.");
    int rc = extractAny(archive, tmp.path, 'a');
    if (rc > 1) {
        printErr("Extraction failed (exit code " + std::to_string(rc) + ").");
        return rc;
    }
    std::error_code ec;
    if (fs::directory_iterator(tmp.path, ec) == fs::directory_iterator()) {
        printErr("The archive contained no files; nothing to convert.");
        return kExitUsage;
    }

    printInfo(color::green, std::string("Creating ") + fmtLabel(target) + " archive " +
                            narrow(outPath.wstring()));
    rc = packTree(tmp.path, outPath, target, "5", false);
    if (rc > 1 || rc < 0) return rc;

    recordHistory(archive, outPath, rc, scanCode > 1 ? 0 : total);
    printInfo(rc == 1 ? color::cyan : color::green,
              rc == 1 ? "Conversion completed with warnings; inspect the output archive."
                      : "Done. The original archive was kept.");
    return rc;
}

int convertToZip(const char* archiveArg) { return convertArchive("auto", "zip", archiveArg); }

int extractCommand(const char* archiveArg, const char* outArg) {
    fs::path archive = fs::absolute(resolveSplitArchive(fs::u8path(archiveArg)));
    if (!isRegularFile(archive)) {
        printErr(std::string("Archive not found: ") + archiveArg);
        return kExitNotFound;
    }
    fs::path outDir = outArg ? fs::absolute(fs::u8path(outArg)) : extractDirFor(archive);
    std::error_code ec;
    fs::create_directories(outDir, ec);
    if (ec || !fs::is_directory(outDir, ec)) {
        printErr("Could not create the output directory: " + narrow(outDir.wstring()));
        return kExitInternal;
    }

    const Settings st = loadSettings();
    int scanCode = 0;
    unsigned long long total = 0, entries = 0, folders = 0;
    bool overflow = false;
    const bool scanned = scanArchive(archive, scanCode, total, entries, folders, overflow) &&
                         scanCode <= 1 && !overflow;

    printInfo(color::green, std::string("Extracting ") + fmtLabel(detectFormat(archive)) +
                            " archive to " + narrow(outDir.wstring()) + " (" +
                            overwriteText(st.overwrite) + ").");
    const int rc = extractAny(archive, outDir, st.overwrite);
    recordHistory(archive, outDir, rc, scanned ? total : 0);
    return rc;
}

int testArchive(const char* archiveArg) {
    fs::path archive = fs::absolute(resolveSplitArchive(fs::u8path(archiveArg)));
    if (!isRegularFile(archive)) {
        printErr(std::string("Archive not found: ") + archiveArg);
        return kExitNotFound;
    }
    const int rc = static_cast<int>(run7z({L"t", L"--", archive.wstring()}));
    if (rc == 0)
        printInfo(color::green, "Integrity test passed.");
    else if (rc == 1)
        printInfo(color::cyan, "Integrity test finished with warnings.");
    else
        printErr("Integrity test FAILED (exit code " + std::to_string(rc) + ", " +
                 exitMeaning(rc) + ").");
    return rc;
}

int infoArchive(const char* archiveArg) {
    fs::path archive = fs::absolute(resolveSplitArchive(fs::u8path(archiveArg)));
    if (!isRegularFile(archive)) {
        printErr(std::string("Archive not found: ") + archiveArg);
        return kExitNotFound;
    }
    int code = 0;
    unsigned long long total = 0, entries = 0, folders = 0;
    bool overflow = false;
    std::vector<std::string> header;
    if (!scanArchive(archive, code, total, entries, folders, overflow, &header))
        return kExitInternal;
    if (code > 1) {
        printErr("7-Zip could not read the archive (exit code " + std::to_string(code) +
                 "). It may be corrupt, or its file names may be encrypted.");
        return code;
    }

    const Fmt fmt = detectFormat(archive);
    printInfo(color::green, "File: " + narrow(archive.filename().wstring()));
    printInfo(color::green, std::string("Format by extension: ") + fmtLabel(fmt));
    std::string type;
    for (const auto& h : header) {
        printInfo(color::green, "  " + h);
        if (h.rfind("Type = ", 0) == 0) type = lowerA(h.substr(7));
    }

    std::string want;
    switch (fmt) {
        case Fmt::Sevenz: want = "7z"; break;
        case Fmt::Zip: want = "zip"; break;
        case Fmt::Tar: want = "tar"; break;
        case Fmt::TarGz:
        case Fmt::Gz: want = "gzip"; break;
        case Fmt::TarBz2:
        case Fmt::Bz2: want = "bzip2"; break;
        case Fmt::TarXz:
        case Fmt::Xz: want = "xz"; break;
        case Fmt::Rar: want = "rar"; break;
        default: break;
    }
    if (!want.empty() && !type.empty() && type.rfind(want, 0) != 0)
        printInfo(color::cyan, "Warning: the extension suggests " + want +
                               " but 7-Zip reads this file as " + type + ".");

    if (!overflow)
        printInfo(color::green, "Files: " + std::to_string(entries - folders) +
                                ", folders: " + std::to_string(folders) +
                                ", uncompressed: " + fmtBytes(total));
    if (isTarWrapped(fmt))
        printInfo(color::cyan, "Compressed tar: the sizes above describe the outer layer only.");
    return 0;
}

int showFormats() {
    const bool rar = !findRarExe().empty();
    printInfo(color::green, ".7z               read, create (-encrypt: AES-256 + hidden names)");
    printInfo(color::green, ".zip              read, create (UTF-8 names)");
    printInfo(color::green, ".tar              read, create");
    printInfo(color::green, ".tar.gz  .tgz     read (auto-unwrapped), create");
    printInfo(color::green, ".tar.bz2 .tbz2    read (auto-unwrapped), create");
    printInfo(color::green, ".tar.xz  .txz     read (auto-unwrapped), create");
    printInfo(color::green, ".gz .bz2 .xz      read; single files via -compress-file");
    printInfo(rar ? color::green : color::cyan,
              std::string(".rar              read, test, convert from; create/repair: ") +
                  (rar ? "available (WinRAR's Rar.exe found)"
                       : "NOT available (needs WinRAR's Rar.exe)"));
    return 0;
}

int compressFile(const std::string& fmtName, const char* fileArg, const std::string& level) {
    std::wstring type, ext;
    const std::string f = lowerA(fmtName);
    if (f == "gz") {
        type = L"gzip";
        ext = L".gz";
    } else if (f == "bz2") {
        type = L"bzip2";
        ext = L".bz2";
    } else if (f == "xz") {
        type = L"xz";
        ext = L".xz";
    } else {
        printErr("FORMAT must be gz, bz2 or xz.");
        return kExitUsage;
    }
    if (!validLevel(level)) {
        printErr("LEVEL must be a single digit 0-9.");
        return kExitUsage;
    }
    fs::path file = fs::absolute(fs::u8path(fileArg));
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) {
        printErr("Not a file (use -recompress for folders): " + narrow(file.wstring()));
        return kExitNotFound;
    }
    fs::path out = file;
    out += ext;
    if (fs::exists(out)) {
        printErr("Output already exists, refusing to overwrite: " + narrow(out.wstring()));
        return kExitUsage;
    }

    const int rc = static_cast<int>(run7z({L"a", L"-t" + type, L"-mx=" + widen(level), L"--",
                                           out.wstring(), file.wstring()}));
    if (rc > 1) {
        fs::remove(out, ec);
        printErr("Compression failed (7-Zip exit code " + std::to_string(rc) + ").");
        return rc;
    }
    if (!isRegularFile(out)) {
        printErr("7-Zip reported completion but the compressed output is missing.");
        return kExitInternal;
    }
    if (rc == 1) printInfo(color::cyan, "Compression completed with warnings.");
    auto before = fs::file_size(file, ec);
    std::error_code ec2;
    auto after = fs::file_size(out, ec2);
    if (!ec && !ec2)
        printInfo(color::green, "Created " + narrow(out.wstring()) + ": " + fmtBytes(before) +
                                " -> " + fmtBytes(after) + ". The original was kept.");
    return rc;
}

int rarRepair(const char* archiveArg) {
    fs::path archive = fs::absolute(resolveSplitArchive(fs::u8path(archiveArg)));
    if (!isRegularFile(archive)) {
        printErr(std::string("Archive not found: ") + archiveArg);
        return kExitNotFound;
    }
    if (detectFormat(archive) != Fmt::Rar) {
        printErr("-rar-repair only works on .rar archives.");
        return kExitUsage;
    }
    fs::path rar = findRarExe();
    if (rar.empty()) {
        printErr("Repairing needs WinRAR's Rar.exe, which was not found.");
        return kExitNotFound;
    }
    printInfo(color::green, "Repairing " + narrow(archive.wstring()) +
                            ". A repaired copy is written next to it (fixed.* or rebuilt.*); "
                            "the original is not modified.");
    const int rc = runTool(rar, {L"r", L"--", archive.wstring()}, archive.parent_path());
    if (rc < 0) return kExitInternal;
    if (rc != 0) printErr("Rar.exe reported exit code " + std::to_string(rc) + ".");
    return rc;
}

class EightExt {
public:
    int execute(std::vector<std::string> args) {
        try {
            if (args.size() >= 3 && args[1] == "-C") {
                fs::path dir = fs::u8path(args[2]);
                if (!isDirectory(dir)) {
                    printErr("Directory does not exist: " + args[2]);
                    return kExitNotFound;
                }
                std::error_code cdec;
                fs::current_path(dir, cdec);
                if (cdec) {
                    printErr("Could not change directory: " + cdec.message());
                    return kExitInternal;
                }
                args.erase(args.begin() + 1, args.begin() + 3);
            }

            if (args.size() < 2) {
                printUsage();
                return kExitUsage;
            }
            const std::string cmd = args[1];

            if (cmd == "-uncompressed") {
                if (args.size() != 4 || args[2] != "size") {
                    printErr("Usage: -uncompressed size ARCHIVE");
                    return kExitUsage;
                }
                return listArchive(args[3].c_str());
            }

            if (cmd == "-ver") {
                printInfo(color::cyan, std::string("Wrapper Version: ") + kVersion);
                return static_cast<int>(run7z({L"i"}));
            }

            if (cmd.rfind("-block-", 0) == 0) {
                const std::string ext = cmd.substr(7);
                if (ext.empty() || !validExtension(ext)) {
                    printErr("Invalid extension. Use letters, digits, _ or -.");
                    return kExitUsage;
                }
                if (args.size() != 5 || args[2] != "-from-extracting") {
                    printErr("Usage: -block-EXT -from-extracting ARCHIVE OUTPUT_DIR");
                    return kExitUsage;
                }
                fs::path archive = resolveSplitArchive(fs::u8path(args[3]));
                if (!isRegularFile(archive)) {
                    printErr("Archive not found: " + args[3]);
                    return kExitNotFound;
                }
                fs::path outDir = fs::absolute(fs::u8path(args[4]));
                std::error_code mkec;
                fs::create_directories(outDir, mkec);
                if (mkec || !fs::is_directory(outDir, mkec)) {
                    printErr("Could not create the output directory: " +
                             narrow(outDir.wstring()));
                    return kExitInternal;
                }
                const Settings st = loadSettings();
                printInfo(color::green,
                          "Extracting while excluding *." + ext + " files into " +
                          narrow(outDir.wstring()) + " (" + overwriteText(st.overwrite) + ").");
                int bCode = 0;
                unsigned long long bTotal = 0, bEntries = 0, bFolders = 0;
                bool bOver = false;
                if (!scanArchive(archive, bCode, bTotal, bEntries, bFolders, bOver) ||
                    bCode > 1 || bOver)
                    bTotal = 0;
                const int rc =
                    extractAny(archive, outDir, st.overwrite, L"-x!*." + widen(ext));
                recordHistory(archive, outDir, rc, bTotal);
                return rc;
            }

            if (cmd == "-convert") {
                if (args.size() != 5 || args[3].rfind("-as_", 0) != 0) {
                    printErr("Usage: -convert SOURCE_FORMAT -as_TARGET ARCHIVE "
                             "(SOURCE_FORMAT may be 'auto')");
                    return kExitUsage;
                }
                return convertArchive(args[2], args[3].substr(4), args[4].c_str());
            }

            if (cmd == "-recompress" || cmd == "-encrypt") {
                const bool enc = (cmd == "-encrypt");
                if (args.size() < 3 || args.size() > 5) {
                    printErr(enc ? "Usage: -encrypt OUTPUT.7z [LEVEL 0-9] [SOURCE_DIR]"
                                 : "Usage: -recompress OUTPUT_ARCHIVE [LEVEL 0-9] [SOURCE_DIR]");
                    return kExitUsage;
                }
                const std::string level = args.size() >= 4 ? args[3] : "5";
                const std::string source =
                    args.size() >= 5 ? args[4] : "_secure_encrypted_sandbox";
                if (!validLevel(level)) {
                    printErr("LEVEL must be a single digit 0-9.");
                    return kExitUsage;
                }
                if (fs::exists(fs::u8path(args[2]))) {
                    printErr("Output already exists, refusing to overwrite: " + args[2]);
                    return kExitUsage;
                }
                if (!prepSource(source)) return kExitNotFound;
                if (args.size() < 5 &&
                    !confirm(std::string(enc ? "Encrypt" : "Recompress") +
                             " the contents of this default directory?")) {
                    printErr("Cancelled.");
                    return kExitUsage;
                }
                return enc ? encryptArchive(args[2].c_str(), level, source)
                           : recompress(args[2].c_str(), level, source);
            }

            if (cmd == "-advanced_options") {
                if (args.size() == 5 && args[2] == "-creatediag7zExtraction" &&
                    args[3] == "-extract")
                    return diagExtract(args[4].c_str());
                if (args.size() == 5 && args[2] == "-showBytesExtracted" &&
                    args[3] == "-compress") {
                    if (!requireDebug()) return kExitUsage;
                    return showBytesExtracted(args[4].c_str());
                }
                printErr("Usage: -advanced_options -creatediag7zExtraction -extract ARCHIVE  |  "
                         "-advanced_options -showBytesExtracted -compress ARCHIVE");
                return kExitUsage;
            }

            if (cmd == "-7zSettings") {
                if (args.size() == 3 && args[2] == "-enable_debugging") return setDebugging(true);
                if (args.size() == 3 && args[2] == "-disable_debugging") return setDebugging(false);
                printErr("Usage: -7zSettings -enable_debugging | -disable_debugging");
                return kExitUsage;
            }

            if (cmd == "-extractionHistory") {
                if (args.size() != 2) {
                    printErr("Usage: -extractionHistory");
                    return kExitUsage;
                }
                return showHistory();
            }

            if (cmd == "-debugging_options") {
                if (!requireDebug()) return kExitUsage;
                if (args.size() == 6 && args[2] == "-extractSizelimit" && args[4] == "-compress")
                    return extractWithLimit(args[3], args[5].c_str());
                if (args.size() == 3 && args[2].size() == 4 && args[2].rfind("-ao", 0) == 0)
                    return setOverwriteMode(args[2][3]);
                printErr("Usage: -debugging_options -extractSizelimit LIMIT -compress ARCHIVE  |  "
                         "-debugging_options -ao{a|s|t|u}");
                return kExitUsage;
            }

            if (cmd == "-extract") {
                if (args.size() < 3 || args.size() > 4) {
                    printErr("Usage: -extract ARCHIVE [OUTPUT_DIR]");
                    return kExitUsage;
                }
                return extractCommand(args[2].c_str(), args.size() == 4 ? args[3].c_str() : nullptr);
            }

            if (cmd == "-test") {
                if (args.size() != 3) {
                    printErr("Usage: -test ARCHIVE");
                    return kExitUsage;
                }
                return testArchive(args[2].c_str());
            }

            if (cmd == "-info") {
                if (args.size() != 3) {
                    printErr("Usage: -info ARCHIVE");
                    return kExitUsage;
                }
                return infoArchive(args[2].c_str());
            }

            if (cmd == "-formats") return showFormats();

            if (cmd == "-compress-file") {
                if (args.size() < 4 || args.size() > 5) {
                    printErr("Usage: -compress-file gz|bz2|xz FILE [LEVEL 0-9]");
                    return kExitUsage;
                }
                return compressFile(args[2], args[3].c_str(), args.size() == 5 ? args[4] : "5");
            }

            if (cmd == "-rar-repair") {
                if (args.size() != 3) {
                    printErr("Usage: -rar-repair ARCHIVE.rar");
                    return kExitUsage;
                }
                return rarRepair(args[2].c_str());
            }

            if (cmd == "-addpath") return pathCommand(true);
            if (cmd == "-removepath") return pathCommand(false);

            if (cmd == "-temp") {
                return showTempStatus();
            }

            printErr("Unknown command: " + cmd);
        } catch (const fs::filesystem_error& e) {
            printErr(std::string("Filesystem error: ") + e.what());
            return kExitInternal;
        } catch (const std::exception& e) {
            printErr(e.what());
            return kExitInternal;
        } catch (...) {
            printErr("Unknown internal error.");
            return kExitInternal;
        }

        printUsage();
        return kExitUsage;
    }
};

int interactiveLaunch() {
    Settings st = loadSettings();
    const fs::path dir = exePath().parent_path();
    printInfo(color::cyan, "This is a command-line tool. Run it from a terminal.");
    if (!dir.empty() && !st.pathAsked && !dirInList(envVar(L"PATH"), dir)) {
        st.pathAsked = true;
        saveSettings(st);
        if (confirm("Add this program's folder to your user PATH so it can be run from any folder?"))
            pathCommand(true);
    }
    printUsage();
    std::cout << "\nPress Enter to close..." << std::flush;
    std::string pause;
    std::getline(std::cin, pause);
    return 0;
}

int main() {
    ConsoleSetup console;

    int argc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!wargv) {
        printErr("Could not read the command line.");
        return kExitInternal;
    }
    std::vector<std::string> args;
    args.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        std::string a = toUtf8(wargv[i]);
        if (a.empty() && wargv[i][0] != L'\0') {
            LocalFree(wargv);
            printErr("A command-line argument is not valid Unicode.");
            return kExitUsage;
        }
        args.push_back(std::move(a));
    }
    LocalFree(wargv);

    if (args.size() == 1 && launchedByDoubleClick()) return interactiveLaunch();

    EightExt cli;
    return cli.execute(std::move(args));
}