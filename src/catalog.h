// Ablage auf dem Ziel: Generationen, Kettenglieder, Lauf-Infos (INI), catalog.json, Aufbewahrung.
//
//   <Ziel>\<PC>\gen-20261126-020000\generation.ini
//                                   \base.vhdx      + base.ini
//                                   \inc-001.vhdx   + inc-001.ini   (Parent: base.vhdx)
//                                   \blockhashes.bin
//   <Ziel>\<PC>\catalog.json, RESTORE.txt, CreatePartitions-UEFI.txt, logs (Ordner)
#pragma once

#include "common.h"

namespace vb {

struct LinkInfo {
    std::wstring file;       // base.vhdx / inc-001.vhdx
    std::wstring path;       // voller Pfad
    std::wstring iniPath;
    std::wstring type;       // full | incremental
    std::wstring status;     // running | ok | failed
    std::wstring started, finished, parent;
    uint64_t blocksWritten = 0, bytesWritten = 0, blocksRepaired = 0;
    std::vector<std::wstring> warnings;
};

struct Generation {
    std::wstring name;       // gen-20261126-020000
    std::wstring dir;
    std::wstring created;
    uint64_t diskSize = 0;
    uint32_t logicalSector = 0;
    std::wstring sourceDiskId;
    std::vector<LinkInfo> links;   // base zuerst, dann inc-001, inc-002, ...
    const LinkInfo* LastOk() const;
};

std::wstring MachineDir(const std::wstring& targetPath);
std::vector<Generation> ListGenerations(const std::wstring& machineDir);

std::wstring LinkFileName(size_t index);   // 0 -> base.vhdx, 1 -> inc-001.vhdx
std::wstring IniForLink(const std::wstring& linkPath);

void WriteLinkInfo(const LinkInfo& l);

// Entfernt unvollständige letzte Glieder (Absturz, Stromausfall) und leere Generationen.
void CleanupIncomplete(const std::wstring& machineDir);

// Löscht die ältesten ganzen Generationen, bis höchstens keep übrig sind (0 = alle behalten).
void ApplyRetention(const std::wstring& machineDir, uint32_t keep);

void WriteCatalogJson(const std::wstring& machineDir);

void MakeReadOnly(const std::wstring& path);

}  // namespace vb
