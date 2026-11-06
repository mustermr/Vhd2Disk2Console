// main.cpp: console entry point for the x64 command-line VHD-to-disk dumper.
//

#include "stdafx.h"
#include <winioctl.h>
#include "../Vhd2disk/VhdDumpProgress.h"
#include "../Vhd2disk/VhdToDisk.h"

#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif

// ANSI escape sequences, only written when the console understands them (see InitColor)
#define VT_RESET		L"\x1b[0m"
#define VT_BOLD			L"\x1b[1m"
#define VT_DIM			L"\x1b[90m"
#define VT_GREEN		L"\x1b[92m"
#define VT_YELLOW		L"\x1b[93m"
#define VT_CYAN			L"\x1b[96m"
#define VT_TITLE		L"\x1b[1;96m"
#define VT_HEADER		L"\x1b[1;36m"
#define VT_BADGE_OK		L"\x1b[1;30;42m"
#define VT_BADGE_WARN	L"\x1b[1;97;43m"
#define VT_BADGE_ERROR	L"\x1b[1;97;41m"
#define VT_CLEAR_EOL	L"\x1b[K"

static BOOL g_bColorOut = FALSE;
static BOOL g_bColorErr = FALSE;

static LPCWSTR Vt(LPCWSTR sCode)
{
	return g_bColorOut ? sCode : L"";
}

static BOOL EnableVt(DWORD nStdHandle)
{
	HANDLE hConsole = GetStdHandle(nStdHandle);
	DWORD dwMode = 0;

	// Fails when the stream is redirected to a file or pipe
	if(!GetConsoleMode(hConsole, &dwMode)) return FALSE;

	return SetConsoleMode(hConsole, dwMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}

static void InitColor(BOOL bAllowed)
{
	size_t nLen = 0;

	if(!bAllowed) return;

	// https://no-color.org
	if(_wgetenv_s(&nLen, NULL, 0, L"NO_COLOR") == 0 && nLen > 0) return;

	g_bColorOut = EnableVt(STD_OUTPUT_HANDLE);
	g_bColorErr = EnableVt(STD_ERROR_HANDLE);
}

static void FormatElapsed(ULONGLONG nMs, WCHAR* sElapsed, size_t nElapsedChars)
{
	ULONGLONG nSec = nMs / 1000;

	if(nSec >= 3600)
		swprintf_s(sElapsed, nElapsedChars, L"%llu:%02llu:%02llu", nSec / 3600, nSec / 60 % 60, nSec % 60);
	else
		swprintf_s(sElapsed, nElapsedChars, L"%llu:%02llu", nSec / 60, nSec % 60);
}

static void PrintBanner()
{
	wprintf(L"\n  %sVhd2diskCli%s  %sdump a VHD onto a physical drive%s\n"
		, Vt(VT_TITLE), Vt(VT_RESET), Vt(VT_DIM), Vt(VT_RESET));
}

static void PrintSection(LPCWSTR sTitle)
{
	wprintf(L"\n  %s%s%s\n", Vt(VT_BOLD), sTitle, Vt(VT_RESET));
}

static void PrintField(LPCWSTR sLabel, LPCWSTR sValue)
{
	wprintf(L"    %s%-8s%s %s\n", Vt(VT_DIM), sLabel, Vt(VT_RESET), sValue);
}

// A colored label such as " ERROR " followed by the message; continuation
// lines of a multi-line message are indented to line up after the label.
static void PrintBadge(FILE* pStream, BOOL bColor, LPCWSTR sStyle, LPCWSTR sBadge, LPCWSTR sMessage)
{
	// The padding spaces only show as part of the colored background
	LPCWSTR sPad = bColor ? L" " : L"";
	int nIndent = 2 + (int)wcslen(sBadge) + 2 * (int)wcslen(sPad) + 2;

	fwprintf(pStream, L"  %s%s%s%s%s  ", bColor ? sStyle : L"", sPad, sBadge, sPad, bColor ? VT_RESET : L"");

	for(LPCWSTR p = sMessage; *p; p++)
	{
		if(*p == L'\n')
			fwprintf(pStream, L"\n%*s", nIndent, L"");
		else
			fputwc(*p, pStream);
	}

	fwprintf(pStream, L"\n");
}

static void PrintError(LPCWSTR sMessage)
{
	fflush(stdout);
	fwprintf(stderr, L"\n");
	PrintBadge(stderr, g_bColorErr, VT_BADGE_ERROR, L"ERROR", sMessage);
}

// Disk prep status lines go under a "Preparing disk" section; block progress is
// a bar redrawn in place under "Writing". Anything else first finishes that line.
class CConsoleProgress : public IVhdDumpProgress
{
	BOOL m_bProgressLine;
	ULONGLONG m_nStart;
	ULONGLONG m_nLastDraw;
	UINT32 m_nLastCurrent;

public:
	CConsoleProgress() : m_bProgressLine(FALSE), m_nStart(0), m_nLastDraw(0), m_nLastCurrent(0) {}

	void EndProgressLine()
	{
		if(!m_bProgressLine) return;

		wprintf(L"\n");
		m_bProgressLine = FALSE;
	}

	void OnStatus(LPCWSTR sStatus)
	{
		EndProgressLine();
		wprintf(L"    %s%s%s\n", Vt(VT_DIM), sStatus, Vt(VT_RESET));
	}

	void OnProgress(UINT32 nCurrent, UINT32 nTotal)
	{
		static const WCHAR sFull[] = L"==============================";
		const int nWidth = ARRAYSIZE(sFull) - 1;

		if(nTotal == 0) return;

		ULONGLONG nNow = GetTickCount64();

		if(m_nStart == 0)
		{
			m_nStart = nNow;
			PrintSection(L"Writing");
		}

		// Redraw at most 5 times a second, but always show the first and last update
		if(m_bProgressLine && (nCurrent == m_nLastCurrent || (nCurrent < nTotal && nNow - m_nLastDraw < 200))) return;
		m_nLastDraw = nNow;
		m_nLastCurrent = nCurrent;

		int nFilled = (int)((UINT64)nCurrent * nWidth / nTotal);
		UINT32 nPercent = (UINT32)((UINT64)nCurrent * 100 / nTotal);
		WCHAR sElapsed[16] = {0};

		FormatElapsed(nNow - m_nStart, sElapsed, ARRAYSIZE(sElapsed));

		wprintf(L"\r    [%s%.*s%s%*s] %s%3u%%%s  %sblock %u/%u  %s%s%s"
			, Vt(VT_CYAN), nFilled, sFull, Vt(VT_RESET), nWidth - nFilled, L""
			, Vt(VT_BOLD), nPercent, Vt(VT_RESET)
			, Vt(VT_DIM), nCurrent, nTotal, sElapsed, Vt(VT_RESET), Vt(VT_CLEAR_EOL));
		fflush(stdout);
		m_bProgressLine = TRUE;
	}

	void OnDriveWriteError(LPCWSTR sMessage)
	{
		EndProgressLine();
		PrintError(sMessage);
	}

	ULONGLONG GetElapsed()
	{
		return m_nStart ? GetTickCount64() - m_nStart : 0;
	}
};

static void PrintOption(LPCWSTR sOption, LPCWSTR sDescription)
{
	wprintf(L"    %s%-16s%s %s\n", Vt(VT_GREEN), sOption, Vt(VT_RESET), sDescription);
}

void PrintUsage()
{
	PrintBanner();

	PrintSection(L"Usage");
	wprintf(L"    Vhd2diskCli.exe %s<vhd-path> <drive>%s [options]\n", Vt(VT_CYAN), Vt(VT_RESET));
	wprintf(L"    Vhd2diskCli.exe --list\n");

	PrintSection(L"Drive");
	wprintf(L"    N, PhysicalDriveN or \\\\.\\PhysicalDriveN (see --list)\n");

	PrintSection(L"Options");
	PrintOption(L"-y, --yes", L"Don't ask for confirmation before writing.");
	PrintOption(L"--no-prepare", L"Don't dismount, clean or offline the drive first.");
	PrintOption(L"--no-color", L"Plain output (also when NO_COLOR is set or output is redirected).");
	PrintOption(L"-l, --list", L"List physical drives with their size and model.");
	PrintOption(L"-h, --help", L"Show this help.");

	PrintSection(L"Notes");
	wprintf(L"    Before writing, the drive's volumes are dismounted, its partition table is\n");
	wprintf(L"    deleted and it is taken offline until reboot, like diskpart CLEAN and\n");
	wprintf(L"    OFFLINE DISK. The VHD's partition table is shown before anything is written.\n\n");
}

// Appends a space-padded ANSI string from a storage descriptor, trimmed
static void AppendTrimmed(CHAR* sDest, size_t nDestChars, const CHAR* sSource)
{
	CHAR sTrimmed[128] = {0};

	while(*sSource == ' ') sSource++;
	strncpy_s(sTrimmed, sSource, _TRUNCATE);

	size_t nLen = strlen(sTrimmed);
	while(nLen > 0 && sTrimmed[nLen - 1] == ' ') sTrimmed[--nLen] = '\0';

	if(nLen == 0) return;

	if(sDest[0] != '\0')
		strncat_s(sDest, nDestChars, " ", _TRUNCATE);

	strncat_s(sDest, nDestChars, sTrimmed, _TRUNCATE);
}

static BOOL GetDriveInfo(LPCWSTR sDrive, WCHAR* sModel, size_t nModelChars, UINT64* pnBytes)
{
	DWORD dwBytes = 0;
	GET_LENGTH_INFORMATION length;
	STORAGE_PROPERTY_QUERY query;
	DWORD64 buffer[128];

	sModel[0] = L'\0';
	*pnBytes = 0;

	HANDLE hDrive = CreateFile(sDrive
		, GENERIC_READ
		, FILE_SHARE_READ | FILE_SHARE_WRITE
		, NULL
		, OPEN_EXISTING
		, 0
		, NULL);

	if(hDrive == INVALID_HANDLE_VALUE) return FALSE;

	if(DeviceIoControl(hDrive, IOCTL_DISK_GET_LENGTH_INFO
		, NULL, 0, &length, sizeof(length), &dwBytes, NULL))
		*pnBytes = length.Length.QuadPart;

	ZeroMemory(&query, sizeof(query));
	query.PropertyId = StorageDeviceProperty;
	query.QueryType = PropertyStandardQuery;

	ZeroMemory(buffer, sizeof(buffer));
	if(DeviceIoControl(hDrive, IOCTL_STORAGE_QUERY_PROPERTY
		, &query, sizeof(query), buffer, sizeof(buffer) - 1, &dwBytes, NULL))
	{
		STORAGE_DEVICE_DESCRIPTOR* pDescriptor = (STORAGE_DEVICE_DESCRIPTOR*)buffer;
		const CHAR* pBase = (const CHAR*)buffer;
		CHAR sAnsi[128] = {0};

		if(pDescriptor->VendorIdOffset && pDescriptor->VendorIdOffset < dwBytes)
			AppendTrimmed(sAnsi, sizeof(sAnsi), pBase + pDescriptor->VendorIdOffset);

		if(pDescriptor->ProductIdOffset && pDescriptor->ProductIdOffset < dwBytes)
			AppendTrimmed(sAnsi, sizeof(sAnsi), pBase + pDescriptor->ProductIdOffset);

		MultiByteToWideChar(CP_ACP, 0, sAnsi, -1, sModel, (int)nModelChars);
	}

	CloseHandle(hDrive);

	return TRUE;
}

void ListPhysicalDrives()
{
	WCHAR sPhysicalDrive[64] = {0};
	WCHAR sModel[128] = {0};
	WCHAR sSize[16] = {0};
	UINT64 nBytes = 0;
	int nFound = 0;

	PrintBanner();
	PrintSection(L"Physical drives");

	for(int i = 0; i < 99; i++)
	{
		swprintf_s(sPhysicalDrive, L"\\\\.\\PhysicalDrive%d", i);

		if(!GetDriveInfo(sPhysicalDrive, sModel, ARRAYSIZE(sModel), &nBytes))
			continue;

		if(nFound == 0)
			wprintf(L"    %s%-4s %-12s %s%s\n", Vt(VT_HEADER), L"#", L"Size", L"Model", Vt(VT_RESET));

		CVhdToDisk::FormatSize(nBytes, sSize, ARRAYSIZE(sSize));
		wprintf(L"    %-4d %-12s %s\n", i, sSize, sModel);
		nFound++;
	}

	if(nFound == 0)
		wprintf(L"    No physical drives could be opened (run as administrator).\n");

	wprintf(L"\n");
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

BOOL PrintVhdInfo(CVhdToDisk& vhd, LPCWSTR sVhdPath)
{
	static VHD_PARTITION_ROW rows[VHD_MAX_PARTITIONS];
	int nRows = 0;
	LPCWSTR sScheme = NULL;
	WCHAR sDiskSize[16] = {0};
	WCHAR sValue[64] = {0};

	PrintSection(L"Source VHD");
	PrintField(L"Path", sVhdPath);

	if(!vhd.ParsePartitions(rows, ARRAYSIZE(rows), &nRows, &sScheme))
		return FALSE;

	CVhdToDisk::FormatSize(vhd.GetDiskSize(), sDiskSize, ARRAYSIZE(sDiskSize));
	swprintf_s(sValue, L"%s (%llu sectors)", sDiskSize, vhd.GetDiskSize() / 512);
	PrintField(L"Size", sValue);
	PrintField(L"Table", sScheme);

	wprintf(L"\n");

	if(nRows == 0)
	{
		wprintf(L"    No partitions found in the VHD.\n");
		return TRUE;
	}

	wprintf(L"    %s%-3s %-9s %-5s %-22s %-12s %-12s %-10s %s%s\n"
		, Vt(VT_HEADER), L"#", L"Kind", L"Boot", L"Type", L"Start LBA", L"Sectors", L"Size", L"Name", Vt(VT_RESET));

	for(int i = 0; i < nRows; i++)
	{
		wprintf(L"    %-3s %-9s %-5s %-22s %-12s %-12s %-10s %s\n"
			, rows[i].sNumber, rows[i].sKind, rows[i].sBoot, rows[i].sType
			, rows[i].sStart, rows[i].sSectors, rows[i].sSize, rows[i].sName);
	}

	return TRUE;
}

void PrintTarget(LPCWSTR sDrive, BOOL bPrepareDisk)
{
	WCHAR sModel[128] = {0};
	WCHAR sSize[16] = {0};
	WCHAR sValue[192] = {0};
	UINT64 nBytes = 0;

	PrintSection(L"Target");
	PrintField(L"Drive", sDrive);

	if(GetDriveInfo(sDrive, sModel, ARRAYSIZE(sModel), &nBytes) && (nBytes || sModel[0]))
	{
		CVhdToDisk::FormatSize(nBytes, sSize, ARRAYSIZE(sSize));
		swprintf_s(sValue, L"%s%s%s", sSize, sModel[0] ? L", " : L"", sModel);
		PrintField(L"Disk", sValue);
	}

	PrintField(L"Prepare", bPrepareDisk
		? L"dismount, clean and take offline before writing"
		: L"no (--no-prepare), write to the drive as it is");
}

int wmain(int argc, wchar_t* argv[])
{
	BOOL bAssumeYes = FALSE;
	BOOL bPrepareDisk = TRUE;
	BOOL bColor = TRUE;
	BOOL bList = FALSE;
	BOOL bHelp = FALSE;
	LPCWSTR sVhdPath = NULL;
	LPCWSTR sDriveArg = NULL;
	LPCWSTR sUnknown = NULL;

	for(int i = 1; i < argc; i++)
	{
		if(wcscmp(argv[i], L"-l") == 0 || wcscmp(argv[i], L"--list") == 0)
			bList = TRUE;
		else if(wcscmp(argv[i], L"-y") == 0 || wcscmp(argv[i], L"--yes") == 0)
			bAssumeYes = TRUE;
		else if(wcscmp(argv[i], L"--no-prepare") == 0)
			bPrepareDisk = FALSE;
		else if(wcscmp(argv[i], L"--no-color") == 0)
			bColor = FALSE;
		else if(wcscmp(argv[i], L"-h") == 0 || wcscmp(argv[i], L"--help") == 0
			|| wcscmp(argv[i], L"-?") == 0 || wcscmp(argv[i], L"/?") == 0)
			bHelp = TRUE;
		else if(argv[i][0] == L'-')
			sUnknown = argv[i];
		else if(!sVhdPath)
			sVhdPath = argv[i];
		else if(!sDriveArg)
			sDriveArg = argv[i];
	}

	InitColor(bColor);

	if(bHelp)
	{
		PrintUsage();
		return 0;
	}

	if(sUnknown)
	{
		WCHAR sMessage[128] = {0};
		swprintf_s(sMessage, L"Unknown option %s (see --help).", sUnknown);
		PrintError(sMessage);
		wprintf(L"\n");
		return 1;
	}

	if(bList)
	{
		ListPhysicalDrives();
		return 0;
	}

	if(!sVhdPath || !sDriveArg)
	{
		PrintUsage();
		return 1;
	}

	WCHAR sDrive[MAX_PATH] = {0};
	NormalizeDrivePath(sDriveArg, sDrive, ARRAYSIZE(sDrive));

	CVhdToDisk vhd(const_cast<LPWSTR>(sVhdPath));

	PrintBanner();

	if(!PrintVhdInfo(vhd, sVhdPath))
	{
		PrintError(L"Can't read the VHD. Check the path; only dynamic VHDs are supported.");
		wprintf(L"\n");
		return 1;
	}

	PrintTarget(sDrive, bPrepareDisk);

	if(!bAssumeYes)
	{
		WCHAR sWarning[MAX_PATH + 64] = {0};
		swprintf_s(sWarning, L"All data on %s will be destroyed.", sDrive);

		wprintf(L"\n");
		PrintBadge(stdout, g_bColorOut, VT_BADGE_WARN, L"WARNING", sWarning);
		wprintf(L"\n  %sProceed? [y/N]:%s ", Vt(VT_YELLOW), Vt(VT_RESET));

		wint_t c = fgetwc(stdin);
		if(c != L'y' && c != L'Y')
		{
			wprintf(L"\n  Aborted. Nothing was written.\n\n");
			return 1;
		}
	}

	if(bPrepareDisk)
		PrintSection(L"Preparing disk");

	CConsoleProgress progress;
	BOOL bResult = vhd.DumpVhdToDisk(const_cast<LPWSTR>(sVhdPath), sDrive, &progress, bPrepareDisk);

	progress.EndProgressLine();
	wprintf(L"\n");

	if(bResult)
	{
		WCHAR sElapsed[16] = {0};
		WCHAR sMessage[MAX_PATH + 64] = {0};

		FormatElapsed(progress.GetElapsed(), sElapsed, ARRAYSIZE(sElapsed));
		swprintf_s(sMessage, L"VHD written to %s in %s.", sDrive, sElapsed);

		PrintBadge(stdout, g_bColorOut, VT_BADGE_OK, L"DONE", sMessage);
		wprintf(L"\n");
		return 0;
	}

	PrintBadge(stdout, g_bColorOut, VT_BADGE_ERROR, L"FAILED", L"The VHD was not written completely.");
	wprintf(L"\n");
	return 1;
}
