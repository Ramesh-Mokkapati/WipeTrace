// FreeSpaceWiper.h : overwrites a drive's free space so that files deleted earlier (normally, not with
// WipeTrace) cannot be recovered. Existing files are not touched.
//
//   1. Deleted-file entries: on NTFS, entries of deleted files in the Master File Table still hold their
//      names and small files' contents. Small temporary files with random content are created until every
//      free entry has been reused (detected when the table has to grow).
//   2. Free space: temporary files of random data are written until the drive is full.
//   3. All temporary files are removed again - also after an error or Stop.
#pragma once

struct FreeSpaceProgress
{
    std::atomic<int>       phase{ 0 };      // 1 deleted entries, 2 free space, 3 cleanup
    std::atomic<ULONGLONG> entries{ 0 };    // phase 1: entries overwritten
    std::atomic<ULONGLONG> written{ 0 };    // phase 2: bytes written
    std::atomic<ULONGLONG> freeAtStart{ 0 };
};

struct FreeSpaceResult
{
    bool      ok = false;
    bool      cancelled = false;
    bool      entriesCleaned = false;   // false on FAT/exFAT (no MFT) or when skipped
    ULONGLONG entries = 0;
    ULONGLONG written = 0;
    CStringW  error;
    CStringW  leftover;                 // temporary folder that could not be fully removed, if any
};

// Caps for tests only; 0 = no cap (the normal case: run until every entry is reused and the drive is full).
struct FreeSpaceLimits
{
    ULONGLONG maxEntries = 0;
    ULONGLONG maxBytes = 0;
};

namespace FreeSpaceWiper
{
    // root: L"D:\\". Blocking; run it on a worker thread.
    void Run(const CStringW& root, bool cleanEntries, const std::atomic<bool>& cancel,
             FreeSpaceProgress& progress, FreeSpaceResult& result, const FreeSpaceLimits& limits = {});
}
