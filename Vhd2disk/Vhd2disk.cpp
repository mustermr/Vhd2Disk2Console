// Vhd2disk.cpp: defines the entry point for the application.
//

#include "stdafx.h"
#include "Vhd2disk.h"
#include "VhdToDisk.h"
#include "URLCtrl.h"


typedef struct _DUMPTHRDSTRUCT
{
	HWND hDlg;
	WCHAR sVhdPath[MAX_PATH];
	WCHAR sDrive[64];
}DUMPTHRDSTRUCT;

LRESULT CALLBACK MainDlgProc( HWND hDlg, UINT Msg, WPARAM wParam, LPARAM lParam );

class CDialogProgress : public IVhdDumpProgress
{
	HWND m_hDlg;

public:
	CDialogProgress(HWND hDlg) : m_hDlg(hDlg) {}

	void OnStatus(LPCWSTR sStatus)
	{
		SendMessage(m_hDlg, MYWM_UPDATE_STATUS, (WPARAM)sStatus, 0);
	}

	void OnProgress(UINT32 nCurrent, UINT32 nTotal)
	{
		WCHAR sText[64] = {0};
		wsprintf(sText, L"dumping blocks... %u/%u", nCurrent, nTotal);
		OnStatus(sText);

		HWND hProgress = GetDlgItem(m_hDlg, IDC_PROGRESS_DUMP);
		SendMessage(hProgress, PBM_SETRANGE32, 0, (LPARAM)nTotal);
		SendMessage(hProgress, PBM_SETPOS, (LPARAM)nCurrent, 0);
	}

	void OnDriveWriteError(LPCWSTR sMessage)
	{
		MessageBox(NULL, sMessage, L"error", 0);
	}
};

void PopulateVolumeList(HWND hDlg, VHD_PARTITION_ROW* pRows, int nRows)
{
	HWND hwdListCtrl = GetDlgItem(hDlg, IDC_LIST_VOLUME);
	if(!hwdListCtrl) return;

	ListView_DeleteAllItems(hwdListCtrl);

	for(int i = 0; i < nRows; i++)
	{
		// Same order as the columns added in AddListHeader
		LPWSTR fields[] = { pRows[i].sNumber, pRows[i].sKind, pRows[i].sBoot, pRows[i].sType
			, pRows[i].sStart, pRows[i].sSize, pRows[i].sName };

		LVITEM item;
		item.mask = LVIF_TEXT;
		item.iItem = i;

		for(int f = 0; f < ARRAYSIZE(fields); f++)
		{
			item.iSubItem = f;
			item.pszText = fields[f];
			item.cchTextMax = (int)wcslen(item.pszText) + 1;

			if(f == 0)
				ListView_InsertItem(hwdListCtrl, &item);
			else
				ListView_SetItem(hwdListCtrl, &item);
		}
	}
}

HINSTANCE hInst;
HWND hWnd;
HWND m_hDlg = NULL;
HICON hIcon = NULL;
HANDLE hDumpThread = NULL;
CVhdToDisk* pVhd2disk = NULL;
DUMPTHRDSTRUCT dmpstruct;


UINT APIENTRY OFNHookProc(HWND hdlg, UINT uiMsg, WPARAM wParam, LPARAM lParam) 
{ 
	if (WM_INITDIALOG==uiMsg) 
	{ 
		HWND hOK=GetDlgItem(GetParent(hdlg), IDOK); 
		SetWindowText(hOK, L"Open VHD"); 
		return 1; 
	} 
	return 0; 
} 

BOOL DoFileDialog(LPWSTR lpszFilename, LPWSTR lpzFilter, LPWSTR lpzExtension) 
{ 
	OPENFILENAME ofn; 

	ZeroMemory(&ofn, sizeof(ofn));
	ofn.lStructSize = sizeof(ofn); 
	ofn.lpstrFile = lpszFilename; 
	ofn.nMaxFile = MAX_PATH; 
	ofn.lpstrFilter = lpzFilter;
	ofn.lpstrDefExt = lpzExtension;
	ofn.Flags = OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_ENABLEHOOK|OFN_EXPLORER|OFN_ENABLEHOOK;; 
	ofn.lpfnHook = (LPOFNHOOKPROC)OFNHookProc; 

	return GetOpenFileName(&ofn); 
} 

void PopulatePhysicalDriveComboBox(HWND hDlg)
{
	HANDLE hFile = NULL;
	WCHAR sPhysicalDrive[64] = {0};
	HWND hCombo = GetDlgItem(hDlg, IDC_COMBO1);
	
	SendMessage(hCombo, CB_RESETCONTENT, 0, 0);

	for(int i = 0; i < 99; i++)
	{
		wsprintf(sPhysicalDrive, L"\\\\.\\PhysicalDrive%d", i);
		hFile = CreateFile(sPhysicalDrive
			, GENERIC_WRITE
			, 0
			, NULL
			, OPEN_EXISTING
			, FILE_FLAG_WRITE_THROUGH | FILE_FLAG_NO_BUFFERING
			, NULL);

		if(hFile != INVALID_HANDLE_VALUE)
		{
			CloseHandle(hFile);
			SendMessage(hCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(sPhysicalDrive));
		}
	}
}

void AddListHeader(HWND hDlg)
{
	static const struct
	{
		LPCWSTR sText;
		int cx;
	} columns[] =
	{
		{ L"#", 30 },
		{ L"Kind", 55 },
		{ L"Boot", 35 },
		{ L"Type", 110 },
		{ L"Start LBA", 70 },
		{ L"Size", 60 },
		{ L"Name", 90 },
	};

	LVCOLUMN col;

	HWND hListCtrl = GetDlgItem(hDlg, IDC_LIST_VOLUME);
	if(!hListCtrl) return;

	ListView_SetExtendedListViewStyle
		(hListCtrl, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_HEADERDRAGDROP);

	for(int i = 0; i < ARRAYSIZE(columns); i++)
	{
		col.mask = LVCF_TEXT | LVCF_WIDTH;
		col.pszText = const_cast<LPWSTR>(columns[i].sText);
		col.fmt = LVCFMT_LEFT;
		col.cx = columns[i].cx;
		ListView_InsertColumn(hListCtrl, i, &col);
	}
}

int WINAPI WinMain( HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd )
{
	hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON_V2D));
	DialogBox( hInstance, MAKEINTRESOURCE(IDD_MAIN_DIAG), hWnd, (DLGPROC)MainDlgProc );
	return 0;
}

DWORD WINAPI DumpThread(LPVOID lpVoid)
{
	DUMPTHRDSTRUCT* pDumpStruct = (DUMPTHRDSTRUCT*)lpVoid;
	CDialogProgress progress(pDumpStruct->hDlg);

	if(pVhd2disk->DumpVhdToDisk(pDumpStruct->sVhdPath, pDumpStruct->sDrive, &progress))
		SendMessage(pDumpStruct->hDlg, MYWM_UPDATE_STATUS, (WPARAM)L"VHD dumped on drive successfully!", 1);
	else
		SendMessage(pDumpStruct->hDlg, MYWM_UPDATE_STATUS, (WPARAM)L"Failed to dump the VHD on drive!", 1);

	return 0;
}

LRESULT CALLBACK MainDlgProc( HWND hDlg, UINT Msg, WPARAM wParam, LPARAM lParam )
{
	WCHAR sVhdPath[MAX_PATH] = {0};
	WCHAR* sPhysicalDrive = 0;
	int nLen = 0;
	DWORD dwThrdID = 0;
	int nComboIndex = 0;

	COLORREF unvisited = RGB(0,102,204);
	COLORREF visited = RGB(128,0,128);
	HFONT hFont;
	LOGFONT lf;


	switch(Msg)
	{
	case WM_INITDIALOG:

		SendMessage(hDlg, WM_SETICON, ICON_BIG, (LPARAM)hIcon);

		PopulatePhysicalDriveComboBox(hDlg);

		AddListHeader(hDlg);
		
		hFont = (HFONT)SendMessage(GetDlgItem(hDlg, IDC_STATIC_NAME), WM_GETFONT, 0, 0);
		GetObject(hFont, sizeof(LOGFONT), &lf);
		lf.lfWeight = FW_BOLD;
		
		SendDlgItemMessage(hDlg, IDC_STATIC_NAME, WM_SETFONT, (WPARAM)CreateFontIndirect(&lf), (LPARAM)TRUE);

		urlctrl_set(GetDlgItem(hDlg, IDC_STATIC_URL), L"http://www.wooxo.com", &unvisited, &visited, UCF_KBD | UCF_FIT);

		SetDlgItemText(hDlg, IDC_STATIC_STATUS, L"IDLE");
		
		return TRUE;

	case WM_COMMAND:
		switch(LOWORD(wParam)) 
		{
		case IDCANCEL:
			DWORD dwExit;
			if(hDumpThread)
			{
				GetExitCodeThread(hDumpThread, &dwExit);
				if(dwExit != STILL_ACTIVE)
				{
					TerminateThread(hDumpThread, -1);
				}
			}
			EndDialog(hDlg, LOWORD( wParam ));
			hDlg = NULL;
			return TRUE;

		case IDC_BUTTON_BROWSE_VHD:
			if (DoFileDialog(sVhdPath, L"VHD Files (*.vhd)\0*.vhd\0All Files (*.*)\0*.*\0", L"vhd") == IDOK)
			{
				static VHD_PARTITION_ROW rows[VHD_MAX_PARTITIONS];
				int nRows = 0;

				SetDlgItemText(hDlg, IDC_EDIT_VHD_FILE, sVhdPath);

				if(pVhd2disk)
					delete pVhd2disk;

				ListView_DeleteAllItems(GetDlgItem(hDlg, IDC_LIST_VOLUME));

				pVhd2disk = new CVhdToDisk(sVhdPath);
				if(pVhd2disk && pVhd2disk->ParsePartitions(rows, ARRAYSIZE(rows), &nRows, NULL))
					PopulateVolumeList(hDlg, rows, nRows);
			}
			return TRUE;

		case IDC_BUTTON_START:

			
			ZeroMemory(&dmpstruct, sizeof(DUMPTHRDSTRUCT));
			dmpstruct.hDlg = hDlg;
			GetDlgItemText(hDlg, IDC_EDIT_VHD_FILE, dmpstruct.sVhdPath, MAX_PATH);
			if(wcslen(dmpstruct.sVhdPath) < 3) return TRUE;

			nComboIndex = SendMessage(GetDlgItem(hDlg, IDC_COMBO1), CB_GETCURSEL, 0, 0);
			if(nComboIndex == CB_ERR) return TRUE;
			nLen = SendMessage(GetDlgItem(hDlg, IDC_COMBO1), CB_GETLBTEXTLEN, nComboIndex, 0);

			if(nLen < 8 || nLen > 64) return TRUE;
			SendMessage(GetDlgItem(hDlg, IDC_COMBO1), CB_GETLBTEXT, nComboIndex, (LPARAM)dmpstruct.sDrive);
			
			if(MessageBox(hDlg, L"Are you sure to proceed? This operation will destroy all data present on the target drive", L"Warning!", MB_OKCANCEL) == IDOK)
			{
				hDumpThread = CreateThread(NULL, 0, DumpThread, &dmpstruct, 0, &dwThrdID);
				if(hDumpThread != INVALID_HANDLE_VALUE)
				{
					EnableWindow(GetDlgItem(hDlg, IDC_BUTTON_START), FALSE);
					EnableWindow(GetDlgItem(hDlg, IDC_BUTTON_BROWSE_VHD), FALSE);
					EnableWindow(GetDlgItem(hDlg, IDC_EDIT_VHD_FILE), FALSE);
					EnableWindow(GetDlgItem(hDlg, IDC_COMBO1), FALSE);

					ShowWindow(GetDlgItem(hDlg, IDC_PROGRESS_DUMP), SW_SHOW);
				}
				else
				{
					SetDlgItemText(hDlg, IDC_STATIC_STATUS, L"Failed to start the dump's thread");
				}
			}

			return TRUE;
		}
		break;
	case WM_SIZING:

		return TRUE;

	case MYWM_UPDATE_STATUS:

		SetDlgItemText(hDlg, IDC_STATIC_STATUS, (LPCWSTR)wParam);

		if(LOWORD(lParam) == 1)
		{
			EnableWindow(GetDlgItem(hDlg, IDC_BUTTON_START), TRUE);
			EnableWindow(GetDlgItem(hDlg, IDC_BUTTON_BROWSE_VHD), TRUE);
			EnableWindow(GetDlgItem(hDlg, IDC_EDIT_VHD_FILE), TRUE);
			EnableWindow(GetDlgItem(hDlg, IDC_COMBO1), TRUE);
		}

		return TRUE;
	
	}

	return FALSE;
}
