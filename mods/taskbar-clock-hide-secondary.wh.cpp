// ==WindhawkMod==
// @id              taskbar-clock-hide-secondary
// @name            Hide taskbar clock on secondary monitors
// @description     Hides the Windows 11 taskbar clock on secondary monitors (or only on the primary one)
// @version         1.0
// @author          denis-zak
// @github          https://github.com/denis-zak
// @include         explorer.exe
// @architecture    x86-64
// @license         GPL-3.0
// @compilerOptions -lole32 -loleaut32 -lruntimeobject -lversion
// ==/WindhawkMod==

// Parts of this code (taskbar XamlRoot lookup, system tray module detection,
// RunFromWindowThread) are based on m417z's "Taskbar tray system icon tweaks"
// and "Taskbar Clock Customization" mods, published under GPL-3.0.

// ==WindhawkModReadme==
/*
# Hide taskbar clock on secondary monitors

Windows 11 shows the clock on every taskbar and has no option to hide it on
only some of them. This mod hides the clock (date and time) on the taskbars of
secondary monitors, while keeping it on the primary monitor. The notification
bell next to the clock stays where it is.

The setting can be switched to hide the clock only on the primary monitor, or
on all taskbars.

Only the Windows 11 taskbar is supported (not the Windows 10 taskbar restored
by ExplorerPatcher or similar tools).
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- hideOn: secondary
  $name: Hide the clock on
  $options:
  - secondary: Secondary monitors
  - primary: Primary monitor
  - all: All monitors
*/
// ==/WindhawkModSettings==

#include <windhawk_utils.h>

#include <atomic>
#include <chrono>
#include <vector>

#undef GetCurrentTime

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/base.h>

using namespace winrt::Windows::UI::Xaml;

// ---------------------------------------------------------------------------
// Settings and state
// ---------------------------------------------------------------------------

enum class HideOn {
    secondary,
    primary,
    all,
};

struct {
    HideOn hideOn;
} g_settings;

enum class TaskbarKind {
    unknown,
    primary,
    secondary,
};

constexpr PCWSTR kClockClassName = L"SystemTray.DateTimeIconContent";
constexpr PCWSTR kIconViewClassName = L"SystemTray.IconView";

// How long to keep trying to find out which taskbar a new clock belongs to
// (e.g. while Explorer is still starting): 50 x 200 ms = 10 seconds.
constexpr int kMaxClassifyAttempts = 50;
constexpr auto kRetryInterval = std::chrono::milliseconds(200);

std::atomic<bool> g_systemTrayModuleHooked;
std::atomic<bool> g_unloading;

// All of the state below is only accessed from the taskbar UI thread (all
// taskbars, including secondary ones, live on the primary taskbar's thread).
struct ClockState {
    winrt::weak_ref<FrameworkElement> clock;
    winrt::weak_ref<FrameworkElement> hiddenIconView;
    winrt::event_token loadedToken{};
    winrt::event_token unloadedToken{};
    bool clockHidden = false;
    int pendingAttempts = 0;
};

std::vector<ClockState> g_clockStates;

DispatcherTimer g_retryTimer{nullptr};
winrt::event_token g_retryTimerTickToken{};

// ---------------------------------------------------------------------------
// Taskbar XamlRoot lookup (taskbar.dll internals)
// ---------------------------------------------------------------------------

void* CTaskBand_ITaskListWndSite_vftable;
void* CSecondaryTaskBand_ITaskListWndSite_vftable;

using CTaskBand_GetTaskbarHost_t = void*(WINAPI*)(void* pThis, void** result);
CTaskBand_GetTaskbarHost_t CTaskBand_GetTaskbarHost_Original;

void* TaskbarHost_FrameHeight_Original;

using CSecondaryTaskBand_GetTaskbarHost_t = void*(WINAPI*)(void* pThis,
                                                           void** result);
CSecondaryTaskBand_GetTaskbarHost_t CSecondaryTaskBand_GetTaskbarHost_Original;

using std__Ref_count_base__Decref_t = void(WINAPI*)(void* pThis);
std__Ref_count_base__Decref_t std__Ref_count_base__Decref_Original;

HWND FindCurrentProcessTaskbarWnd() {
    HWND hTaskbarWnd = nullptr;

    EnumWindows(
        [](HWND hWnd, LPARAM lParam) -> BOOL {
            DWORD dwProcessId;
            WCHAR className[32];
            if (GetWindowThreadProcessId(hWnd, &dwProcessId) &&
                dwProcessId == GetCurrentProcessId() &&
                GetClassName(hWnd, className, ARRAYSIZE(className)) &&
                _wcsicmp(className, L"Shell_TrayWnd") == 0) {
                *reinterpret_cast<HWND*>(lParam) = hWnd;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&hTaskbarWnd));

    return hTaskbarWnd;
}

XamlRoot XamlRootFromTaskbarHostSharedPtr(void* taskbarHostSharedPtr[2]) {
    if (!taskbarHostSharedPtr[0] && !taskbarHostSharedPtr[1]) {
        return nullptr;
    }

    size_t taskbarElementIUnknownOffset = 0x10;

#if defined(_M_X64)
    {
        // 48:83EC 28 | sub rsp,28
        // 48:83C1 48 | add rcx,48
        const BYTE* b = (const BYTE*)TaskbarHost_FrameHeight_Original;
        if (b[0] == 0x48 && b[1] == 0x83 && b[2] == 0xEC && b[4] == 0x48 &&
            b[5] == 0x83 && b[6] == 0xC1 && b[7] <= 0x7F) {
            taskbarElementIUnknownOffset = b[7];
        } else {
            Wh_Log(L"Unsupported TaskbarHost::FrameHeight");
        }
    }
#elif defined(_M_ARM64)
    {
        // 7f2303d5 pacibsp
        // fd7bbfa9 stp     fp, lr, [sp, #-0x10]!
        // fd030091 mov     fp, sp
        // 080c41f8 ldr     x8, [x0, #0x10]!
        const DWORD* p = (const DWORD*)TaskbarHost_FrameHeight_Original;
        if (p[0] == 0xD503237F && (p[1] & 0xFFC07FFF) == 0xA9807BFD &&
            p[2] == 0x910003FD && (p[3] & 0xFFF00FE0) == 0xF8400C00) {
            taskbarElementIUnknownOffset = (p[3] >> 12) & 0xFF;
        } else {
            Wh_Log(L"Unsupported TaskbarHost::FrameHeight");
        }
    }
#else
#error "Unsupported architecture"
#endif

    auto* taskbarElementIUnknown =
        *(IUnknown**)((BYTE*)taskbarHostSharedPtr[0] +
                      taskbarElementIUnknownOffset);

    FrameworkElement taskbarElement = nullptr;
    taskbarElementIUnknown->QueryInterface(winrt::guid_of<FrameworkElement>(),
                                           winrt::put_abi(taskbarElement));

    auto result = taskbarElement ? taskbarElement.XamlRoot() : nullptr;

    std__Ref_count_base__Decref_Original(taskbarHostSharedPtr[1]);

    return result;
}

XamlRoot GetTaskbarXamlRoot(HWND hTaskbarWnd) {
    HWND hTaskSwWnd = (HWND)GetProp(hTaskbarWnd, L"TaskbandHWND");
    if (!hTaskSwWnd) {
        return nullptr;
    }

    void* taskBand = (void*)GetWindowLongPtr(hTaskSwWnd, 0);
    if (!taskBand) {
        return nullptr;
    }

    void* taskBandForTaskListWndSite = taskBand;
    for (int i = 0; *(void**)taskBandForTaskListWndSite !=
                    CTaskBand_ITaskListWndSite_vftable;
         i++) {
        if (i == 20) {
            return nullptr;
        }

        taskBandForTaskListWndSite = (void**)taskBandForTaskListWndSite + 1;
    }

    void* taskbarHostSharedPtr[2]{};
    CTaskBand_GetTaskbarHost_Original(taskBandForTaskListWndSite,
                                      taskbarHostSharedPtr);

    return XamlRootFromTaskbarHostSharedPtr(taskbarHostSharedPtr);
}

XamlRoot GetSecondaryTaskbarXamlRoot(HWND hSecondaryTaskbarWnd) {
    HWND hTaskSwWnd =
        (HWND)FindWindowEx(hSecondaryTaskbarWnd, nullptr, L"WorkerW", nullptr);
    if (!hTaskSwWnd) {
        return nullptr;
    }

    void* taskBand = (void*)GetWindowLongPtr(hTaskSwWnd, 0);
    if (!taskBand) {
        return nullptr;
    }

    void* taskBandForTaskListWndSite = taskBand;
    for (int i = 0; *(void**)taskBandForTaskListWndSite !=
                    CSecondaryTaskBand_ITaskListWndSite_vftable;
         i++) {
        if (i == 20) {
            return nullptr;
        }

        taskBandForTaskListWndSite = (void**)taskBandForTaskListWndSite + 1;
    }

    void* taskbarHostSharedPtr[2]{};
    CSecondaryTaskBand_GetTaskbarHost_Original(taskBandForTaskListWndSite,
                                               taskbarHostSharedPtr);

    return XamlRootFromTaskbarHostSharedPtr(taskbarHostSharedPtr);
}

// Returns the XamlRoot of a taskbar window, and which kind of taskbar it is.
// Returns nullptr for windows which aren't taskbars.
XamlRoot GetXamlRootForTaskbarWindow(HWND hWnd, TaskbarKind* kind) {
    WCHAR className[32];
    if (!GetClassName(hWnd, className, ARRAYSIZE(className))) {
        return nullptr;
    }

    if (_wcsicmp(className, L"Shell_TrayWnd") == 0) {
        *kind = TaskbarKind::primary;
        return GetTaskbarXamlRoot(hWnd);
    }

    if (_wcsicmp(className, L"Shell_SecondaryTrayWnd") == 0) {
        *kind = TaskbarKind::secondary;
        return GetSecondaryTaskbarXamlRoot(hWnd);
    }

    return nullptr;
}

// Must be called from the taskbar UI thread.
TaskbarKind GetTaskbarKind(XamlRoot const& xamlRoot) {
    struct Context {
        XamlRoot xamlRoot{nullptr};
        TaskbarKind kind = TaskbarKind::unknown;
    } context;
    context.xamlRoot = xamlRoot;

    EnumThreadWindows(
        GetCurrentThreadId(),
        [](HWND hWnd, LPARAM lParam) -> BOOL {
            auto& context = *reinterpret_cast<Context*>(lParam);
            try {
                TaskbarKind kind = TaskbarKind::unknown;
                XamlRoot taskbarXamlRoot =
                    GetXamlRootForTaskbarWindow(hWnd, &kind);
                if (taskbarXamlRoot && taskbarXamlRoot == context.xamlRoot) {
                    context.kind = kind;
                    return FALSE;
                }
            } catch (...) {
                Wh_Log(L"Error %08X", winrt::to_hresult());
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&context));

    return context.kind;
}

// ---------------------------------------------------------------------------
// Hiding and showing the clock
// ---------------------------------------------------------------------------

bool ShouldHide(TaskbarKind kind) {
    switch (g_settings.hideOn) {
        case HideOn::secondary:
            return kind == TaskbarKind::secondary;
        case HideOn::primary:
            return kind == TaskbarKind::primary;
        case HideOn::all:
            return true;
    }
    return false;
}

// The clock content is hosted in a SystemTray.IconView ("SystemTrayIcon").
// Collapsing it too removes the empty space left by the clock.
FrameworkElement FindClockIconView(FrameworkElement const& clock) {
    FrameworkElement current = clock;
    for (int i = 0; i < 8; i++) {
        current = Media::VisualTreeHelper::GetParent(current)
                      .try_as<FrameworkElement>();
        if (!current) {
            return nullptr;
        }

        if (winrt::get_class_name(current) == kIconViewClassName) {
            return current;
        }
    }

    return nullptr;
}

void RestoreIconView(ClockState& state) {
    if (auto iconView = state.hiddenIconView.get()) {
        iconView.Visibility(Visibility::Visible);
    }
    state.hiddenIconView = nullptr;
}

void SetClockHidden(ClockState& state,
                    FrameworkElement const& clock,
                    bool hide) {
    if (hide) {
        clock.Visibility(Visibility::Collapsed);
        state.clockHidden = true;

        if (auto iconView = FindClockIconView(clock)) {
            iconView.Visibility(Visibility::Collapsed);
            state.hiddenIconView = winrt::make_weak(iconView);
        }
    } else {
        if (state.clockHidden) {
            clock.Visibility(Visibility::Visible);
            state.clockHidden = false;
        }

        RestoreIconView(state);
    }
}

// Returns true if the clock was handled (or no longer needs handling), false
// if the taskbar couldn't be identified yet and it should be retried.
bool ProcessClockState(ClockState& state) {
    auto clock = state.clock.get();
    if (!clock) {
        return true;
    }

    if (g_unloading) {
        SetClockHidden(state, clock, false);
        return true;
    }

    // Not in the tree right now, the Loaded handler will process it again.
    if (!clock.IsLoaded()) {
        return true;
    }

    TaskbarKind kind = TaskbarKind::unknown;
    if (g_settings.hideOn != HideOn::all) {
        auto xamlRoot = clock.XamlRoot();
        if (xamlRoot) {
            kind = GetTaskbarKind(xamlRoot);
        }

        if (kind == TaskbarKind::unknown) {
            return false;
        }
    }

    bool hide = ShouldHide(kind);
    Wh_Log(L"Clock on %s taskbar, hide=%d",
           kind == TaskbarKind::primary     ? L"primary"
           : kind == TaskbarKind::secondary ? L"secondary"
                                            : L"any",
           hide);

    SetClockHidden(state, clock, hide);
    return true;
}

ClockState* FindClockState(FrameworkElement const& clock) {
    for (auto& state : g_clockStates) {
        if (state.clock.get() == clock) {
            return &state;
        }
    }
    return nullptr;
}

void PruneDeadClockStates() {
    for (auto& state : g_clockStates) {
        if (!state.clock.get()) {
            RestoreIconView(state);
        }
    }

    std::erase_if(g_clockStates,
                  [](ClockState const& state) { return !state.clock.get(); });
}

void OnRetryTimerTick() {
    if (g_unloading) {
        return;
    }

    bool anyPending = false;

    for (auto& state : g_clockStates) {
        if (state.pendingAttempts <= 0) {
            continue;
        }

        bool done = true;
        try {
            done = ProcessClockState(state);
        } catch (...) {
            Wh_Log(L"Error %08X", winrt::to_hresult());
        }

        if (done) {
            state.pendingAttempts = 0;
        } else if (--state.pendingAttempts > 0) {
            anyPending = true;
        } else {
            Wh_Log(L"Giving up: couldn't identify the clock's taskbar");
        }
    }

    PruneDeadClockStates();

    if (!anyPending && g_retryTimer) {
        g_retryTimer.Stop();
    }
}

void ScheduleClock(ClockState& state) {
    state.pendingAttempts = kMaxClassifyAttempts;

    if (!g_retryTimer) {
        g_retryTimer = DispatcherTimer();
        g_retryTimer.Interval(kRetryInterval);
        g_retryTimerTickToken = g_retryTimer.Tick(
            [](winrt::Windows::Foundation::IInspectable const&,
               winrt::Windows::Foundation::IInspectable const&) {
                OnRetryTimerTick();
            });
    }

    if (!g_retryTimer.IsEnabled()) {
        g_retryTimer.Start();
    }
}

void RegisterClock(FrameworkElement const& clock, bool processNow) {
    PruneDeadClockStates();

    ClockState* state = FindClockState(clock);
    if (!state) {
        g_clockStates.push_back({});
        state = &g_clockStates.back();
        state->clock = winrt::make_weak(clock);

        state->loadedToken = clock.Loaded(
            [](winrt::Windows::Foundation::IInspectable const& sender,
               RoutedEventArgs const&) {
                if (g_unloading) {
                    return;
                }

                auto clock = sender.try_as<FrameworkElement>();
                if (!clock) {
                    return;
                }

                if (auto* state = FindClockState(clock)) {
                    ScheduleClock(*state);
                }
            });

        // If the clock leaves the tree, the hosting icon view might be reused
        // for something else, so make it visible again.
        state->unloadedToken = clock.Unloaded(
            [](winrt::Windows::Foundation::IInspectable const& sender,
               RoutedEventArgs const&) {
                auto clock = sender.try_as<FrameworkElement>();
                if (!clock || clock.IsLoaded()) {
                    return;
                }

                if (auto* state = FindClockState(clock)) {
                    RestoreIconView(*state);
                }
            });
    }

    if (processNow && ProcessClockState(*state)) {
        state->pendingAttempts = 0;
        return;
    }

    ScheduleClock(*state);
}

void CleanupAll() {
    if (g_retryTimer) {
        g_retryTimer.Stop();
        g_retryTimer.Tick(g_retryTimerTickToken);
        g_retryTimer = nullptr;
    }

    for (auto& state : g_clockStates) {
        if (auto clock = state.clock.get()) {
            clock.Loaded(state.loadedToken);
            clock.Unloaded(state.unloadedToken);
            SetClockHidden(state, clock, false);
        } else {
            RestoreIconView(state);
        }
    }

    g_clockStates.clear();
}

void CollectClocks(DependencyObject const& element,
                   std::vector<FrameworkElement>& clocks,
                   int depth) {
    if (depth > 64) {
        return;
    }

    int childrenCount = Media::VisualTreeHelper::GetChildrenCount(element);
    for (int i = 0; i < childrenCount; i++) {
        auto child = Media::VisualTreeHelper::GetChild(element, i);
        if (!child) {
            continue;
        }

        auto childElement = child.try_as<FrameworkElement>();
        if (childElement &&
            winrt::get_class_name(childElement) == kClockClassName) {
            clocks.push_back(childElement);
            continue;
        }

        CollectClocks(child, clocks, depth + 1);
    }
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------

// Called for every clock that gets created, on any taskbar.
using DateTimeIconContent_OnApplyTemplate_t = HRESULT(WINAPI*)(void* pThis);
DateTimeIconContent_OnApplyTemplate_t
    DateTimeIconContent_OnApplyTemplate_Original;
HRESULT WINAPI DateTimeIconContent_OnApplyTemplate_Hook(void* pThis) {
    HRESULT ret = DateTimeIconContent_OnApplyTemplate_Original(pThis);

    if (g_unloading) {
        return ret;
    }

    try {
        FrameworkElement clock = nullptr;
        ((IUnknown*)pThis)
            ->QueryInterface(winrt::guid_of<FrameworkElement>(),
                             winrt::put_abi(clock));
        if (clock) {
            // Don't query taskbar internals while the taskbar might still be
            // under construction, let the retry timer do it.
            RegisterClock(clock, false);
        }
    } catch (...) {
        Wh_Log(L"Error %08X", winrt::to_hresult());
    }

    return ret;
}

// ---------------------------------------------------------------------------
// Applying to existing taskbars
// ---------------------------------------------------------------------------

using RunFromWindowThreadProc_t = void(WINAPI*)(void* parameter);

bool RunFromWindowThread(HWND hWnd,
                         RunFromWindowThreadProc_t proc,
                         void* procParam) {
    static const UINT runFromWindowThreadRegisteredMsg =
        RegisterWindowMessage(L"Windhawk_RunFromWindowThread_" WH_MOD_ID);

    struct RUN_FROM_WINDOW_THREAD_PARAM {
        RunFromWindowThreadProc_t proc;
        void* procParam;
    };

    DWORD dwThreadId = GetWindowThreadProcessId(hWnd, nullptr);
    if (dwThreadId == 0) {
        return false;
    }

    if (dwThreadId == GetCurrentThreadId()) {
        proc(procParam);
        return true;
    }

    HHOOK hook = SetWindowsHookEx(
        WH_CALLWNDPROC,
        [](int nCode, WPARAM wParam, LPARAM lParam) -> LRESULT {
            if (nCode == HC_ACTION) {
                const CWPSTRUCT* cwp = (const CWPSTRUCT*)lParam;
                if (cwp->message == runFromWindowThreadRegisteredMsg) {
                    RUN_FROM_WINDOW_THREAD_PARAM* param =
                        (RUN_FROM_WINDOW_THREAD_PARAM*)cwp->lParam;
                    param->proc(param->procParam);
                }
            }

            return CallNextHookEx(nullptr, nCode, wParam, lParam);
        },
        nullptr, dwThreadId);
    if (!hook) {
        return false;
    }

    RUN_FROM_WINDOW_THREAD_PARAM param;
    param.proc = proc;
    param.procParam = procParam;
    SendMessage(hWnd, runFromWindowThreadRegisteredMsg, 0, (LPARAM)&param);

    UnhookWindowsHookEx(hook);

    return true;
}

void ApplySettings() {
    Wh_Log(L"Applying settings");

    HWND hTaskbarWnd = FindCurrentProcessTaskbarWnd();
    if (!hTaskbarWnd) {
        Wh_Log(L"No taskbar found");
        return;
    }

    RunFromWindowThread(
        hTaskbarWnd,
        [](void* pParam) {
            try {
                if (g_unloading) {
                    CleanupAll();
                    return;
                }

                // Secondary taskbars are created on the primary taskbar's
                // thread.
                EnumThreadWindows(
                    GetCurrentThreadId(),
                    [](HWND hWnd, LPARAM lParam) -> BOOL {
                        try {
                            TaskbarKind kind = TaskbarKind::unknown;
                            XamlRoot xamlRoot =
                                GetXamlRootForTaskbarWindow(hWnd, &kind);
                            if (!xamlRoot) {
                                return TRUE;
                            }

                            auto content = xamlRoot.Content();
                            if (!content) {
                                return TRUE;
                            }

                            std::vector<FrameworkElement> clocks;
                            CollectClocks(content, clocks, 0);
                            Wh_Log(L"Found %d clock(s)", (int)clocks.size());

                            for (auto& clock : clocks) {
                                RegisterClock(clock, true);
                            }
                        } catch (...) {
                            Wh_Log(L"Error %08X", winrt::to_hresult());
                        }

                        return TRUE;
                    },
                    0);
            } catch (...) {
                Wh_Log(L"Error %08X", winrt::to_hresult());
            }
        },
        nullptr);
}

// ---------------------------------------------------------------------------
// Module and symbol handling
// ---------------------------------------------------------------------------

bool HookSystemTraySymbols(HMODULE module) {
    // SystemTray.dll, Taskbar.View.dll, ExplorerExtensions.dll
    WindhawkUtils::SYMBOL_HOOK symbolHooks[] = {
        {
            {LR"(public: virtual int __cdecl winrt::impl::produce<struct winrt::SystemTray::implementation::DateTimeIconContent,struct winrt::Windows::UI::Xaml::IFrameworkElementOverrides>::OnApplyTemplate(void))"},
            &DateTimeIconContent_OnApplyTemplate_Original,
            DateTimeIconContent_OnApplyTemplate_Hook,
        },
    };

    if (!WindhawkUtils::HookSymbols(module, symbolHooks,
                                    ARRAYSIZE(symbolHooks))) {
        Wh_Log(L"HookSymbols failed");
        return false;
    }

    return true;
}

VS_FIXEDFILEINFO* GetModuleVersionInfo(HMODULE hModule, UINT* puPtrLen) {
    void* pFixedFileInfo = nullptr;
    UINT uPtrLen = 0;

    HRSRC hResource =
        FindResource(hModule, MAKEINTRESOURCE(VS_VERSION_INFO), RT_VERSION);
    if (hResource) {
        HGLOBAL hGlobal = LoadResource(hModule, hResource);
        if (hGlobal) {
            void* pData = LockResource(hGlobal);
            if (pData) {
                if (!VerQueryValue(pData, L"\\", &pFixedFileInfo, &uPtrLen) ||
                    uPtrLen == 0) {
                    pFixedFileInfo = nullptr;
                    uPtrLen = 0;
                }
            }
        }
    }

    if (puPtrLen) {
        *puPtrLen = uPtrLen;
    }

    return (VS_FIXEDFILEINFO*)pFixedFileInfo;
}

HMODULE GetSystemTrayModuleHandle() {
    HMODULE module = GetModuleHandle(L"SystemTray.dll");
    if (!module) {
        module = GetModuleHandle(L"Taskbar.View.dll");
        if (module) {
            // Starting with Taskbar.View.dll 2604.8002.200.6000, the SystemTray
            // types moved out of Taskbar.View.dll into SystemTray.dll.
            VS_FIXEDFILEINFO* fixedFileInfo =
                GetModuleVersionInfo(module, nullptr);
            WORD moduleMajor =
                fixedFileInfo ? HIWORD(fixedFileInfo->dwFileVersionMS) : 0;
            if (!moduleMajor || moduleMajor >= 2604) {
                Wh_Log(L"Skipping Taskbar.View.dll version %d", moduleMajor);
                module = nullptr;
            }
        }
    }
    if (!module) {
        module = GetModuleHandle(L"ExplorerExtensions.dll");
    }

    return module;
}

void HandleLoadedModuleIfSystemTray(HMODULE module, LPCWSTR lpLibFileName) {
    if (!g_systemTrayModuleHooked && GetSystemTrayModuleHandle() == module &&
        !g_systemTrayModuleHooked.exchange(true)) {
        Wh_Log(L"Loaded %s", lpLibFileName);

        if (HookSystemTraySymbols(module)) {
            Wh_ApplyHookOperations();
        }
    }
}

using LoadLibraryExW_t = decltype(&LoadLibraryExW);
LoadLibraryExW_t LoadLibraryExW_Original;
HMODULE WINAPI LoadLibraryExW_Hook(LPCWSTR lpLibFileName,
                                   HANDLE hFile,
                                   DWORD dwFlags) {
    HMODULE module = LoadLibraryExW_Original(lpLibFileName, hFile, dwFlags);
    if (module) {
        HandleLoadedModuleIfSystemTray(module, lpLibFileName);
    }

    return module;
}

bool HookTaskbarDllSymbols() {
    HMODULE module =
        LoadLibraryEx(L"taskbar.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) {
        Wh_Log(L"Failed to load taskbar.dll");
        return false;
    }

    WindhawkUtils::SYMBOL_HOOK taskbarDllHooks[] = {
        {
            {LR"(const CTaskBand::`vftable'{for `ITaskListWndSite'})"},
            &CTaskBand_ITaskListWndSite_vftable,
        },
        {
            {LR"(const CSecondaryTaskBand::`vftable'{for `ITaskListWndSite'})"},
            &CSecondaryTaskBand_ITaskListWndSite_vftable,
        },
        {
            {LR"(public: virtual class std::shared_ptr<class TaskbarHost> __cdecl CTaskBand::GetTaskbarHost(void)const )"},
            &CTaskBand_GetTaskbarHost_Original,
        },
        {
            {LR"(public: int __cdecl TaskbarHost::FrameHeight(void)const )"},
            &TaskbarHost_FrameHeight_Original,
        },
        {
            {LR"(public: virtual class std::shared_ptr<class TaskbarHost> __cdecl CSecondaryTaskBand::GetTaskbarHost(void)const )"},
            &CSecondaryTaskBand_GetTaskbarHost_Original,
        },
        {
            {LR"(public: void __cdecl std::_Ref_count_base::_Decref(void))"},
            &std__Ref_count_base__Decref_Original,
        },
    };

    return WindhawkUtils::HookSymbols(module, taskbarDllHooks,
                                      ARRAYSIZE(taskbarDllHooks));
}

// ---------------------------------------------------------------------------
// Windhawk entry points
// ---------------------------------------------------------------------------

void LoadSettings() {
    PCWSTR hideOn = Wh_GetStringSetting(L"hideOn");
    g_settings.hideOn = HideOn::secondary;
    if (wcscmp(hideOn, L"primary") == 0) {
        g_settings.hideOn = HideOn::primary;
    } else if (wcscmp(hideOn, L"all") == 0) {
        g_settings.hideOn = HideOn::all;
    }
    Wh_FreeStringSetting(hideOn);
}

BOOL Wh_ModInit() {
    Wh_Log(L">");

    LoadSettings();

    if (!HookTaskbarDllSymbols()) {
        return FALSE;
    }

    if (HMODULE systemTrayModule = GetSystemTrayModuleHandle()) {
        g_systemTrayModuleHooked = true;
        if (!HookSystemTraySymbols(systemTrayModule)) {
            return FALSE;
        }
    } else {
        Wh_Log(L"System tray module not loaded yet");

        HMODULE kernelBaseModule = GetModuleHandle(L"kernelbase.dll");
        auto pKernelBaseLoadLibraryExW =
            (decltype(&LoadLibraryExW))GetProcAddress(kernelBaseModule,
                                                      "LoadLibraryExW");
        WindhawkUtils::SetFunctionHook(pKernelBaseLoadLibraryExW,
                                       LoadLibraryExW_Hook,
                                       &LoadLibraryExW_Original);
    }

    return TRUE;
}

void Wh_ModAfterInit() {
    Wh_Log(L">");

    if (!g_systemTrayModuleHooked) {
        if (HMODULE systemTrayModule = GetSystemTrayModuleHandle()) {
            if (!g_systemTrayModuleHooked.exchange(true)) {
                Wh_Log(L"Got system tray module");

                if (HookSystemTraySymbols(systemTrayModule)) {
                    Wh_ApplyHookOperations();
                }
            }
        }
    }

    ApplySettings();
}

void Wh_ModBeforeUninit() {
    Wh_Log(L">");

    g_unloading = true;

    ApplySettings();
}

void Wh_ModUninit() {
    Wh_Log(L">");
}

void Wh_ModSettingsChanged() {
    Wh_Log(L">");

    LoadSettings();

    ApplySettings();
}
