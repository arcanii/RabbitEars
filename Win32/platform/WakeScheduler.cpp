// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/WakeScheduler.h"

#include <taskschd.h>

#include <string>

#include "platform/Log.h"

#pragma comment(lib, "taskschd.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

namespace rabbitears {
namespace {

constexpr wchar_t kTaskName[] = L"RabbitEars Recording Wake";
constexpr wchar_t kRootFolder[] = L"\\";

// Minimal RAII so every early return can't leak a BSTR / COM ref / COM init.
struct Bstr {
    BSTR b = nullptr;
    explicit Bstr(const wchar_t* s) : b(SysAllocString(s)) {}
    ~Bstr() { if (b) SysFreeString(b); }
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
    operator BSTR() const { return b; }
};

template <class T>
struct ComPtr {
    T* p = nullptr;
    ~ComPtr() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

// The UI thread may already be in an apartment (OLE is initialised for GDI+/IStream work), so
// only balance a CoInitializeEx that WE performed. RPC_E_CHANGED_MODE means someone else picked
// a different apartment — that's fine, we just use theirs and don't uninitialise.
struct ComInit {
    bool owned = false;
    ComInit() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        owned = (hr == S_OK);  // S_FALSE = already initialised on this thread; don't unbalance it
    }
    ~ComInit() { if (owned) CoUninitialize(); }
};

// Task Scheduler wants an ISO-8601 LOCAL time ("2026-07-10T20:30:00", no zone suffix).
std::wstring localIso8601(long long utcEpoch) {
    if (utcEpoch <= 0) return L"";
    // Unix epoch -> FILETIME (100 ns ticks since 1601-01-01).
    const long long ticks = (utcEpoch + 11644473600LL) * 10000000LL;
    FILETIME ft;
    ft.dwLowDateTime = static_cast<DWORD>(ticks & 0xFFFFFFFFLL);
    ft.dwHighDateTime = static_cast<DWORD>(static_cast<unsigned long long>(ticks) >> 32);
    SYSTEMTIME utc{}, loc{};
    if (!FileTimeToSystemTime(&ft, &utc)) return L"";
    if (!SystemTimeToTzSpecificLocalTime(nullptr, &utc, &loc)) return L"";
    wchar_t buf[32];
    swprintf_s(buf, L"%04d-%02d-%02dT%02d:%02d:%02d", loc.wYear, loc.wMonth, loc.wDay, loc.wHour,
               loc.wMinute, loc.wSecond);
    return buf;
}

std::wstring exePath() {
    wchar_t buf[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return buf;
}

// Connect to the local Task Scheduler service and open the root folder.
bool openRootFolder(ComPtr<ITaskService>& svc, ComPtr<ITaskFolder>& root) {
    HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_ITaskService, reinterpret_cast<void**>(&svc));
    if (FAILED(hr) || !svc) {
        diag::warn(L"wake task: CoCreateInstance(TaskScheduler) failed, hr=" + std::to_wstring(hr));
        return false;
    }
    VARIANT empty;
    VariantInit(&empty);  // VT_EMPTY x4 == connect to the local machine as the current user
    hr = svc->Connect(empty, empty, empty, empty);
    if (FAILED(hr)) {
        diag::warn(L"wake task: ITaskService::Connect failed, hr=" + std::to_wstring(hr));
        return false;
    }
    Bstr rootPath(kRootFolder);
    hr = svc->GetFolder(rootPath, &root);
    if (FAILED(hr) || !root) {
        diag::warn(L"wake task: GetFolder failed, hr=" + std::to_wstring(hr));
        return false;
    }
    return true;
}

}  // namespace

bool syncWakeTask(long long fireAtUtc) {
    const std::wstring boundary = localIso8601(fireAtUtc);
    if (boundary.empty()) {
        diag::warn(L"wake task: bad start time, not registering");
        return false;
    }

    ComInit com;
    ComPtr<ITaskService> svc;
    ComPtr<ITaskFolder> root;
    if (!openRootFolder(svc, root)) return false;

    ComPtr<ITaskDefinition> task;
    if (FAILED(svc->NewTask(0, &task)) || !task) return false;

    {  // Who/why — shown in the Task Scheduler UI so the task isn't a mystery to the user.
        ComPtr<IRegistrationInfo> reg;
        if (SUCCEEDED(task->get_RegistrationInfo(&reg)) && reg) {
            Bstr author(L"RabbitEars");
            Bstr desc(L"Wakes this PC and starts RabbitEars for a scheduled recording. "
                      L"Created automatically; removed when no recordings are pending.");
            reg->put_Author(author);
            reg->put_Description(desc);
        }
    }
    {  // Interactive token: no elevation needed to register, runs in the logged-on session.
        ComPtr<IPrincipal> principal;
        if (SUCCEEDED(task->get_Principal(&principal)) && principal) {
            principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN);
            principal->put_RunLevel(TASK_RUNLEVEL_LUA);
        }
    }
    {  // The settings that actually make this a *wake* task, and keep it from being throttled.
        ComPtr<ITaskSettings> set;
        if (SUCCEEDED(task->get_Settings(&set)) && set) {
            set->put_WakeToRun(VARIANT_TRUE);           // the whole point
            set->put_StartWhenAvailable(VARIANT_TRUE);  // machine was off? run at next boot
            set->put_DisallowStartIfOnBatteries(VARIANT_FALSE);  // record on battery too
            set->put_StopIfGoingOnBatteries(VARIANT_FALSE);
            set->put_AllowDemandStart(VARIANT_TRUE);
            set->put_Enabled(VARIANT_TRUE);
            set->put_Hidden(VARIANT_FALSE);
            set->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW);
            Bstr noLimit(L"PT0S");  // no execution time limit — we exit on our own
            set->put_ExecutionTimeLimit(noLimit);
            ComPtr<IIdleSettings> idle;
            if (SUCCEEDED(set->get_IdleSettings(&idle)) && idle) {
                idle->put_StopOnIdleEnd(VARIANT_FALSE);  // don't kill us when the user returns
            }
        }
    }
    {  // One-shot time trigger at (start - lead).
        ComPtr<ITriggerCollection> triggers;
        if (FAILED(task->get_Triggers(&triggers)) || !triggers) return false;
        ComPtr<ITrigger> trigger;
        if (FAILED(triggers->Create(TASK_TRIGGER_TIME, &trigger)) || !trigger) return false;
        ComPtr<ITimeTrigger> timeTrigger;
        if (FAILED(trigger->QueryInterface(IID_ITimeTrigger,
                                           reinterpret_cast<void**>(&timeTrigger))) ||
            !timeTrigger)
            return false;
        Bstr id(L"RabbitEarsWakeTrigger");
        Bstr start(boundary.c_str());
        timeTrigger->put_Id(id);
        timeTrigger->put_StartBoundary(start);
        timeTrigger->put_Enabled(VARIANT_TRUE);
    }
    {  // Launch ourselves with the flag that lets the recording proceed unattended.
        ComPtr<IActionCollection> actions;
        if (FAILED(task->get_Actions(&actions)) || !actions) return false;
        ComPtr<IAction> action;
        if (FAILED(actions->Create(TASK_ACTION_EXEC, &action)) || !action) return false;
        ComPtr<IExecAction> exec;
        if (FAILED(action->QueryInterface(IID_IExecAction, reinterpret_cast<void**>(&exec))) || !exec)
            return false;
        const std::wstring path = exePath();
        Bstr bpath(path.c_str());
        Bstr bargs(L"--scheduled-wake");
        exec->put_Path(bpath);
        exec->put_Arguments(bargs);
    }

    Bstr name(kTaskName);
    VARIANT empty;
    VariantInit(&empty);
    ComPtr<IRegisteredTask> registered;
    const HRESULT hr =
        root->RegisterTaskDefinition(name, task.p, TASK_CREATE_OR_UPDATE, empty, empty,
                                     TASK_LOGON_INTERACTIVE_TOKEN, empty, &registered);
    if (FAILED(hr)) {
        diag::warn(L"wake task: RegisterTaskDefinition failed, hr=" + std::to_wstring(hr));
        return false;
    }
    diag::info(L"wake task registered for " + boundary + L" (local) -> " + exePath());
    return true;
}

void clearWakeTask() {
    ComInit com;
    ComPtr<ITaskService> svc;
    ComPtr<ITaskFolder> root;
    if (!openRootFolder(svc, root)) return;
    Bstr name(kTaskName);
    // DeleteTask fails harmlessly when the task was never registered — don't log that as an error.
    if (SUCCEEDED(root->DeleteTask(name, 0))) diag::info(L"wake task removed (no pending recordings)");
}

bool runWakeTaskNow() {
    ComInit com;
    ComPtr<ITaskService> svc;
    ComPtr<ITaskFolder> root;
    if (!openRootFolder(svc, root)) return false;

    Bstr name(kTaskName);
    ComPtr<IRegisteredTask> task;
    if (FAILED(root->GetTask(name, &task)) || !task) {
        diag::warn(L"run wake task: not registered — nothing pending, or wake-to-record is off");
        return false;
    }
    VARIANT empty;
    VariantInit(&empty);  // VT_EMPTY == no arguments beyond the registered --scheduled-wake
    ComPtr<IRunningTask> running;
    const HRESULT hr = task->Run(empty, &running);
    if (FAILED(hr)) {
        diag::warn(L"run wake task: Run failed, hr=" + std::to_wstring(hr));
        return false;
    }
    diag::info(L"run wake task: demanded start of \"" + std::wstring(kTaskName) + L"\"");
    return true;
}

void setRecordingKeepAwake(bool on) {
    // ES_CONTINUOUS pins the state to THIS thread until cleared with a bare ES_CONTINUOUS.
    // ES_SYSTEM_REQUIRED (no ES_DISPLAY_REQUIRED) keeps the machine awake but lets the screen
    // sleep — recording doesn't need pixels.
    static bool active = false;
    if (on == active) return;
    const EXECUTION_STATE es =
        on ? (ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_AWAYMODE_REQUIRED) : ES_CONTINUOUS;
    if (SetThreadExecutionState(es) == 0) {
        // AWAYMODE isn't supported everywhere; retry without it before giving up.
        if (!on || SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED) == 0) {
            diag::warn(L"keep-awake: SetThreadExecutionState failed");
            return;
        }
    }
    active = on;
    diag::info(on ? L"keep-awake: sleep suppressed while recording"
                  : L"keep-awake: released (no active recordings)");
}

}  // namespace rabbitears
