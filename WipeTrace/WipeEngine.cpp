#include "stdafx.h"
#include "WipeEngine.h"

namespace
{
const DWORD kChunk      = 1024 * 1024;  // bytes per write
const int   kNameRounds = 7;            // random renames before deleting

CStringW Win32Message(DWORD code)
{
    LPWSTR buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, (LPWSTR)&buf, 0, nullptr);
    CStringW s = buf ? buf : L"";
    if (buf)
        LocalFree(buf);
    s.TrimRight();
    if (s.IsEmpty())
        s.Format(L"error %u", code);
    return s;
}

CStringW LastError(LPCWSTR what)
{
    DWORD code = GetLastError();
    CStringW s;
    s.Format(L"%s: %s", what, (LPCWSTR)Win32Message(code));
    return s;
}

bool IsLinkTag(DWORD tag)
{
    return tag == IO_REPARSE_TAG_SYMLINK || tag == IO_REPARSE_TAG_MOUNT_POINT;
}

bool IsCloudTag(DWORD tag)
{
    return (tag & 0xFFFF0FFF) == IO_REPARSE_TAG_CLOUD;     // IO_REPARSE_TAG_CLOUD_1..F share this pattern
}

CStringW ParentOf(const CStringW& path)
{
    int slash = path.ReverseFind(L'\\');
    return slash > 0 ? path.Left(slash) : CStringW();
}

CStringW NameOf(const CStringW& path)
{
    return path.Mid(path.ReverseFind(L'\\') + 1);
}

// Replace the name with random names of the same length a few times, so the original name is
// overwritten in the folder index and in the file's own MFT record. Updates `path` as it goes.
bool ScrambleName(CStringW& path)
{
    static const wchar_t alphabet[] = L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    CStringW parent = ParentOf(path);
    int len = (std::max)(1, NameOf(path).GetLength());
    bool renamed = false;
    for (int round = 0; round < kNameRounds; ++round)
    {
        for (int attempt = 0; attempt < 5; ++attempt)
        {
            std::vector<BYTE> rnd(len);
            BCryptGenRandom(nullptr, rnd.data(), (ULONG)len, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
            CStringW name;
            for (int i = 0; i < len; ++i)
                name += alphabet[rnd[i] % (_countof(alphabet) - 1)];
            CStringW next = parent + L"\\" + name;
            if (MoveFileExW(LongPath(path), LongPath(next), 0))
            {
                path = next;
                renamed = true;
                break;
            }
            DWORD e = GetLastError();
            if (e != ERROR_ALREADY_EXISTS && e != ERROR_FILE_EXISTS)
                return renamed;     // e.g. the folder is open somewhere
        }
    }
    return renamed;
}

bool IsDirEmpty(const CStringW& dir)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(LongPath(dir + L"\\*"), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE)
        return true;
    bool empty = true;
    do
    {
        if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L".."))
        {
            empty = false;
            break;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return empty;
}

void Enumerate(const CStringW& dir, int root, WipePlan& plan, const std::atomic<bool>& cancel)
{
    if (cancel)
        return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(LongPath(dir + L"\\*"), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE)
        return;     // unreadable folder: its removal will fail later and be reported
    do
    {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        WipeTask t;
        t.path = dir + L"\\" + fd.cFileName;
        t.root = root;
        t.isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        bool reparse = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        t.isLink = reparse && IsLinkTag(fd.dwReserved0);
        t.isCloud = reparse && !t.isDir && IsCloudTag(fd.dwReserved0);

        if (t.isDir && !t.isLink)
        {
            Enumerate(t.path, root, plan, cancel);      // contents first...
            plan.dirs.push_back(t);                     // ...so folders come out deepest first
            ++plan.rootDirs[root];
        }
        else
        {
            t.size = ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            if (!t.isLink && !t.isCloud)
            {
                plan.totalBytes += t.size;
                plan.rootBytes[t.root] += t.size;
            }
            plan.files.push_back(t);
            ++plan.rootFiles[root];
        }
    } while (FindNextFileW(h, &fd) && !cancel);
    FindClose(h);
}
} // namespace

CStringW LongPath(const CStringW& path)
{
    if (path.Left(4) == L"\\\\?\\")
        return path;
    if (path.Left(2) == L"\\\\")
        return L"\\\\?\\UNC\\" + path.Mid(2);
    return L"\\\\?\\" + path;
}

CStringW WipeEngine::CheckTarget(const CStringW& fullPath)
{
    CStringW p = fullPath;
    p.TrimRight(L'\\');
    if (p.GetLength() <= 2)
        return L"a whole drive cannot be wiped here";

    auto env = [](LPCWSTR name)
    {
        wchar_t buf[MAX_PATH] = {};
        DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH);
        CStringW s = (n && n < MAX_PATH) ? buf : L"";
        s.TrimRight(L'\\');
        return s;
    };
    auto known = [](REFKNOWNFOLDERID id)
    {
        PWSTR path = nullptr;
        CStringW s;
        if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &path)))
            s = path;
        CoTaskMemFree(path);
        s.TrimRight(L'\\');
        return s;
    };

    CStringW windows = env(L"SystemRoot");
    // Inside the Windows folder: always refused.
    if (!windows.IsEmpty() && (p.CompareNoCase(windows) == 0 ||
        (p.GetLength() > windows.GetLength() && _wcsnicmp(p, windows + L"\\", windows.GetLength() + 1) == 0)))
        return L"it is part of the Windows system folder";

    // These folders, and anything that contains them, are refused (their contents are fine).
    std::vector<CStringW> protectedDirs = {
        windows, env(L"ProgramFiles"), env(L"ProgramFiles(x86)"), env(L"ProgramW6432"), env(L"ProgramData"),
        env(L"SystemDrive") + L"\\Users", env(L"USERPROFILE"),
        known(FOLDERID_Desktop), known(FOLDERID_Documents), known(FOLDERID_Downloads), known(FOLDERID_Pictures),
        known(FOLDERID_Music), known(FOLDERID_Videos), known(FOLDERID_RoamingAppData), known(FOLDERID_LocalAppData),
        env(L"OneDrive"), env(L"OneDriveCommercial"), env(L"OneDriveConsumer"),
    };
    // The standard profile folders also exist under the profile itself when they are redirected (e.g. to OneDrive).
    for (LPCWSTR name : { L"Desktop", L"Documents", L"Downloads", L"Pictures", L"Music", L"Videos", L"AppData",
                          L"OneDrive" })
        protectedDirs.push_back(env(L"USERPROFILE") + L"\\" + name);
    for (const CStringW& d : protectedDirs)
    {
        if (d.GetLength() <= 2)
            continue;
        if (p.CompareNoCase(d) == 0)
            return L"it is a protected system or profile folder (choose the items inside it instead)";
        if (d.GetLength() > p.GetLength() && _wcsnicmp(d, p + L"\\", p.GetLength() + 1) == 0)
            return L"it contains the protected folder " + d;
    }
    return L"";
}

void WipeEngine::BuildPlan(const std::vector<CStringW>& roots, WipePlan& plan, const std::atomic<bool>& cancel)
{
    plan = WipePlan();
    plan.rootErrors.resize(roots.size());
    plan.rootFiles.assign(roots.size(), 0);
    plan.rootDirs.assign(roots.size(), 0);
    plan.rootBytes.assign(roots.size(), 0);

    for (int r = 0; r < (int)roots.size() && !cancel; ++r)
    {
        const CStringW& path = roots[r];
        CStringW refused = CheckTarget(path);
        if (!refused.IsEmpty())
        {
            plan.rootErrors[r] = L"Refused: " + refused;
            continue;
        }
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (!GetFileAttributesExW(LongPath(path), GetFileExInfoStandard, &fa))
        {
            plan.rootErrors[r] = LastError(L"Not found");
            continue;
        }

        WipeTask t;
        t.path = path;
        t.root = r;
        t.isDir = (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (fa.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
        {
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileExW(LongPath(path), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
            if (h != INVALID_HANDLE_VALUE)
            {
                t.isLink = IsLinkTag(fd.dwReserved0);
                t.isCloud = !t.isDir && IsCloudTag(fd.dwReserved0);
                FindClose(h);
            }
        }
        if (t.isDir && !t.isLink)
        {
            Enumerate(path, r, plan, cancel);
            plan.dirs.push_back(t);
            ++plan.rootDirs[r];
        }
        else
        {
            t.size = ((ULONGLONG)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
            if (!t.isLink && !t.isCloud)
            {
                plan.totalBytes += t.size;
                plan.rootBytes[t.root] += t.size;
            }
            plan.files.push_back(t);
            ++plan.rootFiles[r];
        }
    }
}

bool WipeEngine::OverwriteHandle(HANDLE h, ULONGLONG length, int passes, const std::atomic<bool>& cancel,
                                 std::vector<BYTE>& buf, CStringW& err)
{
    for (int pass = 0; pass < passes; ++pass)
    {
        LARGE_INTEGER zero = {};
        if (!SetFilePointerEx(h, zero, nullptr, FILE_BEGIN))
        {
            err = LastError(L"Seek failed");
            return false;
        }
        for (ULONGLONG pos = 0; pos < length; )
        {
            if (cancel)
            {
                err = L"Cancelled (file only partly overwritten, not deleted)";
                return false;
            }
            DWORD n = (DWORD)std::min<ULONGLONG>(kChunk, length - pos);
            BCryptGenRandom(nullptr, buf.data(), n, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
            DWORD written = 0;
            if (!WriteFile(h, buf.data(), n, &written, nullptr) || written != n)
            {
                err = LastError(L"Overwrite failed");
                return false;
            }
            pos += n;
            bytesDone += n;
        }
        if (!FlushFileBuffers(h))
        {
            err = LastError(L"Flush failed");
            return false;
        }
    }
    return true;
}

bool WipeEngine::WipeFile(const WipeTask& t, const WipeOptions& o, const std::atomic<bool>& cancel,
                          std::vector<BYTE>& buf, CStringW& err, bool& notOverwritten)
{
    notOverwritten = false;
    CStringW path = t.path;
    DWORD attrs = GetFileAttributesW(LongPath(path));
    if (attrs == INVALID_FILE_ATTRIBUTES)
    {
        err = LastError(L"Not found");
        return false;
    }
    if (attrs & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_HIDDEN))
        SetFileAttributesW(LongPath(path), attrs & ~(FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_HIDDEN));

    if (t.isLink)
    {
        // Remove the link itself; never follow it.
        notOverwritten = true;
        BOOL ok = t.isDir ? RemoveDirectoryW(LongPath(path)) : DeleteFileW(LongPath(path));
        if (!ok)
            err = LastError(L"Could not remove link");
        return ok != FALSE;
    }

    if (!t.isCloud)
    {
        // Alternate data streams first (they are separate file objects).
        WIN32_FIND_STREAM_DATA sd;
        HANDLE hs = FindFirstStreamW(LongPath(path), FindStreamInfoStandard, &sd, 0);
        if (hs != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (!_wcsicmp(sd.cStreamName, L"::$DATA"))
                    continue;
                HANDLE h = CreateFileW(LongPath(path) + sd.cStreamName, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                       FILE_FLAG_WRITE_THROUGH, nullptr);
                if (h == INVALID_HANDLE_VALUE)
                    continue;
                bool ok = OverwriteHandle(h, (ULONGLONG)sd.StreamSize.QuadPart, o.passes, cancel, buf, err);
                if (ok)
                {
                    LARGE_INTEGER zero = {};
                    SetFilePointerEx(h, zero, nullptr, FILE_BEGIN);
                    SetEndOfFile(h);
                }
                CloseHandle(h);
                if (!ok)
                {
                    FindClose(hs);
                    return false;
                }
            } while (FindNextStreamW(hs, &sd));
            FindClose(hs);
        }

        // Main stream. Exclusive access: fails cleanly if another program has the file open.
        HANDLE h = CreateFileW(LongPath(path), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                               FILE_FLAG_WRITE_THROUGH, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            err = LastError(L"Could not open");
            return false;
        }

        // Compressed or sparse files would place new data in new clusters; store them plainly first.
        DWORD ret = 0;
        if (attrs & FILE_ATTRIBUTE_COMPRESSED)
        {
            USHORT none = COMPRESSION_FORMAT_NONE;
            DeviceIoControl(h, FSCTL_SET_COMPRESSION, &none, sizeof(none), nullptr, 0, &ret, nullptr);
        }
        if (attrs & FILE_ATTRIBUTE_SPARSE_FILE)
        {
            FILE_SET_SPARSE_BUFFER sp = { FALSE };
            DeviceIoControl(h, FSCTL_SET_SPARSE, &sp, sizeof(sp), nullptr, 0, &ret, nullptr);
        }

        // Overwrite up to the allocation size so the slack after the end of the file is covered too.
        FILE_STANDARD_INFO si = {};
        ULONGLONG length = t.size;
        if (GetFileInformationByHandleEx(h, FileStandardInfo, &si, sizeof(si)))
            length = std::max<ULONGLONG>((ULONGLONG)si.EndOfFile.QuadPart, (ULONGLONG)si.AllocationSize.QuadPart);

        bool ok = OverwriteHandle(h, length, o.passes, cancel, buf, err);
        if (ok)
        {
            LARGE_INTEGER zero = {};
            SetFilePointerEx(h, zero, nullptr, FILE_BEGIN);
            SetEndOfFile(h);
            // Hide when the file existed: reset all timestamps to 1 Jan 1980.
            SYSTEMTIME st = { 1980, 1, 2, 1, 0, 0, 0, 0 };
            FILETIME ft;
            SystemTimeToFileTime(&st, &ft);
            SetFileTime(h, &ft, &ft, &ft);
        }
        CloseHandle(h);
        if (!ok)
            return false;
    }
    else
    {
        // Overwriting a cloud placeholder would download it and upload random data to the cloud.
        notOverwritten = true;
    }

    if (o.scrambleNames)
        ScrambleName(path);
    if (!DeleteFileW(LongPath(path)))
    {
        err = LastError(L"Overwritten but could not delete");
        if (path != t.path)
            err += L" (now named " + NameOf(path) + L")";
        return false;
    }
    return true;
}

bool WipeEngine::WipeDir(const WipeTask& t, const WipeOptions& o, CStringW& err)
{
    CStringW path = t.path;
    DWORD attrs = GetFileAttributesW(LongPath(path));
    if (attrs == INVALID_FILE_ATTRIBUTES)
    {
        err = LastError(L"Not found");
        return false;
    }
    if (!IsDirEmpty(path))
    {
        // Leave it, under its own name, so the user can see what is left.
        err = L"Folder not removed: some of its contents could not be deleted";
        return false;
    }
    SetFileAttributesW(LongPath(path), FILE_ATTRIBUTE_DIRECTORY);
    if (o.scrambleNames)
        ScrambleName(path);
    if (!RemoveDirectoryW(LongPath(path)))
    {
        err = LastError(L"Could not remove folder");
        return false;
    }
    return true;
}

void WipeEngine::Run(const WipePlan& plan, const WipeOptions& opt, const std::atomic<bool>& cancel,
                     std::vector<RootResult>& results)
{
    results.assign(plan.rootErrors.size(), RootResult());
    std::mutex mx;
    auto record = [&](const WipeTask& t, bool ok, bool notOverwritten, const CStringW& err)
    {
        std::lock_guard<std::mutex> lock(mx);
        RootResult& r = results[t.root];
        if (ok)
        {
            (t.isDir && !t.isLink) ? ++r.dirs : ++r.files;
            if (notOverwritten)
                ++r.notOverwritten;
        }
        else
        {
            ++r.failed;
            if (r.firstError.IsEmpty())
                r.firstError = NameOf(t.path) + L": " + err;
        }
    };

    // Phase 1: files, in parallel.
    std::atomic<size_t> next{ 0 };
    auto worker = [&]()
    {
        std::vector<BYTE> buf(kChunk);
        for (;;)
        {
            size_t i = next++;
            if (i >= plan.files.size() || cancel)
                break;
            const WipeTask& t = plan.files[i];
            CStringW err;
            bool notOverwritten = false;
            bool ok = WipeFile(t, opt, cancel, buf, err, notOverwritten);
            record(t, ok, notOverwritten, err);
            ++itemsDone;
        }
    };
    int n = (std::max)(1, (std::min)(opt.threads, 16));
    n = (int)std::min<size_t>(n, std::max<size_t>(1, plan.files.size()));
    std::vector<std::thread> pool;
    for (int i = 0; i < n; ++i)
        pool.emplace_back(worker);
    for (std::thread& th : pool)
        th.join();

    // Phase 2: folders, deepest first (they must be empty to be removed).
    for (const WipeTask& t : plan.dirs)
    {
        if (cancel)
            break;
        CStringW err;
        bool ok = WipeDir(t, opt, err);
        record(t, ok, false, err);
        ++itemsDone;
    }
}
