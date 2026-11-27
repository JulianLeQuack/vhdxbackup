# VhdxBackup 0.5.0 (Certified AI Slop)

Image-Backup der Windows-11-Systemplatte als VHDX-Kette, gebaut nach dem Konzept v0.5.
C++17, nur Windows-APIs (VSS, VirtDisk, CNG, WMI, ODBC), keine Fremdbibliotheken, keine Treiber.

> **Status: ungetestet auf echter Hardware.** Der Code ist vollständig und kompiliert, lief aber noch
> nie auf einem Windows-PC. Bitte zuerst auf dem Test-PC nach dem Abschnitt „PoC-Testplan“ prüfen,
> bevor du dich darauf verlässt.

## Schnellstart (Eingabeaufforderung als Administrator)

```cmd
vhdxbackup init E:\Backups        :: Konfiguration anlegen, Ziel prüfen, RESTORE.txt schreiben
vhdxbackup backup                 :: erstes Backup (voll), danach inkrementell
vhdxbackup list                   :: Generationen und Kettenglieder anzeigen
vhdxbackup verify                 :: neueste Kette einbinden und gegen die Block-Hashes prüfen
vhdxbackup schedule 02:00         :: täglich um 2 Uhr als SYSTEM (Windows-Aufgabenplanung)
```

Einstellungen: `%ProgramData%\VhdxBackup\backup.ini` (anderer Pfad mit `--config <Datei>`).
Exit-Codes: `0` ok, `2` ok mit Warnungen, `1` Fehler.

## Ablauf eines Backups

1. Ziel prüfen: Dateisystem (NTFS/ReFS), freier Platz (`min_free_gb`), BitLocker-Status per WMI, Ziel
   darf nicht auf der OS-Platte liegen.
2. Systemplatte finden (Volume von `%WINDIR%` → `IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS`), Layout lesen.
3. Pre-Befehle aus `[pre]` ausführen (z. B. `mysqldump`, `mariadb-dump`). Fehler = Warnung, Backup läuft weiter.
4. VSS: Komponentenmodus, `VSS_BT_FULL`, alle Writer-Komponenten, deren Dateien auf der OS-Platte liegen
   (Auswahl wie im Microsoft-Beispiel VShadow). Bei SQL Server Warnung für Datenbanken im Modell FULL.
   Snapshot-Set über alle NTFS-Volumes ab 1 GiB.
5. VHDX anlegen mit `CreateVirtualDisk`: voll = dynamisch in Größe der Quellplatte, inkrementell =
   Differencing-Disk mit dem letzten Glied als Parent. Einbinden ohne Laufwerksbuchstaben, dann
   **offline setzen**, damit kein Dateisystem darauf zugreift.
6. Platte in 1-MiB-Blöcken kopieren, an dieselben Offsets: NTFS-Volumes aus dem Snapshot (nur belegte
   Cluster laut `FSCTL_GET_VOLUME_BITMAP`, freie Cluster genullt), EFI/MSR/Recovery roh, GPT und Lücken
   roh. Inkrementell wird nur geschrieben, wenn der SHA-256 des Blocks vom Katalog abweicht. Die
   Partitionstabelle wird zuletzt geschrieben.
7. Verify (`[verify] mode=full`): ganze Kette lesen und gegen die Block-Hashes prüfen. Abweichungen
   werden aus dem noch vorhandenen Snapshot neu geschrieben und protokolliert.
8. Trennen, `SetBackupSucceeded` + `BackupComplete`, Block-Hashes atomar speichern, Glied schreibgeschützt
   setzen, Aufbewahrung, `catalog.json`, `RESTORE.txt`, `CreatePartitions-UEFI.txt` aktualisieren.

Bricht ein Lauf ab (Fehler, Strg+C, Stromausfall), wird das unvollständige Glied entfernt; spätestens
beim nächsten Lauf.

## Ablage auf dem Ziel

```
E:\Backups\<COMPUTERNAME>\
  gen-20261126-020000\
    generation.ini
    base.vhdx        + base.ini
    inc-001.vhdx     + inc-001.ini    (Parent: base.vhdx)
    blockhashes.bin  (SHA-256 je 1-MiB-Block, nur für den Hash-Vergleich)
  catalog.json  RESTORE.txt  CreatePartitions-UEFI.txt  logs\
```

Jede `.vhdx` ist ein Wiederherstellungspunkt. Für den Restore braucht man nur die `.vhdx`-Dateien der
Generation und `RESTORE.txt`, nicht das Tool.

## Einstellungen (`backup.ini`)

| Abschnitt / Schlüssel | Standard | Wirkung |
|---|---|---|
| `[target] path` | aus `init` | Zielordner |
| `[target] min_free_gb` | 50 | Warnung bei weniger freiem Platz, nie stilles Löschen |
| `[chain] max_length` | 30 | Inkremente pro Generation, danach neues Vollbackup |
| `[retention] keep_generations` | leer = alle | nur die letzten N ganzen Generationen behalten |
| `[verify] mode` | full | `full` oder `off` |
| `[pre] command1` … `command9` | – | Befehle vor dem Snapshot (über `cmd.exe /c`) |
| `[pre] timeout_sec` | 900 | Zeitlimit je Befehl |
| `[sql] check_recovery_model` | 1 | Warnung bei SQL-Datenbanken im Modell FULL |

## Restore

Steht vollständig in `RESTORE.txt` auf dem Ziel (wird nach jedem Backup neu geschrieben):
Windows-11-Setup-Stick → Umschalt+F10 → `diskpart` (Kette `attach vdisk readonly`, neue Platte mit
`CreatePartitions-UEFI.txt`) → `dism /Capture-Image` + `/Apply-Image` → `bcdboot` → WinRE kopieren,
`reagentc`. Einzelne Dateien: `Mount-DiskImage -ImagePath ... -Access ReadOnly`.

## Bauen

**Fertige exe:** `build/vhdxbackup.exe` (64 Bit, mit MinGW-w64 13 cross-kompiliert, statisch gelinkt;
importiert nur Windows-DLLs: kernel32, advapi32, msvcrt, ole32, oleaut32, bcrypt, odbc32, VirtDisk, VSSAPI).
Das Manifest fordert Administratorrechte an.

**Selbst kompilieren unter Windows (MSVC):**
```cmd
cmake -B build -A x64
cmake --build build --config Release
```
Braucht Visual Studio 2022 oder die Build Tools mit Windows SDK.

**Unter Linux (MinGW):** `./build-mingw.sh`

**GitHub Actions:** `.github/workflows/build.yml` baut beide Varianten und lädt die exe als Artefakt hoch,
sobald der Code in einem Repository liegt.

## Abweichungen vom Konzept v0.5

| Konzept | Umgesetzt | Warum |
|---|---|---|
| Inkremente als `inc-001.avhdx` | `inc-001.vhdx` | Differencing-Disks dürfen `.vhdx` heißen (so legt auch `New-VHD -Differencing` sie an). Doppelklick im Explorer, `Mount-DiskImage` und diskpart erkennen `.vhdx` sicher. |
| Katalog `catalog.json` mit Block-Hashes | `catalog.json` nur zur Anzeige, Hashes binär in `blockhashes.bin`, Laufdaten in `.ini` | Kein eigener JSON-Parser nötig; INI liest Windows selbst (`GetPrivateProfileString`). |
| Modul `service` (Windows-Dienst) | Aufgabe in der Windows-Aufgabenplanung (`schtasks`) | Standardweg für zeitgesteuerte Jobs, kein eigener Dienstcode. Benachrichtigung = Status in der Aufgabenplanung + Logdatei. |
| Verify nach jedem Lauf | Verify mit Reparatur aus dem Snapshot | Fängt ab, falls Windows beim Einbinden kurz auf die Volumes der Kette schreibt, bevor sie offline ist. |
| Recovery-Partition aus dem Snapshot | roh von der Platte (Volumes unter 1 GiB) | VSS braucht für kleine Volumes oft Schattenkopie-Speicher, der dort fehlt; ein Fehler würde das ganze Set abbrechen. Die Recovery-Partition ändert sich praktisch nie. |

## PoC-Testplan für den Test-PC

1. **BitLocker To Go:** USB-Platte an einem Pro-PC verschlüsseln, unter Home entsperren, `vhdxbackup init`
   zeigt „BitLocker aktiv“. Im Setup-Stick `manage-bde -unlock` testen.
2. **Vollbackup:** `vhdxbackup backup`, Log prüfen (Writer, Komponenten, Verify „alle Blöcke stimmen“).
3. **Datei-Restore:** `Mount-DiskImage -Access ReadOnly` auf `base.vhdx`, Dateien öffnen.
4. **Inkrement:** ein paar Dateien ändern, `vhdxbackup backup`, im Log „Inkrement“ und wenige geschriebene
   Blöcke. Achten auf „Verify: … Blöcke wichen ab“: Das zeigt, ob Windows beim Einbinden auf die Kette schreibt.
5. **Plattentausch:** nach `RESTORE.txt` auf eine zweite Platte zurückspielen und booten.
6. **SQL Server Express:** Datenbank anlegen, Backup, im Log `SqlServerWriter`-Komponenten; in SSMS sollte
   das Backup in der Historie stehen.

## Bekannte Grenzen

- Nur die OS-Platte; Datenbanken auf anderen Platten sind nicht im Backup (Warnung im Log).
- Jeder Lauf liest alle belegten Daten der OS-Platte, Verify liest zusätzlich die ganze Kette.
- Übergreifende (dynamische) Volumes werden nicht unterstützt.
- Die Kette trägt dieselbe GPT-Disk-ID wie die Quellplatte. Beim Einbinden auf demselben PC kann Windows
  im Ereignisprotokoll eine Warnung (Ereignis 158) schreiben; im PoC prüfen, ob das stört.
- Kein SMB-3-Ziel getestet (laut Konzept später).

## Lizenz

MIT-Lizenz, Copyright (c) 2026 Julian. Volltext in [`LICENSE`](LICENSE).
