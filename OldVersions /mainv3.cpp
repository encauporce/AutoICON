/*
    Auto-Hiding Desktop Icons (Explorer Restart Edition)

    [功能更新]
    - 退出流程重构：复原状态 -> 杀掉Explorer -> 重启Explorer -> 退出本程序
    - 彻底解决退出后图标消失或无法点击的Bug
    - 保持了之前的配置文件和注册表记忆功能

    [编译指南]
    - Visual Studio: Windows Desktop Application (C++)
    - 依赖库: dwmapi.lib, shell32.lib, advapi32.lib
*/

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <tchar.h>
#include <math.h>
#include <tlhelp32.h> // 用于进程快照

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
// === 全局变量 ===
// ==========================================

#define WM_TRAYICON (WM_USER + 1)
#define ID_TRAY_EXIT 9001
#define ID_PROFILE_START 9100

const TCHAR* REG_SUBKEY = _T("Software\\MyDesktopHider");
const TCHAR* REG_VAL_NAME = _T("LastProfileIndex");

struct GlobalState {
    HWND hContainer;
    HWND hListView;
    HWND hWorkerW;
    HWND hMsgWindow;

    int screenW;
    int screenH;

    const ConfigProfile* cfg;
    int cfgIndex;

    float currentY, targetY, velocityY;
    float currentAlpha, targetAlpha, velocityAlpha;

    int lastRenderY;
    int lastRenderAlpha;

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
// 注册表操作
// ---------------------------------------------------------
void SaveConfigIndex(int index) {
    HKEY hKey;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, REG_SUBKEY, 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        RegSetValueEx(hKey, REG_VAL_NAME, 0, REG_DWORD, (BYTE*)&index, sizeof(index));
        RegCloseKey(hKey);
    }
}

int LoadConfigIndex() {
    HKEY hKey;
    DWORD index = 0;
    DWORD size = sizeof(index);
    if (RegOpenKeyEx(HKEY_CURRENT_USER, REG_SUBKEY, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        if (RegQueryValueEx(hKey, REG_VAL_NAME, NULL, NULL, (BYTE*)&index, &size) != ERROR_SUCCESS) index = 0;
        RegCloseKey(hKey);
    }
    if (index >= (DWORD)PRESET_COUNT) index = 0;
    return (int)index;
}

// ---------------------------------------------------------
// 窗口管理
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
        RECT rect;
        GetWindowRect(g.hContainer, &rect);
        g.screenW = rect.right - rect.left;
        g.screenH = rect.bottom - rect.top;
        if (g.screenH == 0) g.screenH = GetSystemMetrics(SM_CYSCREEN);

        g.currentY = 0.0f; g.velocityY = 0.0f;
        g.currentAlpha = 255.0f; g.velocityAlpha = 0.0f;
        g.lastRenderY = 0; g.lastRenderAlpha = 255;

        SetWindowPos(g.hContainer, NULL, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        EnableLayeredStyle(g.hContainer, true);
        SetLayeredWindowAttributes(g.hContainer, 0, 255, LWA_ALPHA);
    }
}

// ---------------------------------------------------------
// [核心修复] 退出流程：重置 -> 杀Explorer -> 启Explorer
// ---------------------------------------------------------
void PerformExitSequence() {
    // 1. 尝试手动重置图标状态 (虽然重启Explorer会覆盖这个，但作为保险)
    if (g.hContainer && IsWindow(g.hContainer)) {
        SetLayeredWindowAttributes(g.hContainer, 0, 255, LWA_ALPHA);
        EnableLayeredStyle(g.hContainer, false);
        SetWindowPos(g.hContainer, NULL, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    // 2. 清理托盘图标 (防止残留)
    Shell_NotifyIcon(NIM_DELETE, &nid);

    // 3. 查找并结束 Explorer.exe 进程
    // 通常 Progman 或 Shell_TrayWnd 的所有者就是我们要找的 explorer
    DWORD pid = 0;
    HWND hShellWnd = FindWindow(_T("Progman"), NULL);
    if (!hShellWnd) hShellWnd = FindWindow(_T("Shell_TrayWnd"), NULL);

    if (hShellWnd) {
        GetWindowThreadProcessId(hShellWnd, &pid);
        if (pid != 0) {
            HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
            if (hProcess) {
                // 强制终止
                TerminateProcess(hProcess, 1);
                CloseHandle(hProcess);
                // 等待进程完全释放
                Sleep(500);
            }
        }
    }

    // 4. 重新启动 Explorer.exe
    // 这将创建一个全新的桌面环境，确保图标绝对可见且位置正确
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
    _tcscpy_s(nid.szTip, _T("桌面图标自动隐藏"));
    Shell_NotifyIcon(NIM_ADD, &nid);
}

void ShowTrayMenu(HWND hwnd) {
    POINT p; GetCursorPos(&p);
    SetForegroundWindow(hwnd);
    HMENU hMenu = CreatePopupMenu();
    HMENU hSubMenu = CreatePopupMenu();

    for (int i = 0; i < PRESET_COUNT; i++) {
        UINT flags = MF_STRING;
        if (i == g.cfgIndex) flags |= MF_CHECKED;
        AppendMenu(hSubMenu, flags, ID_PROFILE_START + i, PRESETS[i].name);
    }

    AppendMenu(hMenu, MF_POPUP, (UINT_PTR)hSubMenu, _T("切换模式"));
    AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(hMenu, MF_STRING, ID_TRAY_EXIT, _T("完全退出并重置"));

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

    // Render
    int renderY = (int)g.currentY;
    int renderAlpha = (int)g.currentAlpha;
    if (renderAlpha < 0) renderAlpha = 0; if (renderAlpha > 255) renderAlpha = 255;

    if (renderY != g.lastRenderY) {
        SetWindowPos(g.hContainer, NULL, 0, renderY, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        g.lastRenderY = renderY;
    }
    if (renderAlpha != g.lastRenderAlpha) {
        SetLayeredWindowAttributes(g.hContainer, 0, (BYTE)renderAlpha, LWA_ALPHA);
        g.lastRenderAlpha = renderAlpha;
    }
}

bool IsPhysicsIdle() {
    return (fabs(g.targetY - g.currentY) < 0.5f && fabs(g.velocityY) < 1.0f) &&
        (fabs(g.targetAlpha - g.currentAlpha) < 0.5f && fabs(g.velocityAlpha) < 1.0f);
}

// ---------------------------------------------------------
// 消息处理
// ---------------------------------------------------------
bool IsMouseOnDesktop() {
    POINT pt; GetCursorPos(&pt);
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
        if (lParam == WM_RBUTTONUP) ShowTrayMenu(hwnd);
        break;

    case WM_COMMAND:
    {
        int cmdId = LOWORD(wParam);
        if (cmdId == ID_TRAY_EXIT) {
            g.appRunning = false;
        }
        else if (cmdId >= ID_PROFILE_START && cmdId < ID_PROFILE_START + PRESET_COUNT) {
            int index = cmdId - ID_PROFILE_START;
            g.cfgIndex = index;
            g.cfg = &PRESETS[index];
            SaveConfigIndex(index);
            g.lastActiveTime = GetTickCount();
        }
    }
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
    HANDLE hMutex = CreateMutex(NULL, TRUE, _T("Local\\DH_RebootFix_Instance"));
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    g.appRunning = true;
    g.cfgIndex = LoadConfigIndex();
    g.cfg = &PRESETS[g.cfgIndex];
    g.lastActiveTime = GetTickCount();

    CreateMessageWindow(hInstance);
    InitTrayIcon(g.hMsgWindow);
    LocateDesktop();
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

    // [这里] 执行退出序列
    PerformExitSequence();

    CloseHandle(hMutex);
    return (int)msg.wParam;
}
