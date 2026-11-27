#include "catalog.h"

#include "config.h"

#include <algorithm>

namespace vb {

const LinkInfo* Generation::LastOk() const {
    if (links.empty() || links.back().status != L"ok") return nullptr;
    return &links.back();
}

std::wstring MachineDir(const std::wstring& targetPath) {
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
    if (!GetComputerNameW(name, &n)) ThrowWin32(L"GetComputerName");
    return JoinPath(targetPath, name);
}

std::wstring LinkFileName(size_t index) {
    return index == 0 ? std::wstring(L"base.vhdx") : Fmt(L"inc-%03zu.vhdx", index);
}

std::wstring IniForLink(const std::wstring& linkPath) {
    size_t dot = linkPath.find_last_of(L'.');
    return linkPath.substr(0, dot) + L".ini";
}

void WriteLinkInfo(const LinkInfo& l) {
    const std::wstring& p = l.iniPath;
    IniWrite(p, L"run", L"file", l.file);
    IniWrite(p, L"run", L"type", l.type);
    IniWrite(p, L"run", L"status", l.status);
    IniWrite(p, L"run", L"started", l.started);
    IniWrite(p, L"run", L"finished", l.finished);
    IniWrite(p, L"run", L"parent", l.parent);
    IniWrite(p, L"run", L"blocks_written", Fmt(L"%llu", static_cast<unsigned long long>(l.blocksWritten)));
    IniWrite(p, L"run", L"bytes_written", Fmt(L"%llu", static_cast<unsigned long long>(l.bytesWritten)));
    IniWrite(p, L"run", L"blocks_repaired", Fmt(L"%llu", static_cast<unsigned long long>(l.blocksRepaired)));
    WritePrivateProfileSectionW(L"warnings", L"", p.c_str());
    IniWrite(p, L"warnings", L"count", Fmt(L"%zu", l.warnings.size()));
    for (size_t i = 0; i < l.warnings.size(); ++i) {
        IniWrite(p, L"warnings", Fmt(L"w%zu", i + 1).c_str(), l.warnings[i]);
    }
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, p.c_str());  // Cache leeren
}

namespace {

LinkInfo ReadLink(const std::wstring& dir, const std::wstring& file) {
    LinkInfo l;
    l.file = file;
    l.path = JoinPath(dir, file);
    l.iniPath = IniForLink(l.path);
    if (!FileExists(l.iniPath)) {
        l.status = L"unknown";
        return l;
    }
    l.type = IniRead(l.iniPath, L"run", L"type");
    l.status = IniRead(l.iniPath, L"run", L"status", L"unknown");
    l.started = IniRead(l.iniPath, L"run", L"started");
    l.finished = IniRead(l.iniPath, L"run", L"finished");
    l.parent = IniRead(l.iniPath, L"run", L"parent");
    l.blocksWritten = IniReadU64(l.iniPath, L"run", L"blocks_written", 0);
    l.bytesWritten = IniReadU64(l.iniPath, L"run", L"bytes_written", 0);
    l.blocksRepaired = IniReadU64(l.iniPath, L"run", L"blocks_repaired", 0);
    uint64_t wc = IniReadU64(l.iniPath, L"warnings", L"count", 0);
    for (uint64_t i = 1; i <= wc && i < 1000; ++i) {
        l.warnings.push_back(IniRead(l.iniPath, L"warnings", Fmt(L"w%llu", static_cast<unsigned long long>(i)).c_str()));
    }
    return l;
}

void DeleteLink(const LinkInfo& l) {
    for (const auto& p : {l.path, l.iniPath}) {
        if (FileExists(p)) {
            SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
            if (!DeleteFileW(p.c_str())) ThrowWin32(L"Löschen " + p);
        }
    }
}

std::wstring JsonEscape(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) {
        switch (c) {
            case L'"': o += L"\\\""; break;
            case L'\\': o += L"\\\\"; break;
            case L'\n': o += L"\\n"; break;
            case L'\r': o += L"\\r"; break;
            case L'\t': o += L"\\t"; break;
            default:
                if (c < 0x20) o += Fmt(L"\\u%04x", static_cast<unsigned>(c));
                else o += c;
        }
    }
    return o;
}

}  // namespace

std::vector<Generation> ListGenerations(const std::wstring& machineDir) {
    std::vector<Generation> gens;
    auto names = ListDir(machineDir, L"gen-*", true);
    std::sort(names.begin(), names.end());   // Zeitstempel im Namen = chronologisch
    for (const auto& n : names) {
        Generation g;
        g.name = n;
        g.dir = JoinPath(machineDir, n);
        std::wstring gi = JoinPath(g.dir, L"generation.ini");
        g.created = IniRead(gi, L"generation", L"created");
        g.diskSize = IniReadU64(gi, L"generation", L"disk_size", 0);
        g.logicalSector = static_cast<uint32_t>(IniReadU64(gi, L"generation", L"logical_sector", 0));
        g.sourceDiskId = IniRead(gi, L"generation", L"source_disk_id");
        for (size_t i = 0;; ++i) {
            std::wstring f = LinkFileName(i);
            std::wstring p = JoinPath(g.dir, f);
            if (!FileExists(p) && !FileExists(IniForLink(p))) break;
            g.links.push_back(ReadLink(g.dir, f));
        }
        gens.push_back(std::move(g));
    }
    return gens;
}

void CleanupIncomplete(const std::wstring& machineDir) {
    for (auto& g : ListGenerations(machineDir)) {
        while (!g.links.empty() && g.links.back().status != L"ok") {
            LogWarn(L"Unvollständiges Kettenglied %s\\%s (Status %s) wird entfernt.", g.name.c_str(),
                    g.links.back().file.c_str(), g.links.back().status.c_str());
            DeleteLink(g.links.back());
            g.links.pop_back();
        }
        if (g.links.empty()) {
            LogWarn(L"Leere Generation %s wird entfernt.", g.name.c_str());
            DeleteTree(g.dir);
        }
    }
}

void ApplyRetention(const std::wstring& machineDir, uint32_t keep) {
    if (keep == 0) return;
    auto gens = ListGenerations(machineDir);
    if (gens.size() <= keep) return;
    size_t remove = gens.size() - keep;
    for (size_t i = 0; i < remove; ++i) {
        LogInfo(L"Aufbewahrung: lösche ganze Generation %s (%zu Glieder).", gens[i].name.c_str(), gens[i].links.size());
        DeleteTree(gens[i].dir);
    }
}

void MakeReadOnly(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    if (a != INVALID_FILE_ATTRIBUTES) SetFileAttributesW(path.c_str(), a | FILE_ATTRIBUTE_READONLY);
}

void WriteCatalogJson(const std::wstring& machineDir) {
    auto gens = ListGenerations(machineDir);
    std::wstring j = L"{\n";
    j += L"  \"tool\": \"" + std::wstring(kToolName) + L" " + kToolVersion + L"\",\n";
    j += L"  \"updated\": \"" + NowIso() + L"\",\n";
    j += L"  \"note\": \"Nur zur Anzeige. Für die Wiederherstellung wird diese Datei nicht gebraucht, siehe RESTORE.txt.\",\n";
    j += L"  \"generations\": [";
    for (size_t gi = 0; gi < gens.size(); ++gi) {
        const auto& g = gens[gi];
        j += gi ? L",\n" : L"\n";
        j += L"    {\n      \"name\": \"" + JsonEscape(g.name) + L"\",\n";
        j += L"      \"created\": \"" + JsonEscape(g.created) + L"\",\n";
        j += Fmt(L"      \"disk_size\": %llu,\n", static_cast<unsigned long long>(g.diskSize));
        j += Fmt(L"      \"logical_sector\": %u,\n", g.logicalSector);
        j += L"      \"links\": [";
        for (size_t li = 0; li < g.links.size(); ++li) {
            const auto& l = g.links[li];
            j += li ? L",\n" : L"\n";
            j += L"        {\"file\": \"" + JsonEscape(l.file) + L"\", \"type\": \"" + JsonEscape(l.type) +
                 L"\", \"status\": \"" + JsonEscape(l.status) + L"\", \"started\": \"" + JsonEscape(l.started) +
                 L"\", \"finished\": \"" + JsonEscape(l.finished) + L"\", \"parent\": \"" + JsonEscape(l.parent) +
                 Fmt(L"\", \"blocks_written\": %llu, \"bytes_written\": %llu, \"blocks_repaired\": %llu",
                     static_cast<unsigned long long>(l.blocksWritten), static_cast<unsigned long long>(l.bytesWritten),
                     static_cast<unsigned long long>(l.blocksRepaired)) +
                 L", \"warnings\": [";
            for (size_t wi = 0; wi < l.warnings.size(); ++wi) {
                j += (wi ? L", \"" : L"\"") + JsonEscape(l.warnings[wi]) + L"\"";
            }
            j += L"]}";
        }
        j += L"\n      ]\n    }";
    }
    j += L"\n  ]\n}\n";
    WriteTextFileUtf8(JoinPath(machineDir, L"catalog.json"), j, false);
}

}  // namespace vb
