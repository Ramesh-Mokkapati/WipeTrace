#include "stdafx.h"
#include "FreeSpaceWiper.h"
#include "WipeEngine.h"

namespace
{
const DWORD     kFillChunk   = 4 * 1024 * 1024;         // largest write; halved when the drive is nearly full
const ULONGLONG kFillFileMax = 1024ull * 1024 * 1024;   // 1 GB per temporary file (FAT32 allows < 4 GB)
const ULONGLONG kFilesPerDir = 5000;                    // keeps each folder's index small

bool IsFull(DWORD e)
{
    return e == ERROR_DISK_FULL || e == ERROR_HANDLE_DISK_FULL || e == ERROR_DISK_QUOTA_EXCEEDED;
}

CStringW ErrorText(LPCWSTR what, DWORD code)
{
    CStringW s;
    s.Format(L"%s (error %u)", what, code);
    return s;
}

bool QueryMft(HANDLE h, ULONGLONG& validLength, DWORD& recordSize)
{
    // Works on any handle on an NTFS volume, without administrator rights.
    NTFS_VOLUME_DATA_BUFFER vd = {};
    DWORD ret = 0;
    if (!DeviceIoControl(h, FSCTL_GET_NTFS_VOLUME_DATA, nullptr, 0, &vd, sizeof(vd), &ret, nullptr))
        return false;
    validLength = (ULONGLONG)vd.MftValidDataLength.QuadPart;
    recordSize = vd.BytesPerFileRecordSegment;
    return recordSize != 0;
}

bool DeleteTree(const CStringW& dir)
{
    bool ok = true;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(LongPath(dir + L"\\*"), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
                continue;
            CStringW p = dir + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                ok = DeleteTree(p) && ok;
            else
            {
                SetFileAttributesW(LongPath(p), FILE_ATTRIBUTE_NORMAL);
                ok = DeleteFileW(LongPath(p)) && ok;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    SetFileAttributesW(LongPath(dir), FILE_ATTRIBUTE_DIRECTORY);
    return RemoveDirectoryW(LongPath(dir)) && ok;
}

// Phase 1: reuse every free MFT entry with a small random file, overwriting old names and resident data.
void CleanDeletedEntries(const CStringW& work, const std::atomic<bool>& cancel, FreeSpaceProgress& p,
                         FreeSpaceResult& r, ULONGLONG cap)
{
    HANDLE hd = CreateFileW(LongPath(work), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (hd == INVALID_HANDLE_VALUE)
        return;
    ULONGLONG startLength = 0;
    DWORD record = 0;
    if (!QueryMft(hd, startLength, record))
    {
        CloseHandle(hd);    // not NTFS: nothing to do here
        return;
    }
    r.entriesCleaned = true;

    // Fill most of each entry with data while keeping it small enough to stay inside the entry.
    std::vector<BYTE> data(record > 700 ? record - 400 : record / 2);
    ULONGLONG maxFiles = startLength / record;      // never more than the table holds
    if (cap)
        maxFiles = (std::min)(maxFiles, cap);
    CStringW sub;
    for (ULONGLONG n = 0; n < maxFiles; ++n)
    {
        if (cancel)
        {
            r.cancelled = true;
            break;
        }
        if (n % kFilesPerDir == 0)
        {
            sub.Format(L"%s\\e%05llu", (LPCWSTR)work, n / kFilesPerDir);
            if (!CreateDirectoryW(LongPath(sub), nullptr))
                break;
        }
        CStringW f;
        f.Format(L"%s\\%llu", (LPCWSTR)sub, n);
        HANDLE h = CreateFileW(LongPath(f), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            break;
        BCryptGenRandom(nullptr, data.data(), (ULONG)data.size(), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        DWORD w = 0;
        WriteFile(h, data.data(), (DWORD)data.size(), &w, nullptr);
        CloseHandle(h);
        p.entries = n + 1;

        // Once the table has to grow, every free entry has been reused.
        ULONGLONG length = 0;
        DWORD rs = 0;
        if ((n & 255) == 255 && QueryMft(hd, length, rs) && length > startLength)
            break;
    }
    CloseHandle(hd);
    r.entries = p.entries;
}

// Phase 2: write random data until the drive is full.
void FillFreeSpace(const CStringW& root, const CStringW& work, const std::atomic<bool>& cancel,
                   FreeSpaceProgress& p, FreeSpaceResult& r, ULONGLONG cap)
{
    DWORD spc = 0, bps = 0, fc = 0, tc = 0;
    DWORD cluster = GetDiskFreeSpaceW(root, &spc, &bps, &fc, &tc) && spc && bps ? spc * bps : 4096;
    // Unbuffered writes need a sector-aligned buffer and sizes; VirtualAlloc returns page-aligned memory.
    BYTE* buf = static_cast<BYTE*>(VirtualAlloc(nullptr, kFillChunk, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!buf)
    {
        r.error = L"Out of memory";
        return;
    }

    DWORD chunk = kFillChunk;
    bool full = false;
    for (int fileNo = 0; !full; ++fileNo)
    {
        CStringW f;
        f.Format(L"%s\\fill%05d.tmp", (LPCWSTR)work, fileNo);
        HANDLE h = CreateFileW(LongPath(f), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                               FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            DWORD e = GetLastError();
            if (!IsFull(e))
                r.error = ErrorText(L"Could not create a temporary file", e);
            break;
        }
        for (ULONGLONG inFile = 0; inFile < kFillFileMax; )
        {
            if (cancel)
            {
                r.cancelled = true;
                full = true;
                break;
            }
            if (cap && p.written >= cap)
            {
                full = true;
                break;
            }
            BCryptGenRandom(nullptr, buf, chunk, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
            DWORD w = 0;
            if (WriteFile(h, buf, chunk, &w, nullptr) && w == chunk)
            {
                inFile += chunk;
                p.written += chunk;
                continue;
            }
            DWORD e = GetLastError();
            if (IsFull(e) && chunk > cluster)
            {
                chunk = std::max<DWORD>(cluster, chunk / 2);    // fill the last gaps cluster by cluster
                continue;
            }
            if (!IsFull(e))
                r.error = ErrorText(L"Write failed", e);
            full = true;
            break;
        }
        FlushFileBuffers(h);
        CloseHandle(h);
    }
    VirtualFree(buf, 0, MEM_RELEASE);
    r.written = p.written;
}
}

namespace FreeSpaceWiper
{
void Run(const CStringW& root, bool cleanEntries, const std::atomic<bool>& cancel,
         FreeSpaceProgress& p, FreeSpaceResult& r, const FreeSpaceLimits& limits)
{
    r = FreeSpaceResult();
    ULARGE_INTEGER freeCaller = {}, total = {}, freeTotal = {};
    if (GetDiskFreeSpaceExW(root, &freeCaller, &total, &freeTotal))
        p.freeAtStart = freeCaller.QuadPart;

    CStringW work;
    work.Format(L"%sWipeTrace-FreeSpace-%llX", (LPCWSTR)root, GetTickCount64());
    if (!CreateDirectoryW(LongPath(work), nullptr))
    {
        r.error = ErrorText(L"Could not create a temporary folder on the drive", GetLastError());
        return;
    }
    SetFileAttributesW(LongPath(work), FILE_ATTRIBUTE_HIDDEN);

    if (cleanEntries)
    {
        p.phase = 1;
        CleanDeletedEntries(work, cancel, p, r, limits.maxEntries);
    }
    if (!r.cancelled)
    {
        p.phase = 2;
        FillFreeSpace(root, work, cancel, p, r, limits.maxBytes);
    }

    // Always give the space back, whatever happened above.
    p.phase = 3;
    if (!DeleteTree(work))
        r.leftover = work;
    r.ok = r.error.IsEmpty() && !r.cancelled;
}
}
