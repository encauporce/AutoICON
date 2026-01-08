/*
    Auto-Hiding Desktop Icons (Windows 11 Spring Dynamics Edition)

    [升级日志]
    1. 核心引擎：从"线性加速度"升级为"弹簧动力学 (Spring Mass-Damper)"。
       - 模拟 Windows 11 UI 的回弹与阻尼感。
       - 相比旧版，停止时更柔和，启动时更灵敏。
    2. 配置文件：重组为7种标准化预设 (渐变/位移/混合 x 标准/快速 + 常显)。
    3. 逻辑保留：保留了之前所有的蒙版修复、抽屉模式视觉修正和 Z 序保护。

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
// === 弹簧物理常量 ===
// ==========================================
// 模拟 Win11 默认手感: Tension ~150-180, Friction ~20-26 (临界阻尼)
// 快速手感: Tension ~350-400, Friction ~35-40

struct SpringParams {
    float tension;  // 张力 (k): 越大越快，回弹越强
    float friction; // 摩擦 (c): 越大越不震荡，过大会迟滞
    bool  enabled;  // 是否启用该轴运动
};

// ==========================================
// === 配置结构 ===
// ==========================================
struct ConfigProfile {
    const TCHAR* name;
    ULONGLONG hideDelayMs;
    ULONGLONG idleCheckMs;
    SpringParams motion;   // Y轴位移弹簧
    SpringParams opacity;  // 透明度弹簧
};

const ConfigProfile PRESETS[] = {
    // 1. 仅渐变 (默认) - 位移禁用，透明度标准
    { _T("仅渐变 (默认)"), 5000, 200, { 0.0f, 0.0f, false }, { 170.0f, 26.0f, true } },

    // 2. 仅渐变 (快速) - 位移禁用，透明度快速
    { _T("仅渐变 (快速)"), 3000, 100, { 0.0f, 0.0f, false }, { 380.0f, 40.0f, true } },

    // 3. 仅位移 (默认) - 位移标准，透明度禁用 (常亮255，即抽屉模式)
    { _T("仅位移 (默认/抽屉)"), 8000, 200, { 170.0f, 26.0f, true }, { 0.0f, 0.0f, false } },

    // 4. 仅位移 (快速) - 位移快速，透明度禁用
    { _T("仅位移 (快速/抽屉)"), 5000, 100, { 400.0f, 42.0f, true }, { 0.0f, 0.0f, false } },

    // 5. 混合模式 (默认) - 既跑位移也跑透明度
    { _T("渐变 + 位移 (默认)"), 6000, 200, { 160.0f, 26.0f, true }, { 160.0f, 26.0f, true } },

    // 6. 混合模式 (快速)
    { _T("渐变 + 位移 (快速)"), 4000, 100, { 380.0f, 40.0f, true }, { 380.0f, 40.0f, true } },

    // 7. 常显
    { _T("暂停隐藏 (常显)"), 0xFFFFFFFF, 1000, { 200.0f, 30.0f, true }, { 200.0f, 30.0f, true } }
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
// 启动/重启动画的全局减速因子 (模拟从睡眠唤醒时的 sluggish 效果)
const float STARTUP_SPEED_FACTOR = 0.7f;
const ULONGLONG STARTUP_TRANSITION_DELAY = 400;

#define WM_TRAYICON (WM_USER + 1)
#define ID_TRAY_EXIT 9001
#define ID_PROFILE_START 9100
#define ID_MASK_START 9200

const TCHAR* REG_SUBKEY = _T("Software\\MyDesktopHider_Spring");
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

    // 物理状态
    float currentY, velocityY;
    float currentAlpha, velocityAlpha;

    // 逻辑目标
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
// 注册表与设置
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
// 窗口查找与层级
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

// ---------------------------------------------------------
// 蒙版窗口
// ---------------------------------------------------------
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

    SetWindowPos(g.hMaskWindow, NULL, 0, 0, g.screenW, g.screenH, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    EnforceZOrder();
    SetLayeredWindowAttributes(g.hMaskWindow, 0, 0, LWA_ALPHA);
    ShowWindow(g.hMaskWindow, SW_SHOWNA);
}

// ---------------------------------------------------------
// 状态控制
// ---------------------------------------------------------
void TriggerRestartAnimation() {
    g.startupState = STARTUP_PHASE_1_HIDING;
    g.startupPhaseStartTime = GetTickCount64();
    g.isHidden = true;

    // 强制目标为隐藏状态
    g.targetY = (float)g.screenH;
    g.targetAlpha = 0.0f;
}

void ForceShowImmediate() {
    g.startupState = STARTUP_NORMAL;
    g.targetY = 0.0f;
    g.targetAlpha = 255.0f;
    g.currentY = 0.0f;
    g.currentAlpha = 255.0f;
    g.velocityY = 0.0f;
    g.velocityAlpha = 0.0f;
    g.isHidden = false;
    g.lastActiveTime = GetTickCount64();
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

        // 初始化状态
        g.currentY = 0.0f;
        g.currentAlpha = 255.0f;
        g.velocityY = 0.0f;
        g.velocityAlpha = 0.0f;

        g.targetY = (float)g.screenH;
        g.targetAlpha = 0.0f;
        g.isHidden = true;

        g.startupState = STARTUP_PHASE_1_HIDING;
        g.startupPhaseStartTime = GetTickCount64();
        g.hasPendingMask = false;

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
    // 简单重启资源管理器以恢复原状
    ShellExecute(NULL, _T("open"), _T("explorer.exe"), NULL, NULL, SW_SHOWDEFAULT);
}

// ---------------------------------------------------------
// 托盘菜单
// ---------------------------------------------------------
void InitTrayIcon(HWND hwnd) {
    memset(&nid, 0, sizeof(nid));
    nid.cbSize = sizeof(NOTIFYICONDATA);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    _tcscpy_s(nid.szTip, _countof(nid.szTip), _T("桌面图标自动隐藏 (Spring Edition)"));
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
        int checkIndex = g.hasPendingMask ? g.pendingMaskOptIndex : g.maskOptIndex;
        if (i == checkIndex) flags |= MF_CHECKED;
        TCHAR buf[64] = { 0 };
        if (MASK_OPTIONS[i].percent == 0) _tcscpy_s(buf, _countof(buf), _T("关闭"));
        else _stprintf_s(buf, _countof(buf), _T("蒙版浓度 %d%%"), MASK_OPTIONS[i].percent);
        AppendMenu(hSubMask, flags, ID_MASK_START + i, buf);
    }
    AppendMenu(hMenu, MF_POPUP, (UINT_PTR)hSubMask, _T("背景蒙版"));
    AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(hMenu, MF_STRING, ID_TRAY_EXIT, _T("退出程序"));

    TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, p.x, p.y, 0, hwnd, NULL);
    DestroyMenu(hMenu);
}

// ---------------------------------------------------------
// 物理引擎：弹簧系统
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
    if (dt > 0.05f) dt = 0.05f; // 防止过大跳跃
    if (dt < 0.0001f) dt = 0.0001f;
    return dt;
}

// 核心弹簧求解器 (半隐式欧拉积分)
void SolveSpring(float& current, float& velocity, float target, float tension, float friction, float dt) {
    float displacement = current - target;
    float acceleration = -tension * displacement - friction * velocity;
    velocity += acceleration * dt;
    current += velocity * dt;
}

void UpdatePhysics(float dt) {
    if (!g.hContainer) return;

    // 启动阶段全局减速
    if (g.startupState != STARTUP_NORMAL) {
        dt *= STARTUP_SPEED_FACTOR;
    }

    // 1. 确定实际的物理目标
    // -----------------------------------------------------------
    float physicsTargetY = g.targetY;
    float physicsTargetAlpha = g.targetAlpha;

    // 修正：如果配置禁用了某轴运动，强行锁定目标
    if (!g.cfg->motion.enabled) {
        physicsTargetY = 0.0f; // 始终在原位
    }
    if (!g.cfg->opacity.enabled) {
        physicsTargetAlpha = 255.0f; // 始终不透明
    }

    // 2. 物理计算 (Y轴)
    // -----------------------------------------------------------
    if (g.cfg->motion.enabled) {
        SolveSpring(g.currentY, g.velocityY, physicsTargetY,
            g.cfg->motion.tension, g.cfg->motion.friction, dt);
    }
    else {
        // 如果禁用位移，直接归位
        g.currentY = 0.0f;
        g.velocityY = 0.0f;
    }

    // 3. 物理计算 (Alpha轴)
    // -----------------------------------------------------------
    if (g.cfg->opacity.enabled) {
        SolveSpring(g.currentAlpha, g.velocityAlpha, physicsTargetAlpha,
            g.cfg->opacity.tension, g.cfg->opacity.friction, dt);
    }
    else {
        // 如果禁用透明度，直接设为不透明
        g.currentAlpha = 255.0f;
        g.velocityAlpha = 0.0f;
    }

    // 边界钳制
    if (g.currentAlpha < 0.0f) g.currentAlpha = 0.0f;
    if (g.currentAlpha > 255.0f) g.currentAlpha = 255.0f;

    // 4. 渲染应用
    // -----------------------------------------------------------
    int renderY = (int)g.currentY;
    int renderAlpha = (int)g.currentAlpha;

    // 容器窗口应用
    if (renderY != g.lastRenderY) {
        SetWindowPos(g.hContainer, NULL, 0, renderY, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        // 蒙版随动
        if (g.hMaskWindow && IsWindow(g.hMaskWindow) && IsWindowVisible(g.hMaskWindow)) {
            SetWindowPos(g.hMaskWindow, NULL, 0, renderY, g.screenW, g.screenH, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        g.lastRenderY = renderY;
    }

    if (renderAlpha != g.lastRenderAlpha) {
        SetLayeredWindowAttributes(g.hContainer, 0, (BYTE)renderAlpha, LWA_ALPHA);
        g.lastRenderAlpha = renderAlpha;
    }

    // 5. 蒙版智能淡出逻辑 (Drawer Fix)
    // -----------------------------------------------------------
    if (g.hMaskWindow && IsWindowVisible(g.hMaskWindow) && g.maxMaskAlpha > 0) {
        int maskCurrentAlpha = 0;

        if (g.cfg->opacity.enabled) {
            // 普通模式：蒙版透明度跟随图标透明度比例
            float ratio = g.currentAlpha / 255.0f;
            maskCurrentAlpha = (int)(g.maxMaskAlpha * ratio);
        }
        else {
            // 抽屉模式 (图标不透明)：根据 Y 轴位移模拟淡出
            // 当图标滑出屏幕底部 50% 后开始淡出蒙版
            float hiddenRatio = g.currentY / (float)g.screenH;
            float opacityRatio = 1.0f - hiddenRatio;
            // 简单曲线优化
            if (opacityRatio < 0.0f) opacityRatio = 0.0f;
            maskCurrentAlpha = (int)(g.maxMaskAlpha * opacityRatio);
        }

        if (maskCurrentAlpha != g.lastMaskAlpha) {
            SetLayeredWindowAttributes(g.hMaskWindow, 0, (BYTE)maskCurrentAlpha, LWA_ALPHA);
            g.lastMaskAlpha = maskCurrentAlpha;
        }
    }

    // Z序守护
    g.zOrderGuardCounter++;
    if (g.zOrderGuardCounter > 30) { // 提高频率
        EnforceZOrder();
        g.zOrderGuardCounter = 0;
    }
}

// 物理静止判定：速度极小且接近目标
bool IsPhysicsIdle() {
    bool yIdle = true;
    bool alphaIdle = true;

    // 只有启用的轴才参与判定
    if (g.cfg->motion.enabled) {
        float dy = fabs(g.targetY - g.currentY);
        yIdle = (dy < 1.0f && fabs(g.velocityY) < 2.0f);
    }

    if (g.cfg->opacity.enabled) {
        float da = fabs(g.targetAlpha - g.currentAlpha);
        alphaIdle = (da < 1.0f && fabs(g.velocityAlpha) < 2.0f);
    }

    return yIdle && alphaIdle;
}

// ---------------------------------------------------------
// 消息处理与主循环
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
        g.hContainer = NULL; // 触发重定位
        return 0;
    }
    switch (msg) {
    case WM_TRAYICON:
        if (lParam == WM_RBUTTONUP) {
            ForceShowImmediate();
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
    case WM_COMMAND: {
        int cmdId = LOWORD(wParam);
        if (cmdId == ID_TRAY_EXIT) {
            g.appRunning = false;
        }
        else if (cmdId >= ID_PROFILE_START && cmdId < ID_PROFILE_START + PRESET_COUNT) {
            g.cfgIndex = cmdId - ID_PROFILE_START;
            g.cfg = &PRESETS[g.cfgIndex];
            SaveSettings();
            ForceShowImmediate();
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
    wc.lpszClassName = _T("DH_Core_Spring");
    RegisterClassEx(&wc);
    g.hMsgWindow = CreateWindowEx(0, wc.lpszClassName, _T(""), 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, hInstance, NULL);
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE hPrev, LPWSTR lpCmdLine, int nShow) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);

    HANDLE hMutex = CreateMutex(NULL, TRUE, _T("Local\\DH_Spring_Instance"));
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
        MessageBox(NULL, _T("无法定位桌面窗口。"), _T("错误"), MB_ICONERROR);
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

        // 获取输入与时间
        POINT currMouse; GetCursorPos(&currMouse);
        ULONGLONG currTime = GetTickCount64();
        bool isMoving = (abs(currMouse.x - g.lastMousePos.x) > MOUSE_MOVE_THRESHOLD ||
            abs(currMouse.y - g.lastMousePos.y) > MOUSE_MOVE_THRESHOLD);

        // -----------------------------
        // 状态机逻辑
        // -----------------------------
        if (g.startupState == STARTUP_PHASE_1_HIDING) {
            // 等待直到几乎看不见
            bool hiddenEnough = true;
            if (g.cfg->motion.enabled) hiddenEnough &= (g.currentY > g.screenH * 0.9f);
            if (g.cfg->opacity.enabled) hiddenEnough &= (g.currentAlpha < 10.0f);

            // 超时保护
            if (hiddenEnough || (currTime - g.startupPhaseStartTime > 2000)) {
                g.startupState = STARTUP_PHASE_2_WAITING;
                g.waitStartTime = currTime;
            }
        }
        else if (g.startupState == STARTUP_PHASE_2_WAITING) {
            if (currTime - g.waitStartTime > STARTUP_TRANSITION_DELAY) {
                // 应用蒙版更改
                if (g.hasPendingMask) {
                    g.maskOptIndex = g.pendingMaskOptIndex;
                    g.maxMaskAlpha = MASK_OPTIONS[g.maskOptIndex].alpha;
                    g.hasPendingMask = false;
                    SaveSettings();
                    g.lastMaskAlpha = -1;
                }
                if (g.maxMaskAlpha > 0) {
                    if (!g.hMaskWindow) CreateMaskWindow(hInstance);
                    AttachMaskToDesktop();
                }

                g.startupState = STARTUP_PHASE_3_SHOWING;
                g.targetY = 0.0f;
                g.targetAlpha = 255.0f;
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
            // STARTUP_NORMAL
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

        // 物理循环
        if (!IsPhysicsIdle()) {
            float dt = TimerGetDelta();
            UpdatePhysics(dt);
            DwmFlush(); // 保持帧率同步
        }
        else {
            // 确保完全对齐
            if (g.currentY != g.targetY || g.currentAlpha != g.targetAlpha) UpdatePhysics(0.0f);
            Sleep((DWORD)g.cfg->idleCheckMs);
            TimerGetDelta(); // 重置计时器
        }
    }

    WTSUnRegisterSessionNotification(g.hMsgWindow);
    PerformExitSequence();
    if (hMutex) CloseHandle(hMutex);
    return 0;
}