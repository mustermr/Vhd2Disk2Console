# C++ Build Tools Upgrade - Tasks

## Task 1: Fix STRICT Macro Definition Mismatch
**Status**: Not Started  
**Files**: stdafx.h, URLCtrl.cpp  
**Warnings Fixed**: 1 (C4603)

### Steps:
1. Edit Vhd2disk\stdafx.h
   - Add `#define STRICT` after `#pragma once` and before `#include "targetver.h"`
2. Edit Vhd2disk\URLCtrl.cpp
   - Remove lines 6-7 (`#define STRICT` and `#define WIN32_LEAN_AND_MEAN`)
   - Keep include of stdafx.h
3. Build incrementally to validate changes

**Expected Result**: C4603 warning resolved

---

## Task 2: Fix TRACE Macro Nonstandard Extension
**Status**: Not Started  
**Files**: Trace.h  
**Warnings Fixed**: 10 (C4353)

### Steps:
1. Edit Vhd2disk\Trace.h
   - Replace Release mode TRACE macro definition (line 34) from `((void)0)` to `do { (void)0; } while((void)0,0)`
   - Replace Release mode TRACEF macro definition (line 35) from `((void)0)` to `do { (void)0; } while((void)0,0)`
2. Build incrementally to validate changes

**Expected Result**: All 10 C4353 warnings in VhdToDisk.cpp resolved

---

## Task 3: Final Validation
**Status**: Not Started  
**Files**: N/A  
**Purpose**: Confirm all issues resolved

### Steps:
1. Perform full rebuild using `cppupgrade_rebuild_and_get_issues`
2. Verify 0 errors, 0 warnings
3. Confirm no new issues introduced
4. Document results

**Expected Result**: Clean build with 0 errors, 0 warnings

---

## Progress Summary
- **Total Tasks**: 3
- **Completed**: 0
- **In Progress**: 0
- **Not Started**: 3
- **Total Warnings to Fix**: 11
