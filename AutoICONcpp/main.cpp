#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <wtsapi32.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <tchar.h>
#include <math.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <tlhelp32.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "shlwapi.lib")

#ifndef _countof
#define _countof(_Array) (sizeof(_Array) / sizeof(_Array[0]))
#endif

// ==========================================
// === 版本和配置 ===
// ==========================================
#define APP_NAME _T("AutoICON")
#define APP_VERSION _T("v10") // 版本号已更新
#define EXE_NAME _T("AutoICON.exe")
#define REG_UNINSTALL_KEY _T("Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\AutoICON")
#define REG_RUN_KEY _T("Software\\Microsoft\\Windows\\CurrentVersion\\Run")

// 全局路径变量
TCHAR g_szInstallDir[MAX_PATH] = { 0 };
TCHAR g_szInstallExePath[MAX_PATH] = { 0 };

// ==========================================
// === 物理引擎参数 (保持不变) ===
// ==========================================
struct SpringParams {
    float tension;
    float friction;
    bool  enabled;
};

const SpringParams SP_FAST = { 860.0f, 46.0f, true };
const SpringParams SP_NORMAL = { 400.0f, 32.0f, true };
const SpringParams SP_SLOW = { 12.0f,  5.0f, true };
const SpringParams SP_VERYSLOW = { 6.0f,  3.0f,  true };
const SpringParams SP_OFF = { 0.0f,   0.0f,  false };

// ==========================================
// === 配置结构 (保持不变) ===
// ==========================================
struct ConfigProfile {
    const TCHAR* name;
    ULONGLONG hideDelayMs;
    ULONGLONG idleCheckMs;
    SpringParams motionIn;
    SpringParams motionOut;
    SpringParams opacityIn;
    SpringParams opacityOut;
};

const ConfigProfile PRESETS[] = {
    { _T("渐变 (默认) - Fade (Default)"), 5000, 200, SP_OFF, SP_OFF, SP_NORMAL, SP_VERYSLOW },
    { _T("渐变 (快速) - Fade (Fast)"), 3000, 100, SP_OFF, SP_OFF, SP_FAST, SP_SLOW },
    { _T("抽屉 (默认) - Drawer (Default)"), 8000, 200, SP_NORMAL, SP_VERYSLOW, SP_OFF, SP_OFF },
    { _T("抽屉 (快速) - Drawer (Fast)"), 5000, 100, SP_FAST, SP_SLOW, SP_OFF, SP_OFF },
    { _T("滑动 (默认) - Silde (Default)"), 6000, 200, SP_NORMAL, SP_VERYSLOW, SP_NORMAL, SP_VERYSLOW },
    { _T("滑动 (快速) - Silde (Fast)"), 4000, 100, SP_FAST, SP_SLOW, SP_FAST, SP_SLOW },
    { _T("常显 - Always Show"), 0xFFFFFFFF, 1000, SP_FAST, SP_FAST, SP_FAST, SP_FAST }
};
const int PRESET_COUNT = (int)(sizeof(PRESETS) / sizeof(PRESETS[0]));

struct MaskOption {
    int percent;
    int alpha;
};

const MaskOption MASK_OPTIONS[] = {
    { 0, 0 }, { 25, 64 }, { 50, 128 }, { 60, 153 },
    { 65, 166 }, { 70, 179 }, { 75, 191 }, { 80, 204 },
    { 85, 217 }, { 90, 230 }, { 95, 240 }
};
const int MASK_OPT_COUNT = (int)(sizeof(MASK_OPTIONS) / sizeof(MASK_OPTIONS[0]));

// ==========================================
// === 全局状态 (保持不变) ===
// ==========================================
const float STARTUP_SPEED_FACTOR = 0.5f;
const ULONGLONG STARTUP_TRANSITION_DELAY = 500;

#define WM_TRAYICON (WM_USER + 1)
#define ID_TRAY_EXIT 9001
#define ID_TRAY_AUTOSTART 9002
#define ID_PROFILE_START 9100
#define ID_MASK_START 9200

const TCHAR* REG_SUBKEY = _T("Software\\AutoICON");
const TCHAR* REG_VAL_PROFILE = _T("LastProfileIndex");
const TCHAR* REG_VAL_MASK = _T("MaskOpacityIndex");

enum StartupState {
    STARTUP_PHASE_1_HIDING,
    STARTUP_PHASE_2_WAITING,
    STARTUP_PHASE_3_SHOWING,
    STARTUP_NORMAL
};

struct GlobalState {
    HWND hContainer;
    HWND hDesktopParent;
    HWND hMsgWindow;
    HWND hMaskWindow;
    int screenW;
    int screenH;
    const ConfigProfile* cfg;
    int cfgIndex;
    int maskOptIndex;
    int maxMaskAlpha;
    int pendingMaskOptIndex;
    bool hasPendingMask;
    int pendingCfgIndex;
    bool hasPendingCfg;
    float currentY, velocityY;
    float currentAlpha, velocityAlpha;
    float targetY;
    float targetAlpha;
    int lastRenderY;
    int lastRenderAlpha;
    int lastMaskAlpha;
    StartupState startupState;
    ULONGLONG waitStartTime;
    ULONGLONG startupPhaseStartTime;
    int zOrderGuardCounter;
    POINT lastMousePos;
    ULONGLONG lastActiveTime;
    bool isHidden;
    bool appRunning;
    bool isPaused;
} g = { 0 };

NOTIFYICONDATA nid = { 0 };
LARGE_INTEGER qpcFreq;
LARGE_INTEGER qpcLastTime;
UINT g_uMsgTaskbarCreated = 0;
const int MOUSE_MOVE_THRESHOLD = 2;

// ---------------------------------------------------------
// 路径初始化
// ---------------------------------------------------------
void InitGlobalPaths() {
    TCHAR szProgramFiles[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPath(NULL, CSIDL_PROGRAM_FILES, NULL, 0, szProgramFiles))) {
        PathCombine(g_szInstallDir, szProgramFiles, _T("AutoICON"));
    }
    else {
        _tcscpy_s(g_szInstallDir, _countof(g_szInstallDir), _T("C:\\Program Files\\AutoICON"));
    }
    PathCombine(g_szInstallExePath, g_szInstallDir, EXE_NAME);
}

// ---------------------------------------------------------
// 权限与进程工具
// ---------------------------------------------------------
bool IsRunAsAdministrator() {
    BOOL isAdmin = FALSE;
    PSID administratorsGroup = NULL;
    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;

    if (AllocateAndInitializeSid(&ntAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID,
        DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &administratorsGroup)) {
        CheckTokenMembership(NULL, administratorsGroup, &isAdmin);
        FreeSid(administratorsGroup);
    }
    return isAdmin != FALSE;
}

void RequestAdminPrivileges() {
    TCHAR currentPath[MAX_PATH];
    GetModuleFileName(NULL, currentPath, MAX_PATH);

    SHELLEXECUTEINFO sei = { sizeof(sei) };
    sei.lpVerb = _T("runas");
    sei.lpFile = currentPath;
    sei.hwnd = NULL;
    sei.nShow = SW_NORMAL;

    if (!ShellExecuteEx(&sei)) {
        MessageBox(NULL, _T("必须拥有管理员权限才能继续操作。"), APP_NAME, MB_OK | MB_ICONERROR);
        exit(1);
    }
    exit(0);
}

// 修复：排除当前进程自身
bool IsProcessRunning(const TCHAR* processName, DWORD* pPid = NULL) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);

    DWORD currentPid = GetCurrentProcessId();
    bool found = false;

    if (Process32First(snapshot, &pe)) {
        do {
            if (_tcsicmp(pe.szExeFile, processName) == 0 && pe.th32ProcessID != currentPid) {
                found = true;
                if (pPid) *pPid = pe.th32ProcessID;
                break;
            }
        } while (Process32Next(snapshot, &pe));
    }

    CloseHandle(snapshot);
    return found;
}

bool KillProcess(DWORD pid) {
    HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (!hProcess) return false;
    bool result = TerminateProcess(hProcess, 0) != 0;
    CloseHandle(hProcess);
    return result;
}

bool KillRunningProcesses() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);

    DWORD currentPid = GetCurrentProcessId();
    bool killed = false;

    if (Process32First(snapshot, &pe)) {
        do {
            if (_tcsicmp(pe.szExeFile, EXE_NAME) == 0 && pe.th32ProcessID != currentPid) {
                if (KillProcess(pe.th32ProcessID)) {
                    killed = true;
                }
            }
        } while (Process32Next(snapshot, &pe));
    }

    CloseHandle(snapshot);
    return killed;
}

bool WaitForProcessExit(const TCHAR* processName, DWORD timeoutMs = 5000) {
    ULONGLONG startTime = GetTickCount64();
    while (IsProcessRunning(processName)) {
        if (GetTickCount64() - startTime > timeoutMs) return false;
        Sleep(100);
    }
    return true;
}

bool IsInstalled() {
    return GetFileAttributes(g_szInstallExePath) != INVALID_FILE_ATTRIBUTES;
}

bool GetInstalledVersion(TCHAR* version, size_t size) {
    HKEY hKey;
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, REG_UNINSTALL_KEY, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD dwSize = (DWORD)size * sizeof(TCHAR);
        bool result = RegQueryValueEx(hKey, _T("DisplayVersion"), NULL, NULL, (LPBYTE)version, &dwSize) == ERROR_SUCCESS;
        RegCloseKey(hKey);
        return result;
    }
    return false;
}

// 修复：增加重试机制
bool CopyToInstallDirWithRetry() {
    TCHAR currentPath[MAX_PATH];
    GetModuleFileName(NULL, currentPath, MAX_PATH);

    if (!CreateDirectory(g_szInstallDir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        DWORD err = GetLastError();
        TCHAR msg[256];
        _stprintf_s(msg, _countof(msg), _T("无法创建安装目录！\n错误代码: %d"), err);
        MessageBox(NULL, msg, APP_NAME, MB_OK | MB_ICONERROR);
        return false;
    }

    if (GetFileAttributes(g_szInstallExePath) != INVALID_FILE_ATTRIBUTES) {
        KillRunningProcesses();
        if (!WaitForProcessExit(EXE_NAME, 3000)) {
            MessageBox(NULL, _T("无法关闭正在运行的程序！\n请手动关闭后再试。"), APP_NAME, MB_OK | MB_ICONWARNING);
            return false;
        }
        Sleep(500);
    }

    bool copySuccess = false;
    for (int i = 0; i < 5; i++) {
        if (CopyFile(currentPath, g_szInstallExePath, FALSE)) {
            copySuccess = true;
            break;
        }
        Sleep(200);
    }

    if (!copySuccess) {
        DWORD err = GetLastError();
        TCHAR msg[256];
        _stprintf_s(msg, _countof(msg), _T("复制文件失败！\n错误代码: %d\n\n请确保有足够的权限，且文件未被占用。"), err);
        MessageBox(NULL, msg, APP_NAME, MB_OK | MB_ICONERROR);
        return false;
    }
    return true;
}

void RegisterUninstallInfo() {
    HKEY hKey;
    if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, REG_UNINSTALL_KEY, 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        TCHAR uninstallCmd[MAX_PATH];
        _stprintf_s(uninstallCmd, _countof(uninstallCmd), _T("%s /uninstall"), g_szInstallExePath);

        RegSetValueEx(hKey, _T("DisplayName"), 0, REG_SZ, (BYTE*)APP_NAME, (DWORD)(_tcslen(APP_NAME) + 1) * sizeof(TCHAR));
        RegSetValueEx(hKey, _T("DisplayVersion"), 0, REG_SZ, (BYTE*)APP_VERSION, (DWORD)(_tcslen(APP_VERSION) + 1) * sizeof(TCHAR));
        RegSetValueEx(hKey, _T("Publisher"), 0, REG_SZ, (BYTE*)APP_NAME, (DWORD)(_tcslen(APP_NAME) + 1) * sizeof(TCHAR));
        RegSetValueEx(hKey, _T("UninstallString"), 0, REG_SZ, (BYTE*)uninstallCmd, (DWORD)(_tcslen(uninstallCmd) + 1) * sizeof(TCHAR));
        RegSetValueEx(hKey, _T("InstallLocation"), 0, REG_SZ, (BYTE*)g_szInstallDir, (DWORD)(_tcslen(g_szInstallDir) + 1) * sizeof(TCHAR));

        DWORD dwordVal = 1;
        RegSetValueEx(hKey, _T("NoModify"), 0, REG_DWORD, (BYTE*)&dwordVal, sizeof(DWORD));
        RegSetValueEx(hKey, _T("NoRepair"), 0, REG_DWORD, (BYTE*)&dwordVal, sizeof(DWORD));

        RegCloseKey(hKey);
    }
}

void UnregisterUninstallInfo() {
    RegDeleteKey(HKEY_LOCAL_MACHINE, REG_UNINSTALL_KEY);
    RegDeleteKey(HKEY_CURRENT_USER, REG_SUBKEY);
}

bool IsAutoStartEnabled() {
    HKEY hKey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, REG_RUN_KEY, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        TCHAR value[MAX_PATH];
        DWORD size = sizeof(value);
        bool enabled = RegQueryValueEx(hKey, APP_NAME, NULL, NULL, (BYTE*)value, &size) == ERROR_SUCCESS;
        RegCloseKey(hKey);
        return enabled;
    }
    return false;
}

void SetAutoStart(bool enabled) {
    HKEY hKey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, REG_RUN_KEY, 0, KEY_WRITE, &hKey) == ERROR_SUCCESS) {
        if (enabled) {
            RegSetValueEx(hKey, APP_NAME, 0, REG_SZ, (BYTE*)g_szInstallExePath, (DWORD)(_tcslen(g_szInstallExePath) + 1) * sizeof(TCHAR));
        }
        else {
            RegDeleteValue(hKey, APP_NAME);
        }
        RegCloseKey(hKey);
    }
}

void RunInstalledExe() {
    ShellExecute(NULL, _T("open"), g_szInstallExePath, NULL, NULL, SW_SHOWDEFAULT);
}

// ---------------------------------------------------------
// 版本解析辅助函数
// ---------------------------------------------------------
int ParseVersionNumber(const TCHAR* versionStr) {
    if (!versionStr || !*versionStr) return 0;
    const TCHAR* p = versionStr;
    while (*p && !(*p >= _T('0') && *p <= _T('9'))) {
        p++;
    }
    if (!*p) return 0;
    return _ttoi(p);
}

// ---------------------------------------------------------
// 修复后的安装/更新处理逻辑 (完全重写)
// ---------------------------------------------------------
void HandleInstallation() {
    TCHAR currentPath[MAX_PATH];
    GetModuleFileName(NULL, currentPath, MAX_PATH);

    // 如果已经在安装目录，则返回执行主程序
    if (_tcsnicmp(currentPath, g_szInstallDir, _tcslen(g_szInstallDir)) == 0) {
        return;
    }

    if (!IsRunAsAdministrator()) {
        int result = MessageBox(NULL,
            _T("AutoICON 需要管理员权限才能执行安装或更新操作。\n是否允许提权？"),
            APP_NAME, MB_YESNO | MB_ICONQUESTION);
        if (result == IDYES) {
            RequestAdminPrivileges();
        }
        exit(0);
    }

    bool bFileExists = IsInstalled();
    int currentVerNum = ParseVersionNumber(APP_VERSION);
    int installedVerNum = 0;
    TCHAR installedVerStr[32] = { 0 };

    if (bFileExists) {
        if (!GetInstalledVersion(installedVerStr, 32)) {
            _tcscpy_s(installedVerStr, _countof(installedVerStr), _T("Unknown"));
            installedVerNum = 0;
        }
        else {
            installedVerNum = ParseVersionNumber(installedVerStr);
        }
    }

    // --- 分支 A: 全新安装 ---
    if (!bFileExists) {
        TCHAR msg[512];
        _stprintf_s(msg, _countof(msg), _T("欢迎使用 AutoICON！\n\n即将安装版本: %s\n安装位置: %s\n\n是否继续？"), APP_VERSION, g_szInstallDir);

        if (MessageBox(NULL, msg, APP_NAME, MB_YESNO | MB_ICONQUESTION) == IDYES) {
            HWND hWaitWnd = CreateWindow(_T("STATIC"), _T("正在安装..."), WS_POPUP | WS_VISIBLE, 300, 300, 300, 60, NULL, NULL, NULL, NULL);

            if (CopyToInstallDirWithRetry()) {
                RegisterUninstallInfo(); // 写入注册表
                DestroyWindow(hWaitWnd);
                MessageBox(NULL, _T("安装成功！"), APP_NAME, MB_OK | MB_ICONINFORMATION);
                RunInstalledExe();
                exit(0);
            }
            else {
                DestroyWindow(hWaitWnd);
                exit(1);
            }
        }
        else {
            exit(0);
        }
    }

    // --- 分支 B: 升级 ---
    else if (currentVerNum > installedVerNum) {
        TCHAR msg[512];
        _stprintf_s(msg, _countof(msg),
            _T("发现新版本！\n\n当前安装版本: %s\n最新版本: %s\n\n是否立即更新？"),
            installedVerStr, APP_VERSION);

        if (MessageBox(NULL, msg, APP_NAME, MB_YESNO | MB_ICONASTERISK) == IDYES) {
            HWND hWaitWnd = CreateWindow(_T("STATIC"), _T("正在更新..."), WS_POPUP | WS_VISIBLE, 300, 300, 300, 60, NULL, NULL, NULL, NULL);

            if (CopyToInstallDirWithRetry()) {
                RegisterUninstallInfo(); // 更新注册表版本号
                DestroyWindow(hWaitWnd);
                MessageBox(NULL, _T("更新成功！"), APP_NAME, MB_OK | MB_ICONINFORMATION);
                RunInstalledExe();
                exit(0);
            }
            else {
                DestroyWindow(hWaitWnd);
                exit(1);
            }
        }
        else {
            exit(0);
        }
    }

    // --- 分支 C: 相同版本 ---
    else if (currentVerNum == installedVerNum) {
        TCHAR msg[512];
        _stprintf_s(msg, _countof(msg),
            _T("当前已安装最新版本 (%s)。\n\n[是] - 启动已安装的程序\n[否] - 强制重新安装/修复\n[取消] - 退出"),
            APP_VERSION);

        int choice = MessageBox(NULL, msg, APP_NAME, MB_YESNOCANCEL | MB_ICONINFORMATION);

        if (choice == IDYES) {
            RunInstalledExe();
            exit(0);
        }
        else if (choice == IDNO) {
            HWND hWaitWnd = CreateWindow(_T("STATIC"), _T("正在重新安装..."), WS_POPUP | WS_VISIBLE, 300, 300, 300, 60, NULL, NULL, NULL, NULL);
            if (CopyToInstallDirWithRetry()) {
                RegisterUninstallInfo(); // 修复注册表
                DestroyWindow(hWaitWnd);
                MessageBox(NULL, _T("修复成功！"), APP_NAME, MB_OK | MB_ICONINFORMATION);
                RunInstalledExe();
                exit(0);
            }
            else {
                DestroyWindow(hWaitWnd);
                exit(1);
            }
        }
        else {
            exit(0);
        }
    }

    // --- 分支 D: 降级 ---
    else {
        TCHAR msg[512];
        _stprintf_s(msg, _countof(msg),
            _T("警告：检测到旧版本安装包。\n\n系统已安装: %s\n当前安装包: %s\n\n你确定要回退到旧版本吗？"),
            installedVerStr, APP_VERSION);

        if (MessageBox(NULL, msg, APP_NAME, MB_YESNO | MB_ICONWARNING) == IDYES) {
            HWND hWaitWnd = CreateWindow(_T("STATIC"), _T("正在回退版本..."), WS_POPUP | WS_VISIBLE, 300, 300, 300, 60, NULL, NULL, NULL, NULL);
            if (CopyToInstallDirWithRetry()) {
                RegisterUninstallInfo(); // 回退注册表版本号
                DestroyWindow(hWaitWnd);
                MessageBox(NULL, _T("版本回退成功！"), APP_NAME, MB_OK | MB_ICONINFORMATION);
                RunInstalledExe();
                exit(0);
            }
            else {
                DestroyWindow(hWaitWnd);
                exit(1);
            }
        }
        else {
            exit(0);
        }
    }
}

// ---------------------------------------------------------
// 终极修复：基于临时脚本的智能卸载方案
// ---------------------------------------------------------
void HandleUninstall() {
    if (!IsRunAsAdministrator()) {
        MessageBox(NULL, _T("卸载程序需要管理员权限！\n请右键以管理员身份运行。"),
            APP_NAME, MB_OK | MB_ICONERROR);
        return;
    }

    TCHAR currentPath[MAX_PATH];
    GetModuleFileName(NULL, currentPath, MAX_PATH);

    if (_tcsnicmp(currentPath, g_szInstallDir, _tcslen(g_szInstallDir)) != 0) {
        MessageBox(NULL, _T("请从安装目录运行卸载程序。"), APP_NAME, MB_OK | MB_ICONERROR);
        return;
    }

    int result = MessageBox(NULL, _T("确定要卸载 AutoICON 吗？\n这将删除所有相关文件和设置。"), APP_NAME,
        MB_YESNO | MB_ICONQUESTION);
    if (result != IDYES) return;

    // 清理环境
    KillRunningProcesses();
    UnregisterUninstallInfo();
    SetAutoStart(false);

    // 生成自毁脚本
    TCHAR szTempPath[MAX_PATH];
    TCHAR szBatPath[MAX_PATH];

    GetTempPath(MAX_PATH, szTempPath);
    PathCombine(szBatPath, szTempPath, _T("AutoICON_Uninst.bat"));

    FILE* fp = NULL;
    _tfopen_s(&fp, szBatPath, _T("w"));
    if (fp) {
        _ftprintf(fp, _T("@echo off\n"));
        _ftprintf(fp, _T(":LOOP\n"));
        _ftprintf(fp, _T("timeout /t 1 /nobreak > nul\n"));
        // 尝试删除主文件
        _ftprintf(fp, _T("del /F /Q \"%s\"\n"), currentPath);
        // 如果文件还在(说明未退出), 跳转重试
        _ftprintf(fp, _T("if exist \"%s\" goto LOOP\n"), currentPath);
        // 删除目录
        _ftprintf(fp, _T("rmdir /S /Q \"%s\"\n"), g_szInstallDir);
        // 删除脚本自身
        _ftprintf(fp, _T("del \"%%~f0\"\n"));
        fclose(fp);
    }
    else {
        MessageBox(NULL, _T("无法创建卸载脚本，请检查临时目录权限。"), APP_NAME, MB_ICONERROR);
        return;
    }

    ShellExecute(NULL, _T("open"), szBatPath, NULL, NULL, SW_HIDE);

    MessageBox(NULL, _T("卸载已完成！\n\n程序将退出，残留文件将在后台自动清除。"),
        APP_NAME, MB_OK | MB_ICONINFORMATION);

    exit(0);
}

// ---------------------------------------------------------
// 物理引擎与逻辑 (保持不变)
// ---------------------------------------------------------
void UpdatePhysics(float dt);

void TimerInit() {
    QueryPerformanceFrequency(&qpcFreq);
    QueryPerformanceCounter(&qpcLastTime);
}

float TimerGetDelta(bool resetOnly = false) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (resetOnly) {
        qpcLastTime = now;
        return 0.0f;
    }
    float dt = (float)((double)(now.QuadPart - qpcLastTime.QuadPart) / (double)qpcFreq.QuadPart);
    qpcLastTime = now;
    if (dt > 0.05f) dt = 0.05f;
    if (dt < 0.0001f) dt = 0.0001f;
    return dt;
}

void SaveSettings() {
    HKEY hKey;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, REG_SUBKEY, 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        RegSetValueEx(hKey, REG_VAL_PROFILE, 0, REG_DWORD, (BYTE*)&g.cfgIndex, sizeof(g.cfgIndex));
        RegSetValueEx(hKey, REG_VAL_MASK, 0, REG_DWORD, (BYTE*)&g.maskOptIndex, sizeof(g.maskOptIndex));
        RegCloseKey(hKey);
    }
}

void LoadSettings() {
    HKEY hKey;
    g.cfgIndex = 0; g.maskOptIndex = 0;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, REG_SUBKEY, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD size = sizeof(DWORD);
        RegQueryValueEx(hKey, REG_VAL_PROFILE, NULL, NULL, (BYTE*)&g.cfgIndex, &size);
        size = sizeof(DWORD);
        RegQueryValueEx(hKey, REG_VAL_MASK, NULL, NULL, (BYTE*)&g.maskOptIndex, &size);
        RegCloseKey(hKey);
    }
    if (g.cfgIndex < 0 || g.cfgIndex >= PRESET_COUNT) g.cfgIndex = 0;
    if (g.maskOptIndex < 0 || g.maskOptIndex >= MASK_OPT_COUNT) g.maskOptIndex = 0;
    g.cfg = &PRESETS[g.cfgIndex];
    g.maxMaskAlpha = MASK_OPTIONS[g.maskOptIndex].alpha;
}

void ForceShowImmediate() {
    g.startupState = STARTUP_NORMAL;
    g.isHidden = false;
    g.lastActiveTime = GetTickCount64();
    g.targetY = 0.0f;
    g.targetAlpha = 255.0f;
    g.currentY = 0.0f;
    g.currentAlpha = 255.0f;
    g.velocityY = 0.0f;
    g.velocityAlpha = 0.0f;
    g.lastRenderY = -99999;
    g.lastRenderAlpha = -1;
    TimerGetDelta(true);
    UpdatePhysics(0.0f);
}

void TriggerRestartAnimation() {
    g.startupState = STARTUP_PHASE_1_HIDING;
    g.startupPhaseStartTime = GetTickCount64();
    g.targetY = (float)g.screenH;
    g.targetAlpha = 0.0f;
    g.isHidden = true;
}

// ---------------------------------------------------------
// 窗口管理
// ---------------------------------------------------------
BOOL CALLBACK FindSysListViewProc(HWND hwnd, LPARAM lParam) {
    HWND hShellView = FindWindowEx(hwnd, NULL, _T("SHELLDLL_DefView"), NULL);
    if (hShellView) {
        g.hContainer = hShellView;
        g.hDesktopParent = hwnd;
        return FALSE;
    }
    return TRUE;
}

void EnableLayeredStyle(HWND hwnd, bool enable) {
    if (!IsWindow(hwnd)) return;
    LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    if (enable) {
        if (!(exStyle & WS_EX_LAYERED)) SetWindowLongPtr(hwnd, GWL_EXSTYLE, exStyle | WS_EX_LAYERED);
    }
    else {
        if (exStyle & WS_EX_LAYERED) SetWindowLongPtr(hwnd, GWL_EXSTYLE, exStyle & ~WS_EX_LAYERED);
    }
}

void EnforceZOrder() {
    if (!g.hContainer || !IsWindow(g.hContainer)) return;
    SetWindowPos(g.hContainer, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (g.hMaskWindow && IsWindow(g.hMaskWindow)) {
        SetWindowPos(g.hMaskWindow, g.hContainer, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

void CreateMaskWindow(HINSTANCE hInstance) {
    if (g.hMaskWindow && IsWindow(g.hMaskWindow)) return;
    WNDCLASSEX wc = { 0 };
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.lpfnWndProc = DefWindowProc;
    wc.hInstance = hInstance;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = _T("DH_SpringMask");
    RegisterClassEx(&wc);
    g.hMaskWindow = CreateWindowEx(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW,
        wc.lpszClassName, NULL, WS_POPUP, 0, 0, 0, 0, NULL, NULL, hInstance, NULL);
}

void AttachMaskToDesktop() {
    if (!g.hMaskWindow || !IsWindow(g.hMaskWindow)) return;
    if (!g.hContainer || !IsWindow(g.hContainer)) return;
    if (!g.hDesktopParent || !IsWindow(g.hDesktopParent)) return;

    SetParent(g.hMaskWindow, g.hDesktopParent);
    LONG_PTR style = GetWindowLongPtr(g.hMaskWindow, GWL_STYLE);
    style &= ~WS_POPUP; style |= WS_CHILD;
    SetWindowLongPtr(g.hMaskWindow, GWL_STYLE, style);

    int extendedH = (int)(g.screenH * 1.04f);
    int offsetY = (int)(g.screenH * 0.02f);

    SetWindowPos(g.hMaskWindow, NULL, 0, -offsetY, g.screenW, extendedH, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    EnforceZOrder();
    SetLayeredWindowAttributes(g.hMaskWindow, 0, 0, LWA_ALPHA);
    ShowWindow(g.hMaskWindow, SW_SHOWNA);
}

void LocateDesktop(HINSTANCE hInstance) {
    g.hContainer = NULL;
    g.hDesktopParent = NULL;

    HWND hProgman = FindWindow(_T("Progman"), NULL);
    FindSysListViewProc(hProgman, 0);
    if (!g.hContainer) EnumWindows(FindSysListViewProc, 0);

    if (g.hContainer) {
        RECT rect;
        GetWindowRect(g.hContainer, &rect);
        g.screenW = rect.right - rect.left;
        g.screenH = rect.bottom - rect.top;
        if (g.screenH == 0) {
            g.screenW = GetSystemMetrics(SM_CXSCREEN);
            g.screenH = GetSystemMetrics(SM_CYSCREEN);
        }

        ForceShowImmediate();
        g.isHidden = true;
        g.targetY = (float)g.screenH;
        g.targetAlpha = 0.0f;
        g.startupState = STARTUP_PHASE_1_HIDING;
        g.startupPhaseStartTime = GetTickCount64();

        SetWindowPos(g.hContainer, NULL, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        EnableLayeredStyle(g.hContainer, true);
        SetLayeredWindowAttributes(g.hContainer, 0, 255, LWA_ALPHA);

        if (g.hMaskWindow && IsWindow(g.hMaskWindow)) {
            ShowWindow(g.hMaskWindow, SW_HIDE);
        }
    }
}

void PerformExitSequence() {
    if (g.hMaskWindow && IsWindow(g.hMaskWindow)) DestroyWindow(g.hMaskWindow);
    Shell_NotifyIcon(NIM_DELETE, &nid);

    DWORD pid = 0;
    HWND hShellWnd = FindWindow(_T("Shell_TrayWnd"), NULL);
    if (!hShellWnd) hShellWnd = FindWindow(_T("Progman"), NULL);

    if (hShellWnd) {
        GetWindowThreadProcessId(hShellWnd, &pid);
        if (pid != 0) {
            HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
            if (hProcess) {
                TerminateProcess(hProcess, 1);
                CloseHandle(hProcess);
                Sleep(500);
            }
        }
    }
    ShellExecute(NULL, _T("open"), _T("explorer.exe"), NULL, NULL, SW_SHOWDEFAULT);
}

void InitTrayIcon(HWND hwnd) {
    memset(&nid, 0, sizeof(nid));
    nid.cbSize = sizeof(NOTIFYICONDATA);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    _tcscpy_s(nid.szTip, _countof(nid.szTip), APP_NAME);
    Shell_NotifyIcon(NIM_ADD, &nid);
}

void ShowTrayMenu(HWND hwnd) {
    POINT p; GetCursorPos(&p);
    SetForegroundWindow(hwnd);
    HMENU hMenu = CreatePopupMenu();

    HMENU hSubProfile = CreatePopupMenu();
    for (int i = 0; i < PRESET_COUNT; i++) {
        UINT flags = MF_STRING;
        int checkIndex = g.hasPendingCfg ? g.pendingCfgIndex : g.cfgIndex;
        if (i == checkIndex) flags |= MF_CHECKED;
        AppendMenu(hSubProfile, flags, ID_PROFILE_START + i, PRESETS[i].name);
    }
    AppendMenu(hMenu, MF_POPUP, (UINT_PTR)hSubProfile, _T("动画模式 (Animation Mode)"));

    HMENU hSubMask = CreatePopupMenu();
    for (int i = 0; i < MASK_OPT_COUNT; i++) {
        UINT flags = MF_STRING;
        int checkIndex = g.hasPendingMask ? g.pendingMaskOptIndex : g.maskOptIndex;
        if (i == checkIndex) flags |= MF_CHECKED;

        TCHAR buf[64] = { 0 };
        if (MASK_OPTIONS[i].percent == 0) _tcscpy_s(buf, _countof(buf), _T("关闭 (Off)"));
        else _stprintf_s(buf, _countof(buf), _T("%d%% 浓度 (Opacity)"), MASK_OPTIONS[i].percent);

        AppendMenu(hSubMask, flags, ID_MASK_START + i, buf);
    }
    AppendMenu(hMenu, MF_POPUP, (UINT_PTR)hSubMask, _T("背景蒙版 (Background Mask)"));

    AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);

    UINT autoStartFlags = MF_STRING;
    if (IsAutoStartEnabled()) autoStartFlags |= MF_CHECKED;
    AppendMenu(hMenu, autoStartFlags, ID_TRAY_AUTOSTART, _T("开机自启 (Auto Start)"));

    AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(hMenu, MF_STRING, ID_TRAY_EXIT, _T("退出程序 (Exit)"));

    TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, p.x, p.y, 0, hwnd, NULL);
    DestroyMenu(hMenu);
}

// ---------------------------------------------------------
// 物理计算
// ---------------------------------------------------------
void SolveSpring(float& current, float& velocity, float target, const SpringParams& p, float dt) {
    float displacement = current - target;
    float acceleration = -p.tension * displacement - p.friction * velocity;
    velocity += acceleration * dt;
    current += velocity * dt;

    if (_isnan(current) || !_finite(current)) current = target;
    if (_isnan(velocity) || !_finite(velocity)) velocity = 0.0f;
}

void UpdatePhysics(float dt) {
    if (!g.hContainer) return;
    if (g.startupState != STARTUP_NORMAL) dt *= STARTUP_SPEED_FACTOR;

    float physicsTargetY = g.targetY;
    float physicsTargetAlpha = g.targetAlpha;

    const SpringParams* pMotion = g.isHidden ? &g.cfg->motionOut : &g.cfg->motionIn;
    const SpringParams* pOpacity = g.isHidden ? &g.cfg->opacityOut : &g.cfg->opacityIn;

    if (pMotion->enabled) {
        SolveSpring(g.currentY, g.velocityY, physicsTargetY, *pMotion, dt);
    }
    else {
        g.currentY = 0.0f;
        g.velocityY = 0.0f;
    }

    if (pOpacity->enabled) {
        SolveSpring(g.currentAlpha, g.velocityAlpha, physicsTargetAlpha, *pOpacity, dt);
    }
    else {
        g.currentAlpha = 255.0f;
        g.velocityAlpha = 0.0f;
    }

    if (g.currentAlpha < 0.0f) g.currentAlpha = 0.0f;
    if (g.currentAlpha > 255.0f) g.currentAlpha = 255.0f;

    int renderY = (int)g.currentY;
    int renderAlpha = (int)g.currentAlpha;

    if (renderY != g.lastRenderY) {
        SetWindowPos(g.hContainer, NULL, 0, renderY, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        if (g.hMaskWindow && IsWindow(g.hMaskWindow) && IsWindowVisible(g.hMaskWindow)) {
            int extendedH = (int)(g.screenH * 1.04f);
            int offsetY = (int)(g.screenH * 0.02f);
            SetWindowPos(g.hMaskWindow, NULL, 0, renderY - offsetY, g.screenW, extendedH, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        g.lastRenderY = renderY;
    }

    if (renderAlpha != g.lastRenderAlpha) {
        SetLayeredWindowAttributes(g.hContainer, 0, (BYTE)renderAlpha, LWA_ALPHA);
        g.lastRenderAlpha = renderAlpha;
    }

    if (g.hMaskWindow && IsWindow(g.hMaskWindow) && IsWindowVisible(g.hMaskWindow) && g.maxMaskAlpha > 0) {
        int maskCurrentAlpha = 0;
        if (pOpacity->enabled) {
            float ratio = g.currentAlpha / 255.0f;
            maskCurrentAlpha = (int)(g.maxMaskAlpha * ratio);
        }
        else {
            float hiddenRatio = g.currentY / (float)g.screenH;
            float opacityRatio = 1.0f - hiddenRatio;
            if (opacityRatio < 0.0f) opacityRatio = 0.0f;
            maskCurrentAlpha = (int)(g.maxMaskAlpha * opacityRatio);
        }
        if (maskCurrentAlpha != g.lastMaskAlpha) {
            SetLayeredWindowAttributes(g.hMaskWindow, 0, (BYTE)maskCurrentAlpha, LWA_ALPHA);
            g.lastMaskAlpha = maskCurrentAlpha;
        }
    }

    g.zOrderGuardCounter++;
    if (g.zOrderGuardCounter > 30) {
        EnforceZOrder();
        g.zOrderGuardCounter = 0;
    }
}

bool IsPhysicsIdle() {
    const SpringParams* pMotion = g.isHidden ? &g.cfg->motionOut : &g.cfg->motionIn;
    const SpringParams* pOpacity = g.isHidden ? &g.cfg->opacityOut : &g.cfg->opacityIn;

    bool yIdle = true;
    bool alphaIdle = true;

    if (pMotion->enabled) {
        double dy = fabs(g.targetY - g.currentY);
        yIdle = (dy < 1.0f && fabs(g.velocityY) < 2.0f);
    }
    if (pOpacity->enabled) {
        double da = fabs(g.targetAlpha - g.currentAlpha);
        alphaIdle = (da < 1.0f && fabs(g.velocityAlpha) < 2.0f);
    }
    return yIdle && alphaIdle;
}

// ---------------------------------------------------------
// 主消息循环
// ---------------------------------------------------------
bool IsMouseOnDesktop() {
    POINT pt; GetCursorPos(&pt);
    HWND hWin = WindowFromPoint(pt);
    if (!hWin) return false;
    if (hWin == g.hMaskWindow) return true;
    if (hWin == g.hContainer || hWin == g.hDesktopParent) return true;
    if (hWin == FindWindow(_T("Progman"), NULL)) return true;
    HWND hParent = GetParent(hWin);
    if (hParent == g.hContainer || hParent == g.hDesktopParent) return true;
    return false;
}

LRESULT CALLBACK MsgWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == g_uMsgTaskbarCreated && g_uMsgTaskbarCreated != 0) {
        g.hContainer = NULL;
        return 0;
    }
    switch (msg) {
    case WM_TRAYICON:
        if (lParam == WM_RBUTTONUP) {
            ForceShowImmediate();
            ShowTrayMenu(hwnd);
            TimerGetDelta(true);
        }
        break;
    case WM_WTSSESSION_CHANGE:
        if (wParam == WTS_SESSION_LOCK) g.isPaused = true;
        else if (wParam == WTS_SESSION_UNLOCK) {
            g.isPaused = false;
            g.lastActiveTime = GetTickCount64();
            TimerGetDelta(true);
        }
        break;
    case WM_COMMAND: {
        int cmdId = LOWORD(wParam);
        if (cmdId == ID_TRAY_EXIT) {
            g.appRunning = false;
            PerformExitSequence();
        }
        else if (cmdId == ID_TRAY_AUTOSTART) {
            bool current = IsAutoStartEnabled();
            SetAutoStart(!current);
        }
        else if (cmdId >= ID_PROFILE_START && cmdId < ID_PROFILE_START + PRESET_COUNT) {
            g.pendingCfgIndex = cmdId - ID_PROFILE_START;
            g.hasPendingCfg = true;
            TriggerRestartAnimation();
        }
        else if (cmdId >= ID_MASK_START && cmdId < ID_MASK_START + MASK_OPT_COUNT) {
            g.pendingMaskOptIndex = cmdId - ID_MASK_START;
            g.hasPendingMask = true;
            TriggerRestartAnimation();
        }
        break;
    }
    case WM_DISPLAYCHANGE:
        Sleep(500);
        g.hContainer = NULL;
        break;
    case WM_DESTROY:
        g.appRunning = false;
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

void CreateMessageWindow(HINSTANCE hInstance) {
    WNDCLASSEX wc = { 0 };
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.lpfnWndProc = MsgWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = _T("DH_Core_Perfect");
    RegisterClassEx(&wc);
    g.hMsgWindow = CreateWindowEx(0, wc.lpszClassName, _T(""), 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, hInstance, NULL);
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);

    // 初始化路径
    InitGlobalPaths();

    // 处理命令行参数
    if (lpCmdLine && _tcsstr(lpCmdLine, _T("/uninstall"))) {
        HandleUninstall();
        return 0;
    }

    // 检查安装状态
    HandleInstallation();

    // 检查是否在安装目录运行
    TCHAR currentPath[MAX_PATH];
    GetModuleFileName(NULL, currentPath, MAX_PATH);
    if (_tcsnicmp(currentPath, g_szInstallDir, _tcslen(g_szInstallDir)) != 0) {
        MessageBox(NULL, _T("程序必须在安装目录运行！"), APP_NAME, MB_OK | MB_ICONERROR);
        return 1;
    }

    HANDLE hMutex = CreateMutex(NULL, TRUE, _T("Local\\AutoICON_Instance"));
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    g_uMsgTaskbarCreated = RegisterWindowMessage(_T("TaskbarCreated"));
    g.appRunning = true;
    g.isPaused = false;
    LoadSettings();
    g.lastActiveTime = GetTickCount64();

    CreateMessageWindow(hInstance);
    WTSRegisterSessionNotification(g.hMsgWindow, NOTIFY_FOR_THIS_SESSION);
    InitTrayIcon(g.hMsgWindow);
    LocateDesktop(hInstance);
    TimerInit();

    if (!g.hContainer) {
        MessageBox(NULL, _T("无法定位桌面窗口。"), APP_NAME, MB_ICONERROR);
        g.appRunning = false;
    }

    MSG msg;
    while (g.appRunning) {
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) g.appRunning = false;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (!g.appRunning) break;
        if (g.isPaused) { Sleep(1000); continue; }

        if (!IsWindow(g.hContainer)) {
            LocateDesktop(hInstance);
            if (!g.hContainer) { Sleep(500); continue; }
        }

        POINT currMouse; GetCursorPos(&currMouse);
        ULONGLONG currTime = GetTickCount64();
        bool isMoving = (abs(currMouse.x - g.lastMousePos.x) > MOUSE_MOVE_THRESHOLD ||
            abs(currMouse.y - g.lastMousePos.y) > MOUSE_MOVE_THRESHOLD);

        if (g.startupState == STARTUP_PHASE_1_HIDING) {
            bool hiddenEnough = true;
            if (g.currentY < g.screenH * 0.9f && g.currentAlpha > 10.0f) hiddenEnough = false;

            if (hiddenEnough || (currTime - g.startupPhaseStartTime > 2000)) {
                g.startupState = STARTUP_PHASE_2_WAITING;
                g.waitStartTime = currTime;
            }
        }
        else if (g.startupState == STARTUP_PHASE_2_WAITING) {
            if (currTime - g.waitStartTime > STARTUP_TRANSITION_DELAY) {
                if (g.hasPendingCfg) {
                    g.cfgIndex = g.pendingCfgIndex;
                    g.hasPendingCfg = false;
                }
                if (g.hasPendingMask) {
                    g.maskOptIndex = g.pendingMaskOptIndex;
                    g.hasPendingMask = false;
                }
                g.cfg = &PRESETS[g.cfgIndex];
                g.maxMaskAlpha = MASK_OPTIONS[g.maskOptIndex].alpha;
                SaveSettings();

                if (g.maxMaskAlpha <= 0) {
                    if (g.hMaskWindow && IsWindow(g.hMaskWindow)) DestroyWindow(g.hMaskWindow);
                    g.hMaskWindow = NULL;
                }
                else {
                    if (!g.hMaskWindow || !IsWindow(g.hMaskWindow)) CreateMaskWindow(hInstance);
                    AttachMaskToDesktop();
                }

                g.targetY = 0.0f;
                g.targetAlpha = 255.0f;

                if (g.cfg->opacityIn.enabled) {
                    g.currentAlpha = 0.0f;
                }
                else {
                    g.currentAlpha = 255.0f;
                }

                if (g.cfg->motionIn.enabled) {
                    g.currentY = (float)g.screenH;
                }
                else {
                    g.currentY = 0.0f;
                }

                g.velocityY = 0.0f;
                g.velocityAlpha = 0.0f;

                g.startupState = STARTUP_PHASE_3_SHOWING;
                g.isHidden = false;
                g.lastActiveTime = currTime;

                EnforceZOrder();
            }
        }
        else if (g.startupState == STARTUP_PHASE_3_SHOWING) {
            if (IsPhysicsIdle() || (isMoving && IsMouseOnDesktop())) {
                g.startupState = STARTUP_NORMAL;
                g.lastActiveTime = currTime;
            }
        }
        else {
            if (isMoving) {
                if (IsMouseOnDesktop()) {
                    g.lastActiveTime = currTime;
                    g.targetY = 0.0f;
                    g.targetAlpha = 255.0f;
                    g.isHidden = false;
                }
            }
            else {
                if (!g.isHidden && (currTime - g.lastActiveTime > g.cfg->hideDelayMs)) {
                    g.targetY = (float)g.screenH;
                    g.targetAlpha = 0.0f;
                    g.isHidden = true;
                }
            }
        }

        g.lastMousePos = currMouse;

        if (!IsPhysicsIdle()) {
            float dt = TimerGetDelta();
            UpdatePhysics(dt);
            DwmFlush();
        }
        else {
            if (g.currentY != g.targetY || g.currentAlpha != g.targetAlpha) UpdatePhysics(0.0f);
            Sleep((DWORD)g.cfg->idleCheckMs);
            TimerGetDelta(true);
        }
    }

    WTSUnRegisterSessionNotification(g.hMsgWindow);
    if (hMutex) CloseHandle(hMutex);
    return 0;
}
