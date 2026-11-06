#pragma once

#include "VhdDumpProgress.h"

// Gets the target physical drive ready to be written raw, the same way
//   diskpart: SELECT DISK n / CLEAN / OFFLINE DISK
// would, but only doing the steps that are needed:
//   - refuses if the VHD file lives on the target disk, or the disk is too small
//   - locks and dismounts the disk's volumes
//   - deletes the partition table (MBR, or both GPT copies) if there is one
//   - takes the disk offline (until reboot) and clears its read-only flag
// Progress and errors are reported through pProgress.
BOOL PrepareTargetDisk(LPCWSTR sDrive, LPCWSTR sVhdPath, UINT64 nVhdBytes, IVhdDumpProgress* pProgress);
