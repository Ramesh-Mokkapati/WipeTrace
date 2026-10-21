# WipeTrace

A Windows (MFC) utility that **securely deletes files and folders**: contents are overwritten with random
data and names are scrambled before anything is deleted, so neither the data nor the file names can be
recovered by undelete tools. Works from its own window or from the Explorer right-click menu.

Companion to [RecoverAside](../SafeUndelete), which recovers deleted files.

## What happens to each item

For every **file** (on a pool of worker threads):

1. Read-only, hidden and system attributes are cleared.
2. Every data stream is overwritten with cryptographically random data (`BCryptGenRandom`), 1, 3 or 7 passes:
   - alternate data streams (hidden streams attached to the file) as well as the main content;
   - up to the file's **allocation size**, so the slack space after the end of the file is covered too;
   - NTFS-compressed and sparse files are first converted to normal storage, so the new data lands on the
     same clusters as the old.
   Each pass is flushed to disk (`FILE_FLAG_WRITE_THROUGH` and `FlushFileBuffers`).
3. The file is truncated to 0 bytes and its timestamps are reset to 1 Jan 1980.
4. The name is replaced 7 times with random names of the same length, overwriting the original name in the
   folder index and in the file's MFT record.
5. The file is deleted.

**Folders** are handled after all files, deepest first: each is checked to be empty, renamed 7 times to random
names, then removed. A folder that still has contents (because something inside could not be deleted) keeps its
name, so you can see what is left.

## Choosing what to delete

- **Drive and browse tree.** Pick a drive in the *Drive* list; the tree on the left shows its folders and files
  as in Explorer. Double-click a file, or select a file or folder and click **Add Selected**. Links (junctions,
  symbolic links) are shown with `[link]` and are not opened.
- **Right-click in the tree** (or the menu key): **Securely Delete...** wipes just that file or folder (after the
  usual confirmation; **Del** does the same), **Add to List**, and **Show in Explorer**. On a drive the menu offers
  **Wipe Free Space on This Drive...** instead. After a wipe the folder in the tree is re-listed.
- **Add Files / Add Folder**, **drag and drop** onto the list, or the **Explorer right-click menu**.

Every added path is first resolved to its real location, so a folder reached through a link such as
`C:\Documents and Settings` is still checked against the protected folders below.

## Wipe free space

**Wipe Free Space...** makes files that were deleted earlier *the normal way* unrecoverable, without touching
files that still exist. For the drive selected in the *Drive* list:

1. **Entries of deleted files** (NTFS, optional, on by default): the Master File Table keeps an entry for each
   deleted file, with its name and, for small files, its contents. This is what undelete tools such as
   RecoverAside read. WipeTrace creates small temporary files of random data until every free entry has been
   reused, which it detects when the table has to grow (`FSCTL_GET_NTFS_VOLUME_DATA`, no admin rights needed).
2. **Free space**: temporary files of random data are written, unbuffered, until the drive is full; the last
   gaps are filled cluster by cluster.
3. **Clean-up**: all temporary files are removed, also after an error or **Stop**. If anything is left, the
   message names the hidden `WipeTrace-FreeSpace-...` folder to delete.

While step 2 runs the drive is completely full, so other programs saving to it may report "disk full". On
SSDs and flash drives, wear levelling and TRIM limit what overwriting free space can achieve; the confirmation
says so for those drives.

## Safety rules

| Situation | What WipeTrace does |
| --- | --- |
| Drive roots, the Windows folder (and anything in it), Program Files, ProgramData, `C:\Users`, your profile folder and its standard folders (Desktop, Documents, Downloads, ...) | **Refused.** Choose the items inside those folders instead. |
| Symbolic links and junctions | The **link** is removed; its target is never followed or touched. |
| OneDrive / cloud placeholder files | Deleted **without** overwriting, because overwriting would download them and upload random data to the cloud. Empty the cloud recycle bin too. |
| File open in another program | Left alone and reported (it is opened exclusively, so nothing is half-done). |
| Before anything changes | WipeTrace lists everything first and asks you to confirm the totals. |

## Explorer integration

On first start WipeTrace offers to add **"Securely delete with WipeTrace"** to the right-click menu for files and
folders. On Windows 11 it is under **Show more options**. It is per user (`HKCU\Software\Classes\AllFilesystemObjects`),
needs no administrator rights, and can be added or removed at any time with the button under *Options*.

Selecting several items and choosing the menu entry opens **one** window with all of them: Explorer starts one
process per item, and each hands its path to the first window over `WM_COPYDATA`. Explorer shows classic menu
entries for up to 15 selected items; for more, select their folder or drag them onto the window.

Command line:

```bash
WipeTrace.exe "C:\path\file.txt" "D:\old folder"
```

```bash
WipeTrace.exe /install-shell
```

```bash
WipeTrace.exe /uninstall-shell
```

Paths given on the command line are only added to the list; nothing is deleted until you confirm in the window.

## Limitations

- **SSDs, USB flash drives and SD cards** use wear levelling: the drive may write new data to fresh cells and
  keep the old ones until it erases them internally, where no program can reach. For these, full-disk encryption
  (BitLocker) is the reliable protection; overwriting still removes the data from anything that reads the file system.
- Copies elsewhere are not touched: backups, File History, shadow copies (System Restore), cloud versions, and
  temporary copies the editing program made.
- A **hard-linked** file shares its data with its other links; wiping one empties them all.
- Files that need administrator rights are reported as failed; run WipeTrace as administrator to delete them.

## Building

Visual Studio 2022 or later with the **Desktop development with C++** workload and **C++ MFC for latest build
tools**. Open `WipeTrace.sln`, or:

```powershell
.\build.ps1                                        # Release, x64 and x86
.\build.ps1 -Platform x64 -Package -Version 1.0.0  # also zip the exe + README into .\dist
```

Output: `bin\x64\Release\WipeTrace.exe` (`bin\Win32\Release\` for x86), MFC linked statically.

## Continuous integration

[`.github/workflows/build.yml`](.github/workflows/build.yml) builds x64 and x86 on every push and pull request to
`main` (compiler warnings fail the build) and publishes a GitHub release with both zips when a `v*` tag is pushed.

## Project layout

| File | Purpose |
| --- | --- |
| `WipeTrace/WipeEngine.*` | Planning, safety checks, overwrite/rename/delete, worker thread pool. |
| `WipeTrace/FreeSpaceWiper.*` | Free-space wipe: deleted-file entries, free space, clean-up. |
| `WipeTrace/DriveInfo.*` | Drive list (shared with RecoverAside), SSD/flash detection. |
| `WipeTrace/WipeTraceDlg.*` | Main window: item list, options, progress, confirmation. |
| `WipeTrace/WipeTrace.cpp` | Command line and single-instance hand-off between processes. |
| `WipeTrace/ShellIntegration.*` | Right-click menu registration and first-run prompt. |
| `WipeTrace/WipeTrace.rc`, `res/` | Dialog layout, icon, version info. |
