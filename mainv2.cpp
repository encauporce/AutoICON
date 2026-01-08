/*
    Auto-Hiding Desktop Icons (Win32 API + Tray + Non-linear Fade)

    [编译指南]
    1. 项目类型: Windows Desktop Application (C++)
    2. 依赖库: dwmapi.lib, shell32.lib (VS通常自动链接)
*/

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <tchar.h>
#include <math.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")

// ==========================================
// === 开发者配置区域 (CONSTANTS) ===
// ==========================================

// --- 基础设置 ---
const DWORD HIDE_DELAY_MS = 5000;          // 鼠标静止多久后隐藏 (毫秒)
const DWORD IDLE_CHECK_INTERVAL_MS = 100;  // 节能模式下的检测周期
const int   MOUSE_MOVE_THRESHOLD = 2;      // 鼠标移动检测灵敏度

// --- 位置动画设置 (滑入/滑出) ---
// 滑入(显示)的阻尼系数：数值越大，回弹复位越快 (建议 5.0 - 15.0)
const float POS_SPEED_IN_FACTOR = 0.0f;
// 滑出(隐藏)的加速度：数值越大，掉落越快 (建议 2000.0 - 5000.0)
const float POS_ACCEL_OUT_PPS2 = 0.0f;

// --- [新增] 透明度动画设置 (淡入/淡出) ---
// 淡入(显示)的阻尼系数：数值越大，浮现越快 (建议 3.0 - 10.0)
const float ALPHA_SPEED_IN_FACTOR = 16.0f;
// 淡出(隐藏)的加速度：数值越大，消失越快 (建议 100.0 - 1000.0)
// 注意：透明度只有 0-255，加速度不要设太大，否则瞬间就没了
const float ALPHA_ACCEL_OUT_PPS2 = 32.0f;

// ==========================================
// === 托盘图标定义 ===
// ==========================================
#define WM_TRAYICON (WM_USER + 1)
#define ID_TRAY_EXIT 1001

// 全局状态
struct GlobalState {
    HWND hContainer;        // SHELLDLL_DefView
    HWND hListView;         // SysListView32
    HWND hWorkerW;          // WorkerW
    HWND hMsgWindow;        // 消息接收窗

    int screenW;
    int screenH;

    // 位置物理属性
    float currentY;         // 0.0 = 顶部, screenH = 底部
    float targetY;
    float velocityY;

    // [新增] 透明度物理属性
    float currentAlpha;     // 0.0 = 透明, 255.0 = 不透明
    float targetAlpha;
    float velocityAlpha;    // 透明度变化速度

    POINT lastMousePos;
    DWORD lastActiveTime;

    bool isHidden;          // 逻辑状态
    bool appRunning;
} g;

NOTIFYICONDATA nid = { 0 };
LARGE_INTEGER qpcFreq;
LARGE_INTEGER qpcLastTime;

// ---------------------------------------------------------
// 托盘图标
// ---------------------------------------------------------
void InitTrayIcon(HWND hwnd) {
    nid.cbSize = sizeof(NOTIFYICONDATA);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    _tcscpy_s(nid.szTip, _T("桌面图标自动隐藏工具"));
    Shell_NotifyIcon(NIM_ADD, &nid);
}

void RemoveTrayIcon() {
    Shell_NotifyIcon(NIM_DELETE, &nid);
}

// ---------------------------------------------------------
// 核心逻辑函数
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

// 分层窗口控制 (透明度支持)
void EnableLayeredStyle(HWND hwnd, bool enable) {
    if (!hwnd || !IsWindow(hwnd)) return;
    LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    if (enable) {
        if (!(exStyle & WS_EX_LAYERED)) {
            SetWindowLongPtr(hwnd, GWL_EXSTYLE, exStyle | WS_EX_LAYERED);
        }
    }
    else {
        if (exStyle & WS_EX_LAYERED) {
            SetWindowLongPtr(hwnd, GWL_EXSTYLE, exStyle & ~WS_EX_LAYERED);
        }
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
        RECT rect;
        GetWindowRect(g.hContainer, &rect);
        g.screenW = rect.right - rect.left;
        g.screenH = rect.bottom - rect.top;
        if (g.screenH == 0) g.screenH = GetSystemMetrics(SM_CYSCREEN);

        // 初始化位置
        SetWindowPos(g.hContainer, NULL, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        g.currentY = 0.0f;
        g.velocityY = 0.0f;

        // 初始化透明度
        EnableLayeredStyle(g.hContainer, true);
        SetLayeredWindowAttributes(g.hContainer, 0, 255, LWA_ALPHA);
        g.currentAlpha = 255.0f;
        g.velocityAlpha = 0.0f;
    }
}

void RestoreDesktop() {
    if (g.hContainer && IsWindow(g.hContainer)) {
        SetLayeredWindowAttributes(g.hContainer, 0, 255, LWA_ALPHA);
        EnableLayeredStyle(g.hContainer, false); // 移除Layered属性，防止Bug
        SetWindowPos(g.hContainer, NULL, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(g.hContainer, NULL, TRUE);
    }
}

// ---------------------------------------------------------
// 物理引擎 (核心修改部分)
// ---------------------------------------------------------
void TimerInit() {
    QueryPerformanceFrequency(&qpcFreq);
    QueryPerformanceCounter(&qpcLastTime);
}

float TimerGetDelta() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    float dt = (float)(now.QuadPart - qpcLastTime.QuadPart) / (float)qpcFreq.QuadPart;
    qpcLastTime = now;
    if (dt > 0.1f) dt = 0.05f;
    return dt;
}

void UpdatePhysics(float dt) {
    bool needUpdatePos = false;
    bool needUpdateAlpha = false;

    // ---------------------------
    // 1. 位置物理模拟 (Y轴)
    // ---------------------------
    float diffY = g.targetY - g.currentY;

    // 如果还没到位
    if (fabs(diffY) > 0.5f || fabs(g.velocityY) > 1.0f) {
        needUpdatePos = true;

        // 滑出 (Hide): 加速向下
        if (g.targetY > g.currentY) {
            g.velocityY += POS_ACCEL_OUT_PPS2 * dt;
            g.currentY += g.velocityY * dt;
            if (g.currentY > g.targetY) {
                g.currentY = g.targetY;
                g.velocityY = 0.0f;
            }
        }
        // 滑入 (Show): 阻尼向上
        else {
            float desiredSpeed = diffY * POS_SPEED_IN_FACTOR;
            if (desiredSpeed > -50.0f) desiredSpeed = -50.0f; // 最小初速度
            g.currentY += desiredSpeed * dt;
            if (g.currentY < g.targetY) {
                g.currentY = g.targetY;
            }
        }
    }
    else {
        g.currentY = g.targetY; // 强制吸附
    }

    // ---------------------------
    // 2. 透明度物理模拟 (Alpha)
    // ---------------------------
    float diffAlpha = g.targetAlpha - g.currentAlpha;

    // 如果还没到位 (误差允许范围 0.5)
    if (fabs(diffAlpha) > 0.5f || fabs(g.velocityAlpha) > 1.0f) {
        needUpdateAlpha = true;

        // 淡出 (Hide): 目标是0，当前是255 -> 向下加速
        if (g.targetAlpha < g.currentAlpha) {
            // 注意：Alpha是减少的，所以速度也是朝负方向增加
            // 我们让 velocityAlpha 变为正数表示“变化量”，然后减去
            g.velocityAlpha += ALPHA_ACCEL_OUT_PPS2 * dt;
            g.currentAlpha -= g.velocityAlpha * dt;

            if (g.currentAlpha < g.targetAlpha) {
                g.currentAlpha = g.targetAlpha;
                g.velocityAlpha = 0.0f;
            }
        }
        // 淡入 (Show): 目标是255，当前是0 -> 阻尼逼近
        else {
            // P控制器逻辑：距离越远变化越快
            float speed = diffAlpha * ALPHA_SPEED_IN_FACTOR;
            // 限制最小变化速度
            if (speed < 10.0f) speed = 10.0f;

            g.currentAlpha += speed * dt;

            if (g.currentAlpha > g.targetAlpha) {
                g.currentAlpha = g.targetAlpha;
            }
        }
    }
    else {
        g.currentAlpha = g.targetAlpha;
    }

    // ---------------------------
    // 3. 应用变更
    // ---------------------------
    if (g.hContainer) {
        if (needUpdatePos) {
            SetWindowPos(g.hContainer, NULL, 0, (int)g.currentY, 0, 0,
                SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        if (needUpdateAlpha) {
            // 安全限制 0-255
            int finalAlpha = (int)g.currentAlpha;
            if (finalAlpha < 0) finalAlpha = 0;
            if (finalAlpha > 255) finalAlpha = 255;

            SetLayeredWindowAttributes(g.hContainer, 0, (BYTE)finalAlpha, LWA_ALPHA);
        }
    }
}

bool IsPhysicsIdle() {
    // 只有当位置和透明度都完全停止时，才认为物理引擎空闲
    bool posIdle = (fabs(g.targetY - g.currentY) < 0.5f && fabs(g.velocityY) < 1.0f);
    bool alphaIdle = (fabs(g.targetAlpha - g.currentAlpha) < 0.5f && fabs(g.velocityAlpha) < 1.0f);
    return posIdle && alphaIdle;
}

// ---------------------------------------------------------
// 辅助与窗口过程
// ---------------------------------------------------------
bool IsMouseOnDesktop() {
    POINT pt;
    GetCursorPos(&pt);
    HWND hWin = WindowFromPoint(pt);
    if (!hWin) return false;
    if (hWin == g.hListView || hWin == g.hContainer || hWin == g.hWorkerW) return true;
    if (hWin == FindWindow(_T("Progman"), NULL)) return true;
    HWND hParent = GetParent(hWin);
    if (hParent == g.hContainer || hParent == g.hWorkerW) return true;
    return false;
}

LRESULT CALLBACK MsgWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_TRAYICON:
        if (lParam == WM_RBUTTONUP) {
            POINT p; GetCursorPos(&p);
            SetForegroundWindow(hwnd);
            HMENU hMenu = CreatePopupMenu();
            AppendMenu(hMenu, MF_STRING, ID_TRAY_EXIT, _T("退出程序"));
            TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, p.x, p.y, 0, hwnd, NULL);
            DestroyMenu(hMenu);
        }
        break;
    case WM_COMMAND:
        if (LOWORD(wParam) == ID_TRAY_EXIT) g.appRunning = false;
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
    wc.lpszClassName = _T("DesktopHiderMsgWin");
    RegisterClassEx(&wc);
    g.hMsgWindow = CreateWindowEx(0, wc.lpszClassName, _T(""), 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, hInstance, NULL);
}

// ---------------------------------------------------------
// 主入口
// ---------------------------------------------------------
int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrev, _In_ LPWSTR lpCmdLine, _In_ int nShow) {
    HANDLE hMutex = CreateMutex(NULL, TRUE, _T("Local\\MyDesktopHider_Instance"));
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    g.appRunning = true;
    CreateMessageWindow(hInstance);
    InitTrayIcon(g.hMsgWindow);
    LocateDesktop();
    TimerInit();

    if (!g.hContainer) {
        MessageBox(NULL, _T("无法定位桌面图标窗口。"), _T("错误"), MB_ICONERROR);
        RemoveTrayIcon();
        return 1;
    }

    g.lastActiveTime = GetTickCount();
    GetCursorPos(&g.lastMousePos);
    g.isHidden = false;
    g.targetY = 0.0f;
    g.targetAlpha = 255.0f; // 初始目标不透明

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
        bool isOnDesktop = IsMouseOnDesktop();

        if (isMoving && isOnDesktop) {
            g.lastActiveTime = currTime;
            // [动作：显示]
            g.targetY = 0.0f;
            g.targetAlpha = 255.0f;
            g.isHidden = false;
        }
        else if (isMoving && !isOnDesktop) {
            // Mouse moving elsewhere
        }
        else {
            if (!g.isHidden && (currTime - g.lastActiveTime > HIDE_DELAY_MS)) {
                // [动作：隐藏]
                g.targetY = (float)g.screenH;
                g.targetAlpha = 0.0f;
                g.isHidden = true;

                // 重置加速度初始状态，保证每次隐藏都是从0加速
                g.velocityY = 0.0f;
                g.velocityAlpha = 0.0f;
            }
        }

        g.lastMousePos = currMouse;

        // 核心渲染循环
        if (!IsPhysicsIdle()) {
            float dt = TimerGetDelta();
            UpdatePhysics(dt);
            DwmFlush();
        }
        else {
            // 确保最后一次状态被应用
            if (g.currentY != g.targetY || g.currentAlpha != g.targetAlpha) {
                UpdatePhysics(0.0f); // 强制同步
            }
            Sleep(IDLE_CHECK_INTERVAL_MS);
            TimerGetDelta(); // 重置计时器防止dt跳变
        }
    }

    RestoreDesktop();
    RemoveTrayIcon();
    CloseHandle(hMutex);

    return (int)msg.wParam;
}