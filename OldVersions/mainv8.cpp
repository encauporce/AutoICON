/*
    Auto-Hiding Desktop Icons (Animation Transition Fix)

    [核心修复日志]
    1. 动画衔接修复 (Transition Fix)：
       - 在切换配置的间隙(Phase 2)，根据"新模式"的物理特性预设起点。
       - 如果新模式是"仅渐变"，强制将位置归零(Y=0)并将透明度归零(A=0)，确保它从原地淡入，而不是从屏幕外跳进来。
       - 如果新模式是"仅位移"，强制将位置设为底部(Y=H)，确保它从底部滑入。

    2. 视觉稳定性：
       - 黑色蒙版逻辑保持 104% 高度 + 2% 偏移，防止边缘闪烁。
       - 退出程序保持重启 Explorer，防止图标丢失。

    [编译环境]
    - Visual Studio (C++)
    - 链接库: dwmapi.lib;user32.lib;shell32.lib;advapi32.lib;wtsapi32.lib
*/

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <wtsapi32.h>
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

#ifndef _countof
#define _countof(_Array) (sizeof(_Array) / sizeof(_Array[0]))
#endif

// ==========================================
// === 物理引擎参数 ===
// ==========================================
struct SpringParams {
    float tension;  // 张力
    float friction; // 摩擦
    bool  enabled;  // 是否启用
};

const SpringParams SP_FAST = { 860.0f, 46.0f, true };
const SpringParams SP_NORMAL = { 400.0f, 32.0f, true };
const SpringParams SP_SLOW = { 12.0f,  5.0f, true };
const SpringParams SP_VERYSLOW = { 6.0f,  3.0f,  true };
const SpringParams SP_OFF = { 0.0f,   0.0f,  false };

// ==========================================
// === 配置结构 ===
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
    // 1. 仅渐变 (位移禁用 SP_OFF)
    { _T("渐变 (默认) - Fade (Default)"), 5000, 200, SP_OFF, SP_OFF, SP_NORMAL, SP_VERYSLOW },
    // 2. 仅渐变 (快)
    { _T("渐变 (快速) - Fade (Fast)"), 3000, 100, SP_OFF, SP_OFF, SP_FAST, SP_SLOW },
    // 3. 仅位移 (透明度禁用 SP_OFF)
    { _T("抽屉 (默认) - Drawer (Default)"), 8000, 200, SP_NORMAL, SP_VERYSLOW, SP_OFF, SP_OFF },
    // 4. 仅位移 (快)
    { _T("抽屉 (快速) - Drawer (Fast)"), 5000, 100, SP_FAST, SP_SLOW, SP_OFF, SP_OFF },
    // 5. 混合
    { _T("滑动 (默认) - Silde (Default)"), 6000, 200, SP_NORMAL, SP_VERYSLOW, SP_NORMAL, SP_VERYSLOW },
    // 6. 混合 (快)
    { _T("滑动 (快速) - Silde (Fast)"), 4000, 100, SP_FAST, SP_SLOW, SP_FAST, SP_SLOW },
    // 7. 常显
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
// === 全局状态 ===
// ==========================================
const float STARTUP_SPEED_FACTOR = 0.5f;
const ULONGLONG STARTUP_TRANSITION_DELAY = 500;

#define WM_TRAYICON (WM_USER + 1)
#define ID_TRAY_EXIT 9001
#define ID_PROFILE_START 9100
#define ID_MASK_START 9200

const TCHAR* REG_SUBKEY = _T("Software\\MyDesktopHider_TransFix");
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
// 物理引擎前置
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

// ---------------------------------------------------------
// 逻辑控制
// ---------------------------------------------------------

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

// ---------------------------------------------------------
// 菜单
// ---------------------------------------------------------
void InitTrayIcon(HWND hwnd) {
    memset(&nid, 0, sizeof(nid));
    nid.cbSize = sizeof(NOTIFYICONDATA);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    _tcscpy_s(nid.szTip, _countof(nid.szTip), _T("AutoICON"));
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
    AppendMenu(hMenu, MF_STRING, ID_TRAY_EXIT, _T("退出程序 (Exit)"));

    TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, p.x, p.y, 0, hwnd, NULL);
    DestroyMenu(hMenu);
}

// ---------------------------------------------------------
// 物理引擎
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

int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrev, _In_ LPWSTR lpCmdLine, _In_ int nShow) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);

    HANDLE hMutex = CreateMutex(NULL, TRUE, _T("Local\\DH_Perfect_Instance"));
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
        MessageBox(NULL, _T("无法定位桌面窗口。"), _T("Error"), MB_ICONERROR);
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

                // --- 核心修复：重置物理起点，防止"硬切" ---
                // 根据新配置的 Entry 特性，决定起点
                g.targetY = 0.0f;
                g.targetAlpha = 255.0f;

                // 1. 如果新模式有渐变进场 -> 强制 Alpha=0 (从透明开始)
                //    如果新模式无渐变(如仅位移) -> 强制 Alpha=255 (保持可见)
                if (g.cfg->opacityIn.enabled) {
                    g.currentAlpha = 0.0f;
                }
                else {
                    g.currentAlpha = 255.0f;
                }

                // 2. 如果新模式有位移进场 -> 强制 Y=Bottom (从底部开始)
                //    如果新模式无位移(如仅渐变) -> 强制 Y=0 (保持原位)
                if (g.cfg->motionIn.enabled) {
                    g.currentY = (float)g.screenH;
                }
                else {
                    g.currentY = 0.0f;
                }

                // 3. 清除速度，让动画自然开始
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