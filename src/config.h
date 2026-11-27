// Einstellungen aus einer INI-Datei (gelesen mit der Windows-API GetPrivateProfileString).
#pragma once

#include "common.h"

namespace vb {

struct Config {
    std::wstring path;                 // Pfad der INI-Datei
    std::wstring targetPath;           // [target] path, z. B. E:\Backups
    uint64_t minFreeGb = 50;           // [target] min_free_gb
    uint32_t chainMaxLength = 30;      // [chain] max_length
    uint32_t keepGenerations = 0;      // [retention] keep_generations, 0 = alle
    bool verifyFull = true;            // [verify] mode = full | off
    std::vector<std::wstring> preCommands;  // [pre] command1..command9
    uint32_t preTimeoutSec = 900;      // [pre] timeout_sec
    bool sqlCheckRecoveryModel = true; // [sql] check_recovery_model
};

std::wstring DefaultConfigPath();      // %ProgramData%\VhdxBackup\backup.ini
Config LoadConfig(const std::wstring& path);
void WriteDefaultConfig(const std::wstring& path, const std::wstring& targetPath);

// Legt eine leere INI-Datei mit UTF-16-BOM an, damit WritePrivateProfileString Unicode schreibt.
void EnsureUnicodeIni(const std::wstring& path);
void IniWrite(const std::wstring& path, const wchar_t* section, const wchar_t* key, const std::wstring& value);
std::wstring IniRead(const std::wstring& path, const wchar_t* section, const wchar_t* key,
                     const wchar_t* def = L"");
uint64_t IniReadU64(const std::wstring& path, const wchar_t* section, const wchar_t* key, uint64_t def);

}  // namespace vb
