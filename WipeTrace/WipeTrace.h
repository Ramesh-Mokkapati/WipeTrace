// WipeTrace.h : application class
#pragma once

#include "resource.h"

// WM_COPYDATA tag used when a second instance hands its paths to the running window.
const ULONG_PTR kCopyDataPaths = 0x57495045;   // 'WIPE'
// Window property that marks the main window, so later instances can find it.
const wchar_t kMainWindowProp[] = L"WipeTrace.MainWindow";

class CWipeTraceApp : public CWinApp
{
public:
    BOOL InitInstance() override;
    int  ExitInstance() override;
    DECLARE_MESSAGE_MAP()

private:
    HANDLE m_instanceMutex = nullptr;
};

extern CWipeTraceApp theApp;
