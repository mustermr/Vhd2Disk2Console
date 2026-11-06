// main.cpp: console entry point for the x64 command-line VHD-to-disk dumper.
//

#include "stdafx.h"
#include <winioctl.h>
#include <wctype.h>
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
// Black on bright backgrounds: conhost has no bold weight (1 only brightens
// the color, turning black grey), so contrast is what makes badges stand out
#define VT_BADGE_OK		L"\x1b[30;102m"
#define VT_BADGE_WARN	L"\x1b[30;103m"
#define VT_BADGE_ERROR	L"\x1b[30;101m"
#define VT_HIGHLIGHT	L"\x1b[30;106m"
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
	wprintf(L"    %s%-10s%s %s\n", Vt(VT_DIM), sLabel, Vt(VT_RESET), sValue);
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
	BOOL m_bFinishing;
	ULONGLONG m_nStart;
	ULONGLONG m_nLastDraw;
	UINT32 m_nLastCurrent;

public:
	CConsoleProgress() : m_bProgressLine(FALSE), m_bFinishing(FALSE), m_nStart(0), m_nLastDraw(0), m_nLastCurrent(0) {}

	void EndProgressLine()
	{
		if(!m_bProgressLine) return;

		wprintf(L"\n");
		m_bProgressLine = FALSE;
	}

	void OnStatus(LPCWSTR sStatus)
	{
		EndProgressLine();

		// After writing, the only status is setting the disk's online/offline state
		if(m_nStart != 0 && !m_bFinishing)
		{
			m_bFinishing = TRUE;
			PrintSection(L"Finishing");
		}

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

	// Show the name this exe actually has, e.g. Vhd2diskCli_x64.exe
	WCHAR sExePath[MAX_PATH] = {0};
	LPCWSTR sExeName = L"Vhd2diskCli.exe";

	if(GetModuleFileNameW(NULL, sExePath, ARRAYSIZE(sExePath)))
	{
		LPCWSTR sSlash = wcsrchr(sExePath, L'\\');
		sExeName = sSlash ? sSlash + 1 : sExePath;
	}

	PrintSection(L"Usage");
	wprintf(L"    %s %s<vhd-path> [<drive>]%s [options]\n", sExeName, Vt(VT_CYAN), Vt(VT_RESET));
	wprintf(L"    %s --list\n", sExeName);

	PrintSection(L"Drive");
	wprintf(L"    N, PhysicalDriveN, or \\\\.\\PhysicalDriveN (see --list). Without a drive,\n");
	wprintf(L"    the disks are listed and you're asked for the number to deploy to.\n");

	PrintSection(L"Options");
	PrintOption(L"-y, --yes", L"Don't ask for confirmation before writing.");
	PrintOption(L"--no-prepare", L"Don't dismount, clean, or offline the drive first.");
	PrintOption(L"--online", L"Leave the drive online when done, so its new volumes mount.");
	PrintOption(L"--offline", L"Leave the drive offline when done.");
	PrintOption(L"--no-color", L"Plain output (also when NO_COLOR is set or output is redirected).");
	PrintOption(L"-l, --list", L"List physical drives with their size and model.");
	PrintOption(L"-h, --help", L"Show this help.");

	PrintSection(L"Notes");
	wprintf(L"    Before writing, the drive's volumes are dismounted, its partition table is\n");
	wprintf(L"    deleted, and it is taken offline, like diskpart CLEAN and OFFLINE DISK.\n");
	wprintf(L"    When done, the drive is put back online or offline as it was found,\n");
	wprintf(L"    unless --online or --offline says otherwise. These states last until\n");
	wprintf(L"    reboot. The VHD's partition table is shown before anything is written.\n\n");
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

typedef struct
{
	int nNumber;
	UINT64 nBytes;
	BOOL bKnownState;
	BOOL bOffline;
	BOOL bHoldsVhd;
	WCHAR sModel[128];
} DRIVE_ROW;

#define MAX_DRIVES 99

// Physical drives 0-98 that can be opened, which takes administrator rights.
// sVhdPath, if given, marks the drive the VHD is stored on.
static int FindDrives(DRIVE_ROW* pDrives, int nMaxDrives, LPCWSTR sVhdPath)
{
	WCHAR sPhysicalDrive[64] = {0};
	int nFound = 0;

	for(int i = 0; i < MAX_DRIVES && nFound < nMaxDrives; i++)
	{
		DRIVE_ROW* pDrive = &pDrives[nFound];
		ZeroMemory(pDrive, sizeof(*pDrive));

		swprintf_s(sPhysicalDrive, L"\\\\.\\PhysicalDrive%d", i);

		if(!GetDriveInfo(sPhysicalDrive, pDrive->sModel, ARRAYSIZE(pDrive->sModel), &pDrive->nBytes))
			continue;

		pDrive->nNumber = i;
		pDrive->bKnownState = GetDiskOffline(sPhysicalDrive, &pDrive->bOffline);
		pDrive->bHoldsVhd = sVhdPath && IsFileOnDisk(sVhdPath, i);
		nFound++;
	}

	return nFound;
}

static const DRIVE_ROW* FindDriveRow(const DRIVE_ROW* pDrives, int nDrives, int nNumber)
{
	for(int i = 0; i < nDrives; i++)
	{
		if(pDrives[i].nNumber == nNumber)
			return &pDrives[i];
	}

	return NULL;
}

// nTarget: number of the drive to mark as the deploy target, or -1 for none
static void PrintDriveTable(const DRIVE_ROW* pDrives, int nDrives, int nTarget)
{
	WCHAR sSize[16] = {0};

	if(nDrives == 0)
	{
		wprintf(L"    No physical drives could be opened (run as administrator).\n");
		return;
	}

	wprintf(L"    %s%-4s %-11s %-9s %-30s%s\n", Vt(VT_HEADER), L"#", L"Size", L"State", L"Model", Vt(VT_RESET));

	for(int i = 0; i < nDrives; i++)
	{
		const DRIVE_ROW* pDrive = &pDrives[i];
		BOOL bTarget = (pDrive->nNumber == nTarget);
		LPCWSTR sState = !pDrive->bKnownState ? L"?" : pDrive->bOffline ? L"offline" : L"online";

		CVhdToDisk::FormatSize(pDrive->nBytes, sSize, ARRAYSIZE(sSize));

		// The target is marked with ">" so it stands out without color too
		wprintf(L"  %s %s%-4d %-11s %-9s %-30.30s%s"
			, bTarget ? L">" : L" "
			, bTarget ? Vt(VT_HIGHLIGHT) : L""
			, pDrive->nNumber, sSize, sState, pDrive->sModel
			, bTarget ? Vt(VT_RESET) : L"");

		if(bTarget)
			wprintf(L" %starget%s", Vt(VT_BOLD), Vt(VT_RESET));

		if(pDrive->bHoldsVhd)
			wprintf(L" %sholds the VHD%s", Vt(VT_DIM), Vt(VT_RESET));

		wprintf(L"\n");
	}
}

void ListPhysicalDrives()
{
	static DRIVE_ROW drives[MAX_DRIVES];
	int nDrives = FindDrives(drives, ARRAYSIZE(drives), NULL);

	PrintBanner();
	PrintSection(L"Physical drives");
	PrintDriveTable(drives, nDrives, -1);
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
	else if(_wcsnicmp(sArg, L"PhysicalDrive", 13) == 0)
		swprintf_s(sDrive, nDriveChars, L"\\\\.\\%s", sArg);
	else
		wcscpy_s(sDrive, nDriveChars, sArg);

	return TRUE;
}

// N for \\.\PhysicalDriveN, -1 for anything else (such as a file)
static int DriveNumberFromPath(LPCWSTR sDrive)
{
	static const WCHAR sPrefix[] = L"\\\\.\\PhysicalDrive";
	const size_t nPrefix = ARRAYSIZE(sPrefix) - 1;

	if(_wcsnicmp(sDrive, sPrefix, nPrefix) != 0 || sDrive[nPrefix] == L'\0')
		return -1;

	for(LPCWSTR p = sDrive + nPrefix; *p; p++)
	{
		if(*p < L'0' || *p > L'9')
			return -1;
	}

	return _wtoi(sDrive + nPrefix);
}

// Asks for the target disk number until one from the list is typed.
// FALSE if the user cancels with an empty line.
static BOOL PromptForDrive(const DRIVE_ROW* pDrives, int nDrives, int* pnDrive)
{
	WCHAR sLine[64];

	for(;;)
	{
		wprintf(L"\n  %sDisk number to deploy to (Enter to cancel):%s ", Vt(VT_YELLOW), Vt(VT_RESET));
		fflush(stdout);

		ZeroMemory(sLine, sizeof(sLine));
		if(!fgetws(sLine, ARRAYSIZE(sLine), stdin)) return FALSE;

		WCHAR* p = sLine;
		while(iswspace(*p)) p++;

		size_t nLen = wcslen(p);
		while(nLen > 0 && iswspace(p[nLen - 1])) p[--nLen] = L'\0';

		if(nLen == 0) return FALSE;

		WCHAR* pEnd = NULL;
		long nNumber = wcstol(p, &pEnd, 10);
		const DRIVE_ROW* pDrive = (*pEnd == L'\0') ? FindDriveRow(pDrives, nDrives, (int)nNumber) : NULL;

		if(!pDrive)
			wprintf(L"    %s\"%s\" is not a disk number from the list.%s\n", Vt(VT_YELLOW), p, Vt(VT_RESET));
		else if(pDrive->bHoldsVhd)
			wprintf(L"    %sDisk %d holds the VHD, so it can't be the target.%s\n", Vt(VT_YELLOW), pDrive->nNumber, Vt(VT_RESET));
		else
		{
			*pnDrive = pDrive->nNumber;
			return TRUE;
		}
	}
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

void PrintTarget(LPCWSTR sDrive, BOOL bPrepareDisk, DISK_END_STATE nEndState
	, const DRIVE_ROW* pDrives, int nDrives)
{
	const DRIVE_ROW* pTarget = FindDriveRow(pDrives, nDrives, DriveNumberFromPath(sDrive));
	BOOL bOffline = FALSE;
	BOOL bKnownState = FALSE;

	PrintSection(L"Target");

	if(nDrives > 0)
	{
		PrintDriveTable(pDrives, nDrives, pTarget ? pTarget->nNumber : -1);
		wprintf(L"\n");
	}

	PrintField(L"Drive", sDrive);

	if(pTarget)
	{
		bKnownState = pTarget->bKnownState;
		bOffline = pTarget->bOffline;
	}
	else
	{
		// Not in the table (not running as administrator, or not a disk): describe it here
		WCHAR sModel[128] = {0};
		WCHAR sSize[16] = {0};
		WCHAR sValue[192] = {0};
		UINT64 nBytes = 0;

		if(GetDriveInfo(sDrive, sModel, ARRAYSIZE(sModel), &nBytes) && (nBytes || sModel[0]))
		{
			CVhdToDisk::FormatSize(nBytes, sSize, ARRAYSIZE(sSize));
			swprintf_s(sValue, L"%s%s%s", sSize, sModel[0] ? L", " : L"", sModel);
			PrintField(L"Disk", sValue);
		}

		bKnownState = GetDiskOffline(sDrive, &bOffline);

		if(bKnownState)
			PrintField(L"State", bOffline ? L"offline" : L"online");
	}

	PrintField(L"Prepare", bPrepareDisk
		? L"dismount, clean, and take offline before writing"
		: L"no (--no-prepare), write to the drive as it is");

	if(nEndState == DISK_END_ONLINE)
		PrintField(L"When done", L"online (--online)");
	else if(nEndState == DISK_END_OFFLINE)
		PrintField(L"When done", L"offline (--offline)");
	else if(bKnownState)
		PrintField(L"When done", bOffline ? L"offline, as found" : L"back online, as found");
	else
		PrintField(L"When done", L"as found");
}

int wmain(int argc, wchar_t* argv[])
{
	BOOL bAssumeYes = FALSE;
	BOOL bPrepareDisk = TRUE;
	BOOL bColor = TRUE;
	BOOL bList = FALSE;
	BOOL bHelp = FALSE;
	DISK_END_STATE nEndState = DISK_END_AS_FOUND;
	int nEndStateArgs = 0;
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
		else if(wcscmp(argv[i], L"--online") == 0)
		{
			nEndState = DISK_END_ONLINE;
			nEndStateArgs++;
		}
		else if(wcscmp(argv[i], L"--offline") == 0)
		{
			nEndState = DISK_END_OFFLINE;
			nEndStateArgs++;
		}
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

	if(nEndStateArgs > 1)
	{
		PrintError(L"Use only one of --online and --offline.");
		wprintf(L"\n");
		return 1;
	}

	if(bList)
	{
		ListPhysicalDrives();
		return 0;
	}

	if(!sVhdPath)
	{
		PrintUsage();
		return 1;
	}

	// Scripts use --yes; asking for the disk would leave them waiting
	if(!sDriveArg && bAssumeYes)
	{
		PrintError(L"--yes needs a drive. Without one, the disk number is asked for.");
		wprintf(L"\n");
		return 1;
	}

	CVhdToDisk vhd(const_cast<LPWSTR>(sVhdPath));

	PrintBanner();

	if(!PrintVhdInfo(vhd, sVhdPath))
	{
		PrintError(L"Can't read the VHD. Check the path; only dynamic VHDs are supported.");
		wprintf(L"\n");
		return 1;
	}

	static DRIVE_ROW drives[MAX_DRIVES];
	int nDrives = FindDrives(drives, ARRAYSIZE(drives), sVhdPath);
	WCHAR sDrive[MAX_PATH] = {0};

	if(sDriveArg)
	{
		NormalizeDrivePath(sDriveArg, sDrive, ARRAYSIZE(sDrive));

		// Only checkable when the disks could be listed (as administrator)
		int nTarget = DriveNumberFromPath(sDrive);
		const DRIVE_ROW* pTarget = FindDriveRow(drives, nDrives, nTarget);
		WCHAR sMessage[128] = {0};

		if(nDrives > 0 && nTarget >= 0 && !pTarget)
		{
			swprintf_s(sMessage, L"There is no disk %d. Run with --list to see the disks.", nTarget);
			PrintError(sMessage);
			wprintf(L"\n");
			return 1;
		}

		if(pTarget && pTarget->bHoldsVhd)
		{
			swprintf_s(sMessage, L"Disk %d holds the VHD, so it can't be the target.", nTarget);
			PrintError(sMessage);
			wprintf(L"\n");
			return 1;
		}
	}
	else
	{
		int nChosen = -1;

		PrintSection(L"Disks");
		PrintDriveTable(drives, nDrives, -1);

		if(nDrives == 0)
		{
			PrintError(L"No disks to deploy to. Run as administrator, or give the drive.");
			wprintf(L"\n");
			return 1;
		}

		if(!PromptForDrive(drives, nDrives, &nChosen))
		{
			wprintf(L"\n  Aborted. Nothing was written.\n\n");
			return 1;
		}

		swprintf_s(sDrive, L"\\\\.\\PhysicalDrive%d", nChosen);
	}

	PrintTarget(sDrive, bPrepareDisk, nEndState, drives, nDrives);

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
	BOOL bResult = vhd.DumpVhdToDisk(const_cast<LPWSTR>(sVhdPath), sDrive, &progress, bPrepareDisk, nEndState);

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
