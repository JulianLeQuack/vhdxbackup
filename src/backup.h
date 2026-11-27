// Ablauf: Backup (voll/inkrementell), Verify, Liste, Einrichten, Zeitplan.
#pragma once

#include "config.h"

namespace vb {

int RunBackup(const Config& cfg, bool forceFull);
int RunVerify(const Config& cfg);
int RunList(const Config& cfg);
int RunInit(const std::wstring& configPath, const std::wstring& targetPath);
int RunSchedule(const std::wstring& configPath, const std::wstring& time);
int RunUnschedule();
int RunInfo();

// Wird von Strg+C gesetzt; lange Schleifen prüfen es.
void RequestAbort();

}  // namespace vb
