#include "StdAfx.h"
#include "Trace.h"
#include "VhdToDisk.h"
#include "DiskPrep.h"

#include <stdio.h>

CVhdToDisk::CVhdToDisk(void)
{
	m_hVhdFile = NULL;
	m_hPhysicalDrive = NULL;

	ZeroMemory(&m_Foot, sizeof(VHD_FOOTER));
	ZeroMemory(&m_Dyn, sizeof(VHD_DYNAMIC));
}

CVhdToDisk::CVhdToDisk(LPWSTR sPath)
{
	m_hVhdFile = NULL;
	m_hPhysicalDrive = NULL;

	ZeroMemory(&m_Foot, sizeof(VHD_FOOTER));
	ZeroMemory(&m_Dyn, sizeof(VHD_DYNAMIC));

	if(!OpenVhdFile(sPath))
		return;

	if(!ReadFooter())
		return;

	if(!ReadDynHeader())
		return;
}


CVhdToDisk::~CVhdToDisk(void)
{
	if(m_hVhdFile)
		CloseVhdFile();

	if(m_hPhysicalDrive)
		ClosePhysicalDrive();
}

BOOL CVhdToDisk::OpenVhdFile(LPWSTR sPath)
{
	m_hVhdFile = CreateFile(sPath
		, GENERIC_READ
		, FILE_SHARE_READ
		, NULL
		, OPEN_EXISTING
		, FILE_ATTRIBUTE_NORMAL
		, NULL);

	if(m_hVhdFile == INVALID_HANDLE_VALUE) return FALSE;

	return TRUE;
}

BOOL CVhdToDisk::CloseVhdFile()
{
	BOOL bReturn = TRUE;

	if(m_hVhdFile != INVALID_HANDLE_VALUE)
		bReturn = CloseHandle(m_hVhdFile);

	m_hVhdFile = NULL;

	return bReturn;
}

BOOL CVhdToDisk::OpenPhysicalDrive(LPWSTR sDrive)
{
	m_hPhysicalDrive = CreateFile(sDrive
		, GENERIC_WRITE
		, 0
		, NULL
		, OPEN_EXISTING
		, FILE_FLAG_WRITE_THROUGH | FILE_FLAG_NO_BUFFERING
		, NULL);

	if(m_hPhysicalDrive == INVALID_HANDLE_VALUE) return FALSE;

	return TRUE;

}

BOOL CVhdToDisk::ClosePhysicalDrive()
{
	BOOL bReturn = TRUE;

	if(m_hPhysicalDrive != INVALID_HANDLE_VALUE)
		bReturn = CloseHandle(m_hPhysicalDrive);

	m_hPhysicalDrive = NULL;

	return bReturn;
}

BOOL CVhdToDisk::ReadFooter()
{
	BOOL bReturn = FALSE;
	DWORD dwByteRead = 0;

	LARGE_INTEGER filepointer;
	filepointer.QuadPart = 0;

	if(m_hVhdFile == INVALID_HANDLE_VALUE) return FALSE;

	// Dynamic VHDs keep a copy of the footer at the start of the file
	SetFilePointerEx(m_hVhdFile, filepointer, NULL, FILE_BEGIN);

	bReturn = ReadFile(m_hVhdFile, &m_Foot, sizeof(VHD_FOOTER), &dwByteRead, 0);

	if(bReturn)
		bReturn = (sizeof(VHD_FOOTER) == dwByteRead);

	return bReturn;
}

BOOL CVhdToDisk::ReadDynHeader()
{
	BOOL bReturn = FALSE;
	DWORD dwByteRead = 0;
	LARGE_INTEGER filepointer;
	filepointer.QuadPart = 512;

	if(m_hVhdFile == INVALID_HANDLE_VALUE) return FALSE;

	SetFilePointerEx(m_hVhdFile, filepointer, NULL, FILE_BEGIN);

	bReturn = ReadFile(m_hVhdFile, &m_Dyn, sizeof(VHD_DYNAMIC), &dwByteRead, 0);

	if(bReturn)
		bReturn = (sizeof(VHD_DYNAMIC) == dwByteRead);

	return bReturn;
}

static UINT32 LeU32(const BYTE* p)
{
	UINT32 n;
	memcpy(&n, p, sizeof(n));
	return n;
}

static UINT64 LeU64(const BYTE* p)
{
	UINT64 n;
	memcpy(&n, p, sizeof(n));
	return n;
}

static LPCWSTR MbrTypeName(BYTE nType)
{
	switch(nType)
	{
	case 0x01: return L"FAT12";
	case 0x04:
	case 0x06:
	case 0x0E: return L"FAT16";
	case 0x05: return L"Extended";
	case 0x07: return L"NTFS/exFAT";
	case 0x0B:
	case 0x0C: return L"FAT32";
	case 0x0F: return L"Extended LBA";
	case 0x27: return L"WinRE";
	case 0x82: return L"Linux swap";
	case 0x83: return L"Linux";
	case 0x85: return L"Linux extended";
	case 0x8E: return L"Linux LVM";
	case 0xEE: return L"GPT protective";
	case 0xEF: return L"EFI System";
	case 0xFD: return L"Linux RAID";
	}
	return NULL;
}

static const struct
{
	LPCWSTR sGuid;
	LPCWSTR sName;
} g_GptTypes[] =
{
	{ L"C12A7328-F81F-11D2-BA4B-00A0C93EC93B", L"EFI System" },
	{ L"21686148-6449-6E6F-744E-656564454649", L"BIOS boot" },
	{ L"0FC63DAF-8483-4772-8E79-3D69D8477DE4", L"Linux filesystem" },
	{ L"4F68BCE3-E8CD-4DB1-96E7-FBCAF984B709", L"Linux root (x86-64)" },
	{ L"BC13C2FF-59E6-4262-A352-B275FD6F7172", L"Linux extended boot" },
	{ L"0657FD6D-A4AB-43C4-84E5-0933C84B4F4F", L"Linux swap" },
	{ L"E6D6D379-F507-44C2-A23C-238F2A3DF928", L"Linux LVM" },
	{ L"A19D880F-05FC-4D3B-A006-743F0F84911E", L"Linux RAID" },
	{ L"EBD0A0A2-B9E5-4433-87C0-68B6B72699C7", L"Microsoft basic data" },
	{ L"E3C9E316-0B5C-4DB8-817D-F92DF00215AE", L"Microsoft reserved" },
	{ L"DE94BBA4-06D1-4D40-A16A-BFD50179D6AC", L"Windows recovery" },
};

UINT64 CVhdToDisk::GetDiskSize()
{
	return _byteswap_uint64(m_Foot.currentSize);
}

void CVhdToDisk::FormatSize(UINT64 nBytes, WCHAR* sSize, size_t nSizeChars)
{
	static LPCWSTR units[] = { L"B", L"KiB", L"MiB", L"GiB", L"TiB" };
	double dSize = (double)nBytes;
	int nUnit = 0;

	while(dSize >= 1024.0 && nUnit < ARRAYSIZE(units) - 1)
	{
		dSize /= 1024.0;
		nUnit++;
	}

	if(nUnit == 0)
		swprintf_s(sSize, nSizeChars, L"%llu %s", nBytes, units[0]);
	else
		swprintf_s(sSize, nSizeChars, L"%.1f %s", dSize, units[nUnit]);
}

BOOL CVhdToDisk::ReadBat(UINT32** ppBat, UINT32* pnBats)
{
	DWORD dwByteRead = 0;
	LARGE_INTEGER filepointer;
	UINT32 bats = _byteswap_ulong(m_Dyn.maxTableEntries);

	*ppBat = NULL;
	*pnBats = 0;

	if(m_hVhdFile == NULL || m_hVhdFile == INVALID_HANDLE_VALUE || bats == 0) return FALSE;

	UINT32* bat = new UINT32[bats];

	filepointer.QuadPart = _byteswap_uint64(m_Dyn.tableOffset);

	if(!SetFilePointerEx(m_hVhdFile, filepointer, NULL, FILE_BEGIN)
		|| !ReadFile(m_hVhdFile, bat, bats * sizeof(*bat), &dwByteRead, 0)
		|| dwByteRead != bats * sizeof(*bat))
	{
		TRACE("Failed to read the BAT (%u entries at %lld)\n", bats, filepointer.QuadPart);
		delete[] bat;
		return FALSE;
	}

	*ppBat = bat;
	*pnBats = bats;

	return TRUE;
}

BOOL CVhdToDisk::ReadVhdSector(const UINT32* pBat, UINT32 nBats, UINT64 nLba, BYTE* pSector)
{
	DWORD dwByteRead = 0;
	LARGE_INTEGER filepointer;
	UINT32 sectorsPerBlock = _byteswap_ulong(m_Dyn.blockSize) / 512;
	UINT32 blockBitmapSectorCount = (sectorsPerBlock / 8 + 511) / 512;

	if(sectorsPerBlock == 0) return FALSE;

	UINT64 nBlock = nLba / sectorsPerBlock;
	if(nBlock >= nBats) return FALSE;

	UINT32 nBlockSector = _byteswap_ulong(pBat[nBlock]);

	// Unallocated blocks read as zeroes
	if(nBlockSector == 0xFFFFFFFF)
	{
		ZeroMemory(pSector, 512);
		return TRUE;
	}

	filepointer.QuadPart = ((UINT64)nBlockSector + blockBitmapSectorCount + nLba % sectorsPerBlock) * 512LL;

	if(!SetFilePointerEx(m_hVhdFile, filepointer, NULL, FILE_BEGIN)) return FALSE;

	if(!ReadFile(m_hVhdFile, pSector, 512, &dwByteRead, 0)) return FALSE;

	return dwByteRead == 512;
}

void CVhdToDisk::AddMbrRow(VHD_PARTITION_ROW* pRow, int nNumber, LPCWSTR sKind, const BYTE* pEntry, UINT64 nStart)
{
	UINT64 nSectors = LeU32(pEntry + 12);
	LPCWSTR sTypeName = MbrTypeName(pEntry[4]);

	ZeroMemory(pRow, sizeof(*pRow));

	swprintf_s(pRow->sNumber, L"%d", nNumber);
	wcscpy_s(pRow->sKind, sKind);
	wcscpy_s(pRow->sBoot, pEntry[0] == 0x80 ? L"Y" : L"N");

	if(sTypeName)
		swprintf_s(pRow->sType, L"0x%02X %s", pEntry[4], sTypeName);
	else
		swprintf_s(pRow->sType, L"0x%02X", pEntry[4]);

	swprintf_s(pRow->sStart, L"%llu", nStart);
	swprintf_s(pRow->sSectors, L"%llu", nSectors);
	FormatSize(nSectors * 512, pRow->sSize, ARRAYSIZE(pRow->sSize));
}

BOOL CVhdToDisk::ParseLogicalPartitions(const UINT32* pBat, UINT32 nBats, UINT64 nExtStart
	, VHD_PARTITION_ROW* pRows, int nMaxRows, int* pnItem)
{
	BYTE ebr[512];
	UINT64 nEbr = nExtStart;
	int nNumber = 5;	// logical partitions are numbered from 5

	// Each EBR holds one logical partition (relative to that EBR) and a link to
	// the next EBR (relative to the start of the extended partition).
	for(int nGuard = 0; nGuard < VHD_MAX_PARTITIONS && *pnItem < nMaxRows; nGuard++)
	{
		if(!ReadVhdSector(pBat, nBats, nEbr, ebr)) return FALSE;
		if(ebr[510] != 0x55 || ebr[511] != 0xAA) break;

		const BYTE* pLogical = &ebr[0x1BE];
		const BYTE* pNext = &ebr[0x1CE];

		if(pLogical[4] != 0x00)
			AddMbrRow(&pRows[(*pnItem)++], nNumber++, L"Logical", pLogical, nEbr + LeU32(pLogical + 8));

		if(pNext[4] == 0x00 || LeU32(pNext + 8) == 0) break;

		nEbr = nExtStart + LeU32(pNext + 8);
	}

	return TRUE;
}

BOOL CVhdToDisk::ParseGpt(const UINT32* pBat, UINT32 nBats
	, VHD_PARTITION_ROW* pRows, int nMaxRows, int* pnItem)
{
	static const BYTE zeroGuid[16] = {0};
	BYTE header[512];
	BYTE sector[512];

	if(!ReadVhdSector(pBat, nBats, 1, header)) return FALSE;
	if(memcmp(header, "EFI PART", 8) != 0) return FALSE;

	UINT64 nEntryLba = LeU64(header + 72);
	UINT32 nEntries = LeU32(header + 80);
	UINT32 nEntrySize = LeU32(header + 84);

	if(nEntrySize < 128 || nEntrySize > 512 || 512 % nEntrySize != 0) return FALSE;
	if(nEntries > 4096) nEntries = 4096;

	UINT32 nPerSector = 512 / nEntrySize;
	UINT64 nLoadedLba = (UINT64)-1;

	for(UINT32 i = 0; i < nEntries && *pnItem < nMaxRows; i++)
	{
		UINT64 nLba = nEntryLba + i / nPerSector;
		if(nLba != nLoadedLba)
		{
			if(!ReadVhdSector(pBat, nBats, nLba, sector)) return FALSE;
			nLoadedLba = nLba;
		}

		const BYTE* pEntry = &sector[(i % nPerSector) * nEntrySize];
		if(memcmp(pEntry, zeroGuid, sizeof(zeroGuid)) == 0) continue;

		GUID type;
		memcpy(&type, pEntry, sizeof(type));

		WCHAR sGuid[40];
		swprintf_s(sGuid, L"%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X"
			, type.Data1, type.Data2, type.Data3
			, type.Data4[0], type.Data4[1], type.Data4[2], type.Data4[3]
			, type.Data4[4], type.Data4[5], type.Data4[6], type.Data4[7]);

		UINT64 nFirst = LeU64(pEntry + 32);
		UINT64 nLast = LeU64(pEntry + 40);
		UINT64 nSectors = nLast >= nFirst ? nLast - nFirst + 1 : 0;

		VHD_PARTITION_ROW* pRow = &pRows[(*pnItem)++];
		ZeroMemory(pRow, sizeof(*pRow));

		swprintf_s(pRow->sNumber, L"%u", i + 1);
		wcscpy_s(pRow->sKind, L"GPT");

		// Attribute bit 2: legacy BIOS bootable
		wcscpy_s(pRow->sBoot, (LeU64(pEntry + 48) & 0x4) ? L"Y" : L"N");

		wcscpy_s(pRow->sType, sGuid);
		for(int t = 0; t < ARRAYSIZE(g_GptTypes); t++)
		{
			if(_wcsicmp(sGuid, g_GptTypes[t].sGuid) == 0)
			{
				wcscpy_s(pRow->sType, g_GptTypes[t].sName);
				break;
			}
		}

		swprintf_s(pRow->sStart, L"%llu", nFirst);
		swprintf_s(pRow->sSectors, L"%llu", nSectors);
		FormatSize(nSectors * 512, pRow->sSize, ARRAYSIZE(pRow->sSize));

		// Name is 36 UTF-16LE characters, not necessarily terminated
		memcpy(pRow->sName, pEntry + 56, 36 * sizeof(WCHAR));
		pRow->sName[36] = L'\0';
	}

	return TRUE;
}

BOOL CVhdToDisk::ParsePartitions(VHD_PARTITION_ROW* pRows, int nMaxRows, int* pnRows, LPCWSTR* psScheme)
{
	BOOL bReturn = FALSE;
	UINT32* bat = NULL;
	UINT32 bats = 0;
	BYTE mbr[512];
	int nItem = 0;

	if(pnRows) *pnRows = 0;
	if(psScheme) *psScheme = L"none";

	if(!ReadBat(&bat, &bats)) goto clean;
	if(!ReadVhdSector(bat, bats, 0, mbr)) goto clean;

	bReturn = TRUE;

	if(mbr[510] != 0x55 || mbr[511] != 0xAA) goto clean;

	// A protective (or hybrid) MBR entry means the real table is GPT
	for(int i = 0; i < 4; i++)
	{
		if(mbr[0x1BE + i * 16 + 4] == 0xEE)
		{
			if(psScheme) *psScheme = L"GPT";
			bReturn = ParseGpt(bat, bats, pRows, nMaxRows, &nItem);
			goto clean;
		}
	}

	if(psScheme) *psScheme = L"MBR";

	for(int i = 0; i < 4 && nItem < nMaxRows; i++)
	{
		const BYTE* pEntry = &mbr[0x1BE + i * 16];
		BYTE nType = pEntry[4];

		if(nType == 0x00) continue;

		BOOL bExtended = (nType == 0x05 || nType == 0x0F || nType == 0x85);

		AddMbrRow(&pRows[nItem++], i + 1, bExtended ? L"Extended" : L"Primary", pEntry, LeU32(pEntry + 8));

		if(bExtended)
			bReturn = ParseLogicalPartitions(bat, bats, LeU32(pEntry + 8), pRows, nMaxRows, &nItem) && bReturn;
	}

clean:

	if(pnRows) *pnRows = nItem;
	if(bat) delete[] bat;

	return bReturn;
}

BOOL CVhdToDisk::Dump(IVhdDumpProgress* pProgress)
{
	BOOL bReturn = FALSE;
	DWORD dwByteRead = 0;
	UINT32 emptySectors = 0;
	UINT32 usedSectors = 0;
	UINT32 usedZeroes = 0;
	
	LARGE_INTEGER filepointer;
	UINT32 blockBitmapSectorCount = (_byteswap_ulong(m_Dyn.blockSize) / 512 / 8 + 511) / 512;
	UINT32 sectorsPerBlock = _byteswap_ulong(m_Dyn.blockSize) / 512;
	UINT32 bats = _byteswap_ulong(m_Dyn.maxTableEntries);
	
	filepointer.QuadPart = _byteswap_uint64(m_Dyn.tableOffset);

	UINT8* bitmap = new UINT8[blockBitmapSectorCount * 512];
	if(!bitmap) goto clean;

	UINT32* bat = new UINT32[bats * 4];
	if(!bat) goto clean;
	
	char* pBuff = new char[512 * sectorsPerBlock];
	if(!pBuff) goto clean;

	
	if(!SetFilePointerEx(m_hVhdFile, filepointer, NULL, FILE_BEGIN))
	{
		TRACE("Failed to SetFilePointer(0x%08X, %lld, 0, FILE_BEGIN)\n", m_hVhdFile, filepointer.QuadPart);
		goto clean;
	}
	
	if(!ReadFile(m_hVhdFile, bat, bats * sizeof(*bat), &dwByteRead, 0))
	{
		TRACE("Failed to ReadFile(0x%08X, 0x%08X, %d,...) with error 0x%08X\n", m_hVhdFile, bat, bats * 4, GetLastError());
		
		goto clean;
	}

	pProgress->OnProgress(0, bats);

	for(UINT32 b = 0; b < bats; b++)
	{
		if((b + 1) % 10 == 0)
			pProgress->OnProgress(b + 1, bats);

		
		if(_byteswap_ulong(bat[b]) == 0xFFFFFFFF)
		{
			emptySectors += sectorsPerBlock;
			continue;
		}

		UINT64 bo = _byteswap_ulong(bat[b]) * 512LL;

		filepointer.QuadPart = bo;

		bReturn = SetFilePointerEx(m_hVhdFile, filepointer, NULL, FILE_BEGIN);
		if(!bReturn) goto clean;

		bReturn = ReadFile(m_hVhdFile, bitmap, 512 * blockBitmapSectorCount, &dwByteRead, 0);
		if(!bReturn) goto clean;

		UINT64 opos = 0xffffffffffffffffLL;

		filepointer.QuadPart = bo + 512 * blockBitmapSectorCount;

		bReturn = SetFilePointerEx(m_hVhdFile, filepointer, NULL, FILE_BEGIN);
		if(!bReturn) goto clean;

		bReturn = ReadFile(m_hVhdFile, pBuff, 512 * sectorsPerBlock, &dwByteRead, 0);
		if(!bReturn) goto clean;

		filepointer.QuadPart = (b * sectorsPerBlock ) * 512LL;

		TRACE("Writing at %lld\n", filepointer.QuadPart);

		SetFilePointerEx(m_hPhysicalDrive, filepointer, NULL, FILE_BEGIN);

		bReturn = WriteFile(m_hPhysicalDrive, pBuff, 512 * sectorsPerBlock, &dwByteRead, 0);
		if(!bReturn)
		{
			pProgress->OnDriveWriteError(
						L"Can't write on physical drive. It's probably mounted.\n"
								L"You need to put it off line before to be able to write on it.\n"
								L"Microsoft choose this way for security reason...\n"
								L"It's nice for us and avoid to overwrite a non wanted drive.");

			goto clean;
		}

/*
		for (s = 0; s < sectorsPerBlock; s++)
		{
			int empty = 1;
			for (k = 0; k < 512; k++)
			{
				if (pBuff[k + (512 * s)])
				{
					empty = 0;
					break;
				}
			}

			if ((bitmap[s/8] & (1<<(7-s%8))) == 0) 
			{
				emptySectors++;
				if (!empty) 
					TRACE("block %d, sector %d should be empty and isn't.\n", b, s);

			}
			else
			{
				usedSectors++;
				if (empty)
					usedZeroes++;
				else
				{
					filepointer.QuadPart = (b * sectorsPerBlock + s) * 512LL;
					TRACE("Writing at %lld\n", filepointer.QuadPart);
					
					SetFilePointerEx(m_hPhysicalDrive, filepointer, NULL, FILE_BEGIN);

					WriteFile(m_hPhysicalDrive, pBuff + (s * 512), 512, &dwByteRead, 0);
					
				}
			}
		}
		*/
	}

	pProgress->OnProgress(bats, bats);

clean:

	if(bitmap) delete[] bitmap;
	if(bat) delete[] bat;
	if(pBuff) delete[] pBuff;

	return bReturn;
}

BOOL CVhdToDisk::DumpVhdToDisk(const LPWSTR sPath, const LPWSTR sDrive, IVhdDumpProgress* pProgress, BOOL bPrepareDisk)
{
	BOOL bReturn = FALSE;

	if(m_hVhdFile == NULL)
	{
		bReturn = OpenVhdFile(sPath);
		if(!bReturn)
		{
			TRACE("Failed to open %s\n", sPath);
			goto exit;
		}
	}
	
	bReturn = ReadFooter();
	if(!bReturn)
	{
		TRACE("Failed to read footer\n");
		goto clean;
	}

	bReturn = ReadDynHeader();
	if(!bReturn)
	{
		TRACE("Failed to read dynamic header\n");
		goto clean;
	}

	if(bPrepareDisk)
	{
		bReturn = PrepareTargetDisk(sDrive, sPath, GetDiskSize(), pProgress);
		if(!bReturn)
		{
			TRACE("Failed to prepare physical drive: %S\n", sDrive);
			goto clean;
		}
	}

	bReturn = OpenPhysicalDrive(sDrive);
	if(!bReturn)
	{
		WCHAR sMessage[256] = {0};
		swprintf_s(sMessage, L"Can't open %s for writing (error %u).", sDrive, GetLastError());
		pProgress->OnDriveWriteError(sMessage);
		goto clean;
	}

	bReturn = Dump(pProgress);
	if(!bReturn)
	{
		TRACE("Failed to Dump\n");
		goto clean;
	}

clean:

	CloseVhdFile();
	ClosePhysicalDrive();

exit:
	return bReturn;
}