// Systemplatte finden, Partitionslayout lesen, Volumes zuordnen, Volume-Bitmap lesen.
#pragma once

#include "common.h"

namespace vb {

enum class PartKind { Efi, Msr, Basic, Recovery, Other };

struct PartitionInfo {
    uint32_t number = 0;
    uint64_t offset = 0;
    uint64_t length = 0;
    PartKind kind = PartKind::Other;
    GUID gptType{};
    uint64_t gptAttributes = 0;
    uint8_t mbrType = 0;
    std::wstring gptName;
    std::wstring volumeGuidPath;   // \\?\Volume{...}\ (mit Backslash) oder leer
    std::wstring fileSystem;       // NTFS, FAT32, ... oder leer
    std::wstring label;
    std::wstring mountPoints;      // z. B. "C:\"
    bool isWindows = false;        // enthält das laufende Windows
};

struct DiskInfo {
    uint32_t number = 0;
    std::wstring devicePath;       // \\.\PhysicalDriveN
    uint64_t size = 0;
    uint32_t logicalSector = 512;
    uint32_t physicalSector = 512;
    bool gpt = false;
    GUID gptDiskId{};
    uint32_t mbrSignature = 0;
    std::vector<PartitionInfo> parts;   // nach Offset sortiert, ohne MBR-Container
};

// Ermittelt die Platte, auf der das laufende Windows liegt, mit Partitionen und Volumes.
DiskInfo FindSystemDisk();

const wchar_t* PartKindName(PartKind k);

struct VolumeBitmap {
    uint64_t clusterSize = 0;
    uint64_t totalClusters = 0;
    std::vector<uint8_t> bits;   // Bit gesetzt = Cluster belegt
    bool Used(uint64_t lcn) const {
        if (lcn >= totalClusters) return true;
        return (bits[lcn >> 3] >> (lcn & 7)) & 1;
    }
};

// Liest Clustergröße und Belegungs-Bitmap eines NTFS-Volumes (FSCTL_GET_NTFS_VOLUME_DATA,
// FSCTL_GET_VOLUME_BITMAP).
VolumeBitmap ReadVolumeBitmap(HANDLE volume);

uint64_t GetDeviceLength(HANDLE h);  // IOCTL_DISK_GET_LENGTH_INFO

// Plattennummer des Volumes, auf dem path liegt. false, wenn nicht ermittelbar (z. B. Netzwerkpfad).
bool DiskNumberOfPath(const std::wstring& path, DWORD* disk);

}  // namespace vb
