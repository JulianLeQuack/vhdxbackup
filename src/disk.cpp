#include "disk.h"

#include <winioctl.h>

#include <algorithm>
#include <cstddef>

namespace vb {

namespace {

// GPT-Partitionstypen (UEFI-Spezifikation / Microsoft "PARTITION_INFORMATION_GPT").
const GUID kGuidEfi = {0xC12A7328, 0xF81F, 0x11D2, {0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B}};
const GUID kGuidMsr = {0xE3C9E316, 0x0B5C, 0x4DB8, {0x81, 0x7D, 0xF9, 0x2D, 0xF0, 0x02, 0x15, 0xAE}};
const GUID kGuidBasic = {0xEBD0A0A2, 0xB9E5, 0x4433, {0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7}};
const GUID kGuidRecovery = {0xDE94BBA4, 0x06D1, 0x4D40, {0xA1, 0x6A, 0xBF, 0xD5, 0x01, 0x79, 0xD6, 0xAC}};

bool SameGuid(const GUID& a, const GUID& b) { return memcmp(&a, &b, sizeof(GUID)) == 0; }

std::vector<uint8_t> IoctlGrow(HANDLE h, DWORD code, const wchar_t* what, size_t initial) {
    std::vector<uint8_t> buf(initial);
    for (;;) {
        DWORD ret = 0;
        if (DeviceIoControl(h, code, nullptr, 0, buf.data(), static_cast<DWORD>(buf.size()), &ret, nullptr)) {
            buf.resize(ret);
            return buf;
        }
        DWORD e = GetLastError();
        if ((e == ERROR_INSUFFICIENT_BUFFER || e == ERROR_MORE_DATA) && buf.size() < (16u << 20)) {
            buf.resize(buf.size() * 2);
            continue;
        }
        ThrowWin32(what, e);
    }
}

// Liefert Plattennummer und Startoffset eines Volumes, falls es genau einen Extent hat.
bool VolumeExtent(const std::wstring& volumeNoSlash, DWORD* disk, uint64_t* offset, uint64_t* length) {
    UniqueHandle h(CreateFileW(volumeNoSlash.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                               0, nullptr));
    if (!h.Valid()) return false;
    std::vector<uint8_t> buf(sizeof(VOLUME_DISK_EXTENTS) + 8 * sizeof(DISK_EXTENT));
    DWORD ret = 0;
    if (!DeviceIoControl(h.Get(), IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, buf.data(),
                         static_cast<DWORD>(buf.size()), &ret, nullptr)) {
        return false;
    }
    auto* ext = reinterpret_cast<VOLUME_DISK_EXTENTS*>(buf.data());
    if (ext->NumberOfDiskExtents != 1) return false;
    *disk = ext->Extents[0].DiskNumber;
    *offset = static_cast<uint64_t>(ext->Extents[0].StartingOffset.QuadPart);
    *length = static_cast<uint64_t>(ext->Extents[0].ExtentLength.QuadPart);
    return true;
}

std::wstring StripSlash(std::wstring s) {
    if (!s.empty() && s.back() == L'\\') s.pop_back();
    return s;
}

}  // namespace

const wchar_t* PartKindName(PartKind k) {
    switch (k) {
        case PartKind::Efi: return L"EFI";
        case PartKind::Msr: return L"MSR";
        case PartKind::Basic: return L"Daten";
        case PartKind::Recovery: return L"Recovery";
        default: return L"Andere";
    }
}

uint64_t GetDeviceLength(HANDLE h) {
    GET_LENGTH_INFORMATION li{};
    DWORD ret = 0;
    if (!DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0, &li, sizeof(li), &ret, nullptr)) {
        ThrowWin32(L"IOCTL_DISK_GET_LENGTH_INFO");
    }
    return static_cast<uint64_t>(li.Length.QuadPart);
}

DiskInfo FindSystemDisk() {
    wchar_t winDir[MAX_PATH];
    if (!GetWindowsDirectoryW(winDir, MAX_PATH)) ThrowWin32(L"GetWindowsDirectory");
    wchar_t volPath[MAX_PATH];
    if (!GetVolumePathNameW(winDir, volPath, MAX_PATH)) ThrowWin32(L"GetVolumePathName");
    wchar_t winVolGuid[MAX_PATH];
    if (!GetVolumeNameForVolumeMountPointW(volPath, winVolGuid, MAX_PATH)) {
        ThrowWin32(L"GetVolumeNameForVolumeMountPoint");
    }

    DWORD diskNo = 0;
    uint64_t winOff = 0, winLen = 0;
    if (!VolumeExtent(StripSlash(winVolGuid), &diskNo, &winOff, &winLen)) {
        throw Error(L"Das Windows-Volume liegt nicht auf genau einer Platte (übergreifende Volumes werden nicht unterstützt).");
    }

    DiskInfo d;
    d.number = diskNo;
    d.devicePath = Fmt(L"\\\\.\\PhysicalDrive%lu", static_cast<unsigned long>(diskNo));
    UniqueHandle h(CreateFileW(d.devicePath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, 0, nullptr));
    if (!h.Valid()) ThrowWin32(L"Systemplatte öffnen " + d.devicePath);

    d.size = GetDeviceLength(h.Get());

    DISK_GEOMETRY_EX geo{};
    DWORD ret = 0;
    if (!DeviceIoControl(h.Get(), IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0, &geo, sizeof(geo), &ret, nullptr)) {
        ThrowWin32(L"IOCTL_DISK_GET_DRIVE_GEOMETRY_EX");
    }
    d.logicalSector = geo.Geometry.BytesPerSector;
    d.physicalSector = d.logicalSector;
    {
        STORAGE_PROPERTY_QUERY q{};
        q.PropertyId = StorageAccessAlignmentProperty;
        q.QueryType = PropertyStandardQuery;
        STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR a{};
        if (DeviceIoControl(h.Get(), IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), &a, sizeof(a), &ret, nullptr) &&
            ret >= offsetof(STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR, BytesPerPhysicalSector) + sizeof(DWORD) &&
            a.BytesPerPhysicalSector >= d.logicalSector) {
            d.physicalSector = a.BytesPerPhysicalSector;
        }
    }

    auto layoutBuf = IoctlGrow(h.Get(), IOCTL_DISK_GET_DRIVE_LAYOUT_EX,
                                                            L"IOCTL_DISK_GET_DRIVE_LAYOUT_EX", 64 * 1024);
    auto* layout = reinterpret_cast<DRIVE_LAYOUT_INFORMATION_EX*>(layoutBuf.data());
    if (layout->PartitionStyle == PARTITION_STYLE_GPT) {
        d.gpt = true;
        d.gptDiskId = layout->Gpt.DiskId;
    } else if (layout->PartitionStyle == PARTITION_STYLE_MBR) {
        d.mbrSignature = layout->Mbr.Signature;
    } else {
        throw Error(L"Die Systemplatte hat keine Partitionstabelle (RAW).");
    }

    for (DWORD i = 0; i < layout->PartitionCount; ++i) {
        const PARTITION_INFORMATION_EX& pe = layout->PartitionEntry[i];
        if (pe.PartitionLength.QuadPart <= 0) continue;
        PartitionInfo p;
        p.number = pe.PartitionNumber;
        p.offset = static_cast<uint64_t>(pe.StartingOffset.QuadPart);
        p.length = static_cast<uint64_t>(pe.PartitionLength.QuadPart);
        if (pe.PartitionStyle == PARTITION_STYLE_GPT) {
            p.gptType = pe.Gpt.PartitionType;
            p.gptAttributes = pe.Gpt.Attributes;
            p.gptName = std::wstring(pe.Gpt.Name, wcsnlen(pe.Gpt.Name, 36));
            if (SameGuid(p.gptType, kGuidEfi)) p.kind = PartKind::Efi;
            else if (SameGuid(p.gptType, kGuidMsr)) p.kind = PartKind::Msr;
            else if (SameGuid(p.gptType, kGuidBasic)) p.kind = PartKind::Basic;
            else if (SameGuid(p.gptType, kGuidRecovery)) p.kind = PartKind::Recovery;
        } else {
            p.mbrType = pe.Mbr.PartitionType;
            // Erweiterte Container überlappen die logischen Laufwerke: nicht als eigene Partition führen,
            // die EBR-Sektoren werden über die Lücken zwischen den Partitionen mitkopiert.
            if (p.mbrType == PARTITION_EXTENDED || p.mbrType == PARTITION_XINT13_EXTENDED) continue;
            if (p.mbrType == 0xEF) p.kind = PartKind::Efi;
            else if (p.mbrType == 0x27) p.kind = PartKind::Recovery;
            else p.kind = PartKind::Basic;
        }
        d.parts.push_back(p);
    }
    std::sort(d.parts.begin(), d.parts.end(),
              [](const PartitionInfo& a, const PartitionInfo& b) { return a.offset < b.offset; });

    // Volumes den Partitionen zuordnen.
    wchar_t vol[MAX_PATH];
    HANDLE f = FindFirstVolumeW(vol, MAX_PATH);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            std::wstring guidPath = vol;
            DWORD vd = 0;
            uint64_t vo = 0, vl = 0;
            if (!VolumeExtent(StripSlash(guidPath), &vd, &vo, &vl) || vd != diskNo) continue;
            for (auto& p : d.parts) {
                if (p.offset != vo) continue;
                p.volumeGuidPath = guidPath;
                wchar_t fs[MAX_PATH] = {}, label[MAX_PATH] = {};
                if (GetVolumeInformationW(guidPath.c_str(), label, MAX_PATH, nullptr, nullptr, nullptr, fs, MAX_PATH)) {
                    p.fileSystem = fs;
                    p.label = label;
                }
                std::vector<wchar_t> names(1024);
                DWORD len = 0;
                if (GetVolumePathNamesForVolumeNameW(guidPath.c_str(), names.data(),
                                                     static_cast<DWORD>(names.size()), &len)) {
                    std::wstring all;
                    for (const wchar_t* n = names.data(); *n; n += wcslen(n) + 1) {
                        if (!all.empty()) all += L" ";
                        all += n;
                    }
                    p.mountPoints = all;
                }
                if (_wcsicmp(guidPath.c_str(), winVolGuid) == 0) p.isWindows = true;
            }
        } while (FindNextVolumeW(f, vol, MAX_PATH));
        FindVolumeClose(f);
    }

    bool found = false;
    for (const auto& p : d.parts) found = found || p.isWindows;
    if (!found) throw Error(L"Windows-Partition nicht im Layout der Systemplatte gefunden.");
    return d;
}

bool DiskNumberOfPath(const std::wstring& path, DWORD* disk) {
    wchar_t mp[MAX_PATH];
    if (!GetVolumePathNameW(path.c_str(), mp, MAX_PATH)) return false;
    wchar_t guid[MAX_PATH];
    if (!GetVolumeNameForVolumeMountPointW(mp, guid, MAX_PATH)) return false;
    uint64_t o = 0, l = 0;
    return VolumeExtent(StripSlash(guid), disk, &o, &l);
}

VolumeBitmap ReadVolumeBitmap(HANDLE volume) {
    NTFS_VOLUME_DATA_BUFFER nv{};
    DWORD ret = 0;
    if (!DeviceIoControl(volume, FSCTL_GET_NTFS_VOLUME_DATA, nullptr, 0, &nv, sizeof(nv), &ret, nullptr)) {
        ThrowWin32(L"FSCTL_GET_NTFS_VOLUME_DATA");
    }
    VolumeBitmap bm;
    bm.clusterSize = nv.BytesPerCluster;
    bm.totalClusters = static_cast<uint64_t>(nv.TotalClusters.QuadPart);
    bm.bits.assign(static_cast<size_t>((bm.totalClusters + 7) / 8), 0);

    // In Stücken lesen; jede Antwort beginnt an einem Vielfachen von 8 Clustern.
    const size_t chunk = 4u << 20;
    std::vector<uint8_t> out(sizeof(VOLUME_BITMAP_BUFFER) + chunk);
    uint64_t next = 0;
    while (next < bm.totalClusters) {
        STARTING_LCN_INPUT_BUFFER in{};
        in.StartingLcn.QuadPart = static_cast<LONGLONG>(next);
        BOOL ok = DeviceIoControl(volume, FSCTL_GET_VOLUME_BITMAP, &in, sizeof(in), out.data(),
                                  static_cast<DWORD>(out.size()), &ret, nullptr);
        DWORD e = ok ? ERROR_SUCCESS : GetLastError();
        if (!ok && e != ERROR_MORE_DATA) ThrowWin32(L"FSCTL_GET_VOLUME_BITMAP", e);
        auto* vb = reinterpret_cast<VOLUME_BITMAP_BUFFER*>(out.data());
        uint64_t start = static_cast<uint64_t>(vb->StartingLcn.QuadPart);
        uint64_t count = static_cast<uint64_t>(vb->BitmapSize.QuadPart);  // Cluster ab start
        size_t avail = ret > offsetof(VOLUME_BITMAP_BUFFER, Buffer) ? ret - offsetof(VOLUME_BITMAP_BUFFER, Buffer) : 0;
        uint64_t got = std::min<uint64_t>(count, static_cast<uint64_t>(avail) * 8);
        if (start % 8 != 0 || got == 0) throw Error(L"FSCTL_GET_VOLUME_BITMAP: unerwartete Antwort");
        size_t bytes = static_cast<size_t>((got + 7) / 8);
        size_t dst = static_cast<size_t>(start / 8);
        if (dst + bytes > bm.bits.size()) bytes = bm.bits.size() - dst;
        memcpy(bm.bits.data() + dst, vb->Buffer, bytes);
        next = start + got;
        if (ok) break;
    }
    return bm;
}

}  // namespace vb
