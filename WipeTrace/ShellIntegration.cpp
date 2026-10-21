#include "stdafx.h"
#include "ShellIntegration.h"

namespace
{
// AllFilesystemObjects covers both files and folders with one verb.
const wchar_t kVerbKey[]     = L"Software\\Classes\\AllFilesystemObjects\\shell\\WipeTrace";
const wchar_t kSettingsKey[] = L"Software\\WipeTrace";

CString ExePath()
{
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return buf;
}

bool SetString(HKEY key, LPCWSTR name, const CString& value)
{
    return RegSetValueExW(key, name, 0, REG_SZ, (const BYTE*)(LPCWSTR)value,
                          (DWORD)((value.GetLength() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}
}

namespace ShellIntegration
{
bool IsInstalled()
{
    HKEY key;
    CString path = CString(kVerbKey) + L"\\command";
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;
    wchar_t buf[2 * MAX_PATH] = {};
    DWORD size = sizeof(buf) - sizeof(wchar_t), type = 0;
    bool ok = RegQueryValueExW(key, nullptr, nullptr, &type, (BYTE*)buf, &size) == ERROR_SUCCESS;
    RegCloseKey(key);
    // Installed for *this* exe (it may have been moved since).
    return ok && StrStrIW(buf, ExePath()) != nullptr;
}

bool Install(CString& err)
{
    CString exe = ExePath();
    HKEY verb = nullptr, cmd = nullptr;
    LSTATUS s = RegCreateKeyExW(HKEY_CURRENT_USER, kVerbKey, 0, nullptr, 0, KEY_WRITE, nullptr, &verb, nullptr);
    if (s == ERROR_SUCCESS)
    {
        SetString(verb, nullptr, L"Securely delete with WipeTrace");
        SetString(verb, L"Icon", L"\"" + exe + L"\",0");
        s = RegCreateKeyExW(verb, L"command", 0, nullptr, 0, KEY_WRITE, nullptr, &cmd, nullptr);
        if (s == ERROR_SUCCESS)
        {
            // One process per selected item; they hand their paths to the first window (see WipeTrace.cpp).
            if (!SetString(cmd, nullptr, L"\"" + exe + L"\" \"%1\""))
                s = ERROR_WRITE_FAULT;
            RegCloseKey(cmd);
        }
        RegCloseKey(verb);
    }
    if (s != ERROR_SUCCESS)
    {
        err.Format(L"Could not write the registry (error %d).", (int)s);
        return false;
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return true;
}

bool Uninstall(CString& err)
{
    LSTATUS s = RegDeleteTreeW(HKEY_CURRENT_USER, kVerbKey);
    if (s != ERROR_SUCCESS && s != ERROR_FILE_NOT_FOUND)
    {
        err.Format(L"Could not remove the registry entry (error %d).", (int)s);
        return false;
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return true;
}

bool WasPromptShown()
{
    DWORD value = 0, size = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"ShellPromptShown", RRF_RT_REG_DWORD, nullptr,
                        &value, &size) == ERROR_SUCCESS && value;
}

void SetPromptShown()
{
    DWORD one = 1;
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"ShellPromptShown", REG_DWORD, &one, sizeof(one));
}
}
