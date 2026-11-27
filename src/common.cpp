#include "common.h"

#include <cwchar>
#include <cwctype>
#include <mutex>

namespace vb {

// ---------------------------------------------------------------- Strings

std::wstring VFmt(const wchar_t* fmt, va_list ap) {
    std::vector<wchar_t> buf(512);
    for (;;) {
        va_list cp;
        va_copy(cp, ap);
        int n = _vsnwprintf_s(buf.data(), buf.size(), _TRUNCATE, fmt, cp);
        va_end(cp);
        if (n >= 0) return std::wstring(buf.data(), static_cast<size_t>(n));
        if (buf.size() > (1u << 20)) return std::wstring(buf.data());
        buf.resize(buf.size() * 2);
    }
}

std::wstring Fmt(const wchar_t* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::wstring s = VFmt(fmt, ap);
    va_end(ap);
    return s;
}

std::string ToUtf8(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::wstring ToLower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return {};
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::wstring GuidToString(const GUID& g) {
    return Fmt(L"%08lx-%04hx-%04hx-%02x%02x-%02x%02x%02x%02x%02x%02x",
               static_cast<unsigned long>(g.Data1), g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2],
               g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
}

std::wstring JoinPath(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (a.back() == L'\\' || a.back() == L'/') return a + b;
    return a + L"\\" + b;
}

std::wstring NowIso() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    return Fmt(L"%04u-%02u-%02uT%02u:%02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
}

std::wstring NowStamp() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    return Fmt(L"%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
}

std::wstring FormatBytes(uint64_t bytes) {
    const wchar_t* units[] = {L"B", L"KiB", L"MiB", L"GiB", L"TiB"};
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }
    return u == 0 ? Fmt(L"%llu B", static_cast<unsigned long long>(bytes)) : Fmt(L"%.1f %s", v, units[u]);
}

std::wstring FormatWin32Error(DWORD err) {
    wchar_t* msg = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, err, 0, reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
    std::wstring s = n ? Trim(std::wstring(msg, n)) : std::wstring(L"unbekannter Fehler");
    if (msg) LocalFree(msg);
    return Fmt(L"%s (Fehler %lu)", s.c_str(), static_cast<unsigned long>(err));
}

// ---------------------------------------------------------------- Fehler

void ThrowWin32(const std::wstring& what, DWORD err) {
    throw Error(what + L": " + FormatWin32Error(err));
}

void ThrowHr(const std::wstring& what, HRESULT hr) {
    std::wstring text;
    if (HRESULT_FACILITY(hr) == FACILITY_WIN32) {
        text = FormatWin32Error(HRESULT_CODE(hr));
    }
    throw Error(Fmt(L"%s: HRESULT 0x%08lX %s", what.c_str(), static_cast<unsigned long>(hr), text.c_str()));
}

// ---------------------------------------------------------------- Puffer

AlignedBuffer::AlignedBuffer(size_t size) : size_(size) {
    p_ = static_cast<uint8_t*>(VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!p_) ThrowWin32(L"Speicher anfordern");
}

AlignedBuffer::~AlignedBuffer() {
    if (p_) VirtualFree(p_, 0, MEM_RELEASE);
}

// ---------------------------------------------------------------- I/O

void ReadAt(HANDLE h, uint64_t offset, void* buf, uint32_t len, const wchar_t* what) {
    uint8_t* p = static_cast<uint8_t*>(buf);
    while (len > 0) {
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFu);
        ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD got = 0;
        if (!ReadFile(h, p, len, &got, &ov)) {
            ThrowWin32(Fmt(L"Lesen %s bei Offset %llu", what, static_cast<unsigned long long>(offset)));
        }
        if (got == 0) {
            throw Error(Fmt(L"Lesen %s bei Offset %llu: unerwartetes Ende", what,
                            static_cast<unsigned long long>(offset)));
        }
        p += got;
        offset += got;
        len -= got;
    }
}

void WriteAt(HANDLE h, uint64_t offset, const void* buf, uint32_t len, const wchar_t* what) {
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    while (len > 0) {
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFu);
        ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD put = 0;
        if (!WriteFile(h, p, len, &put, &ov)) {
            ThrowWin32(Fmt(L"Schreiben %s bei Offset %llu", what, static_cast<unsigned long long>(offset)));
        }
        if (put == 0) {
            throw Error(Fmt(L"Schreiben %s bei Offset %llu: nichts geschrieben", what,
                            static_cast<unsigned long long>(offset)));
        }
        p += put;
        offset += put;
        len -= put;
    }
}

bool FileExists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool DirExists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

void CreateDirs(const std::wstring& path) {
    if (path.empty() || DirExists(path)) return;
    size_t pos = path.find_last_of(L"\\/");
    if (pos != std::wstring::npos && pos > 0) {
        std::wstring parent = path.substr(0, pos);
        if (!(parent.size() == 2 && parent[1] == L':')) CreateDirs(parent);
    }
    if (!CreateDirectoryW(path.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        ThrowWin32(L"Ordner anlegen " + path);
    }
}

void WriteTextFileUtf8(const std::wstring& path, const std::wstring& text, bool bom) {
    std::wstring tmp = path + L".tmp";
    UniqueHandle h(CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!h.Valid()) ThrowWin32(L"Datei anlegen " + tmp);
    std::string data = ToUtf8(text);
    if (bom) data.insert(0, "\xEF\xBB\xBF");
    DWORD put = 0;
    if (!WriteFile(h.Get(), data.data(), static_cast<DWORD>(data.size()), &put, nullptr) || put != data.size()) {
        ThrowWin32(L"Datei schreiben " + tmp);
    }
    FlushFileBuffers(h.Get());
    h.Reset();
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        ThrowWin32(L"Datei ersetzen " + path);
    }
}

std::vector<std::wstring> ListDir(const std::wstring& dir, const wchar_t* pattern, bool dirs) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(JoinPath(dir, pattern).c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return out;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (isDir == dirs) out.push_back(name);
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    return out;
}

void DeleteTree(const std::wstring& path) {
    for (const auto& f : ListDir(path, L"*", false)) {
        std::wstring p = JoinPath(path, f);
        SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (!DeleteFileW(p.c_str())) ThrowWin32(L"Datei löschen " + p);
    }
    for (const auto& d : ListDir(path, L"*", true)) DeleteTree(JoinPath(path, d));
    if (!RemoveDirectoryW(path.c_str())) ThrowWin32(L"Ordner löschen " + path);
}

// ---------------------------------------------------------------- Logging

namespace {
std::mutex g_logMutex;
HANDLE g_logFile = INVALID_HANDLE_VALUE;
std::vector<std::wstring> g_warnings;
bool g_progressActive = false;

void WriteConsoleText(const std::wstring& s, bool err) {
    HANDLE out = GetStdHandle(err ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    if (out == nullptr || out == INVALID_HANDLE_VALUE) return;
    DWORD mode = 0;
    DWORD n = 0;
    if (GetConsoleMode(out, &mode)) {
        WriteConsoleW(out, s.data(), static_cast<DWORD>(s.size()), &n, nullptr);
    } else {
        std::string u = ToUtf8(s);
        WriteFile(out, u.data(), static_cast<DWORD>(u.size()), &n, nullptr);
    }
}

void LogLine(const wchar_t* level, const std::wstring& msg, bool err) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    std::wstring line = NowIso() + L" " + level + L" " + msg + L"\r\n";
    if (g_progressActive) {
        WriteConsoleText(L"\r\n", false);
        g_progressActive = false;
    }
    WriteConsoleText(std::wstring(level) + L" " + msg + L"\r\n", err);
    if (g_logFile != INVALID_HANDLE_VALUE) {
        std::string u = ToUtf8(line);
        DWORD n = 0;
        WriteFile(g_logFile, u.data(), static_cast<DWORD>(u.size()), &n, nullptr);
    }
}
}  // namespace

void LogOpenFile(const std::wstring& path) {
    LogCloseFile();
    g_logFile = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
}

void LogCloseFile() {
    if (g_logFile != INVALID_HANDLE_VALUE) {
        CloseHandle(g_logFile);
        g_logFile = INVALID_HANDLE_VALUE;
    }
}

void LogInfo(const wchar_t* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::wstring m = VFmt(fmt, ap);
    va_end(ap);
    LogLine(L"[INFO]", m, false);
}

void LogWarn(const wchar_t* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::wstring m = VFmt(fmt, ap);
    va_end(ap);
    {
        std::lock_guard<std::mutex> lock(g_logMutex);
        g_warnings.push_back(m);
    }
    LogLine(L"[WARNUNG]", m, true);
}

void LogError(const wchar_t* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::wstring m = VFmt(fmt, ap);
    va_end(ap);
    LogLine(L"[FEHLER]", m, true);
}

void ConsoleProgress(const std::wstring& line) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    WriteConsoleText(L"\r" + line + L"   ", false);
    g_progressActive = true;
}

const std::vector<std::wstring>& CollectedWarnings() { return g_warnings; }
void ClearCollectedWarnings() { g_warnings.clear(); }

}  // namespace vb
