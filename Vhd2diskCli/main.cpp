// main.cpp: console entry point for the x64 command-line VHD-to-disk dumper.
//

#include "stdafx.h"
#include "../Vhd2disk/VhdDumpProgress.h"
#include "../Vhd2disk/VhdToDisk.h"

// Block progress is one line redrawn in place with \r; status and error
// messages first finish that line so they don't overwrite it.
class CConsoleProgress : public IVhdDumpProgress
{
	BOOL m_bProgressLine;
	ULONGLONG m_nLastDraw;
	UINT32 m_nLastCurrent;

public:
	CConsoleProgress() : m_bProgressLine(FALSE), m_nLastDraw(0), m_nLastCurrent(0) {}

	void EndProgressLine()
	{
		if(!m_bProgressLine) return;

		wprintf(L"\n");
		m_bProgressLine = FALSE;
	}

	void OnStatus(LPCWSTR sStatus)
	{
		EndProgressLine();
		wprintf(L"%s\n", sStatus);
	}

	void OnProgress(UINT32 nCurrent, UINT32 nTotal)
	{
		if(nTotal == 0) return;

		// Redraw at most 5 times a second, but always show the first and last update
		ULONGLONG nNow = GetTickCount64();
		if(m_bProgressLine && (nCurrent == m_nLastCurrent || (nCurrent < nTotal && nNow - m_nLastDraw < 200))) return;
		m_nLastDraw = nNow;
		m_nLastCurrent = nCurrent;

		wprintf(L"\rWriting block %u/%u (%u%%)", nCurrent, nTotal, (UINT32)((UINT64)nCurrent * 100 / nTotal));
		fflush(stdout);
		m_bProgressLine = TRUE;
	}

	void OnDriveWriteError(LPCWSTR sMessage)
	{
		EndProgressLine();
		fflush(stdout);
		fwprintf(stderr, L"%s\n", sMessage);
	}
};

void PrintUsage()
{
	wprintf(L"Vhd2diskCli - dump a VHD file onto a physical drive\n\n");
	wprintf(L"Usage:\n");
	wprintf(L"  Vhd2diskCli.exe -l | --list\n");
	wprintf(L"      List available physical drives.\n\n");
	wprintf(L"  Vhd2diskCli.exe <vhd-path> <N | PhysicalDriveN | \\\\.\\PhysicalDriveN> [-y | --yes] [--no-prepare]\n");
	wprintf(L"      Dump the VHD onto the given physical drive. Before writing, the drive's\n");
	wprintf(L"      volumes are dismounted, its partition table is deleted and it is taken\n");
	wprintf(L"      offline until reboot (like diskpart CLEAN and OFFLINE DISK).\n");
	wprintf(L"      -y, --yes       Skip the confirmation prompt.\n");
	wprintf(L"      --no-prepare    Don't clean or offline the drive; write to it as it is.\n");
}

void ListPhysicalDrives()
{
	WCHAR sPhysicalDrive[64] = {0};
	int nFound = 0;

	for(int i = 0; i < 99; i++)
	{
		wsprintf(sPhysicalDrive, L"\\\\.\\PhysicalDrive%d", i);

		HANDLE hFile = CreateFile(sPhysicalDrive
			, GENERIC_WRITE
			, 0
			, NULL
			, OPEN_EXISTING
			, FILE_FLAG_WRITE_THROUGH | FILE_FLAG_NO_BUFFERING
			, NULL);

		if(hFile != INVALID_HANDLE_VALUE)
		{
			CloseHandle(hFile);
			wprintf(L"%s\n", sPhysicalDrive);
			nFound++;
		}
	}

	if(nFound == 0)
		wprintf(L"No writable physical drives found.\n");
}

BOOL NormalizeDrivePath(LPCWSTR sArg, WCHAR* sDrive, size_t nDriveChars)
{
	BOOL bAllDigits = TRUE;

	for(const WCHAR* p = sArg; *p; p++)
	{
		if(*p < L'0' || *p > L'9')
		{
			bAllDigits = FALSE;
			break;
		}
	}

	if(bAllDigits)
		swprintf_s(sDrive, nDriveChars, L"\\\\.\\PhysicalDrive%s", sArg);
	else
		wcscpy_s(sDrive, nDriveChars, sArg);

	return TRUE;
}

void PrintPartitionTable(CVhdToDisk& vhd)
{
	static VHD_PARTITION_ROW rows[VHD_MAX_PARTITIONS];
	int nRows = 0;
	LPCWSTR sScheme = NULL;
	WCHAR sDiskSize[16] = {0};

	CVhdToDisk::FormatSize(vhd.GetDiskSize(), sDiskSize, ARRAYSIZE(sDiskSize));

	if(!vhd.ParsePartitions(rows, ARRAYSIZE(rows), &nRows, &sScheme))
	{
		wprintf(L"Could not read the partition table from the VHD.\n");
		return;
	}

	wprintf(L"\nVHD disk size: %s (%llu sectors), partition table: %s\n"
		, sDiskSize, vhd.GetDiskSize() / 512, sScheme);

	if(nRows == 0)
	{
		wprintf(L"No partitions found in the VHD.\n");
		return;
	}

	wprintf(L"\nPartitions found in the VHD:\n");
	wprintf(L"%-3s %-9s %-5s %-22s %-12s %-12s %-10s %s\n"
		, L"#", L"Kind", L"Boot", L"Type", L"Start LBA", L"Sectors", L"Size", L"Name");
	for(int i = 0; i < nRows; i++)
	{
		wprintf(L"%-3s %-9s %-5s %-22s %-12s %-12s %-10s %s\n"
			, rows[i].sNumber, rows[i].sKind, rows[i].sBoot, rows[i].sType
			, rows[i].sStart, rows[i].sSectors, rows[i].sSize, rows[i].sName);
	}
}

int wmain(int argc, wchar_t* argv[])
{
	BOOL bAssumeYes = FALSE;
	BOOL bPrepareDisk = TRUE;
	LPCWSTR sVhdPath = NULL;
	LPCWSTR sDriveArg = NULL;

	for(int i = 1; i < argc; i++)
	{
		if(wcscmp(argv[i], L"-l") == 0 || wcscmp(argv[i], L"--list") == 0)
		{
			ListPhysicalDrives();
			return 0;
		}
		else if(wcscmp(argv[i], L"-y") == 0 || wcscmp(argv[i], L"--yes") == 0)
		{
			bAssumeYes = TRUE;
		}
		else if(wcscmp(argv[i], L"--no-prepare") == 0)
		{
			bPrepareDisk = FALSE;
		}
		else if(!sVhdPath)
		{
			sVhdPath = argv[i];
		}
		else if(!sDriveArg)
		{
			sDriveArg = argv[i];
		}
	}

	if(!sVhdPath || !sDriveArg)
	{
		PrintUsage();
		return 1;
	}

	WCHAR sDrive[MAX_PATH] = {0};
	NormalizeDrivePath(sDriveArg, sDrive, ARRAYSIZE(sDrive));

	CVhdToDisk vhd(const_cast<LPWSTR>(sVhdPath));

	PrintPartitionTable(vhd);

	if(!bAssumeYes)
	{
		wprintf(L"\nWarning! This operation will destroy all data present on %s\n", sDrive);
		wprintf(L"Proceed? [y/N]: ");

		wint_t c = fgetwc(stdin);
		if(c != L'y' && c != L'Y')
		{
			wprintf(L"Aborted.\n");
			return 1;
		}
	}

	CConsoleProgress progress;
	BOOL bResult = vhd.DumpVhdToDisk(const_cast<LPWSTR>(sVhdPath), sDrive, &progress, bPrepareDisk);

	progress.EndProgressLine();
	if(bResult)
	{
		wprintf(L"VHD dumped on drive successfully!\n");
		return 0;
	}

	wprintf(L"Failed to dump the VHD on drive!\n");
	return 1;
}
