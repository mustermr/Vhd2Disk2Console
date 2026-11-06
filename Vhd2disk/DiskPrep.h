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

// Whether the disk is currently offline. FALSE if it can't be queried
// (no such disk, or not running as administrator).
BOOL GetDiskOffline(LPCWSTR sDrive, BOOL* pbOffline);

// Takes the disk offline or brings it online (until reboot), reporting through
// pProgress. Bringing it online rescans the partition table so volumes mount.
BOOL SetDiskOffline(LPCWSTR sDrive, BOOL bOffline, IVhdDumpProgress* pProgress);

// What to do with the disk's online/offline state once the dump is over
enum DISK_END_STATE
{
	DISK_END_AS_FOUND,	// back to how it was before the dump started
	DISK_END_ONLINE,
	DISK_END_OFFLINE,
};
