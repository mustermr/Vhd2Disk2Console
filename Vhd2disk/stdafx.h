// stdafx.h : include file for standard system include files,
// or project specific include files that are used frequently,
// but are changed infrequently
//

#pragma once

#define STRICT  // Define before any Windows headers for consistent type checking
#include "targetver.h"

#define WIN32_LEAN_AND_MEAN             // Exclude rarely-used stuff from Windows headers
// Windows Header Files:
#include <windows.h>

// C RunTime Header Files
#include <stdlib.h>
#include <malloc.h>
#include <memory.h>
#include <tchar.h>


#include <Commctrl.h>
#include <commdlg.h> 

#define MYWM_UPDATE_STATUS (WM_USER + 666)
#define MYWM_UPDATE_PROGRESSBAR (WM_USER + 999)