#pragma once

class IVhdDumpProgress
{
public:
	virtual ~IVhdDumpProgress() {}

	virtual void OnStatus(LPCWSTR sStatus) = 0;
	virtual void OnProgress(UINT32 nCurrent, UINT32 nTotal) = 0;
	virtual void OnDriveWriteError(LPCWSTR sMessage) = 0;
};
