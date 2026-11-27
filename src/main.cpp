// VhdxBackup: Image-Backup der Windows-11-Systemplatte als VHDX-Kette (VSS, VirtDisk, Block-Hash).
#include "backup.h"
#include "checks.h"
#include "common.h"
#include "config.h"


using namespace vb;

namespace {

void Usage() {
    LogInfo(L"%s %s - Image-Backup der Windows-Systemplatte als VHDX", kToolName, kToolVersion);
    std::wstring u =
        L"\r\nAufruf (als Administrator):\r\n"
        L"  vhdxbackup init <Zielordner>        Einrichten, z. B. vhdxbackup init E:\\Backups\r\n"
        L"  vhdxbackup backup [--full]          Backup jetzt (inkrementell, bei Bedarf voll)\r\n"
        L"  vhdxbackup verify                   Neueste Kette einbinden und gegen die Block-Hashes prüfen\r\n"
        L"  vhdxbackup list                     Generationen und Kettenglieder anzeigen\r\n"
        L"  vhdxbackup info                     Systemplatte und Partitionen anzeigen\r\n"
        L"  vhdxbackup schedule <HH:MM>         Tägliche Aufgabe in der Aufgabenplanung anlegen\r\n"
        L"  vhdxbackup unschedule               Aufgabe entfernen\r\n"
        L"\r\nOptionen:\r\n"
        L"  --config <Datei>   Einstellungen (Standard: %ProgramData%\\VhdxBackup\\backup.ini)\r\n"
        L"\r\nExit-Codes: 0 = ok, 2 = ok mit Warnungen, 1 = Fehler\r\n";
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD n = 0, mode = 0;
    if (GetConsoleMode(out, &mode)) WriteConsoleW(out, u.data(), static_cast<DWORD>(u.size()), &n, nullptr);
    else {
        std::string s = ToUtf8(u);
        WriteFile(out, s.data(), static_cast<DWORD>(s.size()), &n, nullptr);
    }
}

BOOL WINAPI CtrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        RequestAbort();
        return TRUE;
    }
    return FALSE;
}

bool IsElevated() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION el{};
    DWORD n = 0;
    BOOL ok = GetTokenInformation(tok, TokenElevation, &el, sizeof(el), &n);
    CloseHandle(tok);
    return ok && el.TokenIsElevated;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleCtrlHandler(CtrlHandler, TRUE);
    std::vector<std::wstring> args(argv + 1, argv + argc);
    std::wstring command, configPath = DefaultConfigPath(), positional;
    bool full = false;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::wstring& a = args[i];
        if (a == L"--config" && i + 1 < args.size()) configPath = args[++i];
        else if (a == L"--full") full = true;
        else if (a == L"-h" || a == L"--help" || a == L"/?") command = L"help";
        else if (command.empty()) command = ToLower(a);
        else if (positional.empty()) positional = a;
        else {
            LogError(L"Unbekanntes Argument: %s", a.c_str());
            return 1;
        }
    }
    if (command.empty() || command == L"help") {
        Usage();
        return command.empty() ? 1 : 0;
    }
    if (!IsElevated()) {
        LogError(L"Bitte als Administrator ausführen (VSS und Rohzugriff auf Platten brauchen Adminrechte).");
        return 1;
    }
    try {
        InitCom();
        if (command == L"init") {
            if (positional.empty()) throw Error(L"Zielordner fehlt, z. B. vhdxbackup init E:\\Backups");
            return RunInit(configPath, positional);
        }
        if (command == L"info") return RunInfo();
        if (command == L"schedule") {
            if (positional.empty()) throw Error(L"Uhrzeit fehlt, z. B. vhdxbackup schedule 02:00");
            LoadConfig(configPath);
            return RunSchedule(configPath, positional);
        }
        if (command == L"unschedule") return RunUnschedule();
        Config cfg = LoadConfig(configPath);
        if (command == L"backup") return RunBackup(cfg, full);
        if (command == L"verify") return RunVerify(cfg);
        if (command == L"list") return RunList(cfg);
        LogError(L"Unbekannter Befehl: %s", command.c_str());
        Usage();
        return 1;
    } catch (const Error& e) {
        LogError(L"%s", e.Message().c_str());
        LogCloseFile();
        return 1;
    } catch (const std::exception& e) {
        LogError(L"%s", FromUtf8(e.what()).c_str());
        LogCloseFile();
        return 1;
    }
}
