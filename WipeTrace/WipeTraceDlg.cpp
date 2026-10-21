// WipeTraceDlg.cpp : main dialog - target list, options, shell integration and the wipe run.
//
// A run has two background steps: building the plan (listing every file and folder, nothing is
// changed), then - after the user confirms the totals - the wipe itself on a pool of worker threads.

#include "stdafx.h"
#include "WipeTrace.h"
#include "WipeTraceDlg.h"
#include "ShellIntegration.h"

namespace
{
const UINT WM_FIRST_RUN = WM_APP + 1;
const UINT WM_PLAN_DONE = WM_APP + 2;
const UINT WM_WIPE_DONE = WM_APP + 3;
const UINT WM_FREE_DONE = WM_APP + 4;
const UINT_PTR kProgressTimer = 1;

enum { COL_NAME, COL_LOCATION, COL_TYPE, COL_SIZE, COL_STATUS };
enum { A_MOVEX = 1, A_MOVEY = 2, A_SIZEX = 4, A_SIZEY = 8 };
const int kPasses[] = { 1, 3, 7 };

// The real location of a path: links in the middle are resolved (so protections cannot be bypassed via e.g.
// "C:\Documents and Settings"), but a link that is itself the chosen item is kept as the link.
CStringW ResolvePath(const CStringW& path)
{
    HANDLE h = CreateFileW(LongPath(path), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return path;
    wchar_t buf[32768];
    DWORD n = GetFinalPathNameByHandleW(h, buf, _countof(buf), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(h);
    if (!n || n >= _countof(buf))
        return path;
    CStringW s = buf;
    if (s.Left(8).CompareNoCase(L"\\\\?\\UNC\\") == 0)
        s = L"\\\\" + s.Mid(8);
    else if (s.Left(4) == L"\\\\?\\")
        s = s.Mid(4);
    return s;
}

int SysIconIndex(LPCWSTR path, DWORD attrs, bool useAttrs)
{
    SHFILEINFOW sfi = {};
    UINT flags = SHGFI_SYSICONINDEX | SHGFI_SMALLICON | (useAttrs ? SHGFI_USEFILEATTRIBUTES : 0);
    SHGetFileInfoW(path, attrs, &sfi, sizeof(sfi), flags);
    return sfi.iIcon;
}

struct ThreadArgs
{
    CWipeTraceDlg* dlg;
    HWND hwnd;
};
}

CWipeTraceDlg::CWipeTraceDlg(const std::vector<CString>& initialPaths, CWnd* pParent)
    : CDialogEx(IDD_WIPETRACE_DIALOG, pParent), m_initialPaths(initialPaths)
{
    m_hIcon = AfxGetApp()->LoadIcon(IDR_MAINFRAME);
}

void CWipeTraceDlg::DoDataExchange(CDataExchange* pDX)
{
    CDialogEx::DoDataExchange(pDX);
    DDX_Control(pDX, IDC_TARGETS, m_list);
    DDX_Control(pDX, IDC_TREE, m_tree);
    DDX_Control(pDX, IDC_DRIVE, m_drives);
    DDX_Control(pDX, IDC_PASSES, m_passes);
    DDX_Control(pDX, IDC_THREADSPIN, m_spin);
    DDX_Control(pDX, IDC_PROGRESS, m_progress);
}

BEGIN_MESSAGE_MAP(CWipeTraceDlg, CDialogEx)
    ON_BN_CLICKED(IDC_ADDFILES, &CWipeTraceDlg::OnAddFiles)
    ON_BN_CLICKED(IDC_ADDFOLDER, &CWipeTraceDlg::OnAddFolder)
    ON_BN_CLICKED(IDC_REMOVE, &CWipeTraceDlg::OnRemove)
    ON_BN_CLICKED(IDC_CLEAR, &CWipeTraceDlg::OnClear)
    ON_BN_CLICKED(IDC_WIPE, &CWipeTraceDlg::OnWipe)
    ON_BN_CLICKED(IDC_STOP, &CWipeTraceDlg::OnStop)
    ON_BN_CLICKED(IDC_SHELL, &CWipeTraceDlg::OnShellToggle)
    ON_NOTIFY(LVN_KEYDOWN, IDC_TARGETS, &CWipeTraceDlg::OnListKeyDown)
    ON_WM_DROPFILES()
    ON_WM_COPYDATA()
    ON_WM_TIMER()
    ON_WM_DESTROY()
    ON_WM_SIZE()
    ON_WM_GETMINMAXINFO()
    ON_MESSAGE(WM_FIRST_RUN, &CWipeTraceDlg::OnFirstRun)
    ON_MESSAGE(WM_PLAN_DONE, &CWipeTraceDlg::OnPlanDone)
    ON_MESSAGE(WM_WIPE_DONE, &CWipeTraceDlg::OnWipeDone)
    ON_MESSAGE(WM_FREE_DONE, &CWipeTraceDlg::OnFreeDone)
    ON_CBN_SELCHANGE(IDC_DRIVE, &CWipeTraceDlg::OnDriveChange)
    ON_BN_CLICKED(IDC_WIPEFREE, &CWipeTraceDlg::OnWipeFree)
    ON_BN_CLICKED(IDC_ADDSEL, &CWipeTraceDlg::OnAddSelected)
    ON_NOTIFY(TVN_ITEMEXPANDING, IDC_TREE, &CWipeTraceDlg::OnTreeExpanding)
    ON_NOTIFY(NM_DBLCLK, IDC_TREE, &CWipeTraceDlg::OnTreeDblClick)
    ON_NOTIFY(TVN_KEYDOWN, IDC_TREE, &CWipeTraceDlg::OnTreeKeyDown)
    ON_WM_CONTEXTMENU()
    ON_WM_DEVICECHANGE()
END_MESSAGE_MAP()

BOOL CWipeTraceDlg::OnInitDialog()
{
    CDialogEx::OnInitDialog();
    SetIcon(m_hIcon, TRUE);
    SetIcon(m_hIcon, FALSE);
    SetPropW(GetSafeHwnd(), kMainWindowProp, (HANDLE)1);   // lets later instances find this window
    ChangeWindowMessageFilterEx(GetSafeHwnd(), WM_COPYDATA, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(GetSafeHwnd(), WM_DROPFILES, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(GetSafeHwnd(), 0x0049 /* WM_COPYGLOBALDATA */, MSGFLT_ALLOW, nullptr);

    GetClientRect(&m_initClient);
    GetWindowRect(&m_initWindow);
    const std::pair<UINT, int> anchors[] = {
        { IDC_ADDFILES, A_MOVEX }, { IDC_ADDFOLDER, A_MOVEX }, { IDC_REMOVE, A_MOVEX }, { IDC_CLEAR, A_MOVEX },
        { IDC_TARGETS, A_SIZEX | A_SIZEY }, { IDC_TREE, A_SIZEY }, { IDC_ADDSEL, A_MOVEY },
        { IDC_DRIVE, A_SIZEX }, { IDC_WIPEFREE, A_MOVEX }, { IDC_CLEANMFT, A_MOVEY },
        { IDC_OPTIONS, A_MOVEY | A_SIZEX }, { IDC_STATIC_PASSES, A_MOVEY }, { IDC_PASSES, A_MOVEY },
        { IDC_SCRAMBLE, A_MOVEY }, { IDC_STATIC_THREADS, A_MOVEY }, { IDC_THREADS, A_MOVEY },
        { IDC_THREADSPIN, A_MOVEY }, { IDC_SHELLSTATE, A_MOVEY }, { IDC_SHELL, A_MOVEX | A_MOVEY },
        { IDC_PROGRESS, A_MOVEY | A_SIZEX }, { IDC_NOTE, A_MOVEY | A_SIZEX }, { IDC_STATUS, A_MOVEY | A_SIZEX },
        { IDC_STOP, A_MOVEX | A_MOVEY }, { IDC_WIPE, A_MOVEX | A_MOVEY }, { IDCANCEL, A_MOVEX | A_MOVEY },
    };
    for (auto& a : anchors)
    {
        CRect rc;
        GetDlgItem(a.first)->GetWindowRect(&rc);
        ScreenToClient(&rc);
        m_anchors.push_back({ a.first, rc, a.second });
    }

    m_list.SetExtendedStyle(LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    m_list.InsertColumn(COL_NAME, L"Name", LVCFMT_LEFT, 170);
    m_list.InsertColumn(COL_LOCATION, L"Location", LVCFMT_LEFT, 260);
    m_list.InsertColumn(COL_TYPE, L"Type", LVCFMT_LEFT, 55);
    m_list.InsertColumn(COL_SIZE, L"Size", LVCFMT_RIGHT, 110);
    m_list.InsertColumn(COL_STATUS, L"Status", LVCFMT_LEFT, 240);

    m_passes.AddString(L"1 pass of random data (recommended)");
    m_passes.AddString(L"3 passes of random data");
    m_passes.AddString(L"7 passes of random data");
    m_passes.SetCurSel(0);
    CheckDlgButton(IDC_SCRAMBLE, BST_CHECKED);
    unsigned cores = std::thread::hardware_concurrency();
    m_spin.SetRange32(1, 16);
    m_spin.SetPos32((int)(std::min)(4u, (std::max)(1u, cores)));

    CheckDlgButton(IDC_CLEANMFT, BST_CHECKED);
    SHFILEINFOW sfi = {};
    auto sysImages = (HIMAGELIST)SHGetFileInfoW(L"C:\\", 0, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
    if (sysImages)
        TreeView_SetImageList(m_tree.GetSafeHwnd(), sysImages, TVSIL_NORMAL);   // shared; never destroyed
    RefreshDrives();

    m_progress.SetRange32(0, 1000);
    UpdateShellState();
    AddPaths(m_initialPaths);
    UpdateCounts();
    PostMessage(WM_FIRST_RUN);
    return TRUE;
}

// ---------------------------------------------------------------- target list

void CWipeTraceDlg::AddPaths(const std::vector<CString>& paths)
{
    int added = 0, skipped = 0;
    for (const CString& raw : paths)
    {
        wchar_t full[32768];
        if (!GetFullPathNameW(raw, _countof(full), full, nullptr))
            continue;
        CStringW path = ResolvePath(full);
        if (path.GetLength() > 3)
            path.TrimRight(L'\\');

        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (!GetFileAttributesExW(LongPath(path), GetFileExInfoStandard, &fa))
        {
            ++skipped;
            continue;
        }
        Target t = { path, (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
                     ((ULONGLONG)fa.nFileSizeHigh << 32) | fa.nFileSizeLow };

        // Skip duplicates and items already inside a listed folder; a new folder absorbs listed items inside it.
        bool covered = false;
        for (const Target& o : m_targets)
            if (o.path.CompareNoCase(t.path) == 0 ||
                (o.isDir && t.path.GetLength() > o.path.GetLength() &&
                 _wcsnicmp(t.path, o.path + L"\\", o.path.GetLength() + 1) == 0))
                covered = true;
        if (covered)
        {
            ++skipped;
            continue;
        }
        if (t.isDir && !m_busy)     // rows must not shift while a run is using them
        {
            for (int i = (int)m_targets.size() - 1; i >= 0; --i)
                if (m_targets[i].path.GetLength() > t.path.GetLength() &&
                    _wcsnicmp(m_targets[i].path, t.path + L"\\", t.path.GetLength() + 1) == 0)
                {
                    m_targets.erase(m_targets.begin() + i);
                    m_list.DeleteItem(i);
                }
        }
        m_targets.push_back(t);
        AddRow(t);
        CStringW refused = WipeEngine::CheckTarget(t.path);
        if (!refused.IsEmpty())
            SetRowStatus((int)m_targets.size() - 1, L"Will be refused: " + refused);
        ++added;
    }
    if (!m_busy)
    {
        UpdateCounts();
        if (skipped)
        {
            CString s;
            s.Format(L"Added %d item(s); %d skipped (already listed, inside a listed folder, or not found).", added, skipped);
            SetStatus(s);
        }
    }
}

void CWipeTraceDlg::AddRow(const Target& t)
{
    int row = m_list.GetItemCount();
    int slash = t.path.ReverseFind(L'\\');
    m_list.InsertItem(row, t.path.Mid(slash + 1));
    m_list.SetItemText(row, COL_LOCATION, t.path.Left(slash > 2 ? slash : slash + 1));
    m_list.SetItemText(row, COL_TYPE, t.isDir ? L"Folder" : L"File");
    m_list.SetItemText(row, COL_SIZE, t.isDir ? L"(counted on delete)" : (LPCWSTR)FormatBytes(t.size));
    m_list.SetItemText(row, COL_STATUS, L"Ready");
}

void CWipeTraceDlg::SetRowStatus(int row, const CString& status)
{
    if (row >= 0 && row < m_list.GetItemCount())
        m_list.SetItemText(row, COL_STATUS, status);
}

void CWipeTraceDlg::UpdateCounts()
{
    CString s;
    if (m_targets.empty())
        s = L"Add files or folders, then click Securely Delete.";
    else
        s.Format(L"%zu item(s) listed. Click Securely Delete to wipe them.", m_targets.size());
    SetStatus(s);
}

void CWipeTraceDlg::OnAddFiles()
{
    CFileDialog dlg(TRUE, nullptr, nullptr, OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY,
                    L"All files (*.*)|*.*||", this);
    std::vector<wchar_t> buf(64 * 1024);
    dlg.m_ofn.lpstrFile = buf.data();
    dlg.m_ofn.nMaxFile = (DWORD)buf.size();
    if (dlg.DoModal() != IDOK)
        return;
    std::vector<CString> paths;
    POSITION pos = dlg.GetStartPosition();
    while (pos)
        paths.push_back(dlg.GetNextPathName(pos));
    AddPaths(paths);
}

void CWipeTraceDlg::OnAddFolder()
{
    CFolderPickerDialog dlg(nullptr, OFN_ALLOWMULTISELECT, this);
    if (dlg.DoModal() != IDOK)
        return;
    std::vector<CString> paths;
    POSITION pos = dlg.GetStartPosition();
    while (pos)
        paths.push_back(dlg.GetNextPathName(pos));
    AddPaths(paths);
}

void CWipeTraceDlg::OnRemove()
{
    if (m_busy)
        return;
    for (int i = m_list.GetItemCount() - 1; i >= 0; --i)
        if (m_list.GetItemState(i, LVIS_SELECTED))
        {
            m_list.DeleteItem(i);
            m_targets.erase(m_targets.begin() + i);
        }
    UpdateCounts();
}

void CWipeTraceDlg::OnClear()
{
    if (m_busy)
        return;
    m_list.DeleteAllItems();
    m_targets.clear();
    UpdateCounts();
}

void CWipeTraceDlg::OnListKeyDown(NMHDR* pNMHDR, LRESULT* pResult)
{
    if (reinterpret_cast<NMLVKEYDOWN*>(pNMHDR)->wVKey == VK_DELETE)
        OnRemove();
    *pResult = 0;
}

void CWipeTraceDlg::OnDropFiles(HDROP hDrop)
{
    std::vector<CString> paths;
    UINT n = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < n; ++i)
    {
        UINT len = DragQueryFileW(hDrop, i, nullptr, 0);
        CString p;
        DragQueryFileW(hDrop, i, p.GetBuffer(len + 1), len + 1);
        p.ReleaseBuffer();
        paths.push_back(p);
    }
    DragFinish(hDrop);
    AddPaths(paths);
}

BOOL CWipeTraceDlg::OnCopyData(CWnd*, COPYDATASTRUCT* cds)
{
    // Paths handed over by another WipeTrace process (Explorer starts one per selected item).
    if (!cds || cds->dwData != kCopyDataPaths || !cds->lpData || cds->cbData < sizeof(wchar_t))
        return FALSE;
    CString all((LPCWSTR)cds->lpData, (int)(cds->cbData / sizeof(wchar_t)) - 1);
    std::vector<CString> paths;
    int pos = 0;
    CString tok = all.Tokenize(L"\n", pos);
    while (!tok.IsEmpty())
    {
        paths.push_back(tok);
        tok = all.Tokenize(L"\n", pos);
    }
    AddPaths(paths);
    if (IsIconic())
        ShowWindow(SW_RESTORE);
    SetForegroundWindow();
    return TRUE;
}

// ---------------------------------------------------------------- shell integration

void CWipeTraceDlg::UpdateShellState()
{
    bool installed = ShellIntegration::IsInstalled();
    SetDlgItemText(IDC_SHELLSTATE, installed ? L"Explorer right-click menu: added"
                                             : L"Explorer right-click menu: not added");
    SetDlgItemText(IDC_SHELL, installed ? L"Remove from menu" : L"Add to right-click menu");
}

void CWipeTraceDlg::OnShellToggle()
{
    CString err;
    bool ok = ShellIntegration::IsInstalled() ? ShellIntegration::Uninstall(err) : ShellIntegration::Install(err);
    if (!ok)
        AfxMessageBox(err, MB_ICONERROR);
    UpdateShellState();
}

LRESULT CWipeTraceDlg::OnFirstRun(WPARAM, LPARAM)
{
    // First start: offer the right-click menu entry once.
    if (ShellIntegration::WasPromptShown())
        return 0;
    ShellIntegration::SetPromptShown();
    if (ShellIntegration::IsInstalled())
        return 0;
    if (AfxMessageBox(L"Add \"Securely delete with WipeTrace\" to the Explorer right-click menu for files and folders?\n\n"
                      L"On Windows 11 it appears under \"Show more options\". You can change this later in Options.",
                      MB_YESNO | MB_ICONQUESTION) == IDYES)
    {
        CString err;
        if (!ShellIntegration::Install(err))
            AfxMessageBox(err, MB_ICONERROR);
    }
    UpdateShellState();
    return 0;
}

// ---------------------------------------------------------------- wipe run

WipeOptions CWipeTraceDlg::ReadOptions() const
{
    WipeOptions o;
    int sel = m_passes.GetCurSel();
    o.passes = kPasses[sel >= 0 && sel < (int)_countof(kPasses) ? sel : 0];
    o.scrambleNames = IsDlgButtonChecked(IDC_SCRAMBLE) == BST_CHECKED;
    o.threads = (std::max)(1, (std::min)(16, (int)m_spin.GetPos32()));
    return o;
}

int CWipeTraceDlg::RowFor(const CStringW& path) const
{
    for (int i = 0; i < (int)m_targets.size(); ++i)
        if (m_targets[i].path.CompareNoCase(path) == 0)
            return i;
    return -1;
}

void CWipeTraceDlg::OnWipe()
{
    if (m_busy)
        return;
    std::vector<CStringW> paths;
    for (const Target& t : m_targets)
        paths.push_back(t.path);
    if (paths.empty())
    {
        AfxMessageBox(L"Add the files or folders you want to delete first.", MB_ICONINFORMATION);
        return;
    }
    StartWipe(paths);
}

void CWipeTraceDlg::StartWipe(const std::vector<CStringW>& paths)
{
    // Step 1 of a run: list everything (in the background, nothing changes); OnPlanDone asks to confirm.
    m_runPaths = paths;
    m_runOptions = ReadOptions();
    m_runKind = RunKind::Items;
    m_cancel = false;
    SetBusy(true, L"Counting files and folders (nothing is changed yet)...");

    auto* args = new ThreadArgs{ this, GetSafeHwnd() };
    if (!AfxBeginThread([](LPVOID p) -> UINT
        {
            std::unique_ptr<ThreadArgs> a(static_cast<ThreadArgs*>(p));
            WipeEngine::BuildPlan(a->dlg->m_runPaths, a->dlg->m_plan, a->dlg->m_cancel);
            ::PostMessage(a->hwnd, WM_PLAN_DONE, 0, 0);
            return 0;
        }, args))
    {
        delete args;
        SetBusy(false, L"Could not start the worker thread.");
    }
}

LRESULT CWipeTraceDlg::OnPlanDone(WPARAM, LPARAM)
{
    if (m_closePending)
    {
        SetBusy(false, L"");
        EndDialog(IDCANCEL);
        return 0;
    }
    if (m_cancel)
    {
        SetBusy(false, L"Stopped. Nothing was changed.");
        return 0;
    }

    size_t refused = 0, cloud = 0;
    for (const WipeTask& t : m_plan.files)
        if (t.isCloud)
            ++cloud;
    CStringW firstRefusal;
    for (size_t r = 0; r < m_runPaths.size(); ++r)
    {
        int row = RowFor(m_runPaths[r]);    // -1 when started from the tree for an unlisted item
        if (!m_plan.rootErrors[r].IsEmpty())
        {
            ++refused;
            SetRowStatus(row, m_plan.rootErrors[r]);
            if (firstRefusal.IsEmpty())
                firstRefusal = m_runPaths[r] + L"\n" + m_plan.rootErrors[r];
            continue;
        }
        if (row >= 0 && m_targets[row].isDir)
        {
            CString size;
            size.Format(L"%s in %zu file(s)", (LPCWSTR)FormatBytes(m_plan.rootBytes[r]), m_plan.rootFiles[r]);
            m_list.SetItemText(row, COL_SIZE, size);
        }
        SetRowStatus(row, L"Ready");
    }

    if (m_plan.files.empty() && m_plan.dirs.empty())
    {
        SetBusy(false, L"Nothing was deleted: the item(s) were refused or not found.");
        AfxMessageBox(L"Nothing was deleted.\n\n" + CString(firstRefusal), MB_ICONINFORMATION);
        return 0;
    }

    CString msg;
    msg.Format(L"Permanently delete %zu file(s) and %zu folder(s) (%s)?\n\n"
               L"Each file is overwritten with random data (%d pass%s)%s, then deleted.\n"
               L"This cannot be undone - recovery tools will not get it back.",
               m_plan.files.size(), m_plan.dirs.size(), (LPCWSTR)FormatBytes(m_plan.totalBytes),
               m_runOptions.passes, m_runOptions.passes == 1 ? L"" : L"es",
               m_runOptions.scrambleNames ? L" and its name scrambled" : L"");
    if (refused)
    {
        CString r;
        r.Format(L"\n\n%zu listed item(s) were refused and will be left alone (see Status).", refused);
        msg += r;
    }
    if (cloud)
    {
        CString c;
        c.Format(L"\n\n%zu file(s) are OneDrive/cloud files. They are deleted without overwriting, because "
                 L"overwriting would upload random data to the cloud. Empty the cloud recycle bin as well.", cloud);
        msg += c;
    }
    if (AfxMessageBox(msg, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
    {
        SetBusy(false, L"Cancelled. Nothing was changed.");
        return 0;
    }

    m_engine.bytesDone = 0;
    m_engine.itemsDone = 0;
    SetStatus(L"Wiping...");
    SetTimer(kProgressTimer, 200, nullptr);

    auto* args = new ThreadArgs{ this, GetSafeHwnd() };
    if (!AfxBeginThread([](LPVOID p) -> UINT
        {
            std::unique_ptr<ThreadArgs> a(static_cast<ThreadArgs*>(p));
            auto* results = new std::vector<RootResult>;
            a->dlg->m_engine.Run(a->dlg->m_plan, a->dlg->m_runOptions, a->dlg->m_cancel, *results);
            ::PostMessage(a->hwnd, WM_WIPE_DONE, 0, (LPARAM)results);
            return 0;
        }, args))
    {
        delete args;
        KillTimer(kProgressTimer);
        SetBusy(false, L"Could not start the worker thread.");
    }
    return 0;
}

void CWipeTraceDlg::OnTimer(UINT_PTR nIDEvent)
{
    if (nIDEvent != kProgressTimer)
    {
        CDialogEx::OnTimer(nIDEvent);
        return;
    }
    if (m_runKind == RunKind::FreeSpace)
    {
        CString s;
        ULONGLONG written = m_freeProgress.written, total = m_freeProgress.freeAtStart;
        switch (m_freeProgress.phase)
        {
        case 1:
            m_progress.SetPos(0);
            s.Format(L"Step 1 of 2: overwriting entries left by deleted files on %s... %llu done.",
                     (LPCWSTR)m_freeRoot.Left(2), (ULONGLONG)m_freeProgress.entries);
            break;
        case 2:
            m_progress.SetPos(total ? (int)((std::min)(written, total) * 1000 / total) : 0);
            s.Format(L"Step 2 of 2: filling free space on %s with random data... %s of %s.",
                     (LPCWSTR)m_freeRoot.Left(2), (LPCWSTR)FormatBytes(written), (LPCWSTR)FormatBytes(total));
            break;
        case 3:
            s = L"Removing the temporary files...";
            break;
        }
        if (!s.IsEmpty())
            SetStatus(s);
        return;
    }
    size_t items = m_plan.files.size() + m_plan.dirs.size();
    ULONGLONG total = m_plan.totalBytes * (ULONGLONG)m_runOptions.passes;
    ULONGLONG done = std::min<ULONGLONG>(m_engine.bytesDone, total);
    size_t itemsDone = m_engine.itemsDone;
    int pm = total ? (int)(done * 1000 / total) : (items ? (int)(itemsDone * 1000 / items) : 0);
    m_progress.SetPos(pm);
    CString s;
    s.Format(L"Wiping with %d thread(s)... %zu of %zu item(s), %s of %s written.", m_runOptions.threads,
             itemsDone, items, (LPCWSTR)FormatBytes(done), (LPCWSTR)FormatBytes(total));
    SetStatus(s);
}

LRESULT CWipeTraceDlg::OnWipeDone(WPARAM, LPARAM lParam)
{
    std::unique_ptr<std::vector<RootResult>> results(reinterpret_cast<std::vector<RootResult>*>(lParam));
    KillTimer(kProgressTimer);
    m_runKind = RunKind::None;
    bool cancelled = m_cancel;
    SetBusy(false, L"");
    if (m_closePending)
    {
        EndDialog(IDCANCEL);
        return 0;
    }
    m_progress.SetPos(1000);

    int files = 0, dirs = 0, failed = 0, notOverwritten = 0;
    std::vector<int> finished;
    for (size_t r = 0; r < results->size(); ++r)
    {
        const RootResult& res = (*results)[r];
        files += res.files;
        dirs += res.dirs;
        failed += res.failed;
        notOverwritten += res.notOverwritten;
        if (!m_plan.rootErrors[r].IsEmpty())
            continue;   // refused: status already shows why

        // Rows may have moved if items were added during the run; find this one by path.
        int row = RowFor(m_runPaths[r]);
        RefreshTreeFolderOf(m_runPaths[r]);
        CString s;
        if (res.failed)
            s.Format(L"%d failed - %s", res.failed, (LPCWSTR)res.firstError);
        else if (cancelled && res.files + res.dirs < (int)(m_plan.rootFiles[r] + m_plan.rootDirs[r]))
            s = L"Stopped before finishing";
        else
        {
            s = L"Securely deleted";
            if (res.notOverwritten)
            {
                CString n;
                n.Format(L" (%d link/cloud item(s) removed without overwrite)", res.notOverwritten);
                s += n;
            }
            finished.push_back(row);
        }
        SetRowStatus(row, s);
    }
    // Fully deleted items leave the list; failed ones stay so they can be retried.
    std::sort(finished.begin(), finished.end());
    for (auto it = finished.rbegin(); it != finished.rend(); ++it)
        if (*it >= 0)
        {
            m_list.DeleteItem(*it);
            m_targets.erase(m_targets.begin() + *it);
        }

    CString s;
    s.Format(L"%s%d file(s) and %d folder(s) securely deleted.", cancelled ? L"Stopped. " : L"", files, dirs);
    if (failed)
    {
        CString f;
        f.Format(L" %d item(s) could not be deleted - see Status. Files open in another program, or that need "
                 L"administrator rights, cannot be deleted.", failed);
        s += f;
    }
    SetStatus(s);
    AfxMessageBox(s, failed ? MB_ICONWARNING : MB_ICONINFORMATION);
    return 0;
}

void CWipeTraceDlg::OnStop()
{
    m_cancel = true;
    SetStatus(L"Stopping after the current chunk...");
}

// ---------------------------------------------------------------- drives and browse tree

void CWipeTraceDlg::RefreshDrives()
{
    CString prev;
    int sel = m_drives.GetCurSel();
    if (sel >= 0 && sel < (int)m_volumes.size())
        prev = m_volumes[sel].root;

    m_volumes = EnumerateVolumes();
    m_drives.ResetContent();
    sel = -1;
    for (int i = 0; i < (int)m_volumes.size(); ++i)
    {
        const VolumeInfo& v = m_volumes[i];
        m_drives.AddString(v.Describe() + (v.isSsd ? L"  (SSD/flash)" : L""));
        if (v.root.CompareNoCase(prev) == 0 || (sel < 0 && prev.IsEmpty() && !v.isSystem))
            sel = i;
    }
    if (sel < 0 && !m_volumes.empty())
        sel = 0;
    m_drives.SetCurSel(sel);
    ResetTree();
}

HTREEITEM CWipeTraceDlg::InsertTreeItem(HTREEITEM parent, const CStringW& label, int node, int image, bool hasKids)
{
    TVINSERTSTRUCTW tis = {};
    tis.hParent = parent;
    tis.hInsertAfter = TVI_LAST;
    tis.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_IMAGE | TVIF_SELECTEDIMAGE | TVIF_CHILDREN;
    tis.item.pszText = const_cast<LPWSTR>((LPCWSTR)label);
    tis.item.iImage = tis.item.iSelectedImage = image;
    tis.item.cChildren = hasKids ? 1 : 0;
    tis.item.lParam = node;
    HTREEITEM item = m_tree.InsertItem(&tis);
    m_nodes[node].item = item;
    return item;
}

void CWipeTraceDlg::ResetTree()
{
    m_tree.DeleteAllItems();
    m_nodes.clear();
    HTREEITEM selItem = nullptr;
    int sel = m_drives.GetCurSel();
    for (int i = 0; i < (int)m_volumes.size(); ++i)
    {
        const VolumeInfo& v = m_volumes[i];
        CStringW label;
        label.Format(L"%s (%s)", v.label.IsEmpty() ? L"Local Disk" : (LPCWSTR)v.label, (LPCWSTR)v.root.Left(2));
        m_nodes.push_back({ v.root, true, false });
        HTREEITEM item = InsertTreeItem(TVI_ROOT, label, (int)m_nodes.size() - 1, SysIconIndex(v.root, 0, false), true);
        if (i == sel)
            selItem = item;
    }
    if (selItem)
    {
        m_tree.SelectItem(selItem);
        m_tree.Expand(selItem, TVE_EXPAND);
    }
}

void CWipeTraceDlg::PopulateNode(int node)
{
    if (node < 0 || node >= (int)m_nodes.size() || m_nodes[node].populated || !m_nodes[node].isDir)
        return;
    m_nodes[node].populated = true;
    CStringW dir = m_nodes[node].path;
    if (dir.Right(1) != L"\\")
        dir += L"\\";

    HTREEITEM parent = m_nodes[node].item;
    if (!parent)
        return;

    struct Entry { CStringW name; DWORD attrs; };
    std::vector<Entry> dirs, files;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(LongPath(dir + L"*"), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
                continue;
            ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? dirs : files).push_back({ fd.cFileName, fd.dwFileAttributes });
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    auto byName = [](const Entry& a, const Entry& b) { return a.name.CompareNoCase(b.name) < 0; };
    std::sort(dirs.begin(), dirs.end(), byName);
    std::sort(files.begin(), files.end(), byName);

    m_tree.SetRedraw(FALSE);
    for (const Entry& e : dirs)
    {
        // Links (junctions, symbolic links) are listed but not opened: adding one removes just the link.
        bool link = (e.attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        m_nodes.push_back({ dir + e.name, true, link });
        InsertTreeItem(parent, link ? e.name + L"  [link]" : e.name, (int)m_nodes.size() - 1,
                       SysIconIndex(e.name, FILE_ATTRIBUTE_DIRECTORY, true), !link);
    }
    for (const Entry& e : files)
    {
        m_nodes.push_back({ dir + e.name, false, true });
        InsertTreeItem(parent, e.name, (int)m_nodes.size() - 1, SysIconIndex(e.name, FILE_ATTRIBUTE_NORMAL, true), false);
    }
    if (dirs.empty() && files.empty())
    {
        TVITEMW ti = {};
        ti.mask = TVIF_HANDLE | TVIF_CHILDREN;
        ti.hItem = parent;
        m_tree.SetItem(&ti);
    }
    m_tree.SetRedraw(TRUE);
}

void CWipeTraceDlg::OnTreeExpanding(NMHDR* pNMHDR, LRESULT* pResult)
{
    auto* tv = reinterpret_cast<NMTREEVIEWW*>(pNMHDR);
    if (tv->action == TVE_EXPAND)
    {
        CWaitCursor wait;
        PopulateNode((int)tv->itemNew.lParam);
    }
    *pResult = 0;
}

void CWipeTraceDlg::OnTreeDblClick(NMHDR*, LRESULT* pResult)
{
    // Double-clicking a file adds it; double-clicking a folder just opens it, as in Explorer.
    *pResult = 0;
    HTREEITEM item = m_tree.GetSelectedItem();
    if (!item)
        return;
    int node = (int)m_tree.GetItemData(item);
    if (node >= 0 && node < (int)m_nodes.size() && !m_nodes[node].isDir && !m_busy)
    {
        AddPaths({ CString(m_nodes[node].path) });
        *pResult = 1;
    }
}

void CWipeTraceDlg::OnAddSelected()
{
    HTREEITEM item = m_tree.GetSelectedItem();
    if (!item)
    {
        AfxMessageBox(L"Select a file or folder in the tree first.", MB_ICONINFORMATION);
        return;
    }
    int node = (int)m_tree.GetItemData(item);
    if (node >= 0 && node < (int)m_nodes.size())
        AddPaths({ CString(m_nodes[node].path) });
}

int CWipeTraceDlg::SelectedNode() const
{
    HTREEITEM item = m_tree.GetSelectedItem();
    int node = item ? (int)m_tree.GetItemData(item) : -1;
    return node >= 0 && node < (int)m_nodes.size() ? node : -1;
}

void CWipeTraceDlg::OnContextMenu(CWnd* pWnd, CPoint point)
{
    if (pWnd->GetSafeHwnd() != m_tree.GetSafeHwnd())
        return;
    if (point.x == -1 && point.y == -1)
    {
        // Menu key or Shift+F10: open the menu at the selected item.
        HTREEITEM item = m_tree.GetSelectedItem();
        CRect rc;
        if (!item || !m_tree.GetItemRect(item, &rc, TRUE))
            return;
        point = CPoint(rc.left, rc.bottom);
        m_tree.ClientToScreen(&point);
        ShowTreeMenu(point, true);
        return;
    }
    // Right-click: act on the item under the mouse, and select it so it is clear which one.
    CPoint client = point;
    m_tree.ScreenToClient(&client);
    UINT flags = 0;
    HTREEITEM item = m_tree.HitTest(client, &flags);
    if (!item || !(flags & TVHT_ONITEM))
        return;
    m_tree.SelectItem(item);
    ShowTreeMenu(point, false);
}

void CWipeTraceDlg::ShowTreeMenu(CPoint screenPt, bool)
{
    int node = SelectedNode();
    if (node < 0)
        return;
    const CStringW path = m_nodes[node].path;     // copy: the menu actions may change m_nodes
    bool isDrive = path.GetLength() <= 3;
    UINT busy = m_busy ? MF_GRAYED : 0;

    CMenu menu;
    menu.CreatePopupMenu();
    if (isDrive)
        menu.AppendMenu(MF_STRING | busy, ID_TREE_FREESPACE, L"Wipe &Free Space on This Drive...");
    else
    {
        menu.AppendMenu(MF_STRING | busy, ID_TREE_WIPE, L"&Securely Delete...\tDel");
        menu.AppendMenu(MF_STRING | busy, ID_TREE_ADD, L"&Add to List");
    }
    menu.AppendMenu(MF_SEPARATOR);
    menu.AppendMenu(MF_STRING, ID_TREE_SHOW, isDrive ? L"Open in E&xplorer" : L"Show in E&xplorer");

    UINT cmd = menu.TrackPopupMenu(TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN, screenPt.x, screenPt.y, this);
    switch (cmd)
    {
    case ID_TREE_WIPE:
    {
        // Wipe just this item (it is also listed, so its result shows in Status); the confirmation still comes first.
        CStringW real = ResolvePath(path);
        if (RowFor(real) < 0)
            AddPaths({ CString(real) });
        StartWipe({ real });
        break;
    }
    case ID_TREE_ADD:
        AddPaths({ CString(path) });
        break;
    case ID_TREE_SHOW:
        if (isDrive)
            ShellExecuteW(GetSafeHwnd(), L"open", path, nullptr, nullptr, SW_SHOWNORMAL);
        else
            ShellExecuteW(GetSafeHwnd(), L"open", L"explorer.exe", L"/select,\"" + path + L"\"", nullptr, SW_SHOWNORMAL);
        break;
    case ID_TREE_FREESPACE:
        for (int i = 0; i < (int)m_volumes.size(); ++i)
            if (m_volumes[i].root.CompareNoCase(path) == 0)
                m_drives.SetCurSel(i);
        OnWipeFree();
        break;
    }
}

void CWipeTraceDlg::OnTreeKeyDown(NMHDR* pNMHDR, LRESULT* pResult)
{
    *pResult = 0;
    if (reinterpret_cast<NMTVKEYDOWN*>(pNMHDR)->wVKey != VK_DELETE || m_busy)
        return;
    int node = SelectedNode();
    if (node < 0 || m_nodes[node].path.GetLength() <= 3)
        return;     // drives: use Wipe Free Space instead
    CStringW real = ResolvePath(m_nodes[node].path);
    if (RowFor(real) < 0)
        AddPaths({ CString(real) });
    StartWipe({ real });
}

void CWipeTraceDlg::RefreshTreeFolderOf(const CStringW& path)
{
    // After a wipe, re-list the folder that contained the item so the tree matches the disk again.
    int slash = path.ReverseFind(L'\\');
    if (slash < 2)
        return;
    CStringW parent = path.Left(slash);
    if (parent.GetLength() == 2)
        parent += L"\\";
    int node = -1;
    for (int i = 0; i < (int)m_nodes.size(); ++i)
        if (m_nodes[i].item && m_nodes[i].isDir && m_nodes[i].path.CompareNoCase(parent) == 0)
            node = i;
    if (node < 0 || !m_nodes[node].populated)
        return;

    HTREEITEM item = m_nodes[node].item;
    bool expanded = (m_tree.GetItemState(item, TVIS_EXPANDED) & TVIS_EXPANDED) != 0;
    CStringW prefix = parent.Right(1) == L"\\" ? parent : parent + L"\\";
    for (TreeNode& n : m_nodes)     // everything below goes; forget their (soon invalid) tree items
        if (n.path.GetLength() > prefix.GetLength() && _wcsnicmp(n.path, prefix, prefix.GetLength()) == 0)
            n.item = nullptr;
    m_tree.SetRedraw(FALSE);
    while (HTREEITEM child = m_tree.GetChildItem(item))
        m_tree.DeleteItem(child);
    m_nodes[node].populated = false;
    PopulateNode(node);
    if (expanded)
        m_tree.Expand(item, TVE_EXPAND);
    m_tree.SetRedraw(TRUE);
}

void CWipeTraceDlg::OnDriveChange()
{
    ResetTree();
}

BOOL CWipeTraceDlg::OnDeviceChange(UINT nEventType, DWORD_PTR)
{
    if ((nEventType == DBT_DEVICEARRIVAL || nEventType == DBT_DEVICEREMOVECOMPLETE) && !m_busy)
        RefreshDrives();
    return TRUE;
}

// ---------------------------------------------------------------- free-space wipe

void CWipeTraceDlg::OnWipeFree()
{
    if (m_busy)
        return;
    int sel = m_drives.GetCurSel();
    if (sel < 0 || sel >= (int)m_volumes.size())
    {
        AfxMessageBox(L"Choose a drive first.", MB_ICONINFORMATION);
        return;
    }
    const VolumeInfo v = m_volumes[sel];
    if (v.readOnly)
    {
        AfxMessageBox(L"This drive is read-only.", MB_ICONINFORMATION);
        return;
    }
    bool cleanEntries = IsDlgButtonChecked(IDC_CLEANMFT) == BST_CHECKED;

    CString msg;
    msg.Format(L"Overwrite all free space on %s (%s free) with random data?\n\n"
               L"Files you deleted earlier in the normal way can then no longer be recovered. Files that still exist "
               L"are not touched.",
               (LPCWSTR)v.root.Left(2), (LPCWSTR)FormatBytes(v.freeBytes));
    if (cleanEntries && v.IsNtfs())
        msg += L" It first overwrites the entries left by deleted files, which still hold their names.";
    msg += L"\n\nWhile it runs the drive is completely full for a while: close programs that save to " +
           v.root.Left(2) + L", as they may report \"disk full\" until it finishes. Temporary files are removed at the end.";
    if (v.isSystem)
        msg += L"\n\nThis is the Windows drive: Windows may briefly warn about low disk space.";
    if (v.isSsd)
        msg += L"\n\nThis is an SSD or flash drive. Wear levelling means some old data may survive in cells the "
               L"drive has set aside, and Windows' automatic TRIM already clears freed space on most SSDs. "
               L"Overwriting also adds wear.";
    if (AfxMessageBox(msg, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
        return;

    m_freeRoot = v.root;
    m_freeCleanEntries = cleanEntries;
    m_freeProgress.phase = 0;
    m_freeProgress.entries = 0;
    m_freeProgress.written = 0;
    m_freeProgress.freeAtStart = v.freeBytes;
    m_runKind = RunKind::FreeSpace;
    m_cancel = false;
    SetBusy(true, L"Starting free-space wipe of " + v.root.Left(2) + L"...");
    SetTimer(kProgressTimer, 250, nullptr);

    auto* args = new ThreadArgs{ this, GetSafeHwnd() };
    if (!AfxBeginThread([](LPVOID p) -> UINT
        {
            std::unique_ptr<ThreadArgs> a(static_cast<ThreadArgs*>(p));
            auto* result = new FreeSpaceResult;
            FreeSpaceWiper::Run(a->dlg->m_freeRoot, a->dlg->m_freeCleanEntries, a->dlg->m_cancel,
                                a->dlg->m_freeProgress, *result);
            ::PostMessage(a->hwnd, WM_FREE_DONE, 0, (LPARAM)result);
            return 0;
        }, args))
    {
        delete args;
        KillTimer(kProgressTimer);
        SetBusy(false, L"Could not start the worker thread.");
    }
}

LRESULT CWipeTraceDlg::OnFreeDone(WPARAM, LPARAM lParam)
{
    std::unique_ptr<FreeSpaceResult> r(reinterpret_cast<FreeSpaceResult*>(lParam));
    KillTimer(kProgressTimer);
    m_runKind = RunKind::None;
    SetBusy(false, L"");
    if (m_closePending)
    {
        EndDialog(IDCANCEL);
        return 0;
    }
    m_progress.SetPos(r->ok ? 1000 : 0);

    CString s, drive = m_freeRoot.Left(2);
    if (r->ok)
    {
        s.Format(L"Free space on %s wiped: %s of random data written.", (LPCWSTR)drive, (LPCWSTR)FormatBytes(r->written));
        if (r->entriesCleaned)
        {
            CString e;
            e.Format(L" %llu entries left by deleted files were overwritten.", r->entries);
            s += e;
        }
    }
    else if (r->cancelled)
        s = L"Free-space wipe stopped. Free space was only partly overwritten.";
    else
        s = L"Free-space wipe failed: " + CString(r->error);
    if (!r->leftover.IsEmpty())
        s += L"\n\nSome temporary files could not be removed. Delete this folder to get the space back:\n" +
             CString(r->leftover);
    SetStatus(s);
    RefreshDrives();    // free space figures changed
    AfxMessageBox(s, r->ok && r->leftover.IsEmpty() ? MB_ICONINFORMATION : MB_ICONWARNING);
    return 0;
}

// ---------------------------------------------------------------- misc

void CWipeTraceDlg::SetBusy(bool busy, const CString& status)
{
    m_busy = busy;
    const UINT ids[] = { IDC_ADDFILES, IDC_ADDFOLDER, IDC_REMOVE, IDC_CLEAR, IDC_WIPE, IDC_PASSES,
                         IDC_SCRAMBLE, IDC_THREADS, IDC_THREADSPIN, IDC_SHELL, IDC_DRIVE, IDC_WIPEFREE,
                         IDC_ADDSEL, IDC_CLEANMFT };
    for (UINT id : ids)
        GetDlgItem(id)->EnableWindow(!busy);
    GetDlgItem(IDC_STOP)->EnableWindow(busy);
    if (busy)
        m_progress.SetPos(0);
    if (!status.IsEmpty())
        SetStatus(status);
}

void CWipeTraceDlg::OnCancel()
{
    if (m_busy)
    {
        if (AfxMessageBox(L"Wiping is in progress. Stop and close? The file being wiped right now may be left "
                          L"partly overwritten.", MB_YESNO | MB_ICONQUESTION) == IDYES)
        {
            m_cancel = true;
            m_closePending = true;
        }
        return;
    }
    CDialogEx::OnCancel();
}

void CWipeTraceDlg::OnDestroy()
{
    RemovePropW(GetSafeHwnd(), kMainWindowProp);
    CDialogEx::OnDestroy();
}

void CWipeTraceDlg::OnSize(UINT nType, int cx, int cy)
{
    CDialogEx::OnSize(nType, cx, cy);
    if (m_anchors.empty() || nType == SIZE_MINIMIZED)
        return;
    int dx = cx - m_initClient.Width();
    int dy = cy - m_initClient.Height();
    HDWP hdwp = BeginDeferWindowPos((int)m_anchors.size());
    for (const Anchor& a : m_anchors)
    {
        CRect r = a.rc;
        if (a.flags & A_MOVEX) r.OffsetRect(dx, 0);
        if (a.flags & A_MOVEY) r.OffsetRect(0, dy);
        if (a.flags & A_SIZEX) r.right += dx;
        if (a.flags & A_SIZEY) r.bottom += dy;
        if (hdwp)
            hdwp = DeferWindowPos(hdwp, GetDlgItem(a.id)->GetSafeHwnd(), nullptr, r.left, r.top, r.Width(), r.Height(),
                                  SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
    }
    if (hdwp)
        EndDeferWindowPos(hdwp);
    // Repaint every control: wrapped labels must re-wrap, and moved labels must not leave old text behind.
    RedrawWindow(nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

void CWipeTraceDlg::OnGetMinMaxInfo(MINMAXINFO* lpMMI)
{
    if (!m_initWindow.IsRectEmpty())
    {
        lpMMI->ptMinTrackSize.x = m_initWindow.Width();
        lpMMI->ptMinTrackSize.y = m_initWindow.Height();
    }
    CDialogEx::OnGetMinMaxInfo(lpMMI);
}
