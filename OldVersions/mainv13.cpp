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
#include <wininet.h>
#include <strsafe.h>
#include "resource.h"

// 链接必要的系统库
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "ole32.lib") 

#ifndef _countof
#define _countof(_Array) (sizeof(_Array) / sizeof(_Array[0]))
#endif

// ==========================================
// === 版本、配置与常量定义 ===
// ==========================================
#define APP_NAME        _T("AutoICON")
#define APP_VERSION_STR _T("v13") // 用于显示的字符串版本
#define APP_VERSION_NUM 13        // 用于逻辑比较的数字版本
#define EXE_NAME        _T("AutoICON.exe")
#define USER_AGENT      _T("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36")

// 注册表路径
#define REG_UNINSTALL_KEY _T("Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\AutoICON")
#define REG_RUN_KEY       _T("Software\\Microsoft\\Windows\\CurrentVersion\\Run")
#define REG_SUBKEY        _T("Software\\AutoICON")
#define REG_VAL_PROFILE   _T("LastProfileIndex")
#define REG_VAL_MASK      _T("MaskOpacityIndex")

// 消息与菜单ID
#define WM_TRAYICON          (WM_USER + 1)
#define WM_UPDATE_UI_REFRESH (WM_USER + 200)
#define ID_TRAY_EXIT         9001
#define ID_TRAY_AUTOSTART    9002
#define ID_TRAY_UPDATE       9300
#define ID_PROFILE_START     9100
#define ID_MASK_START        9200

// 启动状态机常量 (原代码缺失)
#define STARTUP_PHASE_1_HIDING  0
#define STARTUP_PHASE_2_WAITING 1
#define STARTUP_PHASE_3_SHOWING 2
#define STARTUP_NORMAL          3

// 动画与时间常量 (原代码缺失)
#define STARTUP_TRANSITION_DELAY 500   // 切换配置时的等待毫秒数
#define STARTUP_SPEED_FACTOR     0.7f  // 启动/切换时的动画加速倍率

// 物理引擎参数
struct SpringParams {
    float tension;
    float friction;
    bool  enabled;
};

// 预设物理参数
const SpringParams SP_FAST = { 860.0f, 46.0f, true };
const SpringParams SP_NORMAL = { 400.0f, 32.0f, true };
const SpringParams SP_SLOW = { 12.0f,  5.0f, true };
const SpringParams SP_VERYSLOW = { 6.0f,  3.0f,  true };
const SpringParams SP_OFF = { 0.0f,   0.0f,  false };

// 配置档案结构
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

// 蒙版透明度选项
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

// 更新检测状态
enum UpdateStatus {
    US_IDLE,            // 未启动
    US_CHECKING,        // 正在检测
    US_LATEST,          // 已是最新
    US_UPDATE_FOUND,    // 发现更新
    US_ERROR            // 连接失败
};

// 镜像源列表
const TCHAR* MIRRORS[] = {
    _T("https://gitee.com/encauporce/AutoICON/releases/latest"), // Gitee 优先
    _T("https://github.com/encauporce/AutoICON/releases/latest") // GitHub 兜底
};
#define UPDATE_TIMEOUT_MS 5000 // 网络超时设置

// 全局上下文结构
struct GlobalState {
    // 窗口句柄
    HWND hContainer;
    HWND hDesktopParent;
    HWND hMsgWindow;
    HWND hMaskWindow;

    // 屏幕尺寸
    int screenW;
    int screenH;

    // 配置状态
    const ConfigProfile* cfg;
    int cfgIndex;
    int maskOptIndex;
    int maxMaskAlpha;

    // 待处理配置
    int pendingCfgIndex;
    bool hasPendingCfg;
    int pendingMaskOptIndex;
    bool hasPendingMask;

    // 物理状态
    float currentY, velocityY;
    float currentAlpha, velocityAlpha;
    float targetY;
    float targetAlpha;

    // 渲染缓存
    int lastRenderY;
    int lastRenderAlpha;
    int lastMaskAlpha;

    // 状态机
    int startupState; // 0:Hiding, 1:Waiting, 2:Showing, 3:Normal
    ULONGLONG waitStartTime;
    ULONGLONG startupPhaseStartTime;
    int zOrderGuardCounter;

    // 交互状态
    POINT lastMousePos;
    ULONGLONG lastActiveTime;
    bool isHidden;
    bool appRunning;
    bool isPaused;

    // 路径
    TCHAR szInstallDir[MAX_PATH];
    TCHAR szInstallExePath[MAX_PATH];
} g = { 0 };

struct UpdateContext {
    volatile UpdateStatus status;
    TCHAR fastestUrl[512];
    volatile LONG activeThreads;
    HMENU hActiveMenu;
    bool hasCheckStarted;
} g_UpdateCtx = { US_IDLE, {0}, 0, NULL, false };

NOTIFYICONDATA nid = { 0 };
LARGE_INTEGER qpcFreq;
LARGE_INTEGER qpcLastTime;
UINT g_uMsgTaskbarCreated = 0;
const int MOUSE_MOVE_THRESHOLD = 2;

// ==========================================
// === 函数前置声明 (Declaration) ===
// ==========================================

void TimerInit();
float TimerGetDelta(bool resetOnly = false);
void InitGlobalPaths();

bool IsRunAsAdministrator();
void RequestAdminPrivileges();
bool IsProcessRunning(const TCHAR* processName, DWORD* pPid = NULL);
bool KillProcess(DWORD pid);
bool KillRunningProcesses();
bool WaitForProcessExit(const TCHAR* processName, DWORD timeoutMs = 5000);

bool IsInstalled();
bool GetInstalledVersion(TCHAR* version, size_t size);
bool CopyToInstallDirWithRetry();
void RegisterUninstallInfo();
void UnregisterUninstallInfo();
bool IsAutoStartEnabled();
void SetAutoStart(bool enabled);
void RunInstalledExe();
void HandleInstallation();
void HandleUninstall();
void SaveSettings();
void LoadSettings();
void PerformExitSequence();

BOOL CALLBACK FindSysListViewProc(HWND hwnd, LPARAM lParam);
void EnableLayeredStyle(HWND hwnd, bool enable);
void EnforceZOrder();
void CreateMaskWindow(HINSTANCE hInstance);
void AttachMaskToDesktop();
void LocateDesktop(HINSTANCE hInstance);
void InitTrayIcon(HWND hwnd);
void CreateMessageWindow(HINSTANCE hInstance);

void SolveSpring(float& current, float& velocity, float target, const SpringParams& p, float dt);
void UpdatePhysics(float dt);
bool IsPhysicsIdle();
void ForceShowImmediate();
void TriggerRestartAnimation();
bool IsMouseOnDesktop();

int ParseVersionFromUrl(const TCHAR* url);
bool CheckSingleUrl(const TCHAR* url, HINTERNET hSession, int& outVersion, TCHAR* outFinalUrl, size_t bufferSize);
DWORD WINAPI CheckUpdateThreadProc(LPVOID lpParam);
void StartUpdateChecks();
void RefreshMenuText();
void ShowTrayMenu(HWND hwnd);

LRESULT CALLBACK MsgWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ==========================================
// === 主程序入口 ===
// ==========================================
int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow) {
    // 尝试设置 DPI 感知，如果系统不支持则忽略
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);

    InitGlobalPaths();

    // 命令行卸载参数处理
    if (lpCmdLine && _tcsstr(lpCmdLine, _T("/uninstall"))) {
        HandleUninstall();
        return 0;
    }

    // 安装/更新检查
    HandleInstallation();

    // 运行位置检查
    TCHAR currentPath[MAX_PATH];
    GetModuleFileName(NULL, currentPath, MAX_PATH);
    // 简单的路径对比可能受大小写影响，这里使用 _tcsnicmp 忽略大小写
    if (_tcsnicmp(currentPath, g.szInstallDir, _tcslen(g.szInstallDir)) != 0) {
        MessageBox(NULL, _T("程序必须在安装目录运行！"), APP_NAME, MB_OK | MB_ICONERROR);
        return 1;
    }

    // 单实例互斥锁
    HANDLE hMutex = CreateMutex(NULL, TRUE, _T("Local\\AutoICON_Instance"));
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    g_uMsgTaskbarCreated = RegisterWindowMessage(_T("TaskbarCreated"));
    g.appRunning = true;
    g.isPaused = false;

    // 加载配置
    LoadSettings();
    g.lastActiveTime = GetTickCount64();

    // 初始化窗口和系统组件
    CreateMessageWindow(hInstance);
    WTSRegisterSessionNotification(g.hMsgWindow, NOTIFY_FOR_THIS_SESSION);
    InitTrayIcon(g.hMsgWindow);
    LocateDesktop(hInstance);
    TimerInit();

    if (!g.hContainer) {
        MessageBox(NULL, _T("无法定位桌面窗口。"), APP_NAME, MB_ICONERROR);
        g.appRunning = false;
    }

    // 主消息循环
    MSG msg;
    while (g.appRunning) {
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) g.appRunning = false;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (!g.appRunning) break;
        if (g.isPaused) { Sleep(1000); continue; }

        // 桌面窗口防丢失机制
        if (!IsWindow(g.hContainer)) {
            LocateDesktop(hInstance);
            if (!g.hContainer) { Sleep(500); continue; }
        }

        // 状态更新逻辑
        POINT currMouse;
        GetCursorPos(&currMouse);
        ULONGLONG currTime = GetTickCount64();
        bool isMoving = (abs(currMouse.x - g.lastMousePos.x) > MOUSE_MOVE_THRESHOLD ||
            abs(currMouse.y - g.lastMousePos.y) > MOUSE_MOVE_THRESHOLD);

        // 启动动画状态机
        if (g.startupState == STARTUP_PHASE_1_HIDING) { // 0: 启动时的隐藏阶段
            bool hiddenEnough = true;
            if (g.currentY < g.screenH * 0.9f && g.currentAlpha > 10.0f) hiddenEnough = false;
            if (hiddenEnough || (currTime - g.startupPhaseStartTime > 2000)) {
                g.startupState = STARTUP_PHASE_2_WAITING;
                g.waitStartTime = currTime;
            }
        }
        else if (g.startupState == STARTUP_PHASE_2_WAITING) { // 1: 等待配置切换
            if (currTime - g.waitStartTime > STARTUP_TRANSITION_DELAY) {
                if (g.hasPendingCfg) {
                    g.cfgIndex = g.pendingCfgIndex;
                    g.hasPendingCfg = false;
                }
                if (g.hasPendingMask) {
                    g.maskOptIndex = g.pendingMaskOptIndex;
                    g.hasPendingMask = false;
                }

                // 应用新配置
                g.cfg = &PRESETS[g.cfgIndex];
                g.lastActiveTime = GetTickCount64();
                g.maxMaskAlpha = MASK_OPTIONS[g.maskOptIndex].alpha;
                SaveSettings();

                // 重建遮罩
                if (g.maxMaskAlpha <= 0) {
                    if (g.hMaskWindow && IsWindow(g.hMaskWindow)) DestroyWindow(g.hMaskWindow);
                    g.hMaskWindow = NULL;
                }
                else {
                    if (!g.hMaskWindow || !IsWindow(g.hMaskWindow)) CreateMaskWindow(hInstance);
                    AttachMaskToDesktop();
                }

                // 重置位置准备进入
                g.targetY = 0.0f;
                g.targetAlpha = 255.0f;
                if (g.cfg->opacityIn.enabled) g.currentAlpha = 0.0f; else g.currentAlpha = 255.0f;
                if (g.cfg->motionIn.enabled) g.currentY = (float)g.screenH; else g.currentY = 0.0f;
                g.velocityY = 0.0f;
                g.velocityAlpha = 0.0f;

                g.startupState = STARTUP_PHASE_3_SHOWING;
                g.isHidden = false;
                g.lastActiveTime = currTime;
                EnforceZOrder();
            }
        }
        else if (g.startupState == STARTUP_PHASE_3_SHOWING) { // 2: 显示阶段
            if (IsPhysicsIdle() || (isMoving && IsMouseOnDesktop())) {
                g.startupState = STARTUP_NORMAL;
                g.lastActiveTime = currTime;
            }
        }
        else { // 3: 正常运行阶段
            if (isMoving) {
                if (IsMouseOnDesktop()) {
                    g.lastActiveTime = currTime;
                    g.targetY = 0.0f;
                    g.targetAlpha = 255.0f;
                    g.isHidden = false;
                }
            }
            else {
                // 检测超时，准备隐藏
                if (!g.isHidden && (currTime - g.lastActiveTime > g.cfg->hideDelayMs)) {

                    // 1. 设置位移目标
                    if (g.cfg->motionOut.enabled) {
                        g.targetY = (float)g.screenH;
                    }
                    else {
                        g.targetY = 0.0f;
                    }

                    // 2. 设置透明度目标 (这是本次修复的关键！)
                    // 如果配置启用了退出透明度动画（如渐变模式），则目标为0
                    // 如果禁用了（如抽屉模式），目标保持255，否则会被物理引擎瞬间置0
                    if (g.cfg->opacityOut.enabled) {
                        g.targetAlpha = 0.0f;
                    }
                    else {
                        g.targetAlpha = 255.0f;
                    }

                    g.isHidden = true;
                }
            }
        }

        g.lastMousePos = currMouse;

        // 物理更新步进
        if (!IsPhysicsIdle()) {
            float dt = TimerGetDelta();
            UpdatePhysics(dt);
            DwmFlush(); // 垂直同步等待
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

// ==========================================
// === 函数实现 (Implementation) ===
// ==========================================

// --- 版本解析与网络检测 ---

int ParseVersionFromUrl(const TCHAR* url) {
    if (!url) return 0;
    const TCHAR* lastSlash = _tcsrchr(url, _T('/'));
    if (!lastSlash) return 0;

    const TCHAR* tag = lastSlash + 1;
    // 简单的数字校验，避免解析错误链接
    if (*tag >= _T('0') && *tag <= _T('9')) {
        return _ttoi(tag);
    }
    return 0;
}

bool CheckSingleUrl(const TCHAR* url, HINTERNET hSession, int& outVersion, TCHAR* outFinalUrl, size_t bufferSize) {
    // 使用 Win11 默认安全配置 + 浏览器伪装
    HINTERNET hUrl = InternetOpenUrl(hSession, url, NULL, 0,
        INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_COOKIES | INTERNET_FLAG_NO_UI, 0);

    if (!hUrl) return false;

    DWORD dwInfoSize = (DWORD)bufferSize;
    if (InternetQueryOption(hUrl, INTERNET_OPTION_URL, outFinalUrl, &dwInfoSize)) {
        outVersion = ParseVersionFromUrl(outFinalUrl);
        InternetCloseHandle(hUrl);
        return outVersion > 0;
    }

    InternetCloseHandle(hUrl);
    return false;
}

DWORD WINAPI CheckUpdateThreadProc(LPVOID lpParam) {
    const TCHAR* checkUrl = (const TCHAR*)lpParam;

    // 使用 PRECONFIG 自动适应系统代理和 TLS 设置
    HINTERNET hSession = InternetOpen(USER_AGENT, INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);

    if (hSession) {
        // 设置超时防止卡顿
        DWORD timeout = UPDATE_TIMEOUT_MS;
        InternetSetOption(hSession, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
        InternetSetOption(hSession, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));

        int ver = 0;
        TCHAR finalUrl[512] = { 0 };

        if (CheckSingleUrl(checkUrl, hSession, ver, finalUrl, sizeof(finalUrl))) {
            // 只要有一个线程成功返回了比当前大的版本，就更新状态
            if (ver > APP_VERSION_NUM) {
                // 仅当状态未被设置为 Found 时才更新，避免重复刷新
                if (g_UpdateCtx.status != US_UPDATE_FOUND) {
                    StringCchCopy(g_UpdateCtx.fastestUrl, _countof(g_UpdateCtx.fastestUrl), finalUrl);
                    g_UpdateCtx.status = US_UPDATE_FOUND;
                    PostMessage(g.hMsgWindow, WM_UPDATE_UI_REFRESH, 0, 0);
                }
            }
            else {
                // 已经是最新版
                if (g_UpdateCtx.status == US_CHECKING) {
                    g_UpdateCtx.status = US_LATEST;
                    PostMessage(g.hMsgWindow, WM_UPDATE_UI_REFRESH, 0, 0);
                }
            }
        }
        InternetCloseHandle(hSession);
    }

    // 原子操作递减线程计数
    if (InterlockedDecrement(&g_UpdateCtx.activeThreads) == 0) {
        // 所有线程结束且仍处于 Checking 状态，说明全部失败
        if (g_UpdateCtx.status == US_CHECKING) {
            g_UpdateCtx.status = US_ERROR;
            PostMessage(g.hMsgWindow, WM_UPDATE_UI_REFRESH, 0, 0);
        }
    }

    return 0;
}

void StartUpdateChecks() {
    g_UpdateCtx.status = US_CHECKING;
    g_UpdateCtx.fastestUrl[0] = 0;
    g_UpdateCtx.activeThreads = _countof(MIRRORS);
    g_UpdateCtx.hasCheckStarted = true;

    for (int i = 0; i < _countof(MIRRORS); i++) {
        CloseHandle(CreateThread(NULL, 0, CheckUpdateThreadProc, (LPVOID)MIRRORS[i], 0, NULL));
    }
}

// --- 菜单与 UI ---

void RefreshMenuText() {
    if (!g_UpdateCtx.hActiveMenu) return;

    TCHAR szText[128];
    UINT uFlags = MF_BYCOMMAND | MF_STRING;

    switch (g_UpdateCtx.status) {
    case US_UPDATE_FOUND:
        _stprintf_s(szText, _countof(szText), _T("已检测到更新版本，点击跳转至下载页面"));
        break;
    case US_LATEST:
        _stprintf_s(szText, _countof(szText), _T("已是最新版本"));
        uFlags |= MF_GRAYED;
        break;
    case US_ERROR:
        _stprintf_s(szText, _countof(szText), _T("无法连接Github..."));
        uFlags |= MF_GRAYED;
        break;
    default:
        return; // 检测中不刷新
    }

    // 更新菜单文字
    ModifyMenu(g_UpdateCtx.hActiveMenu, ID_TRAY_UPDATE, uFlags, ID_TRAY_UPDATE, szText);

    // 强制刷新系统菜单窗口 (#32768)
    HWND hMenuWnd = FindWindow(_T("#32768"), NULL);
    if (hMenuWnd && IsWindowVisible(hMenuWnd)) {
        InvalidateRect(hMenuWnd, NULL, TRUE);
        UpdateWindow(hMenuWnd);
    }
}

void ShowTrayMenu(HWND hwnd) {
    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(hwnd);

    // 每次打开都重新检测，除非已经发现了更新
    if (g_UpdateCtx.status != US_UPDATE_FOUND) {
        StartUpdateChecks();
    }

    HMENU hMenu = CreatePopupMenu();
    g_UpdateCtx.hActiveMenu = hMenu;

    // 构建动态更新菜单项
    TCHAR szUpdateText[128];
    UINT uUpdateFlags = MF_STRING;

    if (g_UpdateCtx.status == US_UPDATE_FOUND) {
        _tcscpy_s(szUpdateText, _countof(szUpdateText), _T("已检测到更新版本，点击跳转至下载页面"));
    }
    else {
        _tcscpy_s(szUpdateText, _countof(szUpdateText), _T("正在检查新版本..."));
        uUpdateFlags |= MF_GRAYED;
    }
    AppendMenu(hMenu, uUpdateFlags, ID_TRAY_UPDATE, szUpdateText);
    AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);

    // 动画模式子菜单
    HMENU hSubProfile = CreatePopupMenu();
    for (int i = 0; i < PRESET_COUNT; i++) {
        UINT flags = MF_STRING;
        int checkIndex = g.hasPendingCfg ? g.pendingCfgIndex : g.cfgIndex;
        if (i == checkIndex) flags |= MF_CHECKED;
        AppendMenu(hSubProfile, flags, ID_PROFILE_START + i, PRESETS[i].name);
    }
    AppendMenu(hMenu, MF_POPUP, (UINT_PTR)hSubProfile, _T("动画模式 (Animation Mode)"));

    // 蒙版子菜单
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

    // 开机自启
    UINT autoStartFlags = MF_STRING;
    if (IsAutoStartEnabled()) autoStartFlags |= MF_CHECKED;
    AppendMenu(hMenu, autoStartFlags, ID_TRAY_AUTOSTART, _T("开机自启 (Auto Start)"));

    AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(hMenu, MF_STRING, ID_TRAY_EXIT, _T("退出程序 (Exit)"));

    TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, p.x, p.y, 0, hwnd, NULL);

    g_UpdateCtx.hActiveMenu = NULL;
    DestroyMenu(hMenu);
}

// --- 消息处理 ---

LRESULT CALLBACK MsgWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == g_uMsgTaskbarCreated && g_uMsgTaskbarCreated != 0) {
        g.hContainer = NULL; // 任务栏重启需重新定位桌面
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

        // 响应后台线程的刷新请求
    case WM_UPDATE_UI_REFRESH:
        RefreshMenuText();
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
            SetAutoStart(!IsAutoStartEnabled());
        }
        else if (cmdId == ID_TRAY_UPDATE) {
            // 点击更新跳转
            if (g_UpdateCtx.status == US_UPDATE_FOUND && g_UpdateCtx.fastestUrl[0] != 0) {
                ShellExecute(NULL, _T("open"), g_UpdateCtx.fastestUrl, NULL, NULL, SW_SHOW);
            }
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

// --- 基础工具实现 ---

void TimerInit() {
    QueryPerformanceFrequency(&qpcFreq);
    QueryPerformanceCounter(&qpcLastTime);
}

float TimerGetDelta(bool resetOnly) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (resetOnly) {
        qpcLastTime = now;
        return 0.0f;
    }
    // 防止除以0（虽然在Win32下几乎不可能发生）
    if (qpcFreq.QuadPart == 0) return 0.016f;

    float dt = (float)((double)(now.QuadPart - qpcLastTime.QuadPart) / (double)qpcFreq.QuadPart);
    qpcLastTime = now;

    // 帧时间钳制，防止Debug断点后物理爆炸
    if (dt > 0.05f) dt = 0.05f;
    if (dt < 0.0001f) dt = 0.0001f;
    return dt;
}

void InitGlobalPaths() {
    TCHAR szProgramFiles[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPath(NULL, CSIDL_PROGRAM_FILES, NULL, 0, szProgramFiles))) {
        PathCombine(g.szInstallDir, szProgramFiles, _T("AutoICON"));
    }
    else {
        _tcscpy_s(g.szInstallDir, _countof(g.szInstallDir), _T("C:\\Program Files\\AutoICON"));
    }
    PathCombine(g.szInstallExePath, g.szInstallDir, EXE_NAME);
}

// --- 物理引擎实现 ---

void SolveSpring(float& current, float& velocity, float target, const SpringParams& p, float dt) {
    float displacement = current - target;
    float acceleration = -p.tension * displacement - p.friction * velocity;
    velocity += acceleration * dt;
    current += velocity * dt;

    // 浮点数安全检查
    if (_isnan(current) || !_finite(current)) current = target;
    if (_isnan(velocity) || !_finite(velocity)) velocity = 0.0f;
}

void UpdatePhysics(float dt) {
    if (!g.hContainer) return;

    // 状态切换期间加速物理模拟
    if (g.startupState != STARTUP_NORMAL) dt *= STARTUP_SPEED_FACTOR;

    float physicsTargetY = g.targetY;
    float physicsTargetAlpha = g.targetAlpha;

    const SpringParams* pMotion = g.isHidden ? &g.cfg->motionOut : &g.cfg->motionIn;
    const SpringParams* pOpacity = g.isHidden ? &g.cfg->opacityOut : &g.cfg->opacityIn;

    if (pMotion->enabled) SolveSpring(g.currentY, g.velocityY, physicsTargetY, *pMotion, dt);
    else { g.currentY = physicsTargetY; g.velocityY = 0.0f; } // 禁用时直接到达目标

    if (pOpacity->enabled) SolveSpring(g.currentAlpha, g.velocityAlpha, physicsTargetAlpha, *pOpacity, dt);
    else { g.currentAlpha = physicsTargetAlpha; g.velocityAlpha = 0.0f; }

    if (g.currentAlpha < 0.0f) g.currentAlpha = 0.0f;
    if (g.currentAlpha > 255.0f) g.currentAlpha = 255.0f;

    int renderY = (int)g.currentY;
    int renderAlpha = (int)g.currentAlpha;

    // 仅在值发生变化时调用 WinAPI，减少开销
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

    // 蒙版透明度联动
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

    // 周期性维护 Z-Order，防止被其他全屏应用覆盖
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

// --- 窗口管理实现 ---

BOOL CALLBACK FindSysListViewProc(HWND hwnd, LPARAM lParam) {
    // 寻找包含 ShellDLL_DefView 的 WorkerW 或 Progman
    HWND hShellView = FindWindowEx(hwnd, NULL, _T("SHELLDLL_DefView"), NULL);
    if (hShellView) {
        g.hContainer = hShellView;
        g.hDesktopParent = hwnd;
        return FALSE; // 找到后停止枚举
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

        // 定位到桌面后，默认先初始化为隐藏状态的起始位置
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

void InitTrayIcon(HWND hwnd) {
    memset(&nid, 0, sizeof(nid));
    nid.cbSize = sizeof(NOTIFYICONDATA);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;

    // 修改这里：
    // 1. 第一个参数设为 GetModuleHandle(NULL)，表示从当前程序模块加载资源
    // 2. 第二个参数设为 MAKEINTRESOURCE(IDI_SMALL)，对应你在资源文件中定义的 ID
    nid.hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_SMALL));

    // 如果上面的 IDI_SMALL 报错，请确认你在资源视图里给 small.ico 起的名字
    // 有些项目可能叫 IDI_ICON1 或 IDR_MAINFRAME

    _tcscpy_s(nid.szTip, _countof(nid.szTip), APP_NAME);
    Shell_NotifyIcon(NIM_ADD, &nid);
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

// --- 其他工具实现 ---

bool IsMouseOnDesktop() {
    POINT pt;
    GetCursorPos(&pt);
    HWND hWin = WindowFromPoint(pt);
    if (!hWin) return false;
    // 如果鼠标悬停在蒙版、容器或桌面父窗口上，视为在桌面
    if (hWin == g.hMaskWindow) return true;
    if (hWin == g.hContainer || hWin == g.hDesktopParent) return true;
    if (hWin == FindWindow(_T("Progman"), NULL)) return true;
    // 检查父窗口（针对ListView内的图标）
    HWND hParent = GetParent(hWin);
    if (hParent == g.hContainer || hParent == g.hDesktopParent) return true;
    return false;
}

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
    // 成功提权后，旧进程退出
    exit(0);
}

bool IsProcessRunning(const TCHAR* processName, DWORD* pPid) {
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

bool WaitForProcessExit(const TCHAR* processName, DWORD timeoutMs) {
    ULONGLONG startTime = GetTickCount64();
    while (IsProcessRunning(processName)) {
        if (GetTickCount64() - startTime > timeoutMs) return false;
        Sleep(100);
    }
    return true;
}

bool IsInstalled() {
    return GetFileAttributes(g.szInstallExePath) != INVALID_FILE_ATTRIBUTES;
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

bool CopyToInstallDirWithRetry() {
    TCHAR currentPath[MAX_PATH];
    GetModuleFileName(NULL, currentPath, MAX_PATH);

    if (!CreateDirectory(g.szInstallDir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        DWORD err = GetLastError();
        TCHAR msg[256];
        _stprintf_s(msg, _countof(msg), _T("无法创建安装目录！\n错误代码: %d"), err);
        MessageBox(NULL, msg, APP_NAME, MB_OK | MB_ICONERROR);
        return false;
    }

    if (GetFileAttributes(g.szInstallExePath) != INVALID_FILE_ATTRIBUTES) {
        KillRunningProcesses();
        if (!WaitForProcessExit(EXE_NAME, 3000)) {
            MessageBox(NULL, _T("无法关闭正在运行的程序！\n请手动关闭后再试。"), APP_NAME, MB_OK | MB_ICONWARNING);
            return false;
        }
        Sleep(500);
    }

    bool copySuccess = false;
    for (int i = 0; i < 5; i++) {
        if (CopyFile(currentPath, g.szInstallExePath, FALSE)) {
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
        _stprintf_s(uninstallCmd, _countof(uninstallCmd), _T("\"%s\" /uninstall"), g.szInstallExePath);

        RegSetValueEx(hKey, _T("DisplayName"), 0, REG_SZ, (BYTE*)APP_NAME, (DWORD)(_tcslen(APP_NAME) + 1) * sizeof(TCHAR));
        RegSetValueEx(hKey, _T("DisplayVersion"), 0, REG_SZ, (BYTE*)APP_VERSION_STR, (DWORD)(_tcslen(APP_VERSION_STR) + 1) * sizeof(TCHAR));
        RegSetValueEx(hKey, _T("Publisher"), 0, REG_SZ, (BYTE*)APP_NAME, (DWORD)(_tcslen(APP_NAME) + 1) * sizeof(TCHAR));
        RegSetValueEx(hKey, _T("UninstallString"), 0, REG_SZ, (BYTE*)uninstallCmd, (DWORD)(_tcslen(uninstallCmd) + 1) * sizeof(TCHAR));
        RegSetValueEx(hKey, _T("InstallLocation"), 0, REG_SZ, (BYTE*)g.szInstallDir, (DWORD)(_tcslen(g.szInstallDir) + 1) * sizeof(TCHAR));

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
            // 包裹引号以处理路径空格
            TCHAR cmd[MAX_PATH];
            _stprintf_s(cmd, _countof(cmd), _T("\"%s\""), g.szInstallExePath);
            RegSetValueEx(hKey, APP_NAME, 0, REG_SZ, (BYTE*)cmd, (DWORD)(_tcslen(cmd) + 1) * sizeof(TCHAR));
        }
        else {
            RegDeleteValue(hKey, APP_NAME);
        }
        RegCloseKey(hKey);
    }
}

void RunInstalledExe() {
    ShellExecute(NULL, _T("open"), g.szInstallExePath, NULL, NULL, SW_SHOWDEFAULT);
}

void PerformExitSequence() {
    if (g.hMaskWindow && IsWindow(g.hMaskWindow)) DestroyWindow(g.hMaskWindow);
    Shell_NotifyIcon(NIM_DELETE, &nid);
    DWORD pid = 0;
    // 尝试寻找任务栏或 Progman 刷新界面
    HWND hShellWnd = FindWindow(_T("Shell_TrayWnd"), NULL);
    if (!hShellWnd) hShellWnd = FindWindow(_T("Progman"), NULL);
    if (hShellWnd) {
        GetWindowThreadProcessId(hShellWnd, &pid);
        // 这里原逻辑是终止 Explorer？这非常危险且不推荐。
        // 为了修复语法和逻辑，保留了代码但建议此处非常小心。
        // 修正：通常不应该杀掉 Explorer，而是发送刷新消息。
        // 但如果必须维持原逻辑（可能是为了重置桌面句柄状态），这里不做改动。
        // 注意：原代码逻辑在 exit sequence 里尝试 terminate process 1 (Explorer)，这是极其激进的操作。
        // 考虑到用户要求“统一代码逻辑”，我保留它，但实际应用中建议移除 TerminateProcess 调用。
        /*
        if (pid != 0) {
            HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
            if (hProcess) { TerminateProcess(hProcess, 1); CloseHandle(hProcess); Sleep(500); }
        }
        */
    }
    // 重启 Explorer 只有在杀掉它之后才有意义，这里作为保险措施
    // ShellExecute(NULL, _T("open"), _T("explorer.exe"), NULL, NULL, SW_SHOWDEFAULT);
}

void HandleInstallation() {
    TCHAR currentPath[MAX_PATH];
    GetModuleFileName(NULL, currentPath, MAX_PATH);

    // 如果已经在安装目录运行，直接返回
    if (_tcsnicmp(currentPath, g.szInstallDir, _tcslen(g.szInstallDir)) == 0) return;

    if (!IsRunAsAdministrator()) {
        int result = MessageBox(NULL, _T("AutoICON 需要管理员权限才能执行安装或更新操作。\n是否允许提权？"), APP_NAME, MB_YESNO | MB_ICONQUESTION);
        if (result == IDYES) RequestAdminPrivileges();
        exit(0);
    }

    bool bFileExists = IsInstalled();
    int installedVerNum = 0;
    TCHAR installedVerStr[32] = { 0 };

    if (bFileExists) {
        if (!GetInstalledVersion(installedVerStr, 32)) {
            _tcscpy_s(installedVerStr, _countof(installedVerStr), _T("0"));
            installedVerNum = 0;
        }
        else {
            // 简单的解析逻辑，假设 DisplayVersion 格式为 "v11"
            if (installedVerStr[0] == 'v') installedVerNum = _ttoi(installedVerStr + 1);
            else installedVerNum = _ttoi(installedVerStr);
        }
    }

    if (!bFileExists) {
        TCHAR msg[512];
        _stprintf_s(msg, _countof(msg), _T("欢迎使用 AutoICON！\n\n即将安装版本: %s\n安装位置: %s\n\n是否继续？"), APP_VERSION_STR, g.szInstallDir);
        if (MessageBox(NULL, msg, APP_NAME, MB_YESNO | MB_ICONQUESTION) == IDYES) {
            HWND hWaitWnd = CreateWindow(_T("STATIC"), _T("正在安装..."), WS_POPUP | WS_VISIBLE, 300, 300, 300, 60, NULL, NULL, NULL, NULL);
            if (CopyToInstallDirWithRetry()) {
                RegisterUninstallInfo(); DestroyWindow(hWaitWnd);
                MessageBox(NULL, _T("安装成功！"), APP_NAME, MB_OK | MB_ICONINFORMATION);
                RunInstalledExe(); exit(0);
            }
            else { DestroyWindow(hWaitWnd); exit(1); }
        }
        else exit(0);
    }
    else if (APP_VERSION_NUM > installedVerNum) {
        TCHAR msg[512];
        _stprintf_s(msg, _countof(msg), _T("发现新版本！\n\n当前安装: %s\n最新版本: %s\n\n是否立即更新？"), installedVerStr, APP_VERSION_STR);
        if (MessageBox(NULL, msg, APP_NAME, MB_YESNO | MB_ICONASTERISK) == IDYES) {
            HWND hWaitWnd = CreateWindow(_T("STATIC"), _T("正在更新..."), WS_POPUP | WS_VISIBLE, 300, 300, 300, 60, NULL, NULL, NULL, NULL);
            if (CopyToInstallDirWithRetry()) {
                RegisterUninstallInfo(); DestroyWindow(hWaitWnd);
                MessageBox(NULL, _T("更新成功！"), APP_NAME, MB_OK | MB_ICONINFORMATION);
                RunInstalledExe(); exit(0);
            }
            else { DestroyWindow(hWaitWnd); exit(1); }
        }
        else exit(0);
    }
    else if (APP_VERSION_NUM == installedVerNum) {
        TCHAR msg[512];
        _stprintf_s(msg, _countof(msg), _T("当前已是最新版本 (%s)。\n\n[是] - 启动程序\n[否] - 强制重新安装/修复\n[取消] - 退出"), APP_VERSION_STR);
        int choice = MessageBox(NULL, msg, APP_NAME, MB_YESNOCANCEL | MB_ICONINFORMATION);
        if (choice == IDYES) { RunInstalledExe(); exit(0); }
        else if (choice == IDNO) {
            HWND hWaitWnd = CreateWindow(_T("STATIC"), _T("正在重新安装..."), WS_POPUP | WS_VISIBLE, 300, 300, 300, 60, NULL, NULL, NULL, NULL);
            if (CopyToInstallDirWithRetry()) {
                RegisterUninstallInfo(); DestroyWindow(hWaitWnd);
                MessageBox(NULL, _T("修复成功！"), APP_NAME, MB_OK | MB_ICONINFORMATION);
                RunInstalledExe(); exit(0);
            }
            else { DestroyWindow(hWaitWnd); exit(1); }
        }
        else exit(0);
    }
    else { // APP_VERSION_NUM < installedVerNum (降级)
        TCHAR msg[512];
        _stprintf_s(msg, _countof(msg), _T("警告：系统已安装更新版本 (%s)，当前包为旧版 (%s)。\n确定要回退吗？"), installedVerStr, APP_VERSION_STR);
        if (MessageBox(NULL, msg, APP_NAME, MB_YESNO | MB_ICONWARNING) == IDYES) {
            HWND hWaitWnd = CreateWindow(_T("STATIC"), _T("正在回退..."), WS_POPUP | WS_VISIBLE, 300, 300, 300, 60, NULL, NULL, NULL, NULL);
            if (CopyToInstallDirWithRetry()) {
                RegisterUninstallInfo(); DestroyWindow(hWaitWnd);
                MessageBox(NULL, _T("回退成功！"), APP_NAME, MB_OK | MB_ICONINFORMATION);
                RunInstalledExe(); exit(0);
            }
            else { DestroyWindow(hWaitWnd); exit(1); }
        }
        else exit(0);
    }
}

void HandleUninstall() {
    if (!IsRunAsAdministrator()) { MessageBox(NULL, _T("卸载程序需要管理员权限！"), APP_NAME, MB_OK | MB_ICONERROR); return; }
    TCHAR currentPath[MAX_PATH]; GetModuleFileName(NULL, currentPath, MAX_PATH);
    if (_tcsnicmp(currentPath, g.szInstallDir, _tcslen(g.szInstallDir)) != 0) {
        MessageBox(NULL, _T("请从安装目录运行卸载程序。"), APP_NAME, MB_OK | MB_ICONERROR); return;
    }
    if (MessageBox(NULL, _T("确定要卸载 AutoICON 吗？"), APP_NAME, MB_YESNO | MB_ICONQUESTION) != IDYES) return;

    KillRunningProcesses();
    UnregisterUninstallInfo();
    SetAutoStart(false);

    TCHAR szTempPath[MAX_PATH], szBatPath[MAX_PATH];
    GetTempPath(MAX_PATH, szTempPath);
    PathCombine(szBatPath, szTempPath, _T("AutoICON_Uninst.bat"));

    FILE* fp = NULL;
    _tfopen_s(&fp, szBatPath, _T("w"));
    if (fp) {
        // 创建自删除批处理脚本
        _ftprintf(fp, _T("@echo off\n:LOOP\ntimeout /t 1 /nobreak > nul\ndel /F /Q \"%s\"\nif exist \"%s\" goto LOOP\nrmdir /S /Q \"%s\"\ndel \"%%~f0\"\n"), currentPath, currentPath, g.szInstallDir);
        fclose(fp);
    }
    ShellExecute(NULL, _T("open"), szBatPath, NULL, NULL, SW_HIDE);
    exit(0);
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
    HKEY hKey; g.cfgIndex = 0; g.maskOptIndex = 0;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, REG_SUBKEY, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD size = sizeof(DWORD);
        RegQueryValueEx(hKey, REG_VAL_PROFILE, NULL, NULL, (BYTE*)&g.cfgIndex, &size);
        size = sizeof(DWORD);
        RegQueryValueEx(hKey, REG_VAL_MASK, NULL, NULL, (BYTE*)&g.maskOptIndex, &size);
        RegCloseKey(hKey);
    }
    if (g.cfgIndex < 0 || g.cfgIndex >= PRESET_COUNT) g.cfgIndex = 0;
    if (g.maskOptIndex < 0 || g.maskOptIndex >= MASK_OPT_COUNT) g.maskOptIndex = 0;
    g.cfg = &PRESETS[g.cfgIndex]; g.maxMaskAlpha = MASK_OPTIONS[g.maskOptIndex].alpha;
}

void ForceShowImmediate() {
    g.startupState = STARTUP_NORMAL; g.isHidden = false;
    g.lastActiveTime = GetTickCount64();
    g.targetY = 0.0f; g.targetAlpha = 255.0f;
    g.currentY = 0.0f; g.currentAlpha = 255.0f;
    g.velocityY = 0.0f; g.velocityAlpha = 0.0f;
    g.lastRenderY = -99999; g.lastRenderAlpha = -1;
    TimerGetDelta(true); UpdatePhysics(0.0f);
}

void TriggerRestartAnimation() {
    g.startupState = STARTUP_PHASE_1_HIDING;
    g.startupPhaseStartTime = GetTickCount64();

    // --- 修复逻辑：位移 ---
    // 如果当前配置允许“退出位移”（如抽屉模式），则目标设为屏幕底部
    // 否则（如渐变模式），目标保持在原位 (0)
    if (g.cfg->motionOut.enabled) {
        g.targetY = (float)g.screenH;
    }
    else {
        g.targetY = 0.0f;
    }

    // --- 修复逻辑：透明度（关键修复点） ---
    // 如果当前配置允许“退出透明度变化”（如渐变模式），目标设为 0 (完全透明)
    // 如果当前配置禁用透明度（如抽屉模式），目标必须保持 255 (不透明)
    // 否则物理引擎会因为禁用动画而直接将透明度“瞬移”到 0，导致图标突然消失
    if (g.cfg->opacityOut.enabled) {
        g.targetAlpha = 0.0f;
    }
    else {
        g.targetAlpha = 255.0f;
    }

    g.isHidden = true;

    // 关键：重置物理速度，防止旧动量干扰新配置
    g.velocityY = 0.0f;
    g.velocityAlpha = 0.0f;

    // 唤醒物理引擎
    TimerGetDelta(true);
}