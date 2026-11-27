#include "backup.h"

#include "catalog.h"
#include "checks.h"
#include "disk.h"
#include "hash.h"
#include "restoredoc.h"
#include "vhdx.h"
#include "vss.h"

#include <winioctl.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>

namespace vb {

namespace {

std::atomic<bool> g_abort{false};

void CheckAbort() {
    if (g_abort.load()) throw Error(L"Abgebrochen (Strg+C).");
}

constexpr uint32_t kBlock = HashStore::kBlockSize;
constexpr uint64_t kGapFullCopyLimit = 64ull << 20;   // Lücken bis 64 MiB ganz kopieren
constexpr uint64_t kGapEdge = 1ull << 20;             // sonst Anfang und Ende je 1 MiB (GPT, EBR)
constexpr uint64_t kMinVssVolume = 1ull << 30;        // kleinere Volumes (Recovery) roh lesen

// Ein zusammenhängender Bereich der Quellplatte und woher seine Daten kommen.
struct Extent {
    uint64_t start = 0;
    uint64_t length = 0;
    HANDLE source = INVALID_HANDLE_VALUE;
    uint64_t sourceOffset = 0;
    const VolumeBitmap* bitmap = nullptr;   // nullptr = alles belegt
    bool meta = false;                      // Partitionstabelle/Lücke: beim Vollbackup zuletzt schreiben
    std::wstring desc;
};

class DiskReader {
public:
    explicit DiskReader(std::vector<Extent> ex) : ex_(std::move(ex)), scratch_(kBlock) {
        std::sort(ex_.begin(), ex_.end(), [](const Extent& a, const Extent& b) { return a.start < b.start; });
    }

    bool TouchesMeta(uint64_t b0, uint64_t b1) const {
        for (const auto& e : ex_) {
            if (e.meta && e.start < b1 && e.start + e.length > b0) return true;
        }
        return false;
    }

    // Füllt buf mit dem Inhalt der Quellplatte im Bereich des Blocks. Freie Cluster werden genullt.
    // Rückgabe false = im Block ist nichts belegt.
    bool Read(uint64_t b0, uint32_t len, uint8_t* buf) {
        memset(buf, 0, len);
        uint64_t b1 = b0 + len;
        bool used = false;
        for (const auto& e : ex_) {
            uint64_t s = std::max(b0, e.start);
            uint64_t t = std::min(b1, e.start + e.length);
            if (s >= t) continue;
            uint64_t rel = s - e.start;
            uint32_t n = static_cast<uint32_t>(t - s);
            uint8_t* dst = buf + (s - b0);
            if (e.bitmap) {
                const uint64_t c = e.bitmap->clusterSize;
                uint64_t first = rel / c, last = (rel + n - 1) / c;
                bool any = false;
                for (uint64_t l = first; l <= last && !any; ++l) any = e.bitmap->Used(l);
                if (!any) continue;
                ReadAt(e.source, e.sourceOffset + rel, scratch_.Data(), n, e.desc.c_str());
                memcpy(dst, scratch_.Data(), n);
                for (uint64_t l = first; l <= last; ++l) {
                    if (e.bitmap->Used(l)) continue;
                    uint64_t zs = std::max(l * c, rel), ze = std::min((l + 1) * c, rel + n);
                    memset(dst + (zs - rel), 0, static_cast<size_t>(ze - zs));
                }
                used = true;
            } else {
                ReadAt(e.source, e.sourceOffset + rel, scratch_.Data(), n, e.desc.c_str());
                memcpy(dst, scratch_.Data(), n);
                used = true;
            }
        }
        return used;
    }

private:
    std::vector<Extent> ex_;
    AlignedBuffer scratch_;
};

bool AllZero(const uint8_t* p, size_t n) {
    const uint64_t* q = reinterpret_cast<const uint64_t*>(p);
    for (size_t i = 0; i < n / 8; ++i) if (q[i]) return false;
    for (size_t i = n / 8 * 8; i < n; ++i) if (p[i]) return false;
    return true;
}

class Progress {
public:
    Progress(const wchar_t* phase, uint64_t total) : phase_(phase), total_(total), start_(GetTickCount64()) {}
    void Update(uint64_t done, uint64_t readBytes, uint64_t writtenBytes, bool force = false) {
        ULONGLONG now = GetTickCount64();
        if (!force && now - last_ < 2000) return;
        last_ = now;
        double sec = (now - start_) / 1000.0;
        double mbs = sec > 0 ? (readBytes / 1048576.0) / sec : 0;
        ConsoleProgress(Fmt(L"%s %5.1f%% | gelesen %s | geschrieben %s | %.0f MiB/s", phase_,
                            total_ ? 100.0 * done / total_ : 100.0, FormatBytes(readBytes).c_str(),
                            FormatBytes(writtenBytes).c_str(), mbs));
    }
private:
    const wchar_t* phase_;
    uint64_t total_;
    ULONGLONG start_;
    ULONGLONG last_ = 0;
};

void LogLayout(const DiskInfo& d) {
    LogInfo(L"Systemplatte: %s, %s, Sektor %u/%u Byte, %s, %zu Partitionen", d.devicePath.c_str(),
            FormatBytes(d.size).c_str(), d.logicalSector, d.physicalSector, d.gpt ? L"GPT" : L"MBR", d.parts.size());
    for (const auto& p : d.parts) {
        LogInfo(L"  Partition %u: %-8s Offset %llu, %s, %s %s%s", p.number, PartKindName(p.kind),
                static_cast<unsigned long long>(p.offset), FormatBytes(p.length).c_str(),
                p.fileSystem.empty() ? L"-" : p.fileSystem.c_str(), p.mountPoints.c_str(),
                p.isWindows ? L" (Windows)" : L"");
    }
}

std::wstring DiskIdString(const DiskInfo& d) {
    return d.gpt ? GuidToString(d.gptDiskId) : Fmt(L"mbr-%08lx", static_cast<unsigned long>(d.mbrSignature));
}

void EnsureTargetNotOnSystemDisk(const std::wstring& target, const DiskInfo& d) {
    DWORD n = 0;
    if (DiskNumberOfPath(target, &n) && n == d.number) {
        throw Error(L"Das Ziel " + target + L" liegt auf der OS-Platte selbst. Bitte eine USB-Platte verwenden.");
    }
}

void RunPreCommands(const Config& cfg) {
    for (const auto& cmd : cfg.preCommands) {
        LogInfo(L"Pre-Befehl: %s", cmd.c_str());
        try {
            DWORD code = RunCommand(cmd, cfg.preTimeoutSec);
            if (code != 0) LogWarn(L"Pre-Befehl endete mit Code %lu: %s", static_cast<unsigned long>(code), cmd.c_str());
            else LogInfo(L"Pre-Befehl erfolgreich.");
        } catch (const Error& e) {
            LogWarn(L"Pre-Befehl fehlgeschlagen (%s): %s", e.Message().c_str(), cmd.c_str());
        }
    }
}

// Liest alle bekannten Blöcke aus der eingebundenen Kette und vergleicht sie mit den Hashes.
// Mit reader != nullptr werden Abweichungen aus dem Snapshot neu geschrieben (Reparatur).
uint64_t VerifyChain(HANDLE vdisk, HashStore& hs, DiskReader* reader, uint64_t* repaired) {
    Sha256 sha;
    AlignedBuffer buf(kBlock);
    uint64_t known = 0;
    for (const auto& h : hs.hashes) if (HashStore::Known(h)) ++known;
    Progress prog(L"Prüfen ", known);
    uint64_t done = 0, readBytes = 0, mismatches = 0;
    for (uint64_t i = 0; i < hs.BlockCount(); ++i) {
        if (!HashStore::Known(hs.hashes[i])) continue;
        CheckAbort();
        uint64_t off = i * kBlock;
        uint32_t len = static_cast<uint32_t>(std::min<uint64_t>(kBlock, hs.diskSize - off));
        ReadAt(vdisk, off, buf.Data(), len, L"VHDX-Kette");
        readBytes += len;
        if (sha.Compute(buf.Data(), len) != hs.hashes[i]) {
            ++mismatches;
            if (!reader) {
                LogError(L"Block %llu (Offset %llu) weicht vom Katalog ab.", static_cast<unsigned long long>(i),
                         static_cast<unsigned long long>(off));
            } else {
                reader->Read(off, len, buf.Data());
                Hash32 nh = sha.Compute(buf.Data(), len);
                WriteAt(vdisk, off, buf.Data(), len, L"VHDX (Reparatur)");
                ReadAt(vdisk, off, buf.Data(), len, L"VHDX (Reparatur prüfen)");
                if (sha.Compute(buf.Data(), len) != nh) {
                    throw Error(Fmt(L"Block %llu lässt sich nicht korrekt in das VHDX schreiben.",
                                    static_cast<unsigned long long>(i)));
                }
                hs.hashes[i] = nh;
                ++*repaired;
            }
        }
        ++done;
        prog.Update(done, readBytes, 0);
    }
    prog.Update(done, readBytes, 0, true);
    return mismatches;
}

}  // namespace

void RequestAbort() { g_abort = true; }

int RunBackup(const Config& cfg, bool forceFull) {
    const std::wstring machineDir = MachineDir(cfg.targetPath);
    CreateDirs(JoinPath(machineDir, L"logs"));
    LogOpenFile(JoinPath(JoinPath(machineDir, L"logs"), NowStamp() + L".log"));
    ClearCollectedWarnings();
    LogInfo(L"%s %s: Backup nach %s", kToolName, kToolVersion, machineDir.c_str());

    CheckTargetVolume(cfg.targetPath, cfg.minFreeGb);
    {
        std::wstring info;
        EncryptionState es = QueryTargetEncryption(cfg.targetPath, &info);
        if (es == EncryptionState::NotEncrypted) {
            LogWarn(L"Das Ziel ist nicht verschlüsselt (%s). Das Image enthält alle Daten im Klartext. "
                    L"Empfohlen: BitLocker To Go auf der USB-Platte.", info.c_str());
        } else {
            LogInfo(L"Verschlüsselung des Ziels: %s", info.c_str());
        }
    }

    DiskInfo disk = FindSystemDisk();
    LogLayout(disk);
    EnsureTargetNotOnSystemDisk(cfg.targetPath, disk);
    if (disk.size % disk.logicalSector != 0) throw Error(L"Plattengröße ist kein Vielfaches der Sektorgröße.");

    CleanupIncomplete(machineDir);
    auto gens = ListGenerations(machineDir);

    // ---- Voll oder inkrementell?
    bool full = forceFull;
    std::wstring reason = forceFull ? L"--full angegeben" : L"";
    HashStore oldHashes;
    const Generation* gen = gens.empty() ? nullptr : &gens.back();
    if (!full && !gen) { full = true; reason = L"noch keine Generation vorhanden"; }
    if (!full && gen->links.size() - 1 >= cfg.chainMaxLength) {
        full = true;
        reason = Fmt(L"Generation hat %zu Inkremente (chain.max_length = %u)", gen->links.size() - 1, cfg.chainMaxLength);
    }
    if (!full && (gen->diskSize != disk.size || gen->logicalSector != disk.logicalSector)) {
        full = true;
        reason = L"Größe oder Sektorgröße der Systemplatte hat sich geändert";
    }
    if (!full && gen->sourceDiskId != DiskIdString(disk)) {
        full = true;
        reason = L"andere Systemplatte als in der letzten Generation";
    }
    if (!full) {
        if (!oldHashes.Load(JoinPath(gen->dir, L"blockhashes.bin")) || oldHashes.diskSize != disk.size ||
            oldHashes.blockSize != kBlock || _wcsicmp(oldHashes.link.c_str(), gen->links.back().file.c_str()) != 0) {
            full = true;
            reason = L"Block-Hashes der letzten Generation fehlen oder passen nicht zum letzten Glied";
        }
    }
    LogInfo(full ? L"Typ: Vollbackup (%s)" : L"Typ: Inkrement%s", reason.c_str());

    // ---- Pre-Befehle, dann VSS
    RunPreCommands(cfg);

    VssSession vss;
    struct Snap { const PartitionInfo* part; VSS_ID id; };
    std::vector<Snap> snaps;
    std::vector<std::wstring> vssVolumes;
    for (const auto& p : disk.parts) {
        bool candidate = !p.volumeGuidPath.empty() && (p.fileSystem == L"NTFS" || p.fileSystem == L"ReFS") &&
                         (p.length >= kMinVssVolume || p.isWindows);
        if (candidate && vss.IsVolumeSupported(p.volumeGuidPath)) {
            vssVolumes.push_back(p.volumeGuidPath);
            snaps.push_back({&p, GUID_NULL});
        } else if (p.isWindows) {
            throw Error(L"VSS unterstützt das Windows-Volume nicht.");
        }
    }
    vss.SelectComponents(vssVolumes);
    if (cfg.sqlCheckRecoveryModel) CheckSqlRecoveryModels(vss.SqlInstances());
    vss.StartSnapshotSet();
    for (auto& s : snaps) s.id = vss.AddVolume(s.part->volumeGuidPath);
    LogInfo(L"Erstelle VSS-Snapshot-Set über %zu Volumes ...", snaps.size());
    vss.PrepareAndCreateSnapshots();
    LogInfo(L"Snapshot-Set erstellt.");

    // ---- Quellen öffnen und Extents bilden
    UniqueHandle physical(CreateFileW(disk.devicePath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                      OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, nullptr));
    if (!physical.Valid()) ThrowWin32(L"Systemplatte öffnen");
    std::vector<UniqueHandle> snapHandles;
    std::vector<std::unique_ptr<VolumeBitmap>> bitmaps;
    std::vector<Extent> extents;

    auto addRaw = [&](uint64_t start, uint64_t len, bool meta, const std::wstring& desc) {
        if (len == 0) return;
        Extent e;
        e.start = start;
        e.length = len;
        e.source = physical.Get();
        e.sourceOffset = start;
        e.meta = meta;
        e.desc = desc;
        extents.push_back(e);
    };
    auto addGap = [&](uint64_t a, uint64_t b) {
        if (b <= a) return;
        if (b - a <= kGapFullCopyLimit) {
            addRaw(a, b - a, true, L"Partitionstabelle/Lücke");
        } else {
            addRaw(a, kGapEdge, true, L"Partitionstabelle/Lücke (Anfang)");
            addRaw(b - kGapEdge, kGapEdge, true, L"Partitionstabelle/Lücke (Ende)");
        }
    };

    uint64_t cursor = 0;
    for (const auto& p : disk.parts) {
        addGap(cursor, p.offset);
        auto it = std::find_if(snaps.begin(), snaps.end(), [&](const Snap& s) { return s.part == &p; });
        if (it != snaps.end()) {
            std::wstring dev = vss.SnapshotDevice(it->id);
            UniqueHandle h(CreateFileW(dev.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                       OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, nullptr));
            if (!h.Valid()) ThrowWin32(L"Snapshot öffnen " + dev);
            DWORD ret = 0;
            DeviceIoControl(h.Get(), FSCTL_ALLOW_EXTENDED_DASD_IO, nullptr, 0, nullptr, 0, &ret, nullptr);
            uint64_t volLen = std::min(GetDeviceLength(h.Get()), p.length);
            volLen -= volLen % disk.logicalSector;
            Extent e;
            e.start = p.offset;
            e.length = volLen;
            e.source = h.Get();
            e.desc = Fmt(L"Snapshot Partition %u", p.number);
            if (p.fileSystem == L"NTFS") {
                bitmaps.push_back(std::make_unique<VolumeBitmap>(ReadVolumeBitmap(h.Get())));
                e.bitmap = bitmaps.back().get();
                uint64_t usedClusters = 0;
                for (uint64_t l = 0; l < e.bitmap->totalClusters; ++l) usedClusters += e.bitmap->Used(l);
                LogInfo(L"Partition %u aus Snapshot: %s belegt von %s (Cluster %llu Byte)", p.number,
                        FormatBytes(usedClusters * e.bitmap->clusterSize).c_str(), FormatBytes(volLen).c_str(),
                        static_cast<unsigned long long>(e.bitmap->clusterSize));
            } else {
                LogInfo(L"Partition %u aus Snapshot, vollständig (%s)", p.number, FormatBytes(volLen).c_str());
            }
            extents.push_back(e);
            snapHandles.push_back(std::move(h));
            if (volLen < p.length) addRaw(p.offset + volLen, p.length - volLen, false, L"Partitionsende");
        } else {
            LogInfo(L"Partition %u (%s) roh von der Platte, %s", p.number, PartKindName(p.kind),
                    FormatBytes(p.length).c_str());
            addRaw(p.offset, p.length, false, Fmt(L"Partition %u roh", p.number));
        }
        cursor = std::max(cursor, p.offset + p.length);
    }
    addGap(cursor, disk.size);
    DiskReader reader(extents);

    // ---- Kettenglied anlegen
    std::wstring genDir, linkFile, linkPath, parentPath;
    if (full) {
        genDir = JoinPath(machineDir, L"gen-" + NowStamp());
        CreateDirs(genDir);
        std::wstring gi = JoinPath(genDir, L"generation.ini");
        IniWrite(gi, L"generation", L"created", NowIso());
        IniWrite(gi, L"generation", L"disk_size", Fmt(L"%llu", static_cast<unsigned long long>(disk.size)));
        IniWrite(gi, L"generation", L"logical_sector", Fmt(L"%u", disk.logicalSector));
        IniWrite(gi, L"generation", L"physical_sector", Fmt(L"%u", disk.physicalSector));
        IniWrite(gi, L"generation", L"block_size", Fmt(L"%u", kBlock));
        IniWrite(gi, L"generation", L"source_disk_id", DiskIdString(disk));
        linkFile = LinkFileName(0);
        linkPath = JoinPath(genDir, linkFile);
    } else {
        genDir = gen->dir;
        linkFile = LinkFileName(gen->links.size());
        linkPath = JoinPath(genDir, linkFile);
        parentPath = gen->links.back().path;
    }

    LinkInfo info;
    info.file = linkFile;
    info.path = linkPath;
    info.iniPath = IniForLink(linkPath);
    info.type = full ? L"full" : L"incremental";
    info.status = L"running";
    info.started = NowIso();
    info.parent = full ? L"" : gen->links.back().file;
    WriteLinkInfo(info);

    HashStore hashes;
    if (full) hashes.Init(disk.size);
    else hashes = oldHashes;
    hashes.link = linkFile;

    try {
        if (full) VirtualDisk::CreateDynamic(linkPath, disk.size, disk.logicalSector, disk.physicalSector);
        else VirtualDisk::CreateDifferencing(linkPath, parentPath, disk.logicalSector);
        LogInfo(L"Angelegt: %s%s%s", linkPath.c_str(), full ? L"" : L"  (Parent ", full ? L"" : (info.parent + L")").c_str());

        VirtualDisk vd = VirtualDisk::Open(linkPath, false);
        vd.Attach(false);
        std::wstring phys = vd.PhysicalPath();
        UniqueHandle vdisk = OpenPhysicalDisk(phys, true);
        SetDiskOffline(vdisk.Get());
        LogInfo(L"VHDX eingebunden als %s (offline, ohne Laufwerksbuchstaben).", phys.c_str());
        if (GetDeviceLength(vdisk.Get()) != disk.size) throw Error(L"Größe der virtuellen Platte passt nicht zur Quelle.");

        // ---- Kopieren: zuerst Partitionsdaten, Partitionstabelle zuletzt
        Sha256 sha;
        AlignedBuffer buf(kBlock);
        uint64_t readBytes = 0, writtenBytes = 0;
        std::vector<uint64_t> deferred;
        Progress prog(L"Kopieren", hashes.BlockCount());
        uint64_t done = 0;
        auto process = [&](uint64_t i) {
            CheckAbort();
            uint64_t off = i * kBlock;
            uint32_t len = static_cast<uint32_t>(std::min<uint64_t>(kBlock, disk.size - off));
            if (!reader.Read(off, len, buf.Data())) return;
            readBytes += len;
            Hash32 h = sha.Compute(buf.Data(), len);
            const bool known = !full && HashStore::Known(oldHashes.hashes[i]);
            if (known && oldHashes.hashes[i] == h) return;   // unverändert
            if (!known && AllZero(buf.Data(), len)) {
                hashes.hashes[i] = h;   // nie geschrieben: das Image liest hier ohnehin Nullen
                return;
            }
            WriteAt(vdisk.Get(), off, buf.Data(), len, L"VHDX");
            writtenBytes += len;
            ++info.blocksWritten;
            hashes.hashes[i] = h;
        };
        for (uint64_t i = 0; i < hashes.BlockCount(); ++i) {
            uint64_t off = i * kBlock;
            if (reader.TouchesMeta(off, off + kBlock)) deferred.push_back(i);
            else process(i);
            prog.Update(++done, readBytes, writtenBytes);
        }
        for (uint64_t i : deferred) process(i);
        prog.Update(done, readBytes, writtenBytes, true);
        if (!FlushFileBuffers(vdisk.Get())) ThrowWin32(L"VHDX-Daten sichern");
        info.bytesWritten = writtenBytes;
        LogInfo(L"Kopiert: %s gelesen, %llu Blöcke / %s geschrieben.", FormatBytes(readBytes).c_str(),
                static_cast<unsigned long long>(info.blocksWritten), FormatBytes(writtenBytes).c_str());

        // ---- Verify mit Reparatur (Snapshot ist noch da)
        if (cfg.verifyFull) {
            LogInfo(L"Prüfe die ganze Kette gegen die Block-Hashes ...");
            uint64_t mism = VerifyChain(vdisk.Get(), hashes, &reader, &info.blocksRepaired);
            if (mism) {
                LogWarn(L"Verify: %llu Blöcke wichen ab und wurden aus dem Snapshot neu geschrieben.",
                        static_cast<unsigned long long>(mism));
                FlushFileBuffers(vdisk.Get());
            } else {
                LogInfo(L"Verify: alle Blöcke stimmen.");
            }
        }

        vdisk.Reset();
        vd.Detach();

        snapHandles.clear();
        vss.Complete(true);

        hashes.Save(JoinPath(genDir, L"blockhashes.bin"));
        info.status = L"ok";
        info.finished = NowIso();
        info.warnings = CollectedWarnings();
        WriteLinkInfo(info);
        MakeReadOnly(linkPath);
    } catch (...) {
        info.status = L"failed";
        info.finished = NowIso();
        try {
            WriteLinkInfo(info);
            // Erst nach dem Trennen (Destruktor von VirtualDisk) löschen.
            SetFileAttributesW(linkPath.c_str(), FILE_ATTRIBUTE_NORMAL);
            DeleteFileW(linkPath.c_str());
            DeleteFileW(info.iniPath.c_str());
            if (full) DeleteTree(genDir);
            WriteCatalogJson(machineDir);
        } catch (const Error& e) {
            LogError(L"Aufräumen nach Fehler: %s", e.Message().c_str());
        }
        throw;
    }

    ApplyRetention(machineDir, cfg.keepGenerations);
    WriteCatalogJson(machineDir);
    WriteRestoreDocs(machineDir, disk, linkPath);
    CheckTargetVolume(cfg.targetPath, cfg.minFreeGb);
    LogInfo(L"Backup erfolgreich: %s (%zu Warnungen).", linkPath.c_str(), CollectedWarnings().size());
    LogCloseFile();
    return CollectedWarnings().empty() ? 0 : 2;
}

int RunVerify(const Config& cfg) {
    const std::wstring machineDir = MachineDir(cfg.targetPath);
    auto gens = ListGenerations(machineDir);
    if (gens.empty() || !gens.back().LastOk()) throw Error(L"Keine vollständige Generation gefunden in " + machineDir);
    const Generation& g = gens.back();
    const LinkInfo& l = *g.LastOk();
    HashStore hs;
    if (!hs.Load(JoinPath(g.dir, L"blockhashes.bin")) || _wcsicmp(hs.link.c_str(), l.file.c_str()) != 0) {
        throw Error(L"Block-Hashes fehlen oder gehören nicht zu " + l.file);
    }
    LogInfo(L"Prüfe %s\\%s (Kette mit %zu Gliedern) ...", g.name.c_str(), l.file.c_str(), g.links.size());
    VirtualDisk vd = VirtualDisk::Open(l.path, true);
    vd.Attach(true);
    UniqueHandle h = OpenPhysicalDisk(vd.PhysicalPath(), false);
    if (GetDeviceLength(h.Get()) != hs.diskSize) throw Error(L"Größe der Kette passt nicht zu den Block-Hashes.");
    uint64_t rep = 0;
    uint64_t mism = VerifyChain(h.Get(), hs, nullptr, &rep);
    h.Reset();
    vd.Detach();
    if (mism) {
        LogError(L"Verify: %llu Blöcke weichen ab.", static_cast<unsigned long long>(mism));
        return 1;
    }
    LogInfo(L"Verify: alle Blöcke stimmen, Kette ist lesbar.");
    return 0;
}

int RunList(const Config& cfg) {
    const std::wstring machineDir = MachineDir(cfg.targetPath);
    auto gens = ListGenerations(machineDir);
    if (gens.empty()) {
        LogInfo(L"Keine Backups in %s.", machineDir.c_str());
        return 0;
    }
    for (const auto& g : gens) {
        LogInfo(L"%s  (angelegt %s, Platte %s)", g.name.c_str(), g.created.c_str(), FormatBytes(g.diskSize).c_str());
        for (const auto& l : g.links) {
            LogInfo(L"   %-14s %-11s %-7s %s  geschrieben %s, %zu Warnungen", l.file.c_str(), l.type.c_str(),
                    l.status.c_str(), l.finished.c_str(), FormatBytes(l.bytesWritten).c_str(), l.warnings.size());
        }
    }
    return 0;
}

int RunInfo() {
    DiskInfo d = FindSystemDisk();
    LogLayout(d);
    return 0;
}

int RunInit(const std::wstring& configPath, const std::wstring& targetPath) {
    std::wstring target = targetPath;
    while (!target.empty() && (target.back() == L'\\' || target.back() == L'/')) target.pop_back();
    if (target.size() < 2) throw Error(L"Bitte einen Zielordner angeben, z. B. E:\\Backups");
    if (FileExists(configPath)) {
        IniWrite(configPath, L"target", L"path", target);
        LogInfo(L"Konfiguration aktualisiert: %s", configPath.c_str());
    } else {
        WriteDefaultConfig(configPath, target);
        LogInfo(L"Konfiguration angelegt: %s", configPath.c_str());
    }
    std::wstring machineDir = MachineDir(target);
    CreateDirs(JoinPath(machineDir, L"logs"));
    CheckTargetVolume(target, 50);
    std::wstring info;
    EncryptionState es = QueryTargetEncryption(target, &info);
    if (es == EncryptionState::NotEncrypted) {
        LogWarn(L"Das Ziel ist NICHT verschlüsselt (%s). Empfohlen: BitLocker To Go auf der USB-Platte "
                L"(einmalig an einem PC mit Windows Pro/Enterprise/Education einrichten).", info.c_str());
    } else {
        LogInfo(L"Verschlüsselung des Ziels: %s", info.c_str());
    }
    DiskInfo d = FindSystemDisk();
    LogLayout(d);
    EnsureTargetNotOnSystemDisk(target, d);
    WriteRestoreDocs(machineDir, d, JoinPath(machineDir, L"gen-...\\inc-NNN.vhdx"));
    LogInfo(L"Fertig. Erstes Backup starten mit: vhdxbackup backup");
    return 0;
}

int RunSchedule(const std::wstring& configPath, const std::wstring& time) {
    if (time.size() != 5 || time[2] != L':') throw Error(L"Uhrzeit im Format HH:MM angeben, z. B. 02:00");
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring tr = L"\\\"" + std::wstring(exe) + L"\\\" backup --config \\\"" + configPath + L"\\\"";
    std::wstring cmd = L"schtasks.exe /Create /F /TN \"VhdxBackup\" /SC DAILY /ST " + time +
                       L" /RU SYSTEM /RL HIGHEST /TR \"" + tr + L"\"";
    LogInfo(L"Lege Aufgabe in der Windows-Aufgabenplanung an: %s", cmd.c_str());
    DWORD code = RunCommand(cmd, 60);
    if (code != 0) throw Error(Fmt(L"schtasks endete mit Code %lu", static_cast<unsigned long>(code)));
    LogInfo(L"Täglich um %s als SYSTEM. Ergebnis steht in der Aufgabenplanung und in den Logs auf dem Ziel.",
            time.c_str());
    return 0;
}

int RunUnschedule() {
    DWORD code = RunCommand(L"schtasks.exe /Delete /F /TN \"VhdxBackup\"", 60);
    if (code != 0) throw Error(Fmt(L"schtasks endete mit Code %lu", static_cast<unsigned long>(code)));
    LogInfo(L"Aufgabe VhdxBackup entfernt.");
    return 0;
}

}  // namespace vb
