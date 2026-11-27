#include "checks.h"

#include <objbase.h>
#include <wbemidl.h>
#include <sql.h>
#include <sqlext.h>

namespace vb {

void InitCom() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) ThrowHr(L"CoInitializeEx", hr);
    // Einstellungen wie im VSS-Beispiel VShadow von Microsoft.
    hr = CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_PKT_PRIVACY, RPC_C_IMP_LEVEL_IDENTIFY,
                              nullptr, EOAC_DYNAMIC_CLOAKING, nullptr);
    if (FAILED(hr) && hr != RPC_E_TOO_LATE) ThrowHr(L"CoInitializeSecurity", hr);
}

EncryptionState QueryTargetEncryption(const std::wstring& targetPath, std::wstring* info) {
    if (targetPath.size() < 2 || targetPath[1] != L':') {
        *info = L"Ziel ist kein lokales Laufwerk, Verschlüsselung wird dort verwaltet";
        return EncryptionState::Unknown;
    }
    std::wstring letter = targetPath.substr(0, 2);
    ComPtr<IWbemLocator> loc;
    HRESULT hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator,
                                  reinterpret_cast<void**>(loc.Put()));
    if (FAILED(hr)) { *info = Fmt(L"WMI nicht verfügbar (0x%08lX)", static_cast<unsigned long>(hr)); return EncryptionState::Unknown; }
    ComPtr<IWbemServices> svc;
    BSTR ns = SysAllocString(L"ROOT\\CIMV2\\Security\\MicrosoftVolumeEncryption");
    hr = loc->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, svc.Put());
    SysFreeString(ns);
    if (FAILED(hr)) {
        *info = Fmt(L"BitLocker-WMI-Namespace nicht erreichbar (0x%08lX)", static_cast<unsigned long>(hr));
        return EncryptionState::Unknown;
    }
    CoSetProxyBlanket(svc.Get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_PKT_PRIVACY,
                      RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
    std::wstring q = L"SELECT ConversionStatus, ProtectionStatus FROM Win32_EncryptableVolume WHERE DriveLetter='" +
                     letter + L"'";
    BSTR wql = SysAllocString(L"WQL");
    BSTR query = SysAllocString(q.c_str());
    ComPtr<IEnumWbemClassObject> en;
    hr = svc->ExecQuery(wql, query, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, en.Put());
    SysFreeString(wql);
    SysFreeString(query);
    if (FAILED(hr)) { *info = Fmt(L"WMI-Abfrage fehlgeschlagen (0x%08lX)", static_cast<unsigned long>(hr)); return EncryptionState::Unknown; }
    ComPtr<IWbemClassObject> obj;
    ULONG n = 0;
    hr = en->Next(10000, 1, obj.Put(), &n);
    if (FAILED(hr) || n == 0) {
        *info = L"Laufwerk ist bei BitLocker nicht bekannt";
        return EncryptionState::NotEncrypted;
    }
    VARIANT conv;
    VariantInit(&conv);
    hr = obj->Get(L"ConversionStatus", 0, &conv, nullptr, nullptr);
    long cs = (SUCCEEDED(hr) && (conv.vt == VT_I4 || conv.vt == VT_UI4)) ? conv.lVal : -1;
    VariantClear(&conv);
    // ConversionStatus: 0 = vollständig entschlüsselt, 1 = vollständig verschlüsselt, 2..5 = in Arbeit/angehalten.
    if (cs == 0) { *info = L"BitLocker aus"; return EncryptionState::NotEncrypted; }
    if (cs == 1) { *info = L"BitLocker aktiv, vollständig verschlüsselt"; return EncryptionState::Encrypted; }
    if (cs > 1) { *info = Fmt(L"BitLocker in Umwandlung (ConversionStatus %ld)", cs); return EncryptionState::Encrypted; }
    *info = L"Status nicht lesbar";
    return EncryptionState::Unknown;
}

void CheckTargetVolume(const std::wstring& targetPath, uint64_t minFreeGb) {
    wchar_t root[MAX_PATH];
    if (!GetVolumePathNameW(targetPath.c_str(), root, MAX_PATH)) ThrowWin32(L"Ziel-Volume ermitteln " + targetPath);
    wchar_t fs[64] = {};
    if (GetVolumeInformationW(root, nullptr, 0, nullptr, nullptr, nullptr, fs, 64)) {
        std::wstring f = fs;
        if (f == L"FAT" || f == L"FAT32") {
            throw Error(L"Ziel " + std::wstring(root) + L" ist " + f + L". VHDX-Dateien brauchen NTFS (empfohlen) oder ReFS.");
        }
        if (f != L"NTFS" && f != L"ReFS") {
            LogWarn(L"Ziel %s hat das Dateisystem %s. Empfohlen ist NTFS.", root, fs);
        }
    }
    ULARGE_INTEGER freeBytes{}, total{}, totalFree{};
    if (GetDiskFreeSpaceExW(root, &freeBytes, &total, &totalFree)) {
        uint64_t freeGb = freeBytes.QuadPart >> 30;
        if (freeGb < minFreeGb) {
            LogWarn(L"Auf dem Ziel sind nur noch %llu GB frei (Grenze %llu GB). Es wird nichts automatisch gelöscht; "
                    L"bitte [retention] keep_generations setzen oder alte Generationen von Hand entfernen.",
                    static_cast<unsigned long long>(freeGb), static_cast<unsigned long long>(minFreeGb));
        }
    }
}

namespace {

std::wstring OdbcError(SQLSMALLINT type, SQLHANDLE h) {
    SQLWCHAR state[6], msg[512];
    SQLINTEGER native = 0;
    SQLSMALLINT len = 0;
    if (SQL_SUCCEEDED(SQLGetDiagRecW(type, h, 1, state, &native, msg, 512, &len))) {
        return std::wstring(reinterpret_cast<wchar_t*>(msg), len);
    }
    return L"unbekannter ODBC-Fehler";
}

}  // namespace

void CheckSqlRecoveryModels(const std::vector<std::wstring>& instances) {
    if (instances.empty()) return;
    SQLHENV env = SQL_NULL_HENV;
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env))) {
        LogWarn(L"SQL-Modellprüfung: ODBC nicht verfügbar.");
        return;
    }
    SQLSetEnvAttr(env, SQL_ATTR_ODBC_VERSION, reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0);
    for (const auto& inst : instances) {
        SQLHDBC dbc = SQL_NULL_HDBC;
        SQLAllocHandle(SQL_HANDLE_DBC, env, &dbc);
        SQLSetConnectAttrW(dbc, SQL_LOGIN_TIMEOUT, reinterpret_cast<SQLPOINTER>(10), 0);
        std::wstring cs = L"Driver={SQL Server};Server=" + inst + L";Trusted_Connection=Yes;";
        SQLRETURN r = SQLDriverConnectW(dbc, nullptr, reinterpret_cast<SQLWCHAR*>(&cs[0]), SQL_NTS, nullptr, 0, nullptr,
                                        SQL_DRIVER_NOPROMPT);
        if (!SQL_SUCCEEDED(r)) {
            LogWarn(L"SQL-Modellprüfung für %s nicht möglich: %s", inst.c_str(), OdbcError(SQL_HANDLE_DBC, dbc).c_str());
            SQLFreeHandle(SQL_HANDLE_DBC, dbc);
            continue;
        }
        SQLHSTMT st = SQL_NULL_HSTMT;
        SQLAllocHandle(SQL_HANDLE_STMT, dbc, &st);
        std::wstring q = L"SELECT name FROM sys.databases WHERE recovery_model_desc = 'FULL' AND database_id > 4";
        if (SQL_SUCCEEDED(SQLExecDirectW(st, reinterpret_cast<SQLWCHAR*>(&q[0]), SQL_NTS))) {
            while (SQL_SUCCEEDED(SQLFetch(st))) {
                wchar_t name[256] = {};
                SQLLEN ind = 0;
                if (SQL_SUCCEEDED(SQLGetData(st, 1, SQL_C_WCHAR, name, sizeof(name), &ind))) {
                    LogWarn(L"SQL Server %s: Datenbank \"%s\" läuft im Modell FULL. Das Transaktionsprotokoll wächst "
                            L"ohne Log-Backups weiter. Empfohlen: ALTER DATABASE [%s] SET RECOVERY SIMPLE;",
                            inst.c_str(), name, name);
                }
            }
        } else {
            LogWarn(L"SQL-Modellprüfung für %s: Abfrage fehlgeschlagen: %s", inst.c_str(),
                    OdbcError(SQL_HANDLE_STMT, st).c_str());
        }
        SQLFreeHandle(SQL_HANDLE_STMT, st);
        SQLDisconnect(dbc);
        SQLFreeHandle(SQL_HANDLE_DBC, dbc);
    }
    SQLFreeHandle(SQL_HANDLE_ENV, env);
}

}  // namespace vb
