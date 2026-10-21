// ShellIntegration.h : Explorer right-click menu entry for files and folders (per user, no admin needed).
#pragma once

namespace ShellIntegration
{
    bool IsInstalled();
    bool Install(CString& err);
    bool Uninstall(CString& err);

    // First-run bookkeeping, stored under HKCU\Software\WipeTrace.
    bool WasPromptShown();
    void SetPromptShown();
}
