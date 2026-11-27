// VSS-Requester: application-aware Snapshot-Set über alle Volumes der OS-Platte (VSS_BT_FULL).
#pragma once

#include "common.h"

#include <vss.h>
#include <vswriter.h>
#include <vsbackup.h>

namespace vb {

class VssSession {
public:
    VssSession();
    ~VssSession();
    VssSession(const VssSession&) = delete;
    VssSession& operator=(const VssSession&) = delete;

    // Writer-Metadaten sammeln und alle Komponenten auswählen, deren Dateien auf den
    // angegebenen Volumes (\\?\Volume{...}\) liegen.
    void SelectComponents(const std::vector<std::wstring>& volumes);

    bool IsVolumeSupported(const std::wstring& volume);
    void StartSnapshotSet();
    VSS_ID AddVolume(const std::wstring& volume);
    void PrepareAndCreateSnapshots();
    std::wstring SnapshotDevice(const VSS_ID& id);   // \\?\GLOBALROOT\Device\HarddiskVolumeShadowCopyN

    // Nach dem Kopieren: Erfolg je Komponente melden, BackupComplete. Danach werden die Snapshots
    // beim Freigeben automatisch gelöscht (nicht persistenter Kontext VSS_CTX_BACKUP).
    void Complete(bool success);

    // SQL-Server-Instanzen (aus den Komponenten des SqlServerWriter), für die Modellprüfung.
    const std::vector<std::wstring>& SqlInstances() const { return sqlInstances_; }

private:
    struct Component {
        VSS_ID instance{};
        VSS_ID writer{};
        std::wstring writerName;
        VSS_COMPONENT_TYPE type = VSS_CT_UNDEFINED;
        std::wstring logicalPath;
        std::wstring name;
    };
    void Wait(IVssAsync* async, const wchar_t* what);
    void CheckWriters(const wchar_t* phase);

    IVssBackupComponents* bc_ = nullptr;
    bool comInit_ = false;
    bool snapshotsTaken_ = false;
    bool completed_ = false;
    std::vector<Component> selected_;
    std::vector<std::wstring> sqlInstances_;
};

// Führt eine Kommandozeile über cmd.exe /c aus und wartet mit Timeout. Liefert den Exit-Code
// oder wirft bei Start-/Timeout-Fehler.
DWORD RunCommand(const std::wstring& command, uint32_t timeoutSec);

}  // namespace vb
