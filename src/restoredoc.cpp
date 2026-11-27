#include "restoredoc.h"

#include <algorithm>

namespace vb {

namespace {

uint64_t Mb(uint64_t bytes) { return (bytes + (1u << 20) - 1) >> 20; }

// Pfad ohne Laufwerksbuchstaben, z. B. E:\Backups\PC01 -> \Backups\PC01
std::wstring WithoutDrive(const std::wstring& p) {
    if (p.size() >= 2 && p[1] == L':') return p.substr(2);
    return p;
}

}  // namespace

void WriteRestoreDocs(const std::wstring& machineDir, const DiskInfo& disk, const std::wstring& newestLinkPath) {
    const PartitionInfo* efi = nullptr;
    const PartitionInfo* recovery = nullptr;
    std::vector<const PartitionInfo*> extra;
    for (const auto& p : disk.parts) {
        if (p.kind == PartKind::Efi && !efi) efi = &p;
        else if (p.kind == PartKind::Recovery && !recovery) recovery = &p;
        else if (p.isWindows) continue;
        else if (p.kind == PartKind::Basic) extra.push_back(&p);
    }
    uint64_t minEfi = disk.logicalSector >= 4096 ? 300 : 200;
    uint64_t efiMb = efi ? std::max<uint64_t>(Mb(efi->length), minEfi) : minEfi;
    uint64_t recMb = recovery ? Mb(recovery->length) : 0;

    // ---- CreatePartitions-UEFI.txt (Microsoft-Vorlage aus den OEM-Deployment-Beispielskripten)
    std::wstring dp;
    dp += L"rem == CreatePartitions-UEFI.txt ==\r\n";
    dp += L"rem == Nach der Microsoft-Vorlage (OEM deployment sample scripts), Groessen von dieser Systemplatte.\r\n";
    dp += L"rem == ACHTUNG: 'select disk 0' loescht Datentraeger 0 vollstaendig. Vorher mit diskpart\r\n";
    dp += L"rem    'list disk' pruefen, ob 0 die NEUE Platte ist, sonst die Nummer hier aendern (notepad).\r\n";
    dp += L"select disk 0\r\n";
    dp += L"clean\r\n";
    dp += L"convert gpt\r\n";
    dp += L"rem == 1. System partition =========================\r\n";
    dp += Fmt(L"create partition efi size=%llu\r\n", static_cast<unsigned long long>(efiMb));
    dp += L"format quick fs=fat32 label=\"System\"\r\n";
    dp += L"assign letter=\"S\"\r\n";
    dp += L"rem == 2. Microsoft Reserved (MSR) partition =======\r\n";
    dp += L"create partition msr size=16\r\n";
    dp += L"rem == 3. Windows partition ========================\r\n";
    dp += L"create partition primary\r\n";
    if (recovery) {
        dp += Fmt(L"shrink minimum=%llu\r\n", static_cast<unsigned long long>(recMb));
    }
    dp += L"format quick fs=ntfs label=\"Windows\"\r\n";
    dp += L"assign letter=\"W\"\r\n";
    if (recovery) {
        dp += L"rem === 4. Recovery partition ======================\r\n";
        dp += L"create partition primary\r\n";
        dp += L"format quick fs=ntfs label=\"Recovery\"\r\n";
        dp += L"assign letter=\"R\"\r\n";
        dp += L"set id=\"de94bba4-06d1-4d40-a16a-bfd50179d6ac\"\r\n";
        dp += L"gpt attributes=0x8000000000000001\r\n";
    }
    dp += L"list volume\r\n";
    dp += L"exit\r\n";
    WriteTextFileUtf8(JoinPath(machineDir, L"CreatePartitions-UEFI.txt"), dp, false);

    // ---- RESTORE.txt
    std::wstring rel = WithoutDrive(machineDir);
    std::wstring linkRel = WithoutDrive(newestLinkPath);
    std::wstring t;
    t += L"WIEDERHERSTELLUNG MIT DEM WINDOWS-11-SETUP-STICK\r\n";
    t += L"================================================\r\n\r\n";
    t += L"Erzeugt von " + std::wstring(kToolName) + L" " + kToolVersion + L" am " + NowIso() + L".\r\n";
    t += L"Für die Wiederherstellung wird dieses Tool NICHT gebraucht. Alles sind Windows-Bordmittel.\r\n\r\n";
    t += L"Quellplatte beim letzten Backup:\r\n";
    t += Fmt(L"  Größe %s, Sektor %u Byte, %s\r\n", FormatBytes(disk.size).c_str(), disk.logicalSector,
             disk.gpt ? L"GPT (UEFI)" : L"MBR (BIOS)");
    for (const auto& p : disk.parts) {
        t += Fmt(L"  Partition %u: %-8s %10s  %s %s%s\r\n", p.number, PartKindName(p.kind), FormatBytes(p.length).c_str(),
                 p.fileSystem.c_str(), p.mountPoints.c_str(), p.isWindows ? L" (Windows)" : L"");
    }
    t += L"\r\nNeuestes Kettenglied zum Zeitpunkt dieser Datei:\r\n  N:" + linkRel + L"\r\n";
    t += L"Jede .vhdx-Datei ist ein Wiederherstellungspunkt. inc-005.vhdx enthält z. B. den Stand nach dem 5. Inkrement\r\n";
    t += L"und braucht alle älteren Dateien derselben Generation im selben Ordner.\r\n\r\n";
    t += L"WICHTIG: Kettenglieder nie beschreibbar einbinden. Ein geändertes Elternteil macht alle späteren Glieder\r\n";
    t += L"unbrauchbar. Immer 'attach vdisk readonly' bzw. Mount-DiskImage -Access ReadOnly verwenden.\r\n\r\n";

    t += L"A) EINZELNE DATEIEN ZURÜCKHOLEN (laufendes Windows, auch Home)\r\n";
    t += L"--------------------------------------------------------------\r\n";
    t += L"PowerShell als Administrator:\r\n";
    t += L"  Mount-DiskImage -ImagePath \"<Pfad>\\inc-005.vhdx\" -Access ReadOnly\r\n";
    t += L"Die Volumes erscheinen in der Datenträgerverwaltung; bei Bedarf dort einen Laufwerksbuchstaben vergeben.\r\n";
    t += L"Danach: Dismount-DiskImage -ImagePath \"<Pfad>\\inc-005.vhdx\"\r\n\r\n";

    t += L"B) GANZE PLATTE WIEDERHERSTELLEN (Plattentausch im gleichen PC)\r\n";
    t += L"---------------------------------------------------------------\r\n";
    t += L"1. Neue Platte einbauen, vom offiziellen Windows-11-Installationsstick booten (Media Creation Tool),\r\n";
    t += L"   bei der Sprachauswahl Umschalt+F10 drücken. Es öffnet sich eine Eingabeaufforderung.\r\n\r\n";
    t += L"2. Backup-Platte anstecken und ihren Buchstaben finden:\r\n";
    t += L"     diskpart\r\n     list volume\r\n     exit\r\n";
    t += L"   Im Folgenden heißt sie N:. Ist sie mit BitLocker To Go verschlüsselt:\r\n";
    t += L"     manage-bde -unlock N: -RecoveryPassword <48-stelliger Wiederherstellungsschlüssel>\r\n\r\n";
    t += L"3. Gewünschtes Kettenglied schreibgeschützt einbinden und Buchstaben vergeben:\r\n";
    t += L"     diskpart\r\n";
    t += L"     select vdisk file=N:" + linkRel + L"\r\n";
    t += L"     attach vdisk readonly\r\n";
    t += L"     list volume\r\n";
    t += L"     select volume <Nummer des Windows-Volumes aus dem Backup>\r\n";
    t += L"     assign letter=V\r\n";
    if (recovery) {
        t += L"     select volume <Nummer des Recovery-Volumes aus dem Backup>\r\n";
        t += L"     assign letter=Q\r\n";
    }
    t += L"     exit\r\n\r\n";
    t += L"4. Neue Platte partitionieren (System S:, Windows W:, Recovery R:).\r\n";
    t += L"   VORHER in diskpart mit 'list disk' prüfen, welche Nummer die neue Platte hat, und die Zeile\r\n";
    t += L"   'select disk 0' in der Datei bei Bedarf anpassen:  notepad N:" + rel + L"\\CreatePartitions-UEFI.txt\r\n";
    t += L"     diskpart /s N:" + rel + L"\\CreatePartitions-UEFI.txt\r\n";
    t += L"   Die neue Platte darf kleiner sein als die alte, solange die belegten Daten passen.\r\n\r\n";
    t += L"5. Windows übertragen. DISM wendet keine VHDX direkt an, daher über eine WIM-Datei:\r\n";
    t += L"     md N:\\tmp\r\n";
    t += L"     dism /Capture-Image /ImageFile:N:\\tmp\\restore.wim /CaptureDir:V:\\ /Name:Restore /ScratchDir:N:\\tmp\r\n";
    t += L"     dism /Apply-Image /ImageFile:N:\\tmp\\restore.wim /Index:1 /ApplyDir:W:\\ /ScratchDir:N:\\tmp\r\n";
    t += L"   Braucht freien Platz auf N: in Größe der belegten Daten.\r\n\r\n";
    t += L"6. Bootdateien und Recovery einrichten:\r\n";
    t += L"     W:\\Windows\\System32\\bcdboot W:\\Windows /s S: /f UEFI\r\n";
    if (recovery) {
        t += L"     md R:\\Recovery\\WindowsRE\r\n";
        t += L"     copy Q:\\Recovery\\WindowsRE\\winre.wim R:\\Recovery\\WindowsRE\\\r\n";
        t += L"     W:\\Windows\\System32\\reagentc /setreimage /path R:\\Recovery\\WindowsRE /target W:\\Windows\r\n";
    }
    t += L"\r\n7. Aufräumen und neu starten:\r\n";
    t += L"     diskpart\r\n     select vdisk file=N:" + linkRel + L"\r\n     detach vdisk\r\n     exit\r\n";
    t += L"     del N:\\tmp\\restore.wim\r\n";
    t += L"     wpeutil reboot\r\n";
    t += L"   Danach in Windows die Geräteverschlüsselung bzw. BitLocker wieder einschalten.\r\n\r\n";
    if (!extra.empty()) {
        t += L"Weitere Datenpartitionen dieser Platte (nicht im Skript enthalten):\r\n";
        for (const auto* p : extra) {
            t += Fmt(L"  Partition %u, %s, %s %s\r\n", p->number, FormatBytes(p->length).c_str(), p->fileSystem.c_str(),
                     p->mountPoints.c_str());
        }
        t += L"  Nach Schritt 4 mit diskpart ('shrink' auf W:, 'create partition primary', 'format quick fs=ntfs')\r\n";
        t += L"  Platz schaffen und den Inhalt wie in Schritt 5 per dism /Capture-Image und /Apply-Image übertragen.\r\n\r\n";
    }
    if (!disk.gpt) {
        t += L"HINWEIS: Diese Platte ist MBR/BIOS. Das Skript ist für UEFI/GPT. Für BIOS die Microsoft-Vorlage\r\n";
        t += L"CreatePartitions-BIOS.txt verwenden und bcdboot mit /f BIOS aufrufen.\r\n\r\n";
    }
    t += L"Quellen: Microsoft Learn, \"Capture and Apply Windows, System, and Recovery Partitions\" und\r\n";
    t += L"\"OEM deployment of Windows desktop editions sample scripts\" (CreatePartitions-UEFI.txt).\r\n";
    WriteTextFileUtf8(JoinPath(machineDir, L"RESTORE.txt"), t, true);
}

}  // namespace vb
