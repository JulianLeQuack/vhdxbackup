// Dünne Hülle um die Windows-VirtDisk-API. Windows erzeugt und verwaltet das VHDX-Format selbst.
#pragma once

#include "common.h"

namespace vb {

class VirtualDisk {
public:
    VirtualDisk() = default;
    ~VirtualDisk();
    VirtualDisk(VirtualDisk&& o) noexcept : h_(std::move(o.h_)), attached_(o.attached_) { o.attached_ = false; }
    VirtualDisk(const VirtualDisk&) = delete;
    VirtualDisk& operator=(const VirtualDisk&) = delete;

    // Dynamisches VHDX in Größe der Quellplatte.
    static void CreateDynamic(const std::wstring& path, uint64_t size, uint32_t logicalSector,
                              uint32_t physicalSector);
    // Differencing-VHDX mit parentPath als Elternteil (Windows speichert relativen und absoluten Pfad).
    static void CreateDifferencing(const std::wstring& path, const std::wstring& parentPath, uint32_t logicalSector);

    static VirtualDisk Open(const std::wstring& path, bool readOnly);

    // Ohne Laufwerksbuchstaben einbinden. Die Bindung endet spätestens beim Schließen des Handles.
    void Attach(bool readOnly);
    void Detach();
    std::wstring PhysicalPath() const;   // \\.\PhysicalDriveN

private:
    UniqueHandle h_;
    bool attached_ = false;
};

// Öffnet die eingebundene virtuelle Platte für Rohzugriff (mit kurzen Wiederholungen, bis das Gerät da ist).
UniqueHandle OpenPhysicalDisk(const std::wstring& physicalPath, bool write);

// Setzt die Platte offline (nicht persistent). Dann sind ihre Volumes abgemeldet, kein Dateisystem
// greift darauf zu, und Rohschreibzugriffe sind erlaubt.
void SetDiskOffline(HANDLE disk);

}  // namespace vb
