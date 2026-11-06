#include "StdAfx.h"
#include "DiskPrep.h"
#include "VhdToDisk.h"

#include <stdio.h>
#include <stdarg.h>
#include <winioctl.h>

#define MAX_LOCKED_VOLUMES 64

static BOOL Fail(IVhdDumpProgress* pProgress, DWORD dwError, LPCWSTR sFormat, ...)
{
	WCHAR sMessage[512] = {0};
	va_list args;

	va_start(args, sFormat);
	vswprintf_s(sMessage, sFormat, args);
	va_end(args);

	if(dwError != ERROR_SUCCESS)
	{
		WCHAR sError[256] = {0};

		FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS
			, NULL, dwError, 0, sError, ARRAYSIZE(sError), NULL);

		// FormatMessage ends the text with CRLF
		size_t nLen = wcslen(sError);
		while(nLen > 0 && (sError[nLen - 1] == L'\r' || sError[nLen - 1] == L'\n'))
			sError[--nLen] = L'\0';

		size_t nUsed = wcslen(sMessage);
		swprintf_s(sMessage + nUsed, ARRAYSIZE(sMessage) - nUsed, L"\nError %u: %s", dwError, sError);
	}

	pProgress->OnDriveWriteError(sMessage);

	return FALSE;
}

// sVolume is a volume GUID path as returned by FindFirstVolume, with a trailing backslash
static HANDLE OpenVolume(LPCWSTR sVolume, DWORD dwAccess)
{
	WCHAR sDevice[MAX_PATH] = {0};

	wcscpy_s(sDevice, sVolume);

	// Without the trailing backslash it opens the volume device, not its root directory
	size_t nLen = wcslen(sDevice);
	if(nLen > 0 && sDevice[nLen - 1] == L'\\')
		sDevice[nLen - 1] = L'\0';

	return CreateFile(sDevice
		, dwAccess
		, FILE_SHARE_READ | FILE_SHARE_WRITE
		, NULL
		, OPEN_EXISTING
		, 0
		, NULL);
}

static BOOL IsVolumeOnDisk(HANDLE hVolume, DWORD nDisk)
{
	// Room for 32 extents; DWORD64 keeps the buffer aligned for LARGE_INTEGER
	DWORD64 buffer[(sizeof(VOLUME_DISK_EXTENTS) + 31 * sizeof(DISK_EXTENT)) / sizeof(DWORD64) + 1];
	VOLUME_DISK_EXTENTS* pExtents = (VOLUME_DISK_EXTENTS*)buffer;
	DWORD dwBytes = 0;

	if(!DeviceIoControl(hVolume, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS
		, NULL, 0, buffer, sizeof(buffer), &dwBytes, NULL))
		return FALSE;

	for(DWORD i = 0; i < pExtents->NumberOfDiskExtents; i++)
	{
		if(pExtents->Extents[i].DiskNumber == nDisk)
			return TRUE;
	}

	return FALSE;
}

static BOOL IsFileOnDisk(LPCWSTR sPath, DWORD nDisk)
{
	WCHAR sMountPoint[MAX_PATH] = {0};
	WCHAR sVolume[MAX_PATH] = {0};
	BOOL bReturn = FALSE;

	if(!GetVolumePathNameW(sPath, sMountPoint, ARRAYSIZE(sMountPoint))) return FALSE;

	// Fails for network shares, which can't be on the target disk anyway
	if(!GetVolumeNameForVolumeMountPointW(sMountPoint, sVolume, ARRAYSIZE(sVolume))) return FALSE;

	HANDLE hVolume = OpenVolume(sVolume, 0);
	if(hVolume == INVALID_HANDLE_VALUE) return FALSE;

	bReturn = IsVolumeOnDisk(hVolume, nDisk);

	CloseHandle(hVolume);

	return bReturn;
}

static HANDLE OpenDisk(LPCWSTR sDrive)
{
	return CreateFile(sDrive
		, GENERIC_READ | GENERIC_WRITE
		, FILE_SHARE_READ | FILE_SHARE_WRITE
		, NULL
		, OPEN_EXISTING
		, 0
		, NULL);
}

// Sets or clears the offline attribute, clearing the attributes in
// dwClearMask at the same time. Not persisted: a reboot restores the
// disk's normal state. Leaves GetLastError set on failure.
static BOOL SetOfflineAttribute(HANDLE hDisk, BOOL bOffline, DWORDLONG dwClearMask)
{
	SET_DISK_ATTRIBUTES setAttributes;
	DWORD dwBytes = 0;

	ZeroMemory(&setAttributes, sizeof(setAttributes));
	setAttributes.Version = sizeof(setAttributes);
	setAttributes.Persist = FALSE;
	setAttributes.Attributes = bOffline ? DISK_ATTRIBUTE_OFFLINE : 0;
	setAttributes.AttributesMask = DISK_ATTRIBUTE_OFFLINE | dwClearMask;

	// Right after a clean or a dump the disk can still be busy (the reason the
	// diskpart scripts offline in a separate run), so retry for a few seconds
	for(int nTry = 1; ; nTry++)
	{
		if(DeviceIoControl(hDisk, IOCTL_DISK_SET_DISK_ATTRIBUTES
			, &setAttributes, sizeof(setAttributes), NULL, 0, &dwBytes, NULL))
			return TRUE;

		if(nTry == 10)
			return FALSE;

		Sleep(500);
	}
}

static BOOL QueryDisk(HANDLE hDisk, DWORD* pnDisk, BOOL* pbOffline)
{
	STORAGE_DEVICE_NUMBER deviceNumber;
	GET_DISK_ATTRIBUTES attributes;
	DWORD dwBytes = 0;

	if(!DeviceIoControl(hDisk, IOCTL_STORAGE_GET_DEVICE_NUMBER
		, NULL, 0, &deviceNumber, sizeof(deviceNumber), &dwBytes, NULL))
		return FALSE;

	ZeroMemory(&attributes, sizeof(attributes));
	if(!DeviceIoControl(hDisk, IOCTL_DISK_GET_DISK_ATTRIBUTES
		, NULL, 0, &attributes, sizeof(attributes), &dwBytes, NULL))
		return FALSE;

	*pnDisk = deviceNumber.DeviceNumber;
	*pbOffline = (attributes.Attributes & DISK_ATTRIBUTE_OFFLINE) != 0;

	return TRUE;
}

BOOL GetDiskOffline(LPCWSTR sDrive, BOOL* pbOffline)
{
	DWORD nDisk = 0;
	HANDLE hDisk = OpenDisk(sDrive);

	if(hDisk == INVALID_HANDLE_VALUE) return FALSE;

	BOOL bReturn = QueryDisk(hDisk, &nDisk, pbOffline);

	CloseHandle(hDisk);

	return bReturn;
}

BOOL SetDiskOffline(LPCWSTR sDrive, BOOL bOffline, IVhdDumpProgress* pProgress)
{
	BOOL bReturn = FALSE;
	BOOL bIsOffline = FALSE;
	DWORD nDisk = 0;
	DWORD dwBytes = 0;
	WCHAR sStatus[128] = {0};
	LPCWSTR sState = bOffline ? L"offline" : L"online";

	HANDLE hDisk = OpenDisk(sDrive);

	if(hDisk == INVALID_HANDLE_VALUE)
		return Fail(pProgress, GetLastError(), L"Can't open %s to set it %s.", sDrive, sState);

	if(!QueryDisk(hDisk, &nDisk, &bIsOffline))
	{
		Fail(pProgress, GetLastError(), L"Can't read the state of %s.", sDrive);
		goto clean;
	}

	if(bIsOffline == bOffline)
	{
		swprintf_s(sStatus, L"Disk %u is %s.", nDisk, sState);
		pProgress->OnStatus(sStatus);
		bReturn = TRUE;
		goto clean;
	}

	swprintf_s(sStatus, bOffline ? L"Taking disk %u offline..." : L"Bringing disk %u online...", nDisk);
	pProgress->OnStatus(sStatus);

	if(!SetOfflineAttribute(hDisk, bOffline, 0))
	{
		Fail(pProgress, GetLastError(), L"Can't set disk %u %s.", nDisk, sState);
		goto clean;
	}

	// Have Windows read the newly written partition table so its volumes mount
	if(!bOffline)
		DeviceIoControl(hDisk, IOCTL_DISK_UPDATE_PROPERTIES, NULL, 0, NULL, 0, &dwBytes, NULL);

	swprintf_s(sStatus, L"Disk %u is %s.", nDisk, sState);
	pProgress->OnStatus(sStatus);

	bReturn = TRUE;

clean:

	CloseHandle(hDisk);

	return bReturn;
}

BOOL PrepareTargetDisk(LPCWSTR sDrive, LPCWSTR sVhdPath, UINT64 nVhdBytes, IVhdDumpProgress* pProgress)
{
	BOOL bReturn = FALSE;
	BOOL bOffline = FALSE;
	BOOL bReadOnly = FALSE;
	BOOL bHasLayout = TRUE;
	DWORD dwBytes = 0;
	DWORD nDisk = 0;
	HANDLE hDisk = INVALID_HANDLE_VALUE;
	HANDLE hFind = INVALID_HANDLE_VALUE;
	HANDLE hLocked[MAX_LOCKED_VOLUMES];
	int nLocked = 0;
	BYTE* pLayout = NULL;
	WCHAR sStatus[256] = {0};
	WCHAR sVolume[MAX_PATH] = {0};
	STORAGE_DEVICE_NUMBER deviceNumber;
	GET_LENGTH_INFORMATION length;
	GET_DISK_ATTRIBUTES attributes;

	hDisk = CreateFile(sDrive
		, GENERIC_READ | GENERIC_WRITE
		, FILE_SHARE_READ | FILE_SHARE_WRITE
		, NULL
		, OPEN_EXISTING
		, 0
		, NULL);

	if(hDisk == INVALID_HANDLE_VALUE)
	{
		Fail(pProgress, GetLastError(), L"Can't open %s.", sDrive);
		goto clean;
	}

	if(!DeviceIoControl(hDisk, IOCTL_STORAGE_GET_DEVICE_NUMBER
		, NULL, 0, &deviceNumber, sizeof(deviceNumber), &dwBytes, NULL))
	{
		Fail(pProgress, GetLastError(), L"Can't get the disk number of %s.", sDrive);
		goto clean;
	}

	nDisk = deviceNumber.DeviceNumber;

	if(sVhdPath && IsFileOnDisk(sVhdPath, nDisk))
	{
		Fail(pProgress, ERROR_SUCCESS
			, L"The VHD file is stored on disk %u, which is the target disk.\n"
			  L"Copy the VHD to another disk first.", nDisk);
		goto clean;
	}

	if(!DeviceIoControl(hDisk, IOCTL_DISK_GET_LENGTH_INFO
		, NULL, 0, &length, sizeof(length), &dwBytes, NULL))
	{
		Fail(pProgress, GetLastError(), L"Can't get the size of disk %u.", nDisk);
		goto clean;
	}

	if((UINT64)length.Length.QuadPart < nVhdBytes)
	{
		WCHAR sDiskSize[16] = {0};
		WCHAR sVhdSize[16] = {0};

		CVhdToDisk::FormatSize(length.Length.QuadPart, sDiskSize, ARRAYSIZE(sDiskSize));
		CVhdToDisk::FormatSize(nVhdBytes, sVhdSize, ARRAYSIZE(sVhdSize));

		Fail(pProgress, ERROR_SUCCESS
			, L"Disk %u is too small: it holds %s (%llu bytes) but the VHD is %s (%llu bytes)."
			, nDisk, sDiskSize, length.Length.QuadPart, sVhdSize, nVhdBytes);
		goto clean;
	}

	ZeroMemory(&attributes, sizeof(attributes));
	if(DeviceIoControl(hDisk, IOCTL_DISK_GET_DISK_ATTRIBUTES
		, NULL, 0, &attributes, sizeof(attributes), &dwBytes, NULL))
	{
		bOffline = (attributes.Attributes & DISK_ATTRIBUTE_OFFLINE) != 0;
		bReadOnly = (attributes.Attributes & DISK_ATTRIBUTE_READ_ONLY) != 0;
	}

	// If the layout can't be read, clean anyway
	pLayout = new BYTE[64 * 1024];
	if(DeviceIoControl(hDisk, IOCTL_DISK_GET_DRIVE_LAYOUT_EX
		, NULL, 0, pLayout, 64 * 1024, &dwBytes, NULL))
	{
		bHasLayout = ((DRIVE_LAYOUT_INFORMATION_EX*)pLayout)->PartitionStyle != PARTITION_STYLE_RAW;
	}

	swprintf_s(sStatus, L"Disk %u: %s, %s, %s."
		, nDisk
		, bOffline ? L"offline" : L"online"
		, bReadOnly ? L"read-only" : L"writable"
		, bHasLayout ? L"has a partition table" : L"already clean");
	pProgress->OnStatus(sStatus);

	// An offline disk has no mounted volumes
	if(!bOffline)
	{
		hFind = FindFirstVolumeW(sVolume, ARRAYSIZE(sVolume));

		while(hFind != INVALID_HANDLE_VALUE)
		{
			HANDLE hVolume = OpenVolume(sVolume, GENERIC_READ | GENERIC_WRITE);

			if(hVolume != INVALID_HANDLE_VALUE && IsVolumeOnDisk(hVolume, nDisk))
			{
				swprintf_s(sStatus, L"Dismounting %s", sVolume);
				pProgress->OnStatus(sStatus);

				// Lock fails if files are open; dismounting still forces them closed
				DeviceIoControl(hVolume, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &dwBytes, NULL);

				if(!DeviceIoControl(hVolume, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &dwBytes, NULL))
				{
					Fail(pProgress, GetLastError(), L"Can't dismount %s on disk %u.", sVolume, nDisk);
					CloseHandle(hVolume);
					goto clean;
				}

				// Keep it locked so it doesn't remount before the clean
				if(nLocked < MAX_LOCKED_VOLUMES)
					hLocked[nLocked++] = hVolume;
				else
					CloseHandle(hVolume);
			}
			else if(hVolume != INVALID_HANDLE_VALUE)
			{
				CloseHandle(hVolume);
			}

			if(!FindNextVolumeW(hFind, sVolume, ARRAYSIZE(sVolume)))
				break;
		}
	}

	if(bHasLayout)
	{
		swprintf_s(sStatus, L"Cleaning disk %u (deleting its partition table)...", nDisk);
		pProgress->OnStatus(sStatus);

		if(!DeviceIoControl(hDisk, IOCTL_DISK_DELETE_DRIVE_LAYOUT, NULL, 0, NULL, 0, &dwBytes, NULL))
		{
			Fail(pProgress, GetLastError(), L"Can't clean disk %u.", nDisk);
			goto clean;
		}

		DeviceIoControl(hDisk, IOCTL_DISK_UPDATE_PROPERTIES, NULL, 0, NULL, 0, &dwBytes, NULL);
	}

	// The volumes are gone with the partition table
	while(nLocked > 0)
		CloseHandle(hLocked[--nLocked]);

	if(!bOffline || bReadOnly)
	{
		swprintf_s(sStatus, L"Taking disk %u offline...", nDisk);
		pProgress->OnStatus(sStatus);

		if(!SetOfflineAttribute(hDisk, TRUE, DISK_ATTRIBUTE_READ_ONLY))
		{
			Fail(pProgress, GetLastError(), L"Can't take disk %u offline.", nDisk);
			goto clean;
		}
	}

	swprintf_s(sStatus, L"Disk %u is clean and offline.", nDisk);
	pProgress->OnStatus(sStatus);

	bReturn = TRUE;

clean:

	while(nLocked > 0)
		CloseHandle(hLocked[--nLocked]);

	if(hFind != INVALID_HANDLE_VALUE)
		FindVolumeClose(hFind);

	if(pLayout) delete[] pLayout;

	if(hDisk != INVALID_HANDLE_VALUE)
		CloseHandle(hDisk);

	return bReturn;
}
