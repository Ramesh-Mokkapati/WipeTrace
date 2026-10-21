// WipeEngine.h : secure deletion of files and folders.
//
// For every file: each data stream (including alternate data streams and the cluster slack at its end)
// is overwritten with cryptographically random data, the file is truncated to zero, its timestamps are
// reset, its name is replaced several times with random names of the same length, and then it is deleted.
// Folders are emptied first, then renamed the same way and removed. Files are processed on a pool of
// worker threads; folders are removed afterwards, deepest first.
#pragma once

struct WipeOptions
{
    int  passes = 1;            // random-data overwrite passes
    bool scrambleNames = true;  // rename to random names before deleting
    int  threads = 4;           // worker threads for files
};

struct WipeTask
{
    CStringW path;              // full path, without the \\?\ prefix
    bool     isDir = false;
    bool     isLink = false;    // symbolic link / junction: the link is removed, its target is never touched
    bool     isCloud = false;   // cloud placeholder (e.g. OneDrive): deleted without overwriting
    ULONGLONG size = 0;
    int      root = 0;          // index of the item the user chose
};

struct WipePlan
{
    std::vector<WipeTask> files;        // wiped in parallel
    std::vector<WipeTask> dirs;         // removed after all files, deepest first
    std::vector<CStringW> rootErrors;   // per chosen item: why it cannot be processed (empty = fine)
    std::vector<size_t>   rootFiles, rootDirs;
    std::vector<ULONGLONG> rootBytes;
    ULONGLONG totalBytes = 0;
};

struct RootResult
{
    int files = 0;              // files deleted
    int dirs = 0;               // folders deleted
    int failed = 0;
    int notOverwritten = 0;     // links and cloud files: removed but nothing to overwrite locally
    CStringW firstError;
};

class WipeEngine
{
public:
    // Empty when the path may be wiped; otherwise the reason it is refused (system and profile folders, drive roots).
    static CStringW CheckTarget(const CStringW& fullPath);

    // Expands the chosen items into the work list. Does not modify anything.
    static void BuildPlan(const std::vector<CStringW>& roots, WipePlan& plan, const std::atomic<bool>& cancel);

    // Blocking. Progress is published through the counters below while it runs.
    void Run(const WipePlan& plan, const WipeOptions& opt, const std::atomic<bool>& cancel,
             std::vector<RootResult>& results);

    std::atomic<ULONGLONG> bytesDone{ 0 };
    std::atomic<size_t>    itemsDone{ 0 };

private:
    bool WipeFile(const WipeTask& t, const WipeOptions& o, const std::atomic<bool>& cancel,
                  std::vector<BYTE>& buf, CStringW& err, bool& notOverwritten);
    bool WipeDir(const WipeTask& t, const WipeOptions& o, CStringW& err);
    bool OverwriteHandle(HANDLE h, ULONGLONG length, int passes, const std::atomic<bool>& cancel,
                         std::vector<BYTE>& buf, CStringW& err);
};

// "\\?\" form of a path, so long paths work.
CStringW LongPath(const CStringW& path);
