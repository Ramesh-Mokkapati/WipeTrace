// stdafx.h : precompiled header for WipeTrace
#pragma once

#ifndef VC_EXTRALEAN
#define VC_EXTRALEAN
#endif

#include <SDKDDKVer.h>

#define _ATL_CSTRING_EXPLICIT_CONSTRUCTORS

#include <afxwin.h>
#include <afxext.h>
#include <afxdialogex.h>
#include <afxcmn.h>

#include <winioctl.h>
#include <dbt.h>
#include <shellapi.h>
#include <shlobj.h>
#include <bcrypt.h>
#include <shlwapi.h>

#include <vector>
#include <string>
#include <atomic>
#include <mutex>
#include <thread>
#include <memory>
#include <algorithm>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
