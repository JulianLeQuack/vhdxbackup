#include "vss.h"

#include <algorithm>

namespace vb {

namespace {

std::wstring VolumeOfPath(const std::wstring& rawPath) {
    wchar_t exp[MAX_PATH * 2];
    DWORD n = ExpandEnvironmentStringsW(rawPath.c_str(), exp, MAX_PATH * 2);
    std::wstring path = (n > 0 && n <= MAX_PATH * 2) ? std::wstring(exp) : rawPath;
    if (path.empty()) return {};
    wchar_t mp[MAX_PATH];
    if (!GetVolumePathNameW(path.c_str(), mp, MAX_PATH)) return {};
    wchar_t guid[MAX_PATH];
    if (!GetVolumeNameForVolumeMountPointW(mp, guid, MAX_PATH)) return {};
    return guid;
}

bool PathIsPrefix(const std::wstring& parentFull, const std::wstring& childLogicalPath) {
    // childLogicalPath ist der logische Pfad einer Komponente; parentFull = lp\name der möglichen Eltern.
    std::wstring a = ToLower(parentFull), b = ToLower(childLogicalPath);
    if (b == a) return true;
    return b.size() > a.size() && b.compare(0, a.size(), a) == 0 && b[a.size()] == L'\\';
}

std::wstring FullPath(const std::wstring& lp, const std::wstring& name) {
    return lp.empty() ? name : lp + L"\\" + name;
}

}  // namespace

VssSession::VssSession() {
    HRESULT hr = CreateVssBackupComponents(&bc_);
    if (hr == E_ACCESSDENIED) throw Error(L"VSS: Zugriff verweigert. Das Tool muss als Administrator laufen.");
    CheckHr(hr, L"CreateVssBackupComponents");
    CheckHr(bc_->InitializeForBackup(nullptr), L"IVssBackupComponents::InitializeForBackup");
    CheckHr(bc_->SetContext(VSS_CTX_BACKUP), L"IVssBackupComponents::SetContext");
    // Komponentenmodus, kein Systemstatus-Backup, Typ FULL (unser Tool ist das einzige Backup).
    CheckHr(bc_->SetBackupState(true, false, VSS_BT_FULL, false), L"IVssBackupComponents::SetBackupState");
}

VssSession::~VssSession() {
    if (bc_) {
        if (snapshotsTaken_ && !completed_) bc_->AbortBackup();
        bc_->Release();
        bc_ = nullptr;
    }
}

void VssSession::Wait(IVssAsync* async, const wchar_t* what) {
    HRESULT hr = async->Wait(INFINITE);
    HRESULT status = S_OK;
    if (SUCCEEDED(hr)) hr = async->QueryStatus(&status, nullptr);
    async->Release();
    CheckHr(hr, what);
    if (status != VSS_S_ASYNC_FINISHED) ThrowHr(what, status);
}

void VssSession::CheckWriters(const wchar_t* phase) {
    IVssAsync* async = nullptr;
    CheckHr(bc_->GatherWriterStatus(&async), L"GatherWriterStatus");
    Wait(async, L"GatherWriterStatus");
    UINT count = 0;
    CheckHr(bc_->GetWriterStatusCount(&count), L"GetWriterStatusCount");
    for (UINT i = 0; i < count; ++i) {
        VSS_ID inst{}, wid{};
        BStr name;
        VSS_WRITER_STATE state = VSS_WS_UNKNOWN;
        HRESULT failure = S_OK;
        if (FAILED(bc_->GetWriterStatus(i, &inst, &wid, name.Put(), &state, &failure))) continue;
        bool failed = state >= VSS_WS_FAILED_AT_IDENTIFY && state <= VSS_WS_FAILED_AT_BACKUPSHUTDOWN;
        if (failed || FAILED(failure)) {
            LogWarn(L"VSS-Writer \"%s\" meldet nach %s Status %d, Fehler 0x%08lX. Seine Daten sind nur "
                    L"absturzkonsistent gesichert.",
                    name.Str().c_str(), phase, static_cast<int>(state), static_cast<unsigned long>(failure));
        }
    }
    bc_->FreeWriterStatus();
}

void VssSession::SelectComponents(const std::vector<std::wstring>& volumes) {
    IVssAsync* async = nullptr;
    CheckHr(bc_->GatherWriterMetadata(&async), L"GatherWriterMetadata");
    Wait(async, L"GatherWriterMetadata");

    std::vector<std::wstring> vols;
    for (const auto& v : volumes) vols.push_back(ToLower(v));
    auto onOurVolumes = [&](const std::wstring& path) {
        std::wstring v = ToLower(VolumeOfPath(path));
        return !v.empty() && std::find(vols.begin(), vols.end(), v) != vols.end();
    };

    UINT writers = 0;
    CheckHr(bc_->GetWriterMetadataCount(&writers), L"GetWriterMetadataCount");
    for (UINT w = 0; w < writers; ++w) {
        VSS_ID instance{};
        IVssExamineWriterMetadata* md = nullptr;
        if (FAILED(bc_->GetWriterMetadata(w, &instance, &md)) || !md) continue;
        VSS_ID inst2{}, writerId{};
        BStr writerName;
        VSS_USAGE_TYPE usage = VSS_UT_UNDEFINED;
        VSS_SOURCE_TYPE source = VSS_ST_UNDEFINED;
        UINT inc = 0, exc = 0, comps = 0;
        if (FAILED(md->GetIdentity(&inst2, &writerId, writerName.Put(), &usage, &source)) ||
            FAILED(md->GetFileCounts(&inc, &exc, &comps))) {
            md->Release();
            continue;
        }
        std::wstring wname = writerName.Str();

        struct C {
            Component c;
            bool filesInside = true;
            bool hasFiles = false;
        };
        std::vector<C> all;
        for (UINT i = 0; i < comps; ++i) {
            IVssWMComponent* comp = nullptr;
            if (FAILED(md->GetComponent(i, &comp)) || !comp) continue;
            PVSSCOMPONENTINFO info = nullptr;
            if (SUCCEEDED(comp->GetComponentInfo(&info)) && info) {
                C x;
                x.c.instance = instance;
                x.c.writer = writerId;
                x.c.writerName = wname;
                x.c.type = info->type;
                x.c.logicalPath = info->bstrLogicalPath ? info->bstrLogicalPath : L"";
                x.c.name = info->bstrComponentName ? info->bstrComponentName : L"";
                auto check = [&](IVssWMFiledesc* fd) {
                    if (!fd) return;
                    BStr p;
                    if (SUCCEEDED(fd->GetPath(p.Put()))) {
                        x.hasFiles = true;
                        if (!onOurVolumes(p.Str())) x.filesInside = false;
                    }
                    fd->Release();
                };
                for (UINT f = 0; f < info->cFileCount; ++f) {
                    IVssWMFiledesc* fd = nullptr;
                    if (SUCCEEDED(comp->GetFile(f, &fd))) check(fd);
                }
                for (UINT f = 0; f < info->cDatabases; ++f) {
                    IVssWMFiledesc* fd = nullptr;
                    if (SUCCEEDED(comp->GetDatabaseFile(f, &fd))) check(fd);
                }
                for (UINT f = 0; f < info->cLogFiles; ++f) {
                    IVssWMFiledesc* fd = nullptr;
                    if (SUCCEEDED(comp->GetDatabaseLogFile(f, &fd))) check(fd);
                }
                all.push_back(x);
                comp->FreeComponentInfo(info);
            }
            comp->Release();
        }
        md->Release();

        if (_wcsicmp(wname.c_str(), L"SqlServerWriter") == 0) {
            for (const auto& x : all) {
                std::wstring inst = x.c.logicalPath;
                if (!inst.empty() &&
                    std::find(sqlInstances_.begin(), sqlInstances_.end(), inst) == sqlInstances_.end()) {
                    sqlInstances_.push_back(inst);
                }
            }
        }

        // Writer für bootfähigen Systemstatus (z. B. ASR, System Writer) nur bei Systemstatus-Backups.
        if (usage == VSS_UT_BOOTABLESYSTEMSTATE) continue;

        // Nur Komponenten ohne Eltern-Komponente explizit hinzufügen; Unterkomponenten sind
        // damit eingeschlossen. Wie im Microsoft-Beispiel VShadow: ausschließen, wenn Dateien
        // der Komponente oder ihrer Unterkomponenten außerhalb der gesicherten Volumes liegen.
        for (const auto& x : all) {
            bool topLevel = true;
            for (const auto& y : all) {
                if (&x == &y) continue;
                if (!x.c.logicalPath.empty() && PathIsPrefix(FullPath(y.c.logicalPath, y.c.name), x.c.logicalPath)) {
                    topLevel = false;
                    break;
                }
            }
            if (!topLevel) continue;
            bool inside = x.filesInside;
            std::wstring full = FullPath(x.c.logicalPath, x.c.name);
            for (const auto& y : all) {
                if (&x != &y && PathIsPrefix(full, y.c.logicalPath) && !y.filesInside) inside = false;
            }
            if (!inside) {
                LogWarn(L"VSS-Komponente \"%s\" (%s) liegt ganz oder teilweise außerhalb der OS-Platte und wird "
                        L"nicht application-aware gesichert.",
                        full.c_str(), wname.c_str());
                continue;
            }
            HRESULT hr = bc_->AddComponent(x.c.instance, x.c.writer, x.c.type,
                                           x.c.logicalPath.empty() ? nullptr : x.c.logicalPath.c_str(),
                                           x.c.name.c_str());
            if (FAILED(hr)) {
                LogWarn(L"VSS-Komponente \"%s\" (%s) konnte nicht ausgewählt werden: 0x%08lX", full.c_str(),
                        wname.c_str(), static_cast<unsigned long>(hr));
                continue;
            }
            selected_.push_back(x.c);
        }
    }
    LogInfo(L"VSS: %u Writer, %zu Komponenten ausgewählt.", writers, selected_.size());
    for (const auto& c : selected_) {
        LogInfo(L"  Komponente %s : %s", c.writerName.c_str(), FullPath(c.logicalPath, c.name).c_str());
    }
}

bool VssSession::IsVolumeSupported(const std::wstring& volume) {
    BOOL supported = FALSE;
    std::wstring v = volume;
    HRESULT hr = bc_->IsVolumeSupported(GUID_NULL, &v[0], &supported);
    return SUCCEEDED(hr) && supported;
}

void VssSession::StartSnapshotSet() {
    VSS_ID setId{};
    CheckHr(bc_->StartSnapshotSet(&setId), L"StartSnapshotSet");
}

VSS_ID VssSession::AddVolume(const std::wstring& volume) {
    VSS_ID id{};
    std::wstring v = volume;
    CheckHr(bc_->AddToSnapshotSet(&v[0], GUID_NULL, &id), (L"AddToSnapshotSet " + volume).c_str());
    return id;
}

void VssSession::PrepareAndCreateSnapshots() {
    IVssAsync* async = nullptr;
    snapshotsTaken_ = true;  // ab PrepareForBackup bei Fehlern AbortBackup
    CheckHr(bc_->PrepareForBackup(&async), L"PrepareForBackup");
    Wait(async, L"PrepareForBackup");
    CheckWriters(L"PrepareForBackup");
    CheckHr(bc_->DoSnapshotSet(&async), L"DoSnapshotSet");
    Wait(async, L"DoSnapshotSet");
    CheckWriters(L"DoSnapshotSet");
}

std::wstring VssSession::SnapshotDevice(const VSS_ID& id) {
    VSS_SNAPSHOT_PROP prop{};
    CheckHr(bc_->GetSnapshotProperties(id, &prop), L"GetSnapshotProperties");
    std::wstring dev = prop.m_pwszSnapshotDeviceObject ? prop.m_pwszSnapshotDeviceObject : L"";
    VssFreeSnapshotProperties(&prop);
    if (dev.empty()) throw Error(L"VSS: Snapshot ohne Geräteobjekt");
    return dev;
}

void VssSession::Complete(bool success) {
    for (const auto& c : selected_) {
        bc_->SetBackupSucceeded(c.instance, c.writer, c.type, c.logicalPath.empty() ? nullptr : c.logicalPath.c_str(),
                                c.name.c_str(), success);
    }
    IVssAsync* async = nullptr;
    CheckHr(bc_->BackupComplete(&async), L"BackupComplete");
    Wait(async, L"BackupComplete");
    completed_ = true;
    CheckWriters(L"BackupComplete");
}

DWORD RunCommand(const std::wstring& command, uint32_t timeoutSec) {
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring cmdExe = JoinPath(sys, L"cmd.exe");
    std::wstring line = L"\"" + cmdExe + L"\" /d /s /c \"" + command + L"\"";
    std::vector<wchar_t> buf(line.begin(), line.end());
    buf.push_back(L'\0');
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(cmdExe.c_str(), buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                        &pi)) {
        ThrowWin32(L"Befehl starten");
    }
    UniqueHandle proc(pi.hProcess), thread(pi.hThread);
    DWORD w = WaitForSingleObject(proc.Get(), timeoutSec * 1000u);
    if (w == WAIT_TIMEOUT) {
        TerminateProcess(proc.Get(), 1);
        throw Error(Fmt(L"Zeitlimit von %u Sekunden überschritten, Befehl abgebrochen", timeoutSec));
    }
    DWORD code = 1;
    GetExitCodeProcess(proc.Get(), &code);
    return code;
}

}  // namespace vb
