// VhdxBackup - gemeinsame Hilfsfunktionen: Fehler, RAII, Logging, Strings.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace vb {

constexpr const wchar_t* kToolName = L"VhdxBackup";
constexpr const wchar_t* kToolVersion = L"0.5.0";

// ---------------------------------------------------------------- Strings
std::wstring Fmt(const wchar_t* fmt, ...);
std::wstring VFmt(const wchar_t* fmt, va_list ap);
std::string ToUtf8(const std::wstring& s);
std::wstring FromUtf8(const std::string& s);
std::wstring ToLower(std::wstring s);
std::wstring Trim(const std::wstring& s);
std::wstring GuidToString(const GUID& g);
std::wstring JoinPath(const std::wstring& a, const std::wstring& b);
std::wstring NowIso();            // 2026-11-26T14:03:00 (lokale Zeit)
std::wstring NowStamp();          // 20261126-140300 (für Dateinamen)
std::wstring FormatBytes(uint64_t bytes);
std::wstring FormatWin32Error(DWORD err);

// ---------------------------------------------------------------- Fehler
class Error : public std::runtime_error {
public:
    explicit Error(std::wstring msg) : std::runtime_error(ToUtf8(msg)), msg_(std::move(msg)) {}
    const std::wstring& Message() const { return msg_; }
private:
    std::wstring msg_;
};

[[noreturn]] void ThrowWin32(const std::wstring& what, DWORD err = GetLastError());
[[noreturn]] void ThrowHr(const std::wstring& what, HRESULT hr);
inline void CheckHr(HRESULT hr, const wchar_t* what) {
    if (FAILED(hr)) ThrowHr(what, hr);
}

// ---------------------------------------------------------------- RAII
class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE h) : h_(h) {}
    ~UniqueHandle() { Reset(); }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& o) noexcept : h_(o.Release()) {}
    UniqueHandle& operator=(UniqueHandle&& o) noexcept {
        if (this != &o) { Reset(o.Release()); }
        return *this;
    }
    HANDLE Get() const { return h_; }
    bool Valid() const { return h_ != nullptr && h_ != INVALID_HANDLE_VALUE; }
    HANDLE Release() { HANDLE h = h_; h_ = INVALID_HANDLE_VALUE; return h; }
    void Reset(HANDLE h = INVALID_HANDLE_VALUE) {
        if (Valid()) CloseHandle(h_);
        h_ = h;
    }
private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
};

template <class T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { Reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    T* Get() const { return p_; }
    T* operator->() const { return p_; }
    T** Put() { Reset(); return &p_; }
    void Reset() { if (p_) { p_->Release(); p_ = nullptr; } }
    explicit operator bool() const { return p_ != nullptr; }
private:
    T* p_ = nullptr;
};

class BStr {
public:
    BStr() = default;
    ~BStr() { if (b_) SysFreeString(b_); }
    BStr(const BStr&) = delete;
    BStr& operator=(const BStr&) = delete;
    BSTR* Put() { if (b_) { SysFreeString(b_); b_ = nullptr; } return &b_; }
    std::wstring Str() const { return b_ ? std::wstring(b_, SysStringLen(b_)) : std::wstring(); }
private:
    BSTR b_ = nullptr;
};

// Seitenausgerichteter Puffer für ungepufferte Datenträger-I/O.
class AlignedBuffer {
public:
    explicit AlignedBuffer(size_t size);
    ~AlignedBuffer();
    AlignedBuffer(const AlignedBuffer&) = delete;
    AlignedBuffer& operator=(const AlignedBuffer&) = delete;
    uint8_t* Data() const { return p_; }
    size_t Size() const { return size_; }
private:
    uint8_t* p_ = nullptr;
    size_t size_ = 0;
};

// ---------------------------------------------------------------- I/O
// Liest/schreibt exakt len Bytes an einem absoluten Offset (synchrones Handle).
void ReadAt(HANDLE h, uint64_t offset, void* buf, uint32_t len, const wchar_t* what);
void WriteAt(HANDLE h, uint64_t offset, const void* buf, uint32_t len, const wchar_t* what);

bool FileExists(const std::wstring& path);
bool DirExists(const std::wstring& path);
void CreateDirs(const std::wstring& path);
void WriteTextFileUtf8(const std::wstring& path, const std::wstring& text, bool bom);
void DeleteTree(const std::wstring& path);
std::vector<std::wstring> ListDir(const std::wstring& dir, const wchar_t* pattern, bool dirs);

// ---------------------------------------------------------------- Logging
void LogOpenFile(const std::wstring& path);
void LogCloseFile();
void LogInfo(const wchar_t* fmt, ...);
void LogWarn(const wchar_t* fmt, ...);   // wird zusätzlich als Warnung gesammelt
void LogError(const wchar_t* fmt, ...);
void ConsoleProgress(const std::wstring& line);  // nur Konsole, überschreibt Zeile
const std::vector<std::wstring>& CollectedWarnings();
void ClearCollectedWarnings();

}  // namespace vb
