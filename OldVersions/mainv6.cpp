/*
    Auto-Hiding Desktop Icons (Refined Logic & UX)

    [修改日志]
    1. 修复启动计时漂移：进入 NORMAL 模式瞬间重置计时器，扣除动画播放消耗的时间。
    2. 交互优化：右键托盘或切换配置时，强制图标立即显示 (ForceShow)。
    3. 逻辑微调：IsMouseOnDesktop 排除了透明蒙版的干扰，防止误判。
    4. 保持了之前的启动动画减速因子和测试常量。

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
#include <stdio.h> 
#include <stdlib.h> 

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
// === 测试配置 ===
// ==========================================
const ULONGLONG STARTUP_TRANSITION_DELAY = 100;
const float STARTUP_SPEED_FACTOR = 0.5f;

// ==========================================
// === 配置结构 ===
// ==========================================
struct ConfigProfile {
    const TCHAR* name;
    ULONGLONG hideDelayMs;
    ULONGLONG idleCheckMs;
    float posSpeedIn;
    float posAccelOut;
    float alphaSpeedIn;
    float alphaAccelOut;
};

const ConfigProfile PRESETS[] = {
    { _T("默认"), 5000, 200, 0.0f, 0.0f, 16.0f, 128.0f },
    { _T("快速"), 3000, 100, 0.0f, 0.0f, 32.0f, 4096.0f },
    { _T("默认滑动"), 8000, 200, 12.0f, 512.0f, 4.0f, 1024.0f },
    { _T("快速滑动"), 5000, 100, 24.0f, 1024.0f, 8.0f, 1024.0f },
    { _T("默认抽屉"), 10000, 200, 16.0f, 2048.0f, 0.0f, 0.0f },
    { _T("快速抽屉"), 6000, 100, 28.0f, 8192.0f, 0.0f, 0.0f },
    { _T("暂停隐藏 (常显)"), 0xFFFFFFFF, 1000, 20.0f, 3000.0f, 20.0f, 1000.0f }
};
const int PRESET_COUNT = (int)(sizeof(PRESETS) / sizeof(PRESETS[0]));

struct MaskOption {
    int percent;
    int alpha;
};

const MaskOption MASK_OPTIONS[] = {
    { 0, 0 }, { 20, 51 }, { 40, 102 }, { 50, 128 },
    { 60, 153 }, { 70, 179 }, { 75, 191 }, { 80, 204 },
    { 85, 217 }, { 90, 230 }, { 95, 242 }
};
const int MASK_OPT_COUNT = (int)(sizeof(MASK_OPTIONS) / sizeof(MASK_OPTIONS[0]));

// ==========================================
// === 全局状态 ===
// ==========================================

#define WM_TRAYICON (WM_USER + 1)
#define ID_TRAY_EXIT 9001
#define ID_PROFILE_START 9100
#define ID_MASK_START 9200

const TCHAR* REG_SUBKEY = _T("Software\\MyDesktopHider");
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

    float currentY, targetY, velocityY;
    float currentAlpha, targetAlpha, velocityAlpha;

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
// 辅助函数：强制显示桌面 (用于交互触发)
// ---------------------------------------------------------
void ForceShowDesktop() {
    g.startupState = STARTUP_NORMAL; // 结束任何启动状态
    g.targetY = 0.0f;
    g.targetAlpha = 255.0f;
    g.isHidden = false;
    g.lastActiveTime = GetTickCount64(); // 重置闲置计时器
    // 立即更新物理目标，防止还在滑动
    g.currentY = 0.0f;
    g.currentAlpha = 255.0f;
}

// ---------------------------------------------------------
// 注册表
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
    g.maxMaskAlpha = MASK_OPTIONS[g.maskOptIndex].alpha;
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
    wc.lpszClassName = _T("DH_MaskWindow");
    RegisterClassEx(&wc);
    // 使用 WS_EX_TRANSPARENT 确保鼠标点击穿透
    g.hMaskWindow = CreateWindowEx(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW,
        _T("DH_MaskWindow"), NULL, WS_POPUP, 0, 0, 0, 0, NULL, NULL, hInstance, NULL);
}

void AttachMaskToDesktop() {
    if (!g.hMaskWindow || !IsWindow(g.hMaskWindow)) return;
    if (!g.hContainer || !IsWindow(g.hContainer)) return;
    if (!g.hDesktopParent || !IsWindow(g.hDesktopParent)) return;

    SetParent(g.hMaskWindow, g.hDesktopParent);
    LONG_PTR style = GetWindowLongPtr(g.hMaskWindow, GWL_STYLE);
    style &= ~WS_POPUP; style |= WS_CHILD;
    SetWindowLongPtr(g.hMaskWindow, GWL_STYLE, style);

    SetWindowPos(g.hMaskWindow, NULL, 0, 0, g.screenW, g.screenH, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
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

        g.currentY = 0.0f;
        g.currentAlpha = 255.0f;
        g.velocityY = 0.0f;
        g.velocityAlpha = 0.0f;

        g.targetY = (float)g.screenH;
        g.targetAlpha = 0.0f;

        g.isHidden = true;
        g.startupState = STARTUP_PHASE_1_HIDING;
        g.startupPhaseStartTime = GetTickCount64();

        g.lastRenderY = -9999;
        g.lastRenderAlpha = -1;
        g.lastMaskAlpha = -1;
        g.zOrderGuardCounter = 0;

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

    if (g.hContainer && IsWindow(g.hContainer)) {
        EnableLayeredStyle(g.hContainer, false);
        SetWindowPos(g.hContainer, NULL, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        RedrawWindow(g.hContainer, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
    }

    DWORD pid = 0;
    HWND hShellWnd = FindWindow(_T("Progman"), NULL);
    if (!hShellWnd) hShellWnd = FindWindow(_T("Shell_TrayWnd"), NULL);
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
// 托盘与菜单
// ---------------------------------------------------------
void InitTrayIcon(HWND hwnd) {
    memset(&nid, 0, sizeof(nid));
    nid.cbSize = sizeof(NOTIFYICONDATA);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    _tcscpy_s(nid.szTip, _countof(nid.szTip), _T("桌面图标自动隐藏"));
    Shell_NotifyIcon(NIM_ADD, &nid);
}

void ShowTrayMenu(HWND hwnd) {
    POINT p; GetCursorPos(&p);
    SetForegroundWindow(hwnd);
    HMENU hMenu = CreatePopupMenu();

    HMENU hSubProfile = CreatePopupMenu();
    for (int i = 0; i < PRESET_COUNT; i++) {
        UINT flags = MF_STRING;
        if (i == g.cfgIndex) flags |= MF_CHECKED;
        AppendMenu(hSubProfile, flags, ID_PROFILE_START + i, PRESETS[i].name);
    }
    AppendMenu(hMenu, MF_POPUP, (UINT_PTR)hSubProfile, _T("动画模式"));

    HMENU hSubMask = CreatePopupMenu();
    for (int i = 0; i < MASK_OPT_COUNT; i++) {
        UINT flags = MF_STRING;
        if (i == g.maskOptIndex) flags |= MF_CHECKED;
        TCHAR buf[64] = { 0 };
        if (MASK_OPTIONS[i].percent == 0) _tcscpy_s(buf, _countof(buf), _T("关闭"));
        else _stprintf_s(buf, _countof(buf), _T("蒙版浓度 %d%%"), MASK_OPTIONS[i].percent);
        AppendMenu(hSubMask, flags, ID_MASK_START + i, buf);
    }
    AppendMenu(hMenu, MF_POPUP, (UINT_PTR)hSubMask, _T("背景蒙版"));
    AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(hMenu, MF_STRING, ID_TRAY_EXIT, _T("退出并重置"));

    TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, p.x, p.y, 0, hwnd, NULL);
    DestroyMenu(hMenu);
}

// ---------------------------------------------------------
// 物理引擎
// ---------------------------------------------------------
void TimerInit() {
    QueryPerformanceFrequency(&qpcFreq);
    QueryPerformanceCounter(&qpcLastTime);
}

float TimerGetDelta() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    float dt = (float)((double)(now.QuadPart - qpcLastTime.QuadPart) / (double)qpcFreq.QuadPart);
    qpcLastTime = now;
    if (dt > 0.05f) dt = 0.05f;
    if (dt < 0.0001f) dt = 0.0001f;
    return dt;
}

void UpdatePhysics(float dt) {
    if (!g.hContainer) return;

    if (g.startupState != STARTUP_NORMAL) {
        dt *= STARTUP_SPEED_FACTOR;
    }

    // Y轴
    float diffY = g.targetY - g.currentY;
    if (fabs(diffY) > 0.1f || fabs(g.velocityY) > 0.1f) {
        if (g.targetY > g.currentY) {
            g.velocityY += g.cfg->posAccelOut * dt;
            g.currentY += g.velocityY * dt;
            if (g.currentY > g.targetY) { g.currentY = g.targetY; g.velocityY = 0.0f; }
        }
        else {
            float speed = diffY * g.cfg->posSpeedIn;
            if (speed > -20.0f) speed = -20.0f;
            g.currentY += speed * dt;
            if (g.currentY < g.targetY) g.currentY = g.targetY;
        }
    }
    else { g.currentY = g.targetY; }

    // Alpha
    float diffAlpha = g.targetAlpha - g.currentAlpha;
    if (fabs(diffAlpha) > 0.1f || fabs(g.velocityAlpha) > 0.1f) {
        if (g.targetAlpha < g.currentAlpha) {
            g.velocityAlpha += g.cfg->alphaAccelOut * dt;
            g.currentAlpha -= g.velocityAlpha * dt;
            if (g.currentAlpha < g.targetAlpha) { g.currentAlpha = g.targetAlpha; g.velocityAlpha = 0.0f; }
        }
        else {
            float speed = diffAlpha * g.cfg->alphaSpeedIn;
            if (speed < 10.0f) speed = 10.0f;
            g.currentAlpha += speed * dt;
            if (g.currentAlpha > g.targetAlpha) g.currentAlpha = g.targetAlpha;
        }
    }
    else { g.currentAlpha = g.targetAlpha; }

    if (isnan(g.currentY)) g.currentY = g.targetY;
    if (isnan(g.currentAlpha)) g.currentAlpha = g.targetAlpha;

    // --- 渲染 ---
    int renderY = (int)g.currentY;
    int renderAlpha = (int)g.currentAlpha;
    if (renderAlpha < 0) renderAlpha = 0; if (renderAlpha > 255) renderAlpha = 255;

    // 1. 图标层
    if (renderY != g.lastRenderY) {
        SetWindowPos(g.hContainer, NULL, 0, renderY, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        if (g.hMaskWindow && IsWindow(g.hMaskWindow) && IsWindowVisible(g.hMaskWindow)) {
            SetWindowPos(g.hMaskWindow, NULL, 0, renderY, g.screenW, g.screenH, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        g.lastRenderY = renderY;
    }

    if (renderAlpha != g.lastRenderAlpha) {
        SetLayeredWindowAttributes(g.hContainer, 0, (BYTE)renderAlpha, LWA_ALPHA);
        g.lastRenderAlpha = renderAlpha;
    }

    // 2. 蒙版层
    if (g.hMaskWindow && IsWindowVisible(g.hMaskWindow)) {
        if (g.maxMaskAlpha > 0) {
            float ratio = g.currentAlpha / 255.0f;
            int maskCurrentAlpha = (int)(g.maxMaskAlpha * ratio);
            if (maskCurrentAlpha != g.lastMaskAlpha) {
                SetLayeredWindowAttributes(g.hMaskWindow, 0, (BYTE)maskCurrentAlpha, LWA_ALPHA);
                g.lastMaskAlpha = maskCurrentAlpha;
            }
        }
    }

    g.zOrderGuardCounter++;
    if (g.zOrderGuardCounter > 60) {
        EnforceZOrder();
        g.zOrderGuardCounter = 0;
    }
}

bool IsPhysicsIdle() {
    return (fabs(g.targetY - g.currentY) < 0.5f && fabs(g.velocityY) < 1.0f) &&
        (fabs(g.targetAlpha - g.currentAlpha) < 0.5f && fabs(g.velocityAlpha) < 1.0f);
}

// ---------------------------------------------------------
// 消息处理 (UX 优化)
// ---------------------------------------------------------
bool IsMouseOnDesktop() {
    POINT pt; GetCursorPos(&pt);
    HWND hWin = WindowFromPoint(pt);
    if (!hWin) return false;

    // [优化] 如果鼠标在蒙版上，由于蒙版全屏且透明，应该视为在桌面上
    // 但如果蒙版完全隐藏或不可见，就不算。
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
        g.hDesktopParent = NULL;
        return 0;
    }

    switch (msg) {
    case WM_TRAYICON:
        if (lParam == WM_RBUTTONUP) {
            // [UX优化] 打开菜单时，立即唤醒图标
            ForceShowDesktop();
            ShowTrayMenu(hwnd);
        }
        break;

    case WM_WTSSESSION_CHANGE:
        if (wParam == WTS_SESSION_LOCK) g.isPaused = true;
        else if (wParam == WTS_SESSION_UNLOCK) {
            g.isPaused = false;
            g.lastActiveTime = GetTickCount64();
            TimerGetDelta();
        }
        break;

    case WM_COMMAND:
    {
        int cmdId = LOWORD(wParam);
        if (cmdId == ID_TRAY_EXIT) {
            g.appRunning = false;
        }
        else if (cmdId >= ID_PROFILE_START && cmdId < ID_PROFILE_START + PRESET_COUNT) {
            g.cfgIndex = cmdId - ID_PROFILE_START;
            g.cfg = &PRESETS[g.cfgIndex];
            SaveSettings();
            // [UX优化] 切换配置后，立即重置状态并显示
            ForceShowDesktop();
        }
        else if (cmdId >= ID_MASK_START && cmdId < ID_MASK_START + MASK_OPT_COUNT) {
            g.maskOptIndex = cmdId - ID_MASK_START;
            g.maxMaskAlpha = MASK_OPTIONS[g.maskOptIndex].alpha;
            SaveSettings();
            g.lastMaskAlpha = -1;
            if (!g.hMaskWindow) { CreateMaskWindow(GetModuleHandle(NULL)); AttachMaskToDesktop(); }
            else AttachMaskToDesktop();
            // [UX优化] 修改蒙版也唤醒一下，方便查看效果
            ForceShowDesktop();
        }
    }
    break;

    case WM_DISPLAYCHANGE:
        Sleep(500);
        g.hContainer = NULL;
        break;

    case WM_CLOSE:
    case WM_DESTROY:
    case WM_ENDSESSION:
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
    wc.lpszClassName = _T("DH_Core");
    RegisterClassEx(&wc);
    g.hMsgWindow = CreateWindowEx(0, wc.lpszClassName, _T(""), 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, hInstance, NULL);
}

// ---------------------------------------------------------
// 主入口
// ---------------------------------------------------------
int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrev, _In_ LPWSTR lpCmdLine, _In_ int nShow) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);

    HANDLE hMutex = CreateMutex(NULL, TRUE, _T("Local\\DH_StrictFix_Instance"));
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    g_uMsgTaskbarCreated = RegisterWindowMessage(_T("TaskbarCreated"));

    g.appRunning = true;
    g.isPaused = false;
    LoadSettings();
    g.cfg = &PRESETS[g.cfgIndex];
    g.lastActiveTime = GetTickCount64();

    CreateMessageWindow(hInstance);
    WTSRegisterSessionNotification(g.hMsgWindow, NOTIFY_FOR_THIS_SESSION);

    InitTrayIcon(g.hMsgWindow);
    LocateDesktop(hInstance);
    TimerInit();

    if (!g.hContainer) {
        MessageBox(NULL, _T("桌面未找到"), _T("Error"), MB_ICONERROR);
        Shell_NotifyIcon(NIM_DELETE, &nid);
        WTSUnRegisterSessionNotification(g.hMsgWindow);
        if (hMutex) CloseHandle(hMutex);
        return 1;
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

        POINT currMouse;
        GetCursorPos(&currMouse);
        ULONGLONG currTime = GetTickCount64();
        bool isMoving = (abs(currMouse.x - g.lastMousePos.x) > MOUSE_MOVE_THRESHOLD ||
            abs(currMouse.y - g.lastMousePos.y) > MOUSE_MOVE_THRESHOLD);

        // --- 启动状态机 ---
        if (g.startupState == STARTUP_PHASE_1_HIDING) {
            bool isMostlyHidden = (g.currentAlpha < 5.0f || g.currentY > g.screenH * 0.9f);

            float safeFactor = (STARTUP_SPEED_FACTOR > 0.1f) ? STARTUP_SPEED_FACTOR : 0.1f;
            ULONGLONG dynamicTimeout = (ULONGLONG)(1500.0f / safeFactor);

            bool isTimedOut = (currTime - g.startupPhaseStartTime > dynamicTimeout);

            if (isMostlyHidden || isTimedOut) {
                g.startupState = STARTUP_PHASE_2_WAITING;
                g.waitStartTime = currTime;
            }
        }
        else if (g.startupState == STARTUP_PHASE_2_WAITING) {
            if (currTime - g.waitStartTime > STARTUP_TRANSITION_DELAY) {
                if (g.maxMaskAlpha > 0) {
                    if (!g.hMaskWindow) CreateMaskWindow(hInstance);
                    AttachMaskToDesktop();
                }
                g.startupState = STARTUP_PHASE_3_SHOWING;
                g.targetY = 0.0f;
                g.targetAlpha = 255.0f;
                g.isHidden = false;
                g.lastActiveTime = currTime; // 这里的计时不重要，因为动画还在跑
                EnforceZOrder();
            }
        }
        else if (g.startupState == STARTUP_PHASE_3_SHOWING) {
            // [修复1] 动画结束，进入 NORMAL 模式的那一瞬间，重置计时器
            // 确保“第一次隐藏”的时间是从动画完全停止那一刻开始算的
            if (IsPhysicsIdle()) {
                g.startupState = STARTUP_NORMAL;
                g.lastActiveTime = currTime; // <--- 关键修复：重置基准时间
            }
            // 如果动画过程中用户动了鼠标，也直接进入 Normal 并重置
            if (isMoving && IsMouseOnDesktop()) {
                g.startupState = STARTUP_NORMAL;
                g.lastActiveTime = currTime; // <--- 关键修复
            }
        }
        else {
            // 正常模式 (STARTUP_NORMAL)
            if (isMoving) {
                if (IsMouseOnDesktop()) {
                    g.lastActiveTime = currTime;
                    g.targetY = 0.0f;
                    g.targetAlpha = 255.0f;
                    g.isHidden = false;
                }
            }
            else {
                // 判断超时
                if (!g.isHidden && (currTime - g.lastActiveTime > g.cfg->hideDelayMs)) {
                    g.targetY = (float)g.screenH;
                    g.targetAlpha = 0.0f;
                    g.isHidden = true;
                    g.velocityY = 0.0f;
                    g.velocityAlpha = 0.0f;
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

            static int idleCount = 0;
            if (++idleCount > 10) {
                EnforceZOrder();
                idleCount = 0;
            }
            Sleep((DWORD)g.cfg->idleCheckMs);
            TimerGetDelta();
        }
    }

    WTSUnRegisterSessionNotification(g.hMsgWindow);
    PerformExitSequence();
    if (hMutex) CloseHandle(hMutex);
    return (int)msg.wParam;
}