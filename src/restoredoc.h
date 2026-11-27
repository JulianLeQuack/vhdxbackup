// Erzeugt RESTORE.txt und CreatePartitions-UEFI.txt auf dem Ziel (Restore nur mit Windows-Setup-Stick).
#pragma once

#include "disk.h"

namespace vb {

void WriteRestoreDocs(const std::wstring& machineDir, const DiskInfo& disk, const std::wstring& newestLinkPath);

}  // namespace vb
