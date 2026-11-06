#pragma once

#include "VhdDumpProgress.h"
#include "DiskPrep.h"

typedef struct
{
	UINT16 cylinders;
	UCHAR heads;
	UCHAR sectors;
} VHD_GEOMETRY;


typedef struct _VHD_FOOTER
{
	CHAR cookie[8];
	UINT32 features;
	UINT32 version;
	UINT64 dataOffset;
	UINT32 timeStamp;
	CHAR creatorApplication[4];
	UINT32 creatorVersion;
	CHAR creatorOS[4];
	UINT64 originalSize;
	UINT64 currentSize;
	VHD_GEOMETRY diskGeometry;
	UINT32 diskType;
	UINT32 checksum;
	UCHAR uniqueId[16];
	UCHAR savedState;
	UCHAR padding[427];
}VHD_FOOTER, *PVHD_FOOTER;


typedef struct
{
	CHAR cookie[8];
	UINT64 dataOffset;
	UINT64 tableOffset;
	UINT32 headerVersion;
	UINT32 maxTableEntries;
	UINT32 blockSize;
	UINT32 checksum;
	UCHAR parentUniqueId[16];
	UINT32 parentTimeStamp;
	UINT32 reserved1;
	UCHAR parentUnicodeName[512];
	struct
	{
		UCHAR platformCode[4];
		UINT32 platformDataSpace;
		UINT32 platformDataLength;
		UINT32 reserved;
		UINT64 platformDataOffset;
	} partentLocator[8];
	UCHAR reserved2[256];
} VHD_DYNAMIC;


#define VHD_MAX_PARTITIONS 128

typedef struct
{
	WCHAR sNumber[8];	// partition number as Linux numbers it (sdaN)
	WCHAR sKind[12];	// Primary, Extended, Logical, or GPT
	WCHAR sBoot[4];
	WCHAR sType[40];
	WCHAR sStart[24];	// first LBA
	WCHAR sSectors[24];
	WCHAR sSize[16];	// human readable
	WCHAR sName[40];	// GPT partition name
} VHD_PARTITION_ROW;


class CVhdToDisk
{
	VHD_FOOTER	m_Foot;
	VHD_DYNAMIC m_Dyn;

	HANDLE		m_hVhdFile;
	HANDLE		m_hPhysicalDrive;

public:
	CVhdToDisk(void);
	CVhdToDisk(LPWSTR sPath);
	~CVhdToDisk(void);

	// bPrepareDisk: dismount, clean, and offline the target first (see DiskPrep.h)
	// nEndState: the disk's online/offline state once done, by default as it was found
	BOOL DumpVhdToDisk(const LPWSTR sPath, const LPWSTR sDrive, IVhdDumpProgress* pProgress
		, BOOL bPrepareDisk = TRUE, DISK_END_STATE nEndState = DISK_END_AS_FOUND);


	// Lists MBR primary, extended, and logical partitions, or the GPT entries
	// when the MBR is protective. *psScheme gets L"MBR", L"GPT" or L"none".
	BOOL ParsePartitions(VHD_PARTITION_ROW* pRows, int nMaxRows, int* pnRows, LPCWSTR* psScheme);

	// Virtual disk size in bytes, from the VHD footer.
	UINT64 GetDiskSize();

	static void FormatSize(UINT64 nBytes, WCHAR* sSize, size_t nSizeChars);

protected:

	BOOL OpenVhdFile(LPWSTR sPath);
	BOOL CloseVhdFile();

	BOOL OpenPhysicalDrive(LPWSTR sDrive);
	BOOL ClosePhysicalDrive();

	BOOL ReadFooter();
	BOOL ReadDynHeader();


	UINT64 GetFirstSectorAddress();

	BOOL ReadBat(UINT32** ppBat, UINT32* pnBats);
	BOOL ReadVhdSector(const UINT32* pBat, UINT32 nBats, UINT64 nLba, BYTE* pSector);

	void AddMbrRow(VHD_PARTITION_ROW* pRow, int nNumber, LPCWSTR sKind, const BYTE* pEntry, UINT64 nStart);
	BOOL ParseLogicalPartitions(const UINT32* pBat, UINT32 nBats, UINT64 nExtStart
		, VHD_PARTITION_ROW* pRows, int nMaxRows, int* pnItem);
	BOOL ParseGpt(const UINT32* pBat, UINT32 nBats
		, VHD_PARTITION_ROW* pRows, int nMaxRows, int* pnItem);

	BOOL Dump(IVhdDumpProgress* pProgress);
};
