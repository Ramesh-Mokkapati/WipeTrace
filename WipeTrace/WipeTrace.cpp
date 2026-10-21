// WipeTrace.cpp : application entry, command line and single-instance hand-off.
//
// Explorer starts one process per selected item. The first one opens the window; the others find it,
// pass their paths over WM_COPYDATA and exit, so a multi-selection ends up in a single list.

#include "stdafx.h"
#include "WipeTrace.h"
#include "WipeTraceDlg.h"
#include "ShellIntegration.h"

BEGIN_MESSAGE_MAP(CWipeTraceApp, CWinApp)
END_MESSAGE_MAP()

CWipeTraceApp theApp;

namespace
{
HWND FindMainWindow()
{
    HWND found = nullptr;
    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL
    {
        if (GetPropW(hwnd, kMainWindowProp))
        {
            *reinterpret_cast<HWND*>(lp) = hwnd;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&found));
    return found;
}

bool HandToRunningInstance(const std::vector<CString>& paths)
{
    CString joined;
    for (const CString& p : paths)
        joined += p + L"\n";

    // The first instance may still be starting (Explorer launches them together): wait up to 5 s.
    for (int i = 0; i < 50; ++i)
    {
        HWND wnd = FindMainWindow();
        if (wnd)
        {
            DWORD pid = 0;
            GetWindowThreadProcessId(wnd, &pid);
            AllowSetForegroundWindow(pid);
            COPYDATASTRUCT cds = { kCopyDataPaths, (DWORD)((joined.GetLength() + 1) * sizeof(wchar_t)),
                                   (PVOID)(LPCWSTR)joined };
            DWORD_PTR result = 0;
            return SendMessageTimeoutW(wnd, WM_COPYDATA, 0, (LPARAM)&cds, SMTO_ABORTIFHUNG, 5000, &result) != 0;
        }
        Sleep(100);
    }
    return false;
}
}

BOOL CWipeTraceApp::InitInstance()
{
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_WIN95_CLASSES | ICC_PROGRESS_CLASS | ICC_UPDOWN_CLASS };
    InitCommonControlsEx(&icc);
    CWinApp::InitInstance();

    // Command line: paths to wipe, or /install-shell, /uninstall-shell (for installers and scripts).
    std::vector<CString> paths;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i)
    {
        CString a = argv[i];
        CString err;
        if (a.CompareNoCase(L"/install-shell") == 0)
            return ShellIntegration::Install(err) ? FALSE : (AfxMessageBox(err, MB_ICONERROR), FALSE);
        if (a.CompareNoCase(L"/uninstall-shell") == 0)
            return ShellIntegration::Uninstall(err) ? FALSE : (AfxMessageBox(err, MB_ICONERROR), FALSE);
        if (!a.IsEmpty())
            paths.push_back(a);
    }
    if (argv)
        LocalFree(argv);

    m_instanceMutex = CreateMutexW(nullptr, FALSE, L"Local\\WipeTrace.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        if (paths.empty())
        {
            HWND wnd = FindMainWindow();
            if (wnd)
            {
                ShowWindow(wnd, SW_RESTORE);
                SetForegroundWindow(wnd);
                return FALSE;
            }
        }
        else if (HandToRunningInstance(paths))
            return FALSE;
        // Otherwise the other instance is gone or stuck: open our own window.
    }

    CWipeTraceDlg dlg(paths);
    m_pMainWnd = &dlg;
    dlg.DoModal();
    return FALSE;
}

int CWipeTraceApp::ExitInstance()
{
    if (m_instanceMutex)
        CloseHandle(m_instanceMutex);
    return CWinApp::ExitInstance();
}
