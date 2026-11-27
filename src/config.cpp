#include "config.h"

#include <cwchar>

namespace vb {

std::wstring DefaultConfigPath() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"ProgramData", buf, MAX_PATH);
    std::wstring base = (n > 0 && n < MAX_PATH) ? std::wstring(buf) : std::wstring(L"C:\\ProgramData");
    return JoinPath(JoinPath(base, kToolName), L"backup.ini");
}

void EnsureUnicodeIni(const std::wstring& path) {
    if (FileExists(path)) return;
    UniqueHandle h(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!h.Valid()) {
        if (GetLastError() == ERROR_FILE_EXISTS) return;
        ThrowWin32(L"INI-Datei anlegen " + path);
    }
    const unsigned char bom[2] = {0xFF, 0xFE};
    DWORD n = 0;
    WriteFile(h.Get(), bom, 2, &n, nullptr);
}

void IniWrite(const std::wstring& path, const wchar_t* section, const wchar_t* key, const std::wstring& value) {
    EnsureUnicodeIni(path);
    if (!WritePrivateProfileStringW(section, key, value.c_str(), path.c_str())) {
        ThrowWin32(L"INI schreiben " + path);
    }
}

std::wstring IniRead(const std::wstring& path, const wchar_t* section, const wchar_t* key, const wchar_t* def) {
    std::vector<wchar_t> buf(4096);
    for (;;) {
        DWORD n = GetPrivateProfileStringW(section, key, def, buf.data(), static_cast<DWORD>(buf.size()),
                                           path.c_str());
        if (n < buf.size() - 2) return Trim(std::wstring(buf.data(), n));
        buf.resize(buf.size() * 4);
    }
}

uint64_t IniReadU64(const std::wstring& path, const wchar_t* section, const wchar_t* key, uint64_t def) {
    std::wstring v = IniRead(path, section, key, L"");
    if (v.empty()) return def;
    wchar_t* end = nullptr;
    unsigned long long x = wcstoull(v.c_str(), &end, 10);
    if (end == v.c_str() || *end != L'\0') {
        throw Error(Fmt(L"Ungültiger Zahlenwert [%s] %s=%s in %s", section, key, v.c_str(), path.c_str()));
    }
    return x;
}

Config LoadConfig(const std::wstring& path) {
    if (!FileExists(path)) {
        throw Error(L"Konfiguration nicht gefunden: " + path +
                    L"\nZuerst einrichten mit: vhdxbackup init <Zielordner>, z. B. vhdxbackup init E:\\Backups");
    }
    Config c;
    c.path = path;
    c.targetPath = IniRead(path, L"target", L"path");
    if (c.targetPath.empty()) throw Error(L"[target] path fehlt in " + path);
    while (!c.targetPath.empty() && (c.targetPath.back() == L'\\' || c.targetPath.back() == L'/')) {
        c.targetPath.pop_back();
    }
    c.minFreeGb = IniReadU64(path, L"target", L"min_free_gb", 50);
    c.chainMaxLength = static_cast<uint32_t>(IniReadU64(path, L"chain", L"max_length", 30));
    if (c.chainMaxLength < 1) c.chainMaxLength = 1;
    c.keepGenerations = static_cast<uint32_t>(IniReadU64(path, L"retention", L"keep_generations", 0));
    std::wstring vm = ToLower(IniRead(path, L"verify", L"mode", L"full"));
    if (vm == L"full") c.verifyFull = true;
    else if (vm == L"off") c.verifyFull = false;
    else throw Error(L"[verify] mode muss full oder off sein");
    for (int i = 1; i <= 9; ++i) {
        std::wstring key = Fmt(L"command%d", i);
        std::wstring cmd = IniRead(path, L"pre", key.c_str());
        if (!cmd.empty()) c.preCommands.push_back(cmd);
    }
    c.preTimeoutSec = static_cast<uint32_t>(IniReadU64(path, L"pre", L"timeout_sec", 900));
    c.sqlCheckRecoveryModel = IniReadU64(path, L"sql", L"check_recovery_model", 1) != 0;
    return c;
}

void WriteDefaultConfig(const std::wstring& path, const std::wstring& targetPath) {
    std::wstring text =
        L"; VhdxBackup Einstellungen\r\n"
        L"; Diese Datei wird mit GetPrivateProfileString gelesen (normales Windows-INI-Format).\r\n"
        L"\r\n"
        L"[target]\r\n"
        L"; Zielordner auf der USB-Platte. Pro PC entsteht darin ein Unterordner mit dem Computernamen.\r\n"
        L"path=" + targetPath + L"\r\n"
        L"; Unter diesem freien Platz (GB) gibt es eine Warnung. Es wird nie automatisch gelöscht.\r\n"
        L"min_free_gb=50\r\n"
        L"\r\n"
        L"[chain]\r\n"
        L"; Inkremente pro Generation, danach beginnt ein neues Vollbackup.\r\n"
        L"max_length=30\r\n"
        L"\r\n"
        L"[retention]\r\n"
        L"; Leer = alle Generationen behalten. Zahl = nur die letzten N Generationen behalten.\r\n"
        L"keep_generations=\r\n"
        L"\r\n"
        L"[verify]\r\n"
        L"; full = nach jedem Lauf die ganze Kette einbinden, lesen und gegen die Block-Hashes prüfen.\r\n"
        L"; off  = keine Prüfung (schneller, nicht empfohlen).\r\n"
        L"mode=full\r\n"
        L"\r\n"
        L"[pre]\r\n"
        L"; Befehle vor dem Snapshot, z. B. Datenbank-Dumps. Laufen über cmd.exe /c.\r\n"
        L"; Der Dump muss auf der OS-Platte landen, dann ist er im Image.\r\n"
        L"; command1=\"C:\\Program Files\\MySQL\\MySQL Server 8.4\\bin\\mysqldump.exe\" --defaults-extra-file=C:\\Backup\\my.cnf --all-databases --single-transaction --result-file=C:\\Backup\\mysql-all.sql\r\n"
        L"; command2=\"C:\\Program Files\\MariaDB 11.4\\bin\\mariadb-dump.exe\" --defaults-extra-file=C:\\Backup\\mariadb.cnf --all-databases --single-transaction --result-file=C:\\Backup\\mariadb-all.sql\r\n"
        L"timeout_sec=900\r\n"
        L"\r\n"
        L"[sql]\r\n"
        L"; 1 = vor dem Backup warnen, wenn eine SQL-Server-Datenbank im Modell FULL läuft.\r\n"
        L"check_recovery_model=1\r\n";
    size_t pos = path.find_last_of(L'\\');
    if (pos != std::wstring::npos) CreateDirs(path.substr(0, pos));
    // UTF-16 LE mit BOM: so liest und schreibt GetPrivateProfileString Unicode.
    UniqueHandle h(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!h.Valid()) ThrowWin32(L"Konfiguration schreiben " + path);
    std::wstring data = L"\xFEFF" + text;
    DWORD n = 0;
    if (!WriteFile(h.Get(), data.data(), static_cast<DWORD>(data.size() * sizeof(wchar_t)), &n, nullptr)) {
        ThrowWin32(L"Konfiguration schreiben " + path);
    }
}

}  // namespace vb
