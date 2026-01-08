/*
    Auto-Hiding Desktop Icons (High DPI + Dark Mask Edition)

    [修复与改进]
    1. 增加 DPI 感知：完美适配 125%, 150%, 200% 等高分屏，解决模糊和坐标错位问题。
    2. 增加分辨率变更响应：插拔显示器或改分辨率后自动调整蒙版大小。
    3. 逻辑优化：确保蒙版始终覆盖全屏。

    [编译指南]
    - Visual Studio: Windows Desktop Application (C++)
    - 依赖库: dwmapi.lib, shell32.lib, advapi32.lib, user32.lib
*/

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <tchar.h>
#include <math.h>
#include <vector>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

// ==========================================
// === 配置文件定义 ===
// ==========================================

struct ConfigProfile {
    const TCHAR* name;
    DWORD hideDelayMs;
    DWORD idleCheckMs;
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
const int PRESET_COUNT = sizeof(PRESETS) / sizeof(PRESETS[0]);

// ==========================================
// === 蒙版浓度选项 ===
// ==========================================
struct MaskOption {
    int percent;
    int alpha;
};

const MaskOption MASK_OPTIONS[] = {
    { 0, 0 },       // 关闭
    { 20, 51 },
    { 40, 102 },
    { 50, 128 },
    { 60, 153 },
    { 70, 179 },
    { 75, 191 },
    { 80, 204 },
    { 85, 217 },
    { 90, 230 },
    { 95, 242 }
};
const int MASK_OPT_COUNT = sizeof(MASK_OPTIONS) / sizeof(MASK_OPTIONS[0]);

// ==========================================
// === 全局变量 ===
// ==========================================

#define WM_TRAYICON (WM_USER + 1)
#define ID_TRAY_EXIT 9001
#define ID_PROFILE_START 9100
#define ID_MASK_START 9200

const TCHAR* REG_SUBKEY = _T("Software\\MyDesktopHider");
const TCHAR* REG_VAL_PROFILE = _T("LastProfileIndex");
const TCHAR* REG_VAL_MASK = _T("MaskOpacityIndex");

struct GlobalState {
    HWND hContainer;        // SHELLDLL_DefView
    HWND hListView;         // SysListView32
    HWND hWorkerW;          // Wallpaper container
    HWND hMsgWindow;        // Message receiver
    HWND hMaskWindow;       // Black Mask

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

    POINT lastMousePos;
    DWORD lastActiveTime;
    bool isHidden;
    bool appRunning;
} g;

NOTIFYICONDATA nid = { 0 };
LARGE_INTEGER qpcFreq;
LARGE_INTEGER qpcLastTime;
const int MOUSE_MOVE_THRESHOLD = 2;

// ---------------------------------------------------------
// 注册表与配置
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
    g.cfgIndex = 0;
    g.maskOptIndex = 0;

    if (RegOpenKeyEx(HKEY_CURRENT_USER, REG_SUBKEY, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD size = sizeof(DWORD);
        RegQueryValueEx(hKey, REG_VAL_PROFILE, NULL, NULL, (BYTE*)&g.cfgIndex, &size);
        size = sizeof(DWORD);
        RegQueryValueEx(hKey, REG_VAL_MASK, NULL, NULL, (BYTE*)&g.maskOptIndex, &size);
        RegCloseKey(hKey);
    }

    if (g.cfgIndex >= PRESET_COUNT) g.cfgIndex = 0;
    if (g.maskOptIndex >= MASK_OPT_COUNT) g.maskOptIndex = 0;

    g.maxMaskAlpha = MASK_OPTIONS[g.maskOptIndex].alpha;
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
    wc.lpszClassName = _T("DH_MaskWindow");
    RegisterClassEx(&wc);

    // WS_EX_TOOLWINDOW: 隐藏 Alt+Tab
    // WS_EX_TRANSPARENT: 鼠标穿透
    g.hMaskWindow = CreateWindowEx(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW,
        _T("DH_MaskWindow"),
        NULL,
        WS_POPUP,
        0, 0, g.screenW, g.screenH, // 初始化时使用当前获取的屏幕宽高
        NULL, NULL, hInstance, NULL);
}

void AttachMaskToDesktop() {
    if (!g.hMaskWindow || !g.hWorkerW) return;

    SetParent(g.hMaskWindow, g.hWorkerW);
    // 确保蒙版覆盖整个屏幕 (适配动态分辨率变化)
    SetWindowPos(g.hMaskWindow, g.hContainer, 0, 0, g.screenW, g.screenH, SWP_NOACTIVATE);
    SetLayeredWindowAttributes(g.hMaskWindow, 0, 0, LWA_ALPHA);
    ShowWindow(g.hMaskWindow, SW_SHOWNA);
}

// ---------------------------------------------------------
// 核心：窗口查找与尺寸同步
// ---------------------------------------------------------
BOOL CALLBACK FindSysListViewProc(HWND hwnd, LPARAM lParam) {
    HWND hShellView = FindWindowEx(hwnd, NULL, _T("SHELLDLL_DefView"), NULL);
    if (hShellView) {
        HWND hListView = FindWindowEx(hShellView, NULL, _T("SysListView32"), NULL);
        if (hListView) {
            g.hContainer = hShellView;
            g.hListView = hListView;
            g.hWorkerW = hwnd;
            return FALSE;
        }
    }
    return TRUE;
}

void EnableLayeredStyle(HWND hwnd, bool enable) {
    if (!hwnd || !IsWindow(hwnd)) return;
    LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    if (enable) {
        if (!(exStyle & WS_EX_LAYERED)) SetWindowLongPtr(hwnd, GWL_EXSTYLE, exStyle | WS_EX_LAYERED);
    }
    else {
        if (exStyle & WS_EX_LAYERED) SetWindowLongPtr(hwnd, GWL_EXSTYLE, exStyle & ~WS_EX_LAYERED);
    }
}

void LocateDesktop() {
    g.hContainer = NULL;
    g.hListView = NULL;
    g.hWorkerW = NULL;
    HWND hProgman = FindWindow(_T("Progman"), NULL);
    FindSysListViewProc(hProgman, 0);
    if (!g.hContainer) EnumWindows(FindSysListViewProc, 0);

    if (g.hContainer) {
        // [修复] 获取真实的物理像素尺寸 (因为已开启 DPI Aware)
        RECT rect;
        GetWindowRect(g.hContainer, &rect);
        g.screenW = rect.right - rect.left;
        g.screenH = rect.bottom - rect.top;

        // 如果获取失败，兜底方案
        if (g.screenH == 0) {
            g.screenW = GetSystemMetrics(SM_CXSCREEN);
            g.screenH = GetSystemMetrics(SM_CYSCREEN);
        }

        // 状态重置
        g.currentY = 0.0f; g.velocityY = 0.0f;
        g.currentAlpha = 255.0f; g.velocityAlpha = 0.0f;
        g.lastRenderY = 0; g.lastRenderAlpha = 255; g.lastMaskAlpha = -1;

        // 图标归位
        SetWindowPos(g.hContainer, NULL, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        EnableLayeredStyle(g.hContainer, true);
        SetLayeredWindowAttributes(g.hContainer, 0, 255, LWA_ALPHA);

        // 更新蒙版尺寸以匹配新分辨率
        AttachMaskToDesktop();
    }
}

// ---------------------------------------------------------
// 退出修复逻辑
// ---------------------------------------------------------
void PerformExitSequence() {
    if (g.hMaskWindow && IsWindow(g.hMaskWindow)) DestroyWindow(g.hMaskWindow);
    Shell_NotifyIcon(NIM_DELETE, &nid);

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
    _tcscpy_s(nid.szTip, _T("桌面图标自动隐藏 (DPI适配版)"));
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
        TCHAR buf[32];
        if (MASK_OPTIONS[i].percent == 0) _tcscpy_s(buf, _T("关闭 (0%)"));
        else _stprintf_s(buf, _T("浓度 %d%%"), MASK_OPTIONS[i].percent);
        AppendMenu(hSubMask, flags, ID_MASK_START + i, buf);
    }
    AppendMenu(hMenu, MF_POPUP, (UINT_PTR)hSubMask, _T("蒙版浓度"));

    AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(hMenu, MF_STRING, ID_TRAY_EXIT, _T("退出并重启Explorer"));

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
    if (dt > 0.1f) dt = 0.05f;
    return dt;
}

void UpdatePhysics(float dt) {
    if (!g.hContainer) return;

    // --- Pos Y ---
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

    // --- Alpha ---
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

    // --- Render ---
    int renderY = (int)g.currentY;
    int renderAlpha = (int)g.currentAlpha;
    if (renderAlpha < 0) renderAlpha = 0; if (renderAlpha > 255) renderAlpha = 255;

    if (renderY != g.lastRenderY) {
        SetWindowPos(g.hContainer, NULL, 0, renderY, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        // 同步移动蒙版，确保它始终跟着图标层走
        if (g.hMaskWindow) {
            SetWindowPos(g.hMaskWindow, g.hContainer, 0, renderY, g.screenW, g.screenH, SWP_NOACTIVATE);
        }
        g.lastRenderY = renderY;
    }

    if (renderAlpha != g.lastRenderAlpha) {
        SetLayeredWindowAttributes(g.hContainer, 0, (BYTE)renderAlpha, LWA_ALPHA);
        g.lastRenderAlpha = renderAlpha;
    }

    // --- Mask Alpha ---
    if (g.hMaskWindow && g.maxMaskAlpha > 0) {
        float ratio = g.currentAlpha / 255.0f;
        int maskCurrentAlpha = (int)(g.maxMaskAlpha * ratio);

        if (maskCurrentAlpha != g.lastMaskAlpha) {
            SetLayeredWindowAttributes(g.hMaskWindow, 0, (BYTE)maskCurrentAlpha, LWA_ALPHA);
            g.lastMaskAlpha = maskCurrentAlpha;
        }
    }
    else if (g.hMaskWindow && g.maxMaskAlpha == 0 && g.lastMaskAlpha != 0) {
        SetLayeredWindowAttributes(g.hMaskWindow, 0, 0, LWA_ALPHA);
        g.lastMaskAlpha = 0;
    }
}

bool IsPhysicsIdle() {
    return (fabs(g.targetY - g.currentY) < 0.5f && fabs(g.velocityY) < 1.0f) &&
        (fabs(g.targetAlpha - g.currentAlpha) < 0.5f && fabs(g.velocityAlpha) < 1.0f);
}

// ---------------------------------------------------------
// 消息处理 (WM_DISPLAYCHANGE 支持)
// ---------------------------------------------------------
bool IsMouseOnDesktop() {
    POINT pt; GetCursorPos(&pt);
    HWND hWin = WindowFromPoint(pt);
    if (!hWin) return false;
    if (hWin == g.hListView || hWin == g.hContainer || hWin == g.hWorkerW || hWin == g.hMaskWindow) return true;
    if (hWin == FindWindow(_T("Progman"), NULL)) return true;
    HWND hParent = GetParent(hWin);
    if (hParent == g.hContainer || hParent == g.hWorkerW) return true;
    return false;
}

LRESULT CALLBACK MsgWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_TRAYICON:
        if (lParam == WM_RBUTTONUP) ShowTrayMenu(hwnd);
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
            g.lastActiveTime = GetTickCount();
        }
        else if (cmdId >= ID_MASK_START && cmdId < ID_MASK_START + MASK_OPT_COUNT) {
            g.maskOptIndex = cmdId - ID_MASK_START;
            g.maxMaskAlpha = MASK_OPTIONS[g.maskOptIndex].alpha;
            SaveSettings();
            g.lastMaskAlpha = -1;
        }
    }
    break;

    // [修复] 监听分辨率变化 (WM_DISPLAYCHANGE)
    // 当用户修改分辨率或缩放比例时，重新获取桌面尺寸并调整蒙版
    case WM_DISPLAYCHANGE:
        Sleep(500); // 等待系统应用完变更
        LocateDesktop();
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
    // [修复] 声明 DPI 感知，防止高分屏下界面模糊和坐标错误
    SetProcessDPIAware();

    HANDLE hMutex = CreateMutex(NULL, TRUE, _T("Local\\DH_HiDPI_Mask_Instance"));
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    g.appRunning = true;
    LoadSettings();
    g.cfg = &PRESETS[g.cfgIndex];
    g.lastActiveTime = GetTickCount();

    CreateMessageWindow(hInstance);
    InitTrayIcon(g.hMsgWindow);

    // 初始化时，screenW/H 会在 LocateDesktop 中被正确填充 (物理像素)
    LocateDesktop();
    CreateMaskWindow(hInstance);
    AttachMaskToDesktop(); // 确保蒙版创建后立即应用大小

    TimerInit();
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);

    if (!g.hContainer) {
        MessageBox(NULL, _T("桌面未找到"), _T("Error"), MB_ICONERROR);
        Shell_NotifyIcon(NIM_DELETE, &nid);
        return 1;
    }

    g.isHidden = false;
    g.targetY = 0.0f;
    g.targetAlpha = 255.0f;

    MSG msg;
    while (g.appRunning) {
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) g.appRunning = false;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (!g.appRunning) break;

        if (!IsWindow(g.hContainer)) {
            LocateDesktop();
            if (!g.hContainer) { Sleep(1000); continue; }
        }

        POINT currMouse;
        GetCursorPos(&currMouse);
        DWORD currTime = GetTickCount();
        bool isMoving = (abs(currMouse.x - g.lastMousePos.x) > MOUSE_MOVE_THRESHOLD ||
            abs(currMouse.y - g.lastMousePos.y) > MOUSE_MOVE_THRESHOLD);

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
                g.velocityY = 0.0f;
                g.velocityAlpha = 0.0f;
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
            Sleep(g.cfg->idleCheckMs);
            TimerGetDelta();
        }
    }

    PerformExitSequence();
    CloseHandle(hMutex);
    return (int)msg.wParam;
}