#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
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
#include <strsafe.h>
#include "resource.h"
#include <string>
#include <powrprof.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.h>
#include <commctrl.h>


// 链接必要的系统库
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib") 
#pragma comment(lib, "PowrProf.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(linker,"\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' \
version='6.0.0.0' \
processorArchitecture='*' \
publicKeyToken='6595b64144ccf1df' \
language='*'\"")

#ifdef UNICODE
typedef std::wstring tstring;
#else
typedef std::string tstring;
#endif

#ifndef _countof
#define _countof(_Array) (sizeof(_Array) / sizeof(_Array[0]))
#endif

// ==========================================
// === 版本、配置与常量定义 ===
// ==========================================
#define APP_NAME        _T("AutoICON")
#define APP_VERSION_STR _T("v22") // 用于显示的字符串版本
#define APP_VERSION_NUM 22        // 用于逻辑比较的数字版本
#define EXE_NAME        _T("AutoICON.exe")

// 设置文件
#define SETTINGS_FILE      _T("settings.ini")
#define SETTINGS_SECTION   _T("Settings")
#define SET_VAL_PROFILE    _T("LastProfileIndex")
#define SET_VAL_MASK       _T("MaskOpacityIndex")
#define SET_VAL_POWERSAVER _T("PowerSaverOverride")
#ifdef STORE_BUILD
    // 使用 StartupTask，不需要 REG_RUN_KEY
#else
#define REG_RUN_KEY _T("Software\\Microsoft\\Windows\\CurrentVersion\\Run")
#endif

// 消息与菜单ID
#define WM_TRAYICON          (WM_USER + 1)
#define WM_ENV_CHECK_UPDATE  (WM_USER + 201)
#define ID_TRAY_EXIT         9001
#define ID_TRAY_AUTOSTART    9002
#define ID_PROFILE_START     9100
#define ID_MASK_START        9200
#define ID_TRAY_POWERSAVER   9003
#define ID_TRAY_POWERSAVER   9003
#define ID_TRAY_SUPPORT      9004 // 新增：支持项目菜单ID

// 启动状态机常量 (原代码缺失)
#define STARTUP_PHASE_1_HIDING  0
#define STARTUP_PHASE_2_WAITING 1
#define STARTUP_PHASE_3_SHOWING 2
#define STARTUP_NORMAL          3

// 动画与时间常量 (原代码缺失)
#define STARTUP_TRANSITION_DELAY 700   // 切换配置时的等待毫秒数
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
    UINT nameId;
    ULONGLONG hideDelayMs;
    ULONGLONG idleCheckMs;
    SpringParams motionIn;
    SpringParams motionOut;
    SpringParams opacityIn;
    SpringParams opacityOut;
};

const ConfigProfile PRESETS[] = {
    { IDS_PROFILE_FADE_DEFAULT, 5000, 200, SP_OFF, SP_OFF, SP_NORMAL, SP_VERYSLOW },
    { IDS_PROFILE_FADE_FAST,    3000, 100, SP_OFF, SP_OFF, SP_FAST, SP_SLOW },
    { IDS_PROFILE_DRAWER_DEFAULT, 8000, 200, SP_NORMAL, SP_VERYSLOW, SP_OFF, SP_OFF },
    { IDS_PROFILE_DRAWER_FAST,  5000, 100, SP_FAST, SP_SLOW, SP_OFF, SP_OFF },
    { IDS_PROFILE_SLIDE_DEFAULT, 6000, 200, SP_NORMAL, SP_VERYSLOW, SP_NORMAL, SP_VERYSLOW },
    { IDS_PROFILE_SLIDE_FAST,   4000, 100, SP_FAST, SP_SLOW, SP_FAST, SP_SLOW },
    { IDS_PROFILE_ALWAYS_SHOW,  0xFFFFFFFF, 1000, SP_FAST, SP_FAST, SP_FAST, SP_FAST }
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
    { 85, 217 }, { 90, 230 }, { 95, 240 }, { 100, 255 }
};
const int MASK_OPT_COUNT = (int)(sizeof(MASK_OPTIONS) / sizeof(MASK_OPTIONS[0]));

struct DisplayFingerprint {
    TCHAR  deviceName[CCHDEVICENAME]; // 主显示器设备名（主屏切换时变化）
    DWORD  width, height;             // 分辨率
    DWORD  bpp;                       // 位深
    DWORD  frequency;                 // ★ 刷新率（WM_DISPLAYCHANGE 覆盖不到的关键项）
    int    virtX, virtY, virtW, virtH; // 虚拟屏幕原点+尺寸（主屏切换/排列变化）
    int    monitorCount;              // SM_CMONITORS（投影方式变化）
    UINT   dpi;                       // ★ 系统 DPI（缩放变化）
};

// 全局上下文结构
struct GlobalState {
    // 窗口句柄
    HWND hContainer;
    HWND hDesktopParent;
    HWND hMsgWindow;
    HWND hMaskWindow;
    HWND hBackdrop;

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
    ULONGLONG lastActiveTime;
    bool isHidden;
    bool appRunning;
    bool isPaused;
    bool needDisplayReset;
    int vsyncDivisor;
    bool enablePowerSaver;
    bool isPowerSaverActive;
    bool isThirdPartyWallpaperActive;
    // 路径
    TCHAR szInstallDir[MAX_PATH];
    TCHAR szInstallExePath[MAX_PATH];
} g = { 0 };
CRITICAL_SECTION g_csLogic;



NOTIFYICONDATA nid = { 0 };
LARGE_INTEGER qpcFreq;
LARGE_INTEGER qpcLastTime;
UINT g_uMsgTaskbarCreated = 0;
const int MOUSE_MOVE_THRESHOLD = 2;

const GUID GUID_MY_POWER_SAVING_STATUS = { 0xE00958C0, 0xC213, 0x4ACE, { 0xAC, 0x77, 0xFE, 0xCC, 0xED, 0x2E, 0xEE, 0xA5 } };
static bool g_isWindowsSaverToggleOn = false;
static HPOWERNOTIFY g_hPowerNotify = NULL;

// ==========================================
// === 函数前置声明 (Declaration) ===
// ==========================================

tstring GetStringResource(UINT stringID);
void TimerInit();
float TimerGetDelta(bool resetOnly = false);
DWORD WINAPI LogicThreadProc(LPVOID lpParam);

bool IsProcessRunning(const TCHAR* processName, DWORD* pPid = NULL);
bool KillProcess(DWORD pid);
bool KillRunningProcesses();
bool WaitForProcessExit(const TCHAR* processName, DWORD timeoutMs = 5000);

winrt::Windows::ApplicationModel::StartupTask GetStartupTask();
bool IsAutoStartEnabled();
void SetAutoStart(bool enabled);
bool GetSettingsFilePath(TCHAR outPath[MAX_PATH]);
void SaveSettings();
bool LoadSettings();
void PerformExitSequence();

void EnableLayeredStyle(HWND hwnd, bool enable);
void EnforceZOrder();
void CreateMaskWindow(HINSTANCE hInstance);
void AttachMaskToDesktop();
void LocateDesktop(HINSTANCE hInstance);
void InitTrayIcon(HWND hwnd);
void CreateMessageWindow(HINSTANCE hInstance);
LRESULT CALLBACK BackdropWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
bool IsThirdPartyWallpaperActive();

void SolveSpring(float& current, float& velocity, float target, const SpringParams& p, float dt);
void UpdatePhysics(float dt);
void ApplyAnimation();
bool IsPhysicsIdle();
void ForceShowImmediate();
void TriggerRestartAnimation();
bool IsMouseOnDesktop();
bool IsDesktopBusy();
void ShowSupportDialog(HWND hwnd);

void ShowTrayMenu(HWND hwnd);
int GetPrimaryRefreshRate();
int CalculateVSyncDivisor(int refreshRate);
bool CheckDisplayFingerprintChanged();
void GetDisplayFingerprint(DisplayFingerprint& fp);

LRESULT CALLBACK MsgWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ==========================================
// === 主程序入口 ===
// ==========================================
int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine, _In_ int nCmdShow) {
    // 尝试设置 DPI 感知，如果系统不支持则忽略
    winrt::init_apartment();

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);



    

    // 单实例互斥锁
    HANDLE hMutex = CreateMutex(NULL, TRUE, _T("Local\\AutoICON_Instance"));
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    g_uMsgTaskbarCreated = RegisterWindowMessage(_T("TaskbarCreated"));
    g.appRunning = true;
    g.isPaused = false;

    // 加载配置并检测是否首次运行
    bool isFirstRun = LoadSettings();
    g.lastActiveTime = GetTickCount64();

    // 如果是首次运行，弹出提示建议开启开机自启（多语言适配版）
    if (isFirstRun) {
        tstring title = GetStringResource(IDS_APP_NAME);
        tstring message = GetStringResource(IDS_FIRST_RUN_MESSAGE);

        // 防御性处理：如果资源加载失败则使用默认文本
        if (title.empty()) {
            title = APP_NAME;
        }

        if (!message.empty()) {
            int response = MessageBox(
                NULL,
                message.c_str(),
                title.c_str(),
                MB_YESNO | MB_ICONINFORMATION | MB_TOPMOST
            );

            if (response == IDYES) {
                SetAutoStart(true);
            }
        }

        // 立即保存配置生成 settings.ini，避免下次启动重复提示
        SaveSettings();
    }

    // 1. 【修复】必须在创建窗口和定位桌面之前，就初始化多线程锁和计时器！
    TimerInit();
    InitializeCriticalSection(&g_csLogic);

    // 2. 初始化窗口和系统组件
    CreateMessageWindow(hInstance);
    WTSRegisterSessionNotification(g.hMsgWindow, NOTIFY_FOR_THIS_SESSION);
    InitTrayIcon(g.hMsgWindow);
    g.vsyncDivisor = 1;

    // 此时 LocateDesktop 内部再调用 ForceShowImmediate() 时，锁已经准备好了，不会再闪退
    LocateDesktop(hInstance);

    // 3. 启动后台逻辑线程
    HANDLE hLogicThread = CreateThread(NULL, 0, LogicThreadProc, NULL, 0, NULL);
    if (!g.hContainer) {
        MessageBox(NULL, _T("无法定位桌面窗口。"), APP_NAME, MB_ICONERROR);
        g.appRunning = false;
    }

    // 主消息循环
    MSG msg = { 0 };
    while (g.appRunning) {
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) g.appRunning = false;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (!g.appRunning) break;
        if (g.isPaused) { Sleep(1000); continue; }
        
        if (g.needDisplayReset) {
            g.needDisplayReset = false;
            // 阻塞一下主循环，给 Windows Explorer 重绘桌面留点喘息时间
            Sleep(1500);

            // 1. 扬了旧的遮罩和替身
            if (g.hMaskWindow && IsWindow(g.hMaskWindow)) { DestroyWindow(g.hMaskWindow); g.hMaskWindow = NULL; }
            if (g.hBackdrop && IsWindow(g.hBackdrop)) { DestroyWindow(g.hBackdrop); g.hBackdrop = NULL; }

            // 2. 重新寻找桌面容器并更新 g.screenW / g.screenH
            LocateDesktop(hInstance);

            // 3. 重建并贴上遮罩
            if (g.maxMaskAlpha > 0) {
                CreateMaskWindow(hInstance);
                AttachMaskToDesktop();
            }

            // 4. 重置物理引擎状态，防止分辨率突变导致图标飞出银河系卡死
            ForceShowImmediate();
            // ★ 新增：强制同步省电状态下的配置文件
            const ConfigProfile* expectedCfg = nullptr;
            if (g.enablePowerSaver && g.isPowerSaverActive) {
                expectedCfg = &PRESETS[0];    // 省电只允许渐变
            }
            else {
                expectedCfg = &PRESETS[g.cfgIndex]; // 用户选择
            }

            if (g.cfg != expectedCfg) {
                g.cfg = expectedCfg;
                // 当前已是 NORMAL 状态，直接生效即可，无需重启动画
                // 但为了视觉连贯，可以立即刷新所有运动目标
                g.targetY = 0.0f;
                g.targetAlpha = 255.0f;
                // 如果物理引擎有残留位置，也强制归位
                g.currentY = 0.0f;
                g.currentAlpha = 255.0f;
                g.velocityY = 0.0f;
                g.velocityAlpha = 0.0f;
            }
            continue;
        }
        // 桌面窗口防丢失机制
        if (!IsWindow(g.hContainer)) {
            LocateDesktop(hInstance);
            if (!g.hContainer) { Sleep(500); continue; }
            // ★ 补充：Explorer 重启等场景下，蒙版/替身随桌面容器一起恢复
            if (g.maxMaskAlpha > 0 && (!g.hMaskWindow || !IsWindow(g.hMaskWindow))) {
                CreateMaskWindow(hInstance);
                AttachMaskToDesktop();
            }
        }

        // 状态更新逻辑 (剔除旧的鼠标和忙碌检测代码)
        ULONGLONG currTime = GetTickCount64();

        EnterCriticalSection(&g_csLogic);
        bool physicsIdle = IsPhysicsIdle();
        LeaveCriticalSection(&g_csLogic);

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
                EnterCriticalSection(&g_csLogic);
                bool settingsChanged = false;
                if (g.hasPendingCfg) {
                    g.cfgIndex = g.pendingCfgIndex;
                    g.hasPendingCfg = false;
                    settingsChanged = true; // 只有用户真实切换配置时才标记修改
                }
                if (g.hasPendingMask) {
                    g.maskOptIndex = g.pendingMaskOptIndex;
                    g.hasPendingMask = false;
                    settingsChanged = true;
                }

                // 【关键修复】应用新配置：如果启用了智能省电且处于省电状态，则强制覆盖为 PRESETS[0] (默认渐变)
                if (g.enablePowerSaver && g.isPowerSaverActive) {
                    g.cfg = &PRESETS[0];
                }
                else {
                    g.cfg = &PRESETS[g.cfgIndex];
                }

                g.lastActiveTime = currTime;
                g.maxMaskAlpha = MASK_OPTIONS[g.maskOptIndex].alpha;

                // 只有用户显式更改了配置，才保存到注册表，防止自动覆盖污染用户的存档
                if (settingsChanged) {
                    SaveSettings();
                }

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
                LeaveCriticalSection(&g_csLogic);
                DwmFlush();
                TimerGetDelta(true);
                continue;
            }
        }
        else if (g.startupState == STARTUP_PHASE_3_SHOWING) { // 2: 显示阶段
            if (physicsIdle) {
                g.startupState = STARTUP_NORMAL;
                g.lastActiveTime = currTime;
            }
        }
        // 3: 正常运行阶段
        // 物理更新步进 (为了安全，读取时快速加锁)
        

        if (!physicsIdle) {
            float dt = TimerGetDelta();


            EnterCriticalSection(&g_csLogic);
            UpdatePhysics(dt);
            LeaveCriticalSection(&g_csLogic);

            
            DwmFlush(); // 1. 先等待一次物理垂直同步，对齐显卡信号


            EnterCriticalSection(&g_csLogic);
            ApplyAnimation();
            LeaveCriticalSection(&g_csLogic);

            if (g.vsyncDivisor > 1) {
                // 2. 如果开启了省电分频，补足剩余的休眠时间
                // 获取屏幕刷新率算出一帧的毫秒数，比如 60Hz 约等于 16.6ms
                int rr = GetPrimaryRefreshRate();
                if (rr <= 0) rr = 60;

                // 计算需要额外休眠的毫秒数 (分频数 - 1) * 单帧时间
                DWORD extraSleep = (DWORD)((g.vsyncDivisor - 1) * (1000.0f / rr));
                Sleep(extraSleep);
            }
        }
        else {
            // 静态检测：如果有尚未到达的目标，强行推进一步
            EnterCriticalSection(&g_csLogic);
            bool needForceUpdate = (g.currentY != g.targetY || g.currentAlpha != g.targetAlpha);
            if (needForceUpdate) UpdatePhysics(0.0f);
            LeaveCriticalSection(&g_csLogic);
            if (!g.isHidden) {
                EnforceZOrder();
                ApplyAnimation();
            }
            Sleep((DWORD)g.cfg->idleCheckMs);
            TimerGetDelta(true);
        }

        // === 【新增】智能环境检测与切换逻辑 ===
        static ULONGLONG lastEnvActionTime = 0;
        if (currTime - lastEnvActionTime > 3000) {
            lastEnvActionTime = currTime;
            // 真正的环境检测已经交由 LogicThreadProc 并在 WM_ENV_CHECK_UPDATE 中处理
            // 主线程只需要作为兜底防止窗口丢失重置即可
            if (g.hBackdrop && !IsWindow(g.hBackdrop)) {
                g.needDisplayReset = true;
            }
        }
        // ======================================

        // 删除 g.lastMousePos = currMouse; 这一行，已经彻底不需要了。
    }

    if (hLogicThread) {
        WaitForSingleObject(hLogicThread, 1000);
        CloseHandle(hLogicThread);
    }
    DeleteCriticalSection(&g_csLogic);
    WTSUnRegisterSessionNotification(g.hMsgWindow);

    if (hMutex) CloseHandle(hMutex);
    return 0;
}

// ==========================================
// === 【新增】显示环境指纹：刷新率/DPI/主屏/投影变化兜底检测 ===
// ==========================================


void GetDisplayFingerprint(DisplayFingerprint& fp) {
    ZeroMemory(&fp, sizeof(fp));
    DEVMODE dm;
    ZeroMemory(&dm, sizeof(dm));
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettings(NULL, ENUM_CURRENT_SETTINGS, &dm)) {
        _tcscpy_s(fp.deviceName, _countof(fp.deviceName), dm.dmDeviceName);
        fp.width = dm.dmPelsWidth;
        fp.height = dm.dmPelsHeight;
        fp.bpp = dm.dmBitsPerPel;
        fp.frequency = dm.dmDisplayFrequency;
    }
    fp.virtX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    fp.virtY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    fp.virtW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    fp.virtH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    fp.monitorCount = GetSystemMetrics(SM_CMONITORS);
    fp.dpi = GetDpiForSystem(); // ★ 静态链接，要求 Win10 1607+（你已确认）
}

bool CheckDisplayFingerprintChanged() {
    static DisplayFingerprint last = { 0 };
    DisplayFingerprint cur;
    GetDisplayFingerprint(cur);
    bool changed = (memcmp(&last, &cur, sizeof(DisplayFingerprint)) != 0);
    last = cur; // 先更新快照再返回，保证只触发一次
    return changed;
}

int GetPrimaryRefreshRate() {
    DEVMODE dm;
    ZeroMemory(&dm, sizeof(dm));
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettings(NULL, ENUM_CURRENT_SETTINGS, &dm)) {
        if (dm.dmDisplayFrequency > 1) {
            return dm.dmDisplayFrequency; // 比如返回 60, 144, 360 等
        }
    }
    return 60; // 兜底默认值
}

int CalculateVSyncDivisor(int refreshRate) {
    int divisor = (int)((float)refreshRate / 40.0f + 0.5f); // 四舍五入
    if (divisor < 1) divisor = 1;
    return divisor;
}

DWORD WINAPI LogicThreadProc(LPVOID lpParam) {
    ULONGLONG lastBusyCheckTime = 0;
    ULONGLONG lastEnvCheckTime = 0;
    bool localIsBusy = false;
    POINT lastMouse = { 0, 0 };

    while (g.appRunning) {
        // 如果处于暂停、非正常运行状态、或正在重置显示，则挂起逻辑检测
        if (g.isPaused || g.startupState != STARTUP_NORMAL || g.needDisplayReset || !g.hContainer) {
            Sleep(50);
            continue;
        }

        ULONGLONG currTime = GetTickCount64();
        POINT currMouse;
        GetCursorPos(&currMouse);
        bool isMoving = (abs(currMouse.x - lastMouse.x) > MOUSE_MOVE_THRESHOLD ||
            abs(currMouse.y - lastMouse.y) > MOUSE_MOVE_THRESHOLD);
        lastMouse = currMouse;

        // ==========================================
        // 1. 无锁状态下执行高耗时 Windows API
        // ==========================================
        if (currTime - lastBusyCheckTime >= 500) {
            localIsBusy = IsDesktopBusy();
            lastBusyCheckTime = currTime;
        }

            if (currTime - lastEnvCheckTime > 3000) {
                // ★【新增】显示环境指纹：刷新率/DPI/主屏/投影变化兜底触发重适配
                if (CheckDisplayFingerprintChanged()) {
                    g.needDisplayReset = true; // 置位后本线程随后会被 :572 的挂起检查挂起，无冲突
                }
                bool is3rdPartyActive = IsThirdPartyWallpaperActive();
                // ...原有省电/壁纸检测保持不变...

            // 增加：检测省电模式
            bool isBatterySaverOn = false;

            // 1. 检测：物理电池状态 和 Win10/11 右下角节电开关
            SYSTEM_POWER_STATUS sps;
            if (GetSystemPowerStatus(&sps)) {
                if (g_isWindowsSaverToggleOn || sps.ACLineStatus == 0) {
                    isBatterySaverOn = true;
                }
            }

            // 2. 检测：控制面板中的“电源计划”(完美兼容台式机)
            if (!isBatterySaverOn) { // 如果上面没触发，继续查电源计划
                GUID* pActivePolicy = NULL;
                if (PowerGetActiveScheme(NULL, &pActivePolicy) == ERROR_SUCCESS) {
                    if (pActivePolicy != NULL) {
                        // GUID_MAX_POWER_SAVINGS 是系统底层的“节能”计划标示符
                        if (IsEqualGUID(*pActivePolicy, GUID_MAX_POWER_SAVINGS)) {
                            isBatterySaverOn = true;
                        }
                        LocalFree(pActivePolicy); // 必须释放内存防止泄漏
                    }
                }
            }
            g.isThirdPartyWallpaperActive = is3rdPartyActive;
            // 打包发送：wParam 传壁纸状态，lParam 传省电状态
            PostMessage(g.hMsgWindow, WM_ENV_CHECK_UPDATE, (WPARAM)is3rdPartyActive, (LPARAM)isBatterySaverOn);
            lastEnvCheckTime = currTime;
        }

        bool isOnDesktop = false;
        if (isMoving || localIsBusy) {
            isOnDesktop = IsMouseOnDesktop();
        }

        // ==========================================
        // 2. 极速锁定并更新全局目标参数 (耗时 < 1微秒)
        // ==========================================
        EnterCriticalSection(&g_csLogic);

        if (isMoving || localIsBusy) {
            if (localIsBusy || isOnDesktop) {
                g.lastActiveTime = currTime;
                g.targetY = 0.0f;
                g.targetAlpha = 255.0f;
                g.isHidden = false;
            }
        }
        else { // <--- 必须保留这个 else，这是 v16 的原生逻辑
            if (!g.isHidden && (currTime - g.lastActiveTime > g.cfg->hideDelayMs)) {
                g.targetY = g.cfg->motionOut.enabled ? (float)g.screenH : 0.0f;
                g.targetAlpha = g.cfg->opacityOut.enabled ? 0.0f : 255.0f;
                g.isHidden = true;
            }
        }

        LeaveCriticalSection(&g_csLogic);

        // 逻辑线程约 30Hz 运行即可，节省 CPU
        Sleep(30);
    }
    return 0;
}


// ==========================================
// === 最终修正版：精确检测忙碌状态 ===
// ==========================================
bool IsDesktopBusy() {
    // ---------------------------------------------------------
    // 检测 A: 必须是正在重命名 (焦点在 Edit 子控件，而不是图标列表本身)
    // ---------------------------------------------------------
    GUITHREADINFO gti = { sizeof(GUITHREADINFO) };
    HWND hForeground = GetForegroundWindow();

    if (hForeground) {
        DWORD dwThread = GetWindowThreadProcessId(hForeground, NULL);
        if (GetGUIThreadInfo(dwThread, &gti) && gti.hwndFocus) {

            // 关键修改：
            // 1. 必须是 g.hContainer 的子窗口 (不能是 g.hContainer 自己)
            // 2. 类名必须是 "Edit" (确保是文本编辑框)
            if (IsChild(g.hContainer, gti.hwndFocus)) {
                TCHAR className[64];
                if (GetClassName(gti.hwndFocus, className, 64) > 0) {
                    // 只有类名为 Edit 时才判定为正在重命名
                    if (_tcsicmp(className, _T("Edit")) == 0) {
                        return true;
                    }
                }
            }
        }
    }

    // ---------------------------------------------------------
    // 检测 B: 必须是桌面的右键菜单
    // ---------------------------------------------------------
    HWND hMenu = FindWindow(_T("#32768"), NULL);
    if (hMenu && IsWindowVisible(hMenu)) {
        HWND hOwner = GetWindow(hMenu, GW_OWNER);

        // 只有当菜单的“老板”是桌面本身，或者是图标列表时，才算数
        if (hOwner == g.hDesktopParent || hOwner == g.hContainer) {
            return true;
        }

        // 备用判定：通过进程ID匹配
        DWORD menuPid = 0, desktopPid = 0;
        GetWindowThreadProcessId(hMenu, &menuPid);
        GetWindowThreadProcessId(g.hDesktopParent, &desktopPid);

        if (menuPid == desktopPid) {
            if (IsMouseOnDesktop()) return true;
        }
    }

    return false;
}
// ==========================================
// === 【新增】壁纸兼容性补丁函数 ===
// ==========================================

// 1. 替身窗口回调：专门调用系统API画静态壁纸
LRESULT CALLBACK BackdropWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        PaintDesktop(hdc); // 核心魔法：直接画出Windows当前的静态壁纸
        EndPaint(hwnd, &ps);
        return 0;
    }
    // 拦截背景擦除，防止闪烁
    if (msg == WM_ERASEBKGND) return 1;
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// 2. 通用环境检测：判断是否有第三方壁纸软件(WE, Lively等)在运行
bool IsThirdPartyWallpaperActive() {
    if (!g.hDesktopParent) return false;

    // 逻辑：如果图标层的父窗口后面还有其他可见的 WorkerW 窗口，
    // 说明有第三方软件插入了壁纸层。
    bool foundActiveWallpaper = false;
    HWND hWorker = FindWindowEx(NULL, NULL, _T("WorkerW"), NULL);

    while (hWorker) {
        if (hWorker != g.hDesktopParent && IsWindowVisible(hWorker)) {
            // 简单的启发式检查：通常壁纸软件的窗口都很大
            RECT r; GetWindowRect(hWorker, &r);
            if ((r.right - r.left) >= g.screenW) {
                foundActiveWallpaper = true;
                break;
            }
        }
        hWorker = FindWindowEx(NULL, hWorker, _T("WorkerW"), NULL);
    }
    return foundActiveWallpaper;
}



void ShowTrayMenu(HWND hwnd) {
    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(hwnd);

    

    HMENU hMenu = CreatePopupMenu();

   
    // 动画模式子菜单
    
    HMENU hSubProfile = CreatePopupMenu();
    for (int i = 0; i < PRESET_COUNT; i++) {
        UINT flags = MF_STRING;
        int checkIndex = g.hasPendingCfg ? g.pendingCfgIndex : g.cfgIndex;
        if (i == checkIndex) flags |= MF_CHECKED;
        AppendMenu(hSubProfile, flags, ID_PROFILE_START + i, GetStringResource(PRESETS[i].nameId).c_str());
    }
    //AppendMenu(hMenu, MF_POPUP, (UINT_PTR)hSubProfile, _T("动画模式 (Animation Mode)"));
    

    // 【修改处】：定义标志位，根据省电激活状态动态添加灰度属性
    UINT animMenuFlags = MF_POPUP;

    // 直接从资源文件读取 "动画模式" 的多语言文本
    tstring animMenuTitle = GetStringResource(IDS_MENU_ANIMATION);

    // 只有当开关开启且确实处于省电激活状态时，才置灰并追加提示文本
    if (g.enablePowerSaver && g.isPowerSaverActive) {
        // 动态追加 " (已激活)" 小尾巴
        animMenuTitle = GetStringResource(IDS_MENU_POWERSAVER_ACTIVATED);
        animMenuFlags |= MF_GRAYED; // 关键：添加此标志使子菜单无法被打开
    }

    // 使用 .c_str() 将 tstring 转换并传递给 Windows API
    AppendMenu(hMenu, animMenuFlags, (UINT_PTR)hSubProfile, animMenuTitle.c_str());

    // 蒙版子菜单
    HMENU hSubMask = CreatePopupMenu();
    for (int i = 0; i < MASK_OPT_COUNT; i++) {
        UINT flags = MF_STRING;
        int checkIndex = g.hasPendingMask ? g.pendingMaskOptIndex : g.maskOptIndex;
        if (i == checkIndex) flags |= MF_CHECKED;

        TCHAR buf[64] = { 0 };
        if (MASK_OPTIONS[i].percent == 0) _tcscpy_s(buf, _countof(buf), GetStringResource(IDS_MENU_MASK_OFF).c_str());
        else _stprintf_s(buf, _countof(buf), GetStringResource(IDS_MENU_MASK_PERCENT).c_str(), MASK_OPTIONS[i].percent);

        AppendMenu(hSubMask, flags, ID_MASK_START + i, buf);
    }
    AppendMenu(hMenu, MF_POPUP, (UINT_PTR)hSubMask, GetStringResource(IDS_MENU_MASK).c_str());

    AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);

    // === 【新增】省电模式项 ===
    UINT psFlags = MF_STRING;
    if (g.enablePowerSaver) psFlags |= MF_CHECKED;
    AppendMenu(hMenu, psFlags, ID_TRAY_POWERSAVER, GetStringResource(IDS_MENU_POWERSAVER).c_str());

    // 开机自启
    UINT autoStartFlags = MF_STRING;
    if (IsAutoStartEnabled()) autoStartFlags |= MF_CHECKED;
    AppendMenu(hMenu, autoStartFlags, ID_TRAY_AUTOSTART, GetStringResource(IDS_MENU_AUTOSTART).c_str());

    tstring supportMenuText = GetStringResource(IDS_MENU_SUPPORT);
    if (!supportMenuText.empty()) {
        AppendMenu(hMenu, MF_STRING, ID_TRAY_SUPPORT, supportMenuText.c_str());
    }

    AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(hMenu, MF_STRING, ID_TRAY_EXIT, GetStringResource(IDS_MENU_EXIT).c_str());

    TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, p.x, p.y, 0, hwnd, NULL);

    DestroyMenu(hMenu);
}

// --- 消息处理 ---

LRESULT CALLBACK MsgWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == g_uMsgTaskbarCreated && g_uMsgTaskbarCreated != 0) {
        g.hContainer = NULL; // 任务栏重启需重新定位桌面
        return 0;
    }
    switch (msg) {
    case WM_ENV_CHECK_UPDATE: {
        bool physicalBatterySaver = (lParam != 0);
        bool is3rdPartyActive = (wParam != 0);

        if (g.hBackdrop && IsWindow(g.hBackdrop)) {
            bool isVisible = IsWindowVisible(g.hBackdrop);
            if (is3rdPartyActive) {
                if (isVisible) ShowWindow(g.hBackdrop, SW_HIDE);
            }
            else {
                if (!isVisible) {
                    ShowWindow(g.hBackdrop, SW_SHOWNOACTIVATE);
                    SetWindowPos(g.hBackdrop, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                    InvalidateRect(g.hBackdrop, NULL, TRUE);
                }
            }
        }

        // === 【新增】省电模式覆盖及动画重置逻辑 ===
        bool effectiveBatterySaver = (physicalBatterySaver && g.enablePowerSaver);

        // 当“有效省电状态”发生变化时
        if (g.isPowerSaverActive != effectiveBatterySaver) {
            g.isPowerSaverActive = effectiveBatterySaver;

            if (effectiveBatterySaver) {
                int refreshRate = GetPrimaryRefreshRate();
                g.vsyncDivisor = CalculateVSyncDivisor(refreshRate);
            }
            else {
                g.vsyncDivisor = 1; // 恢复全速
            }
            // 直接锁定并设置正确的 cfg
            EnterCriticalSection(&g_csLogic);
            if (effectiveBatterySaver) {
                g.cfg = &PRESETS[0];
            }
            else {
                g.cfg = &PRESETS[g.cfgIndex];
            }
            LeaveCriticalSection(&g_csLogic);

            // 触发一次平滑的动画重启，使程序退场后以新配置（默认渐变 or 恢复用户配置）进场
            TriggerRestartAnimation();
        }
        break;
    }
    case WM_TRAYICON:
        if (lParam == WM_RBUTTONUP) {
            ForceShowImmediate();
            g.isPaused = true;
            ShowTrayMenu(hwnd);
            g.isPaused = false;
            TimerGetDelta(true);
        }
        break;

    case WM_WTSSESSION_CHANGE:
        if (wParam == WTS_SESSION_LOCK) g.isPaused = true;
        else if (wParam == WTS_SESSION_UNLOCK) {
            g.isPaused = false;
            EnterCriticalSection(&g_csLogic);
            g.lastActiveTime = GetTickCount64();
            TimerGetDelta(true);
            LeaveCriticalSection(&g_csLogic);
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
        else if (cmdId == ID_TRAY_SUPPORT) {
            ShowSupportDialog(hwnd);
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
        // === 【新增】处理省电开关点击 ===
        else if (cmdId == ID_TRAY_POWERSAVER) {
            g.enablePowerSaver = !g.enablePowerSaver;
            SaveSettings();

            // 1. 确定要发送的电源状态
            LPARAM newPowerState = (LPARAM)FALSE;

            if (g.enablePowerSaver) {
                // 开启：执行轻量电源检测
                bool isBatOn = false;
                SYSTEM_POWER_STATUS sps;
                if (GetSystemPowerStatus(&sps)) {
                    isBatOn = (g_isWindowsSaverToggleOn || sps.ACLineStatus == 0);
                }
                if (!isBatOn) {
                    GUID* pActivePolicy = NULL;
                    if (PowerGetActiveScheme(NULL, &pActivePolicy) == ERROR_SUCCESS) {
                        if (pActivePolicy) {
                            if (IsEqualGUID(*pActivePolicy, GUID_MAX_POWER_SAVINGS))
                                isBatOn = true;
                            LocalFree(pActivePolicy);
                        }
                    }
                }
                newPowerState = (LPARAM)isBatOn;   // 统一在检测完成后赋值
            }
            else {
                // 关闭：强制发送未省电
                newPowerState = (LPARAM)FALSE;
            }

            // 2. 壁纸状态直接使用后台线程维护的全局标志
            PostMessage(g.hMsgWindow, WM_ENV_CHECK_UPDATE,
                (WPARAM)g.isThirdPartyWallpaperActive,
                newPowerState);
        }
        break;
    }
    case WM_DISPLAYCHANGE:
        // 收到系统切屏通知，打上标记让主循环去干脏活
        g.needDisplayReset = true;
        break;
    case WM_DPICHANGED:
        g.needDisplayReset = true;
        break;
    case WM_SETTINGCHANGE:
        if (wParam == SPI_SETWORKAREA) g.needDisplayReset = true;
        break;
    case WM_POWERBROADCAST:
        // PBT_POWERSETTINGCHANGE 代表具体的电源设置发生了变化
        if (wParam == PBT_POWERSETTINGCHANGE) { // 32787 即 PBT_POWERSETTINGCHANGE
            POWERBROADCAST_SETTING* pbs = (POWERBROADCAST_SETTING*)lParam;
            if (IsEqualGUID(pbs->PowerSetting, GUID_MY_POWER_SAVING_STATUS) && pbs->DataLength == sizeof(DWORD)) {
                // 精准拿到绿叶子开关的实时状态！(1 为开，0 为关)
                g_isWindowsSaverToggleOn = (*(DWORD*)(pbs->Data) != 0);
            }
        }
        break;

        // 记得在 WM_DESTROY 里注销它（防止内存泄漏）
    case WM_DESTROY:
        if (g_hPowerNotify) UnregisterPowerSettingNotification(g_hPowerNotify);
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


// --- 物理引擎实现 ---

void SolveSpring(float& current, float& velocity, float target, const SpringParams& p, float dt) {
    // 引入物理子步 (Sub-stepping)，限制单次最大步长为 16ms (约60fps的间隔)
    // 彻底解决省电模式大卡顿导致的弹簧积分爆炸问题
    const float MAX_STEP = 0.016f;

    while (dt > 0.0f) {
        float step = (dt > MAX_STEP) ? MAX_STEP : dt;

        float displacement = current - target;
        float acceleration = -p.tension * displacement - p.friction * velocity;
        velocity += acceleration * step;
        current += velocity * step;

        dt -= step;
    }

    // 浮点数安全检查
    if (_isnan(current) || !_finite(current)) current = target;
    if (_isnan(velocity) || !_finite(velocity)) velocity = 0.0f;
}

// ================================
// 纯物理计算：只更新数值，不触碰窗口
// ================================
void UpdatePhysics(float dt) {
    if (!g.hContainer) return;

    // 状态切换期间加速物理模拟
    if (g.startupState != STARTUP_NORMAL) dt *= STARTUP_SPEED_FACTOR;

    float physicsTargetY = g.targetY;
    float physicsTargetAlpha = g.targetAlpha;

    const SpringParams* pMotion = g.isHidden ? &g.cfg->motionOut : &g.cfg->motionIn;
    const SpringParams* pOpacity = g.isHidden ? &g.cfg->opacityOut : &g.cfg->opacityIn;

    if (pMotion->enabled)
        SolveSpring(g.currentY, g.velocityY, physicsTargetY, *pMotion, dt);
    else {
        g.currentY = physicsTargetY;
        g.velocityY = 0.0f;
    }

    if (pOpacity->enabled)
        SolveSpring(g.currentAlpha, g.velocityAlpha, physicsTargetAlpha, *pOpacity, dt);
    else {
        g.currentAlpha = physicsTargetAlpha;
        g.velocityAlpha = 0.0f;
    }

    if (g.currentAlpha < 0.0f)   g.currentAlpha = 0.0f;
    if (g.currentAlpha > 255.0f) g.currentAlpha = 255.0f;

    if (g.isHidden) {
        if (g.currentY > (float)g.screenH) {
            g.currentY = (float)g.screenH;
            g.velocityY = 0.0f; // 速度归零，防止回弹
        }
    }
}

// ================================
// 纯渲染提交：把物理结果刷到屏幕
// 必须在 DwmFlush() 之后调用，以精确对齐 VBlank
// ================================
void ApplyAnimation() {
    if (!g.hContainer) return;

    const SpringParams* pOpacity = g.isHidden ? &g.cfg->opacityOut : &g.cfg->opacityIn;

    int renderY = (int)g.currentY;
    int renderAlpha = (int)g.currentAlpha;

    // 仅在值发生变化时调用 WinAPI，减少开销
    if (renderY != g.lastRenderY) {
        SetWindowPos(g.hContainer, NULL, 0, renderY, 0, 0,
            SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
        if (g.hMaskWindow && IsWindow(g.hMaskWindow) && IsWindowVisible(g.hMaskWindow)) {
    int extendedH = (int)(g.screenH * 1.04f);
    int offsetY = (int)(g.screenH * 0.02f);
    
    // 计算当前下落进度 (0.0 表示完全显示，1.0 表示完全隐藏)
    float progress = g.currentY / (float)g.screenH;
    if (progress < 0.0f) progress = 0.0f;
    if (progress > 1.0f) progress = 1.0f;
    
    // 动态计算偏置：完全隐藏时，偏置量缩减为 0
    int dynamicOffsetY = (int)(offsetY * (1.0f - progress));

    SetWindowPos(g.hMaskWindow, NULL, 0, renderY - dynamicOffsetY, g.screenW, extendedH,
        SWP_NOACTIVATE | SWP_NOZORDER);
}
        g.lastRenderY = renderY;
    }

    if (renderAlpha != g.lastRenderAlpha) {
        SetLayeredWindowAttributes(g.hContainer, 0, (BYTE)renderAlpha, LWA_ALPHA);
        g.lastRenderAlpha = renderAlpha;
    }

    // 蒙版透明度联动
    if (g.hMaskWindow && IsWindow(g.hMaskWindow) && IsWindowVisible(g.hMaskWindow) && g.maxMaskAlpha > 0) {
        int maskCurrentAlpha = g.maxMaskAlpha; // 默认直接使用预设的透明度

        if (pOpacity->enabled) {
            // 如果是“渐变/滑动”模式，遮罩跟着图标的透明度一起渐变
            float ratio = g.currentAlpha / 255.0f;
            maskCurrentAlpha = (int)(g.maxMaskAlpha * ratio);
        }
        else {
            // 【修复点】：如果是“纯抽屉”模式 (pOpacity 禁用)，
            // 因为遮罩已经跟着图标层在物理坐标上进行 Y 轴位移了，
            // 随着位移它会自动滑出屏幕外，因此透明度应保持不变，拒绝二次渐变！
            maskCurrentAlpha = g.maxMaskAlpha;
        }

        if (maskCurrentAlpha != g.lastMaskAlpha) {
            SetLayeredWindowAttributes(g.hMaskWindow, 0, (BYTE)maskCurrentAlpha, LWA_ALPHA);
            g.lastMaskAlpha = maskCurrentAlpha;
        }
    }

    // 周期性维护 Z-Order
    static ULONGLONG s_lastZGuardTime = 0;
    ULONGLONG nowTick = GetTickCount64();
    if (g.startupState == STARTUP_NORMAL && (nowTick - s_lastZGuardTime > 2000)) {
        EnforceZOrder();
        s_lastZGuardTime = nowTick;
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

    // 1. 图标容器 (hContainer) 必须在最顶层
    SetWindowPos(g.hContainer, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    // 2. 遮罩 (hMaskWindow) 紧跟在图标容器后面 (InsertAfter hContainer)
    if (g.hMaskWindow && IsWindow(g.hMaskWindow)) {
        SetWindowPos(g.hMaskWindow, g.hContainer, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    // 3. 替身 (hBackdrop) 放在最底层
    if (g.hBackdrop && IsWindow(g.hBackdrop)) {
        SetWindowPos(g.hBackdrop, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
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
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        wc.lpszClassName, NULL, WS_POPUP, 0, 0, 0, 0, NULL, NULL, hInstance, NULL);
}

void AttachMaskToDesktop() {
    if (!g.hMaskWindow || !IsWindow(g.hMaskWindow)) return;
    if (!g.hContainer || !IsWindow(g.hContainer)) return;
    if (!g.hDesktopParent || !IsWindow(g.hDesktopParent)) return;

    // 【新增】已经是目标父窗口，且样式正确，就别重复搞了
    if (GetParent(g.hMaskWindow) == g.hDesktopParent) {
        LONG_PTR style = GetWindowLongPtr(g.hMaskWindow, GWL_STYLE);
        if ((style & WS_CHILD) && !(style & WS_POPUP)) {
            // 只需要更新位置即可
            int extendedH = (int)(g.screenH * 1.04f);
            int offsetY = (int)(g.screenH * 0.02f);
            SetWindowPos(g.hMaskWindow, NULL, 0, -offsetY, g.screenW, extendedH,
                SWP_NOZORDER | SWP_NOACTIVATE);
            if (!IsWindowVisible(g.hMaskWindow)) ShowWindow(g.hMaskWindow, SW_SHOWNA);
            return;
        }
    }
    SetParent(g.hMaskWindow, g.hDesktopParent);
    LONG_PTR style = GetWindowLongPtr(g.hMaskWindow, GWL_STYLE);
    style &= ~WS_POPUP; style |= WS_CHILD;
    SetWindowLongPtr(g.hMaskWindow, GWL_STYLE, style);

    int extendedH = (int)(g.screenH * 1.04f);
    int offsetY = (int)(g.screenH * 0.02f);
    SetWindowPos(g.hMaskWindow, NULL, 0, -offsetY, g.screenW, extendedH,
        SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    EnforceZOrder();
    SetLayeredWindowAttributes(g.hMaskWindow, 0, 0, LWA_ALPHA);
    ShowWindow(g.hMaskWindow, SW_SHOWNA);
}

void LocateDesktop(HINSTANCE hInstance) {
    g.hContainer = NULL;
    g.hDesktopParent = NULL;

    // 1. 【关键】强制 Windows 刷新桌面层级
    //    发送此消息后，Windows 会分离壁纸层和图标层，生成 WorkerW
    //    这是兼容 Wallpaper Engine 的基础
    HWND hProgman = FindWindow(_T("Progman"), NULL);
    SendMessageTimeout(hProgman, 0x052C, 0, 0, SMTO_NORMAL, 1000, NULL);

    // 2. 查找 SHELLDLL_DefView (图标容器)
    //    由于发送了 0x052C，它可能在 WorkerW 下，也可能在 Progman 下
    EnumWindows([](HWND hwnd, LPARAM) -> BOOL {
        if (FindWindowEx(hwnd, NULL, _T("SHELLDLL_DefView"), NULL)) {
            g.hContainer = FindWindowEx(hwnd, NULL, _T("SHELLDLL_DefView"), NULL);
            g.hDesktopParent = hwnd;
            return FALSE;
        }
        return TRUE;
        }, 0);

    // 保底查找
    if (!g.hContainer) {
        if (hProgman) g.hContainer = FindWindowEx(hProgman, NULL, _T("SHELLDLL_DefView"), NULL);
        if (g.hContainer) g.hDesktopParent = hProgman;
    }

    // 3. 初始化容器与替身窗口
    if (g.hContainer) {
        RECT rect;
        GetWindowRect(g.hContainer, &rect);
        g.screenW = rect.right - rect.left;
        g.screenH = rect.bottom - rect.top;
        if (g.screenH == 0) {
            g.screenW = GetSystemMetrics(SM_CXSCREEN);
            g.screenH = GetSystemMetrics(SM_CYSCREEN);
        }

        // 设置图标层为透明分层窗口，允许移动
        EnableLayeredStyle(g.hContainer, true);
        SetLayeredWindowAttributes(g.hContainer, 0, 255, LWA_ALPHA);

        // === 【关键】注册并创建替身窗口 ===
        static bool classRegistered = false;
        if (!classRegistered) {
            WNDCLASSEX wc = { sizeof(wc) };
            wc.lpfnWndProc = BackdropWndProc; // 使用新增的回调
            wc.hInstance = GetModuleHandle(NULL); // 修正为 GetModuleHandle
            wc.lpszClassName = _T("AutoICON_Backdrop");
            wc.hCursor = LoadCursor(NULL, IDC_ARROW);
            RegisterClassEx(&wc);
            classRegistered = true;
        }

        // 如果父窗口变了（例如壁纸软件重启），销毁重建
        if (g.hBackdrop && IsWindow(g.hBackdrop) && GetParent(g.hBackdrop) != g.hDesktopParent) {
            DestroyWindow(g.hBackdrop);
            g.hBackdrop = NULL;
        }

        if (!g.hBackdrop && g.hDesktopParent) {
            g.hBackdrop = CreateWindowEx(
                WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                _T("AutoICON_Backdrop"), NULL,
                WS_CHILD | WS_CLIPSIBLINGS, // 默认隐藏，由主循环控制显示
                0, 0, g.screenW, g.screenH,
                g.hDesktopParent, // 挂在和图标层同一个父窗口下
                NULL, GetModuleHandle(NULL), NULL
            );
            // 放到最底层 (Z-Order Bottom)
            SetWindowPos(g.hBackdrop, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }

        // 默认先初始化动画状态
        ForceShowImmediate();
        g.isHidden = true;
        g.targetY = (float)g.screenH;
        g.targetAlpha = 0.0f;
        g.startupState = STARTUP_PHASE_1_HIDING;
        g.startupPhaseStartTime = GetTickCount64();

        // 修正初始位置
        SetWindowPos(g.hContainer, NULL, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

        if (g.hMaskWindow && IsWindow(g.hMaskWindow) && g.startupState == STARTUP_NORMAL) {
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
    // 关键修改：把 HWND_MESSAGE 改成 NULL
    g.hMsgWindow = CreateWindowEx(0, wc.lpszClassName, _T(""), 0, 0, 0, 0, 0, NULL, NULL, hInstance, NULL);
    if (g.hMsgWindow) {
        g_hPowerNotify = RegisterPowerSettingNotification(g.hMsgWindow, &GUID_MY_POWER_SAVING_STATUS, DEVICE_NOTIFY_WINDOW_HANDLE);
    }
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



// 修复后的函数
// 1. 修复获取任务的函数
winrt::Windows::ApplicationModel::StartupTask GetStartupTask() {
    try {
        // 确保这里的 TaskId 与 Manifest 中的一致
        return winrt::Windows::ApplicationModel::StartupTask::GetAsync(L"AutoICONStartupTask").get();
    }
    catch (winrt::hresult_error const&) { // 去掉变量名 ex，解决 C4101 警告
        return nullptr;
    }
}

// 2. 修复状态检查函数（使用全路径限定名）
bool IsAutoStartEnabled() {
    try {
        auto task = GetStartupTask();
        if (task == nullptr) return false;

        // 使用全路径 winrt::Windows::ApplicationModel::StartupTaskState
        auto state = task.State();
        return state == winrt::Windows::ApplicationModel::StartupTaskState::Enabled ||
            state == winrt::Windows::ApplicationModel::StartupTaskState::EnabledByPolicy;
    }
    catch (...) {
        return false;
    }
}

// 3. 修复设置函数
void SetAutoStart(bool enabled) {
    try {
        auto task = GetStartupTask();
        if (task == nullptr) return;

        if (enabled) {
            task.RequestEnableAsync().get();
        }
        else {
            task.Disable();
        }
    }
    catch (...) {}
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
    }
}

bool GetSettingsFilePath(TCHAR outPath[MAX_PATH]) {
    if (!outPath) return false;

    TCHAR localAppData[MAX_PATH] = { 0 };
    TCHAR appDir[MAX_PATH] = { 0 };

    if (FAILED(SHGetFolderPath(NULL, CSIDL_LOCAL_APPDATA, NULL, SHGFP_TYPE_CURRENT, localAppData))) {
        return false;
    }

    PathCombine(appDir, localAppData, APP_NAME);

    if (!PathFileExists(appDir)) {
        int rc = SHCreateDirectoryEx(NULL, appDir, NULL);
        if (rc != ERROR_SUCCESS && rc != ERROR_ALREADY_EXISTS) {
            return false;
        }
    }

    PathCombine(outPath, appDir, SETTINGS_FILE);
    return true;
}

void SaveSettings() {
    TCHAR path[MAX_PATH] = { 0 };
    if (!GetSettingsFilePath(path)) return;

    TCHAR buf[32];

    _stprintf_s(buf, _countof(buf), _T("%d"), g.cfgIndex);
    WritePrivateProfileString(SETTINGS_SECTION, SET_VAL_PROFILE, buf, path);

    _stprintf_s(buf, _countof(buf), _T("%d"), g.maskOptIndex);
    WritePrivateProfileString(SETTINGS_SECTION, SET_VAL_MASK, buf, path);

    _stprintf_s(buf, _countof(buf), _T("%d"), g.enablePowerSaver ? 1 : 0);
    WritePrivateProfileString(SETTINGS_SECTION, SET_VAL_POWERSAVER, buf, path);

    // 刷新 INI 缓存
    WritePrivateProfileString(NULL, NULL, NULL, path);
}

bool LoadSettings() {
    g.cfgIndex = 0;
    g.maskOptIndex = 0;
    g.enablePowerSaver = true;
    g.isPowerSaverActive = false;
    bool isFirstRun = false;
    TCHAR path[MAX_PATH] = { 0 };

    if (GetSettingsFilePath(path)) {
        // 检测配置文件是否存在
        if (!PathFileExists(path)) {
            isFirstRun = true;
        }
        else {
            g.cfgIndex = (int)GetPrivateProfileInt(
                SETTINGS_SECTION,
                SET_VAL_PROFILE,
                0,
                path
            );

            g.maskOptIndex = (int)GetPrivateProfileInt(
                SETTINGS_SECTION,
                SET_VAL_MASK,
                0,
                path
            );

            g.enablePowerSaver = GetPrivateProfileInt(
                SETTINGS_SECTION,
                SET_VAL_POWERSAVER,
                1,
                path
            ) != 0;
        }
    }

    if (g.cfgIndex < 0 || g.cfgIndex >= PRESET_COUNT) {
        g.cfgIndex = 0;
    }

    if (g.maskOptIndex < 0 || g.maskOptIndex >= MASK_OPT_COUNT) {
        g.maskOptIndex = 0;
    }

    g.cfg = &PRESETS[g.cfgIndex];
    g.maxMaskAlpha = MASK_OPTIONS[g.maskOptIndex].alpha;

    return isFirstRun;
}

void ForceShowImmediate() {
    EnterCriticalSection(&g_csLogic);
    g.startupState = STARTUP_NORMAL; g.isHidden = false;
    g.lastActiveTime = GetTickCount64();
    g.targetY = 0.0f; g.targetAlpha = 255.0f;
    g.currentY = 0.0f; g.currentAlpha = 255.0f;
    g.velocityY = 0.0f; g.velocityAlpha = 0.0f;
    g.lastRenderY = -99999; g.lastRenderAlpha = -1;
    g.lastMaskAlpha = -1;
    TimerGetDelta(true); UpdatePhysics(0.0f); ApplyAnimation();
    LeaveCriticalSection(&g_csLogic);
}

void TriggerRestartAnimation() {
    EnterCriticalSection(&g_csLogic);
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
    LeaveCriticalSection(&g_csLogic);
}   

tstring GetStringResource(UINT stringID) {
    TCHAR buffer[256] = { 0 };
    LoadString(GetModuleHandle(NULL), stringID, buffer, _countof(buffer));
    return tstring(buffer);
}

// 定义 TaskDialogIndirect 的函数指针类型
typedef HRESULT(WINAPI* PFN_TASKDIALOGINDIRECT)(
    const TASKDIALOGCONFIG* pTaskConfig,
    int* pnButton,
    int* pnRadioButton,
    BOOL* pfVerificationFlag
    );

void ShowSupportDialog(HWND hwnd) {
    // -----------------------------------------------------------------
    // 在此处直接填写跳转链接
    // -----------------------------------------------------------------
    const TCHAR* URL_DOMESTIC = _T("https://encauporce.github.io/AutoICON/ShowPage/%E8%B5%9E%E5%8A%A9%E5%B1%95%E7%A4%BA%E9%A1%B5.html"); // 国内微信、支付宝赞助链接
    const TCHAR* URL_KOFI = _T("https://ko-fi.com/encauporce");                             // 海外 Ko-fi 链接
    const TCHAR* URL_STORE = _T("ms-windows-store://review/?ProductId=9N9ZCWV31X2R");             // 微软商店 ProductId (请替换为您实际的产品ID)
    // -----------------------------------------------------------------

    // 动态加载 comctl32.dll，防止启动时链接错误导致程序闪退
    HMODULE hComCtl = LoadLibrary(_T("comctl32.dll"));
    PFN_TASKDIALOGINDIRECT pfnTaskDialogIndirect = NULL;
    if (hComCtl) {
        pfnTaskDialogIndirect = (PFN_TASKDIALOGINDIRECT)GetProcAddress(hComCtl, "TaskDialogIndirect");
    }

    // 动态拉取多语言资源文本（UI显示文字不留硬编码）
    tstring title = GetStringResource(IDS_SUPPORT_TITLE);
    tstring mainInstruction = GetStringResource(IDS_SUPPORT_MAIN);
    tstring content = GetStringResource(IDS_SUPPORT_CONTENT);
    tstring btnDomesticText = GetStringResource(IDS_SUPPORT_BTN_DOMESTIC);
    tstring btnKofiText = GetStringResource(IDS_SUPPORT_BTN_KOFI);
    tstring btnReviewText = GetStringResource(IDS_SUPPORT_BTN_REVIEW);
    tstring btnCancelText = GetStringResource(IDS_SUPPORT_BTN_CANCEL);
    tstring fallbackText = GetStringResource(IDS_SUPPORT_FALLBACK_TEXT);

    bool dialogShown = false;

    // 如果动态获取函数成功，则使用现代任务对话框
    if (pfnTaskDialogIndirect) {
        INITCOMMONCONTROLSEX icex = { sizeof(icex), ICC_STANDARD_CLASSES };
        InitCommonControlsEx(&icex);

        // 定义自定义按钮 ID
        const int ID_BTN_DOMESTIC = 10001;
        const int ID_BTN_KOFI = 10002;
        const int ID_BTN_REVIEW = 10003;

        TASKDIALOGCONFIG tc = { 0 };
        tc.cbSize = sizeof(tc);
        tc.hwndParent = hwnd;
        tc.hInstance = GetModuleHandle(NULL);
        tc.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
        tc.pszWindowTitle = title.c_str();
        tc.pszMainInstruction = mainInstruction.c_str();
        tc.pszContent = content.c_str();
        tc.pszMainIcon = TD_INFORMATION_ICON;

        TASKDIALOG_BUTTON buttons[4] = {};
        buttons[0].nButtonID = ID_BTN_DOMESTIC;
        buttons[0].pszButtonText = btnDomesticText.c_str();
        buttons[1].nButtonID = ID_BTN_KOFI;
        buttons[1].pszButtonText = btnKofiText.c_str();
        buttons[2].nButtonID = ID_BTN_REVIEW;
        buttons[2].pszButtonText = btnReviewText.c_str();
        buttons[3].nButtonID = IDCANCEL;
        buttons[3].pszButtonText = btnCancelText.c_str();

        tc.pButtons = buttons;
        tc.cButtons = 4;

        int selectedButton = 0;
        HRESULT hr = pfnTaskDialogIndirect(&tc, &selectedButton, NULL, NULL);

        if (SUCCEEDED(hr)) {
            dialogShown = true;
            if (selectedButton == ID_BTN_DOMESTIC) {
                ShellExecute(NULL, _T("open"), URL_DOMESTIC, NULL, NULL, SW_SHOWNORMAL);
            }
            else if (selectedButton == ID_BTN_KOFI) {
                ShellExecute(NULL, _T("open"), URL_KOFI, NULL, NULL, SW_SHOWNORMAL);
            }
            else if (selectedButton == ID_BTN_REVIEW) {
                ShellExecute(NULL, _T("open"), URL_STORE, NULL, NULL, SW_SHOWNORMAL);
            }
        }
    }

    // 释放加载的 DLL 资源
    if (hComCtl) {
        FreeLibrary(hComCtl);
    }

    // 如果动态加载失败或不支持，执行优雅降级（MessageBox 保底）
    if (!dialogShown) {
        if (!fallbackText.empty() && !title.empty()) {
            MessageBox(hwnd, fallbackText.c_str(), title.c_str(), MB_OK | MB_ICONINFORMATION);
        }
    }
}