// Prüfungen: Verschlüsselung und Dateisystem des Ziels, SQL-Server-Wiederherstellungsmodell, COM-Start.
#pragma once

#include "common.h"

namespace vb {

void InitCom();   // CoInitializeEx + CoInitializeSecurity wie für VSS-Requester vorgesehen

enum class EncryptionState { Encrypted, NotEncrypted, Unknown };
// BitLocker-Status des Ziel-Laufwerks über WMI (Win32_EncryptableVolume). info = Erläuterung.
EncryptionState QueryTargetEncryption(const std::wstring& targetPath, std::wstring* info);

// Prüft Dateisystem und freien Platz des Ziels. Wirft bei FAT/FAT32, warnt bei exFAT und Platzmangel.
void CheckTargetVolume(const std::wstring& targetPath, uint64_t minFreeGb);

// Warnung für jede Benutzer-Datenbank im Modell FULL (ODBC, Treiber "SQL Server" ist in Windows enthalten).
void CheckSqlRecoveryModels(const std::vector<std::wstring>& instances);

}  // namespace vb
