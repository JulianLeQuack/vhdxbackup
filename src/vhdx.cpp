#include "vhdx.h"

#include <initguid.h>
#include <virtdisk.h>
#include <winioctl.h>

#include <cstddef>

namespace vb {

namespace {

// Aus winioctl.h des Windows SDK; ältere MinGW-Header enthalten sie nicht.
#ifndef IOCTL_DISK_SET_DISK_ATTRIBUTES
#define IOCTL_DISK_SET_DISK_ATTRIBUTES CTL_CODE(IOCTL_DISK_BASE, 0x003d, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#endif
#ifndef IOCTL_DISK_GET_DISK_ATTRIBUTES
#define IOCTL_DISK_GET_DISK_ATTRIBUTES CTL_CODE(IOCTL_DISK_BASE, 0x003c, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif
constexpr DWORDLONG kDiskAttrOffline = 0x1;
constexpr DWORDLONG kDiskAttrReadOnly = 0x2;

struct SetDiskAttributes {
    DWORD Version;
    BOOLEAN Persist;
    BYTE Reserved1[3];
    DWORDLONG Attributes;
    DWORDLONG AttributesMask;
    DWORD Reserved2[4];
};
struct GetDiskAttributes {
    DWORD Version;
    DWORD Reserved1;
    DWORDLONG Attributes;
};

// CREATE_VIRTUAL_DISK_PARAMETERS Version 2 exakt nach der Microsoft-Doku (virtdisk.h des Windows SDK).
// Eigene Definition, weil die MinGW-Header PhysicalSectorSizeInBytes nicht enthalten und damit alle
// folgenden Felder (u. a. ParentPath) falsch liegen würden. Reserve am Ende deckt Version 3/4 ab.
struct CreateParamsV2 {
    CREATE_VIRTUAL_DISK_VERSION Version;
    struct {
        GUID UniqueId;
        ULONGLONG MaximumSize;
        ULONG BlockSizeInBytes;
        ULONG SectorSizeInBytes;
        ULONG PhysicalSectorSizeInBytes;
        PCWSTR ParentPath;
        PCWSTR SourcePath;
        OPEN_VIRTUAL_DISK_FLAG OpenFlags;
        VIRTUAL_STORAGE_TYPE ParentVirtualStorageType;
        VIRTUAL_STORAGE_TYPE SourceVirtualStorageType;
        GUID ResiliencyGuid;
    } Version2;
    BYTE Reserve[128];
};
static_assert(offsetof(CreateParamsV2, Version2) == 8, "Layout");
static_assert(offsetof(CreateParamsV2, Version2.ParentPath) == 48, "Layout wie Windows SDK (x64)");
static_assert(offsetof(CreateParamsV2, Version2.ResiliencyGuid) == 108, "Layout wie Windows SDK (x64)");

DWORD CreateV2(const std::wstring& path, CreateParamsV2& p) {
    VIRTUAL_STORAGE_TYPE type{};
    type.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_VHDX;
    type.VendorId = VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT;
    p.Version = CREATE_VIRTUAL_DISK_VERSION_2;
    HANDLE h = INVALID_HANDLE_VALUE;
    DWORD r = CreateVirtualDisk(&type, path.c_str(), VIRTUAL_DISK_ACCESS_NONE, nullptr, CREATE_VIRTUAL_DISK_FLAG_NONE,
                                0, reinterpret_cast<PCREATE_VIRTUAL_DISK_PARAMETERS>(&p), nullptr, &h);
    if (r == ERROR_SUCCESS) CloseHandle(h);
    return r;
}

VIRTUAL_STORAGE_TYPE VhdxType() {
    VIRTUAL_STORAGE_TYPE t{};
    t.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_VHDX;
    t.VendorId = VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT;
    return t;
}

}  // namespace

VirtualDisk::~VirtualDisk() {
    if (attached_) {
        DetachVirtualDisk(h_.Get(), DETACH_VIRTUAL_DISK_FLAG_NONE, 0);
    }
}

void VirtualDisk::CreateDynamic(const std::wstring& path, uint64_t size, uint32_t logicalSector,
                                uint32_t physicalSector) {
    CreateParamsV2 p{};
    p.Version2.MaximumSize = size;
    p.Version2.BlockSizeInBytes = 0;                      // Standard
    p.Version2.SectorSizeInBytes = logicalSector;         // muss zur Quellplatte passen (GPT zählt in LBA)
    p.Version2.PhysicalSectorSizeInBytes = physicalSector >= 4096 ? 4096 : 512;
    DWORD r = CreateV2(path, p);
    if (r != ERROR_SUCCESS) ThrowWin32(L"CreateVirtualDisk " + path, r);
}

void VirtualDisk::CreateDifferencing(const std::wstring& path, const std::wstring& parentPath, uint32_t logicalSector) {
    CreateParamsV2 p{};
    p.Version2.ParentPath = parentPath.c_str();
    p.Version2.MaximumSize = 0;   // vom Elternteil
    DWORD r = CreateV2(path, p);  // Sektorgrößen 0 = vom Elternteil übernehmen
    if (r == ERROR_INVALID_PARAMETER) {
        DeleteFileW(path.c_str());
        CreateParamsV2 q{};
        q.Version2.ParentPath = parentPath.c_str();
        q.Version2.SectorSizeInBytes = logicalSector;
        r = CreateV2(path, q);
    }
    if (r != ERROR_SUCCESS) ThrowWin32(L"CreateVirtualDisk (Differencing) " + path, r);
}

VirtualDisk VirtualDisk::Open(const std::wstring& path, bool readOnly) {
    VIRTUAL_STORAGE_TYPE type = VhdxType();
    OPEN_VIRTUAL_DISK_PARAMETERS p{};
    p.Version = OPEN_VIRTUAL_DISK_VERSION_2;
    p.Version2.GetInfoOnly = FALSE;
    p.Version2.ReadOnly = readOnly ? TRUE : FALSE;
    HANDLE h = INVALID_HANDLE_VALUE;
    DWORD r = OpenVirtualDisk(&type, path.c_str(), VIRTUAL_DISK_ACCESS_NONE, OPEN_VIRTUAL_DISK_FLAG_NONE, &p, &h);
    if (r != ERROR_SUCCESS) ThrowWin32(L"OpenVirtualDisk " + path, r);
    VirtualDisk d;
    d.h_.Reset(h);
    return d;
}

void VirtualDisk::Attach(bool readOnly) {
    ATTACH_VIRTUAL_DISK_PARAMETERS p{};
    p.Version = ATTACH_VIRTUAL_DISK_VERSION_1;
    ATTACH_VIRTUAL_DISK_FLAG flags = ATTACH_VIRTUAL_DISK_FLAG_NO_DRIVE_LETTER;
    if (readOnly) flags = static_cast<ATTACH_VIRTUAL_DISK_FLAG>(flags | ATTACH_VIRTUAL_DISK_FLAG_READ_ONLY);
    DWORD r = AttachVirtualDisk(h_.Get(), nullptr, flags, 0, &p, nullptr);
    if (r != ERROR_SUCCESS) ThrowWin32(L"AttachVirtualDisk", r);
    attached_ = true;
}

void VirtualDisk::Detach() {
    if (!attached_) return;
    DWORD r = DetachVirtualDisk(h_.Get(), DETACH_VIRTUAL_DISK_FLAG_NONE, 0);
    attached_ = false;
    if (r != ERROR_SUCCESS) ThrowWin32(L"DetachVirtualDisk", r);
}

std::wstring VirtualDisk::PhysicalPath() const {
    std::vector<wchar_t> buf(MAX_PATH);
    for (int i = 0; i < 20; ++i) {
        ULONG size = static_cast<ULONG>(buf.size() * sizeof(wchar_t));
        DWORD r = GetVirtualDiskPhysicalPath(h_.Get(), &size, buf.data());
        if (r == ERROR_SUCCESS) return buf.data();
        if (r == ERROR_INSUFFICIENT_BUFFER) {
            buf.resize(size / sizeof(wchar_t) + 1);
            continue;
        }
        if (i == 19) ThrowWin32(L"GetVirtualDiskPhysicalPath", r);
        Sleep(500);
    }
    throw Error(L"GetVirtualDiskPhysicalPath");
}

UniqueHandle OpenPhysicalDisk(const std::wstring& physicalPath, bool write) {
    DWORD access = GENERIC_READ | (write ? GENERIC_WRITE : 0);
    for (int i = 0;; ++i) {
        UniqueHandle h(CreateFileW(physicalPath.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                   OPEN_EXISTING, FILE_FLAG_NO_BUFFERING | (write ? FILE_FLAG_WRITE_THROUGH : 0),
                                   nullptr));
        if (h.Valid()) return h;
        DWORD e = GetLastError();
        if (i >= 40 || (e != ERROR_FILE_NOT_FOUND && e != ERROR_PATH_NOT_FOUND && e != ERROR_NOT_READY)) {
            ThrowWin32(L"Virtuelle Platte öffnen " + physicalPath, e);
        }
        Sleep(250);
    }
}

void SetDiskOffline(HANDLE disk) {
    SetDiskAttributes s{};
    s.Persist = FALSE;
    s.Attributes = kDiskAttrOffline;
    s.AttributesMask = kDiskAttrOffline | kDiskAttrReadOnly;
    DWORD ret = 0;
    // Die Doku nennt sizeof(GET_DISK_ATTRIBUTES) als Version, die Header-Struktur ist SET_DISK_ATTRIBUTES.
    // Beides versuchen.
    s.Version = sizeof(SetDiskAttributes);
    BOOL ok = DeviceIoControl(disk, IOCTL_DISK_SET_DISK_ATTRIBUTES, &s, sizeof(s), nullptr, 0, &ret, nullptr);
    if (!ok && GetLastError() == ERROR_INVALID_PARAMETER) {
        s.Version = sizeof(GetDiskAttributes);
        ok = DeviceIoControl(disk, IOCTL_DISK_SET_DISK_ATTRIBUTES, &s, sizeof(s), nullptr, 0, &ret, nullptr);
    }
    if (!ok) ThrowWin32(L"Virtuelle Platte offline setzen (IOCTL_DISK_SET_DISK_ATTRIBUTES)");

    GetDiskAttributes g{};
    g.Version = sizeof(g);
    if (DeviceIoControl(disk, IOCTL_DISK_GET_DISK_ATTRIBUTES, nullptr, 0, &g, sizeof(g), &ret, nullptr) &&
        !(g.Attributes & kDiskAttrOffline)) {
        throw Error(L"Virtuelle Platte ist nach dem Offline-Setzen nicht offline. Abbruch, um Schreibkonflikte "
                    L"mit dem Dateisystem zu vermeiden.");
    }
}

}  // namespace vb
