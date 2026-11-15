// ==WindhawkMod==
// @id              lock-screen-clock-styler-fork
// @name            Hide Default Lock Screen Clock
// @name:zh-CN      隐藏默认锁屏时钟
// @description     Hides the default Windows 11 lock screen clock and date text.
// @description:zh-CN 隐藏默认的 Windows 11 锁屏时钟和日期文本。
// @version         1.0.0
// @author          ali0zdas
// @license         MIT
// @include         LockApp.exe
// @architecture    x86-64
// @compilerOptions -lole32 -loleaut32 -lruntimeobject -lpowrprof
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*...*/
// ==/WindhawkModReadme==

#include <windows.h>
#undef GetCurrentTime
#include <powrprof.h>
#include <windhawk_api.h>
#include <windhawk_utils.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace wux = winrt::Windows::UI::Xaml;
namespace wuxc = winrt::Windows::UI::Xaml::Controls;
namespace wuxm = winrt::Windows::UI::Xaml::Media;
namespace wu = winrt::Windows::UI;

// Elements whose Visibility this mod changed. The original local value is kept
// so it can be restored without overwriting Windows' own bindings/state.
struct TrackedElement {
    wux::FrameworkElement element{ nullptr };
    winrt::Windows::Foundation::IInspectable originalValue{ nullptr };
    bool hadLocalValue = false;
};

// no_destroy: releasing these vectors in a CRT destructor at process exit would
// release XAML objects off the UI thread. They are cleared on the UI thread.
[[clang::no_destroy]] std::vector<TrackedElement> g_hiddenClockElements;

[[clang::no_destroy]] wuxc::Grid g_lockRootGrid{ nullptr };

// ---------------------------------------------------------------------------
// Power state tracking (skip work when the display is off / system suspended)
// ---------------------------------------------------------------------------

#ifndef DEVICE_NOTIFY_CALLBACK
#define DEVICE_NOTIFY_CALLBACK 2
#endif

#ifndef PBT_APMSUSPEND
#define PBT_APMSUSPEND 0x0004
#endif

#ifndef PBT_APMRESUMEAUTOMATIC
#define PBT_APMRESUMEAUTOMATIC 0x0012
#endif

#ifndef PBT_APMRESUMESUSPEND
#define PBT_APMRESUMESUSPEND 0x0007
#endif

#ifndef PBT_POWERSETTINGCHANGE
#define PBT_POWERSETTINGCHANGE 0x8013
#endif

typedef ULONG(CALLBACK* DEVICE_NOTIFY_CALLBACK_ROUTINE)(PVOID Context,
                                                        ULONG Type,
                                                        PVOID Setting);

typedef struct _DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS {
    DEVICE_NOTIFY_CALLBACK_ROUTINE Callback;
    PVOID Context;
} DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS, *PDEVICE_NOTIFY_SUBSCRIBE_PARAMETERS;

std::atomic<bool> g_isDisplayOn{ true };
std::atomic<bool> g_isSystemAwake{ true };
HPOWERNOTIFY g_hDisplayNotify = nullptr;
HPOWERNOTIFY g_hSuspendResumeNotify = nullptr;

static const GUID GUID_CONSOLE_DISPLAY_STATE_LOCAL = {
    0x6fe69556, 0x704a, 0x47a0,
    {0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47}};

ULONG CALLBACK SuspendResumeCallback(PVOID /*context*/,
                                     ULONG type,
                                     PVOID /*setting*/) {
    if (type == PBT_APMSUSPEND) {
        Wh_Log(L"Power notification: System suspending (PBT_APMSUSPEND).");
        g_isSystemAwake.store(false);
    } else if (type == PBT_APMRESUMEAUTOMATIC || type == PBT_APMRESUMESUSPEND) {
        Wh_Log(L"Power notification: System resumed (type=0x%x).", type);
        g_isSystemAwake.store(true);
    }
    return 0;
}

ULONG CALLBACK DisplaySettingCallback(PVOID /*context*/,
                                      ULONG type,
                                      PVOID setting) {
    if (type == PBT_POWERSETTINGCHANGE && setting) {
        auto* pbs = reinterpret_cast<const POWERBROADCAST_SETTING*>(setting);
        if (pbs->DataLength >= sizeof(DWORD)) {
            DWORD state = *reinterpret_cast<const DWORD*>(pbs->Data);
            if (state == 0) {
                Wh_Log(L"Display notification: Console display is OFF.");
                g_isDisplayOn.store(false);
            } else {
                Wh_Log(L"Display notification: Console display is ON (state=%u).",
                       state);
                g_isDisplayOn.store(true);
            }
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Run-on-UI-thread helper (used for synchronous teardown / rebuild)
// ---------------------------------------------------------------------------

UINT g_runOnUiThreadMessage = 0;

using RunOnUiThreadProc = void(WINAPI*)(PVOID);

struct RunOnUiThreadParam {
    RunOnUiThreadProc proc;
    PVOID param;
    bool ran;
};

// WH_CALLWNDPROC hook used by RunOnUiThread(). It runs on the target UI thread
// before the window procedure sees the message.
LRESULT CALLBACK RunOnUiThreadHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        const CWPSTRUCT* cwp = reinterpret_cast<const CWPSTRUCT*>(lParam);
        if (cwp && cwp->message == g_runOnUiThreadMessage) {
            auto* p = reinterpret_cast<RunOnUiThreadParam*>(cwp->lParam);
            if (p && p->proc) {
                try {
                    p->proc(p->param);
                    p->ran = true;
                } catch (winrt::hresult_error const& e) {
                    Wh_Log(L"UI task failed: %s", e.message().c_str());
                } catch (...) {
                    Wh_Log(L"UI task failed with an unknown error.");
                }
            }
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

// Runs proc on the thread that owns hWnd and returns only after it has
// finished. Mixing this with queued dispatcher callbacks is not allowed: the
// synchronous path is the only way to guarantee that no mod code is left
// behind when the DLL is unloaded.
bool RunOnUiThread(HWND hWnd, RunOnUiThreadProc proc, PVOID param) {
    if (!hWnd || !proc) {
        return false;
    }

    DWORD threadId = GetWindowThreadProcessId(hWnd, nullptr);
    if (!threadId) {
        return false;
    }

    if (threadId == GetCurrentThreadId()) {
        proc(param);
        return true;
    }

    static std::mutex s_runMutex;
    std::lock_guard<std::mutex> lock(s_runMutex);

    if (!g_runOnUiThreadMessage) {
        return false;
    }

    HHOOK hook = SetWindowsHookExW(WH_CALLWNDPROC, RunOnUiThreadHookProc, nullptr, threadId);
    if (!hook) {
        Wh_Log(L"SetWindowsHookEx(WH_CALLWNDPROC) failed: %u", GetLastError());
        return false;
    }

    RunOnUiThreadParam p{ proc, param, false };
    SendMessageW(hWnd, g_runOnUiThreadMessage, 0, reinterpret_cast<LPARAM>(&p));
    UnhookWindowsHookEx(hook);
    return p.ran;
}

// ---------------------------------------------------------------------------
// Visual tree helpers
// ---------------------------------------------------------------------------

// Restores the local Visibility value that existed before this mod hid an
// element. ClearValue() is used when the element had no local value, which
// keeps Windows' own bindings and visual states intact.
void RestoreTrackedElements(std::vector<TrackedElement>& tracked) {
    auto prop = wux::UIElement::VisibilityProperty();
    for (auto& trackedElement : tracked) {
        if (!trackedElement.element) {
            continue;
        }
        try {
            if (trackedElement.hadLocalValue) {
                trackedElement.element.SetValue(prop, trackedElement.originalValue);
            } else {
                trackedElement.element.ClearValue(prop);
            }
        } catch (...) {
            try {
                trackedElement.element.ClearValue(prop);
            } catch (...) {}
        }
    }
    tracked.clear();
    tracked.shrink_to_fit();
}

void RestoreDefaultElements() {
    RestoreTrackedElements(g_hiddenClockElements);
}

// Keeps the tracking list in sync with the current tree and hides only the
// elements that are actually supposed to be hidden.
void SyncTrackedElements(std::vector<TrackedElement>& tracked,
                         std::vector<wux::FrameworkElement> const& found,
                         bool shouldHide) {
    tracked.erase(
        std::remove_if(tracked.begin(), tracked.end(),
                       [&found](TrackedElement const& trackedElement) {
                           if (!trackedElement.element) {
                               return true;
                           }
                           return std::none_of(
                               found.begin(), found.end(),
                               [&trackedElement](wux::FrameworkElement const& element) {
                                   return element == trackedElement.element;
                               });
                       }),
        tracked.end());

    if (!shouldHide) {
        RestoreTrackedElements(tracked);
        return;
    }

    auto prop = wux::UIElement::VisibilityProperty();
    for (auto const& element : found) {
        if (!element) {
            continue;
        }

        bool alreadyTracked = std::any_of(
            tracked.begin(), tracked.end(),
            [&element](TrackedElement const& trackedElement) {
                return trackedElement.element == element;
            });
        if (alreadyTracked) {
            continue;
        }

        TrackedElement trackedElement;
        trackedElement.element = element;
        try {
            auto localValue = element.ReadLocalValue(prop);
            trackedElement.hadLocalValue =
                !(localValue == wux::DependencyProperty::UnsetValue());
            if (trackedElement.hadLocalValue) {
                trackedElement.originalValue = localValue;
            }
        } catch (...) {
            trackedElement.hadLocalValue = false;
        }

        tracked.push_back(trackedElement);
        try {
            element.Visibility(wux::Visibility::Collapsed);
        } catch (...) {}
    }

    // Re-assert the value on every scan. Local values normally win over visual
    // states, but re-applying is cheap and keeps the element hidden if Windows
    // replaces the tree or resets the property.
    for (auto& trackedElement : tracked) {
        try {
            trackedElement.element.Visibility(wux::Visibility::Collapsed);
        } catch (...) {}
    }
}

struct ScanResult {
    wuxc::Grid root{ nullptr };
    std::vector<wux::FrameworkElement> clockElements;
};

void ScanNode(wux::DependencyObject const& node, ScanResult& result, int depth) {
    if (!node || depth > 96) {
        return;
    }

    if (auto element = node.try_as<wux::FrameworkElement>()) {
        auto name = element.Name();

        if (name == L"LockRootGrid") {
            if (!result.root) {
                if (auto grid = element.try_as<wuxc::Grid>()) {
                    result.root = grid;
                }
            }
        } else if (name == L"TimeAndDatePanel" || name == L"Time" || name == L"Date") {
            result.clockElements.push_back(element);
        }
    }

    int childCount = 0;
    try {
        childCount = wuxm::VisualTreeHelper::GetChildrenCount(node);
    } catch (...) {
        return;
    }
    for (int i = 0; i < childCount; i++) {
        try {
            ScanNode(wuxm::VisualTreeHelper::GetChild(node, i), result, depth + 1);
        } catch (...) {}
    }
}

// Finds the lock screen root and the stock clock elements, then hides them.
// Must run on the lock screen UI thread.
void AttachToTree() {
    if (!g_isSystemAwake.load() || !g_isDisplayOn.load()) {
        return;
    }

    wux::Window currentWindow{nullptr};
    try {
        currentWindow = wux::Window::Current();
        if (!currentWindow || !currentWindow.Visible()) {
            return;
        }
    } catch (...) {
        return;
    }

    wux::UIElement content{nullptr};
    try {
        content = currentWindow.Content();
    } catch (...) {
        return;
    }
    if (!content) {
        return;
    }

    ScanResult scan;
    ScanNode(content, scan, 0);
    if (!scan.root) {
        // The tree is still being built; the next tick will retry.
        return;
    }

    bool sameRoot = (g_lockRootGrid == scan.root);
    if (!sameRoot) {
        RestoreDefaultElements();
        g_lockRootGrid = scan.root;
        Wh_Log(L"Attached to the lock screen visual tree.");
    }

    SyncTrackedElements(g_hiddenClockElements, scan.clockElements, true);
}

// Re-applies hiding on the UI thread.
void WINAPI RebuildOnUiThread(PVOID) {
    RestoreDefaultElements();
    g_lockRootGrid = nullptr;
    AttachToTree();
    Wh_Log(L"Clock hiding re-applied on the lock screen UI thread.");
}

// ---------------------------------------------------------------------------
// Periodic UI work
// ---------------------------------------------------------------------------

std::atomic<HWND> g_coreWindow{ nullptr };
std::atomic<HHOOK> g_getMessageHook{ nullptr };
HANDLE g_stopEvent = nullptr;
HANDLE g_workerThread = nullptr;

ULONGLONG g_lastUiTick = 0;
ULONGLONG g_lastFullScan = 0;
std::atomic<bool> g_settingsDirty{ false };

void OnUiThreadTick() {
    if (!g_isSystemAwake.load() || !g_isDisplayOn.load()) {
        return;
    }

    ULONGLONG now = GetTickCount64();
    if ((now - g_lastUiTick) < 1000) {
        return;
    }

    HWND coreWnd = g_coreWindow.load();
    if (!coreWnd || !IsWindow(coreWnd) || !IsWindowVisible(coreWnd)) {
        return;
    }

    try {
        auto currentWindow = wux::Window::Current();
        if (!currentWindow || !currentWindow.Visible()) {
            return;
        }
    } catch (...) {
        return;
    }

    g_lastUiTick = now;

    if (g_settingsDirty.exchange(false)) {
        try {
            RebuildOnUiThread(nullptr);
        } catch (...) {
            g_settingsDirty = true;
            throw;
        }
        return;
    }

    bool needScan = (!g_lockRootGrid || (now - g_lastFullScan) >= 3000);
    if (needScan) {
        g_lastFullScan = now;
        AttachToTree();
    }
}

LRESULT CALLBACK GetMessageHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        try {
            OnUiThreadTick();
        } catch (winrt::hresult_error const& e) {
            Wh_Log(L"UI tick failed: %s", e.message().c_str());
        } catch (...) {
            Wh_Log(L"UI tick failed with an unknown error.");
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Teardown and worker thread
// ---------------------------------------------------------------------------

void WINAPI TeardownOnUiThread(PVOID) {
    // Unhook from the UI thread: no WH_GETMESSAGE callback can be executing at
    // the same time on this thread, so it is safe for the DLL to unload next.
    HHOOK hook = g_getMessageHook.exchange(nullptr);
    if (hook) {
        UnhookWindowsHookEx(hook);
    }

    RestoreDefaultElements();
    g_lockRootGrid = nullptr;
    Wh_Log(L"UI teardown complete.");
}

BOOL CALLBACK FindCoreWindowProc(HWND hWnd, LPARAM lParam) {
    DWORD processId = 0;
    GetWindowThreadProcessId(hWnd, &processId);
    if (processId != GetCurrentProcessId()) {
        return TRUE;
    }

    WCHAR className[128];
    if (GetClassNameW(hWnd, className, ARRAYSIZE(className)) > 0 &&
        _wcsicmp(className, L"Windows.UI.Core.CoreWindow") == 0) {
        *reinterpret_cast<HWND*>(lParam) = hWnd;
        return FALSE;
    }
    return TRUE;
}

HWND FindCoreWindow() {
    HWND found = nullptr;
    EnumWindows(FindCoreWindowProc, reinterpret_cast<LPARAM>(&found));
    return found;
}

DWORD WINAPI WorkerThreadProc(LPVOID) {
    // Wait for LockApp's CoreWindow, then install the message hook on its
    // thread. Retry until the window exists or the mod is unloaded.
    while (WaitForSingleObject(g_stopEvent, 250) == WAIT_TIMEOUT) {
        if (g_getMessageHook.load()) {
            break;
        }

        if (!g_isSystemAwake.load() || !g_isDisplayOn.load()) {
            continue;
        }

        HWND window = FindCoreWindow();
        if (!window || !IsWindowVisible(window)) {
            continue;
        }

        DWORD threadId = GetWindowThreadProcessId(window, nullptr);
        HHOOK hook = SetWindowsHookExW(WH_GETMESSAGE, GetMessageHookProc, nullptr, threadId);
        if (hook) {
            g_coreWindow = window;
            g_getMessageHook = hook;
            Wh_Log(L"Installed WH_GETMESSAGE hook on UI thread %u (hwnd=%p).",
                   threadId, window);
            break;
        }
        Wh_Log(L"SetWindowsHookEx(WH_GETMESSAGE) failed: %u", GetLastError());
    }

    // Wake the UI thread periodically so the hook re-scans the tree.
    while (WaitForSingleObject(g_stopEvent, 1000) == WAIT_TIMEOUT) {
        if (!g_isSystemAwake.load() || !g_isDisplayOn.load()) {
            continue;
        }

        HWND window = g_coreWindow.load();
        if (!window) {
            continue;
        }
        if (!IsWindow(window)) {
            Wh_Log(L"The lock screen window disappeared; stopping the worker.");
            break;
        }
        if (!IsWindowVisible(window)) {
            continue;
        }
        PostMessageW(window, WM_NULL, 0, 0);
    }

    return 0;
}

// ---------------------------------------------------------------------------
// Windhawk lifecycle
// ---------------------------------------------------------------------------

BOOL Wh_ModInit() {
    Wh_Log(L"Hide Default Lock Screen Clock initializing (Wh_ModInit)...");

    g_runOnUiThreadMessage =
        RegisterWindowMessageW(L"Windhawk_HideDefaultClock_RunOnUiThread_v1");
    if (!g_runOnUiThreadMessage) {
        Wh_Log(L"RegisterWindowMessageW failed: %u", GetLastError());
        return FALSE;
    }

    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_stopEvent) {
        Wh_Log(L"CreateEventW failed: %u", GetLastError());
        return FALSE;
    }

    g_coreWindow = nullptr;
    g_getMessageHook = nullptr;
    g_settingsDirty = false;
    g_isDisplayOn = true;
    g_isSystemAwake = true;

    // Register for console display state (on/off) notifications
    DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS displayParams = {DisplaySettingCallback, nullptr};
    DWORD dispRes = PowerSettingRegisterNotification(
        &GUID_CONSOLE_DISPLAY_STATE_LOCAL,
        DEVICE_NOTIFY_CALLBACK,
        reinterpret_cast<HANDLE>(&displayParams),
        &g_hDisplayNotify);
    if (dispRes != ERROR_SUCCESS) {
        Wh_Log(L"PowerSettingRegisterNotification failed: %u", dispRes);
        g_hDisplayNotify = nullptr;
    }

    // Register for system suspend / resume notifications
    DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS suspendParams = {SuspendResumeCallback, nullptr};
    g_hSuspendResumeNotify = RegisterSuspendResumeNotification(
        reinterpret_cast<HANDLE>(&suspendParams),
        DEVICE_NOTIFY_CALLBACK);
    if (!g_hSuspendResumeNotify) {
        Wh_Log(L"RegisterSuspendResumeNotification failed: %u", GetLastError());
        g_hSuspendResumeNotify = nullptr;
    }

    g_workerThread = CreateThread(nullptr, 0, WorkerThreadProc, nullptr, 0, nullptr);
    if (!g_workerThread) {
        Wh_Log(L"CreateThread failed: %u", GetLastError());
        if (g_hDisplayNotify) {
            PowerSettingUnregisterNotification(g_hDisplayNotify);
            g_hDisplayNotify = nullptr;
        }
        if (g_hSuspendResumeNotify) {
            UnregisterSuspendResumeNotification(g_hSuspendResumeNotify);
            g_hSuspendResumeNotify = nullptr;
        }
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
        return FALSE;
    }

    return TRUE;
}

void Wh_ModUninit() {
    Wh_Log(L"Hide Default Lock Screen Clock uninitializing (Wh_ModUninit)...");

    if (g_hDisplayNotify) {
        PowerSettingUnregisterNotification(g_hDisplayNotify);
        g_hDisplayNotify = nullptr;
    }
    if (g_hSuspendResumeNotify) {
        UnregisterSuspendResumeNotification(g_hSuspendResumeNotify);
        g_hSuspendResumeNotify = nullptr;
    }

    if (g_stopEvent) {
        SetEvent(g_stopEvent);
    }
    if (g_workerThread) {
        WaitForSingleObject(g_workerThread, INFINITE);
        CloseHandle(g_workerThread);
        g_workerThread = nullptr;
    }

    HWND window = g_coreWindow.load();
    HHOOK hook = g_getMessageHook.load();
    if (window && hook && IsWindow(window)) {
        // Synchronous. When this returns, TeardownOnUiThread has finished and
        // the WH_GETMESSAGE hook has been removed from the UI thread.
        if (!RunOnUiThread(window, TeardownOnUiThread, nullptr)) {
            Wh_Log(L"Failed to run teardown on the UI thread.");
            HHOOK remainingHook = g_getMessageHook.exchange(nullptr);
            if (remainingHook) {
                UnhookWindowsHookEx(remainingHook);
            }
        }
    } else {
        HHOOK remainingHook = g_getMessageHook.exchange(nullptr);
        if (remainingHook) {
            UnhookWindowsHookEx(remainingHook);
        }
    }

    g_coreWindow = nullptr;
    if (g_stopEvent) {
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
    }

    Wh_Log(L"Hide Default Lock Screen Clock uninitialized.");
}

void Wh_ModSettingsChanged() {
    Wh_Log(L"Mod settings changed (Wh_ModSettingsChanged)...");

    HWND window = g_coreWindow.load();
    HHOOK hook = g_getMessageHook.load();
    if (hook) {
        if (window && IsWindow(window) &&
            RunOnUiThread(window, RebuildOnUiThread, nullptr)) {
            return;
        }
        Wh_Log(L"Deferring settings change to the UI thread.");
        g_settingsDirty = true;
    } else {
        g_settingsDirty = true;
    }
}