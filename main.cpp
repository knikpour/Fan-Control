#define _WIN32_WINNT 0x0602
#define _WIN32_DCOM
#include <windows.h>
#include <psapi.h>
#include <shellapi.h>
#include <wbemidl.h>

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")

#define CLAMP(x, low, high) (((x) > (high)) ? (high) : (((x) < (low)) ? (low) : (x)))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))

static constexpr int WAIT_RAMP_UP_CYCLES = 2;
static constexpr int WAIT_RAMP_DOWN_CYCLES = 5;
static constexpr int EMERGENCY_TEMP_THRESHOLD = 85;

struct FanState {
    int appliedPercent;
    int rampUpCycle = 1;
    int rampDownCycle = 1;
};

struct CurvePoint {
    int temp;
    int speed;
};

// Global O(1) Lookup Tables (0-149 Celsius)
static int cpuLookup[150] = { 0 };
static int gpuLookup[150] = { 0 };

// WMI Runtime Context
struct WmiContext {
    IWbemServices* pSvc = nullptr;
    BSTR setInstancePath = nullptr;
    BSTR getInstancePath = nullptr;
    BSTR methodSetSuperQuiet = nullptr;
    BSTR methodSetAutoFanStatus = nullptr;
    BSTR methodSetStepFanStatus = nullptr;
    BSTR methodSetFixedFanStatus = nullptr;
    BSTR methodSetFixedFanSpeed = nullptr;
    BSTR methodSetGPUFanDuty = nullptr;
    BSTR methodGetCpuTemp = nullptr;
    BSTR methodGetGpuTemp1 = nullptr;
    BSTR methodGetGpuTemp2 = nullptr;
    BSTR bstrGetClass = nullptr;
    BSTR bstrSetClass = nullptr;

    IWbemClassObject* pCPUFanInst = nullptr;
    IWbemClassObject* pGPUFanInst = nullptr;
    IWbemClassObject* pTempInst = nullptr;
};

static WmiContext g_wmi;
static HWND g_hWnd = NULL;
static bool g_bRunning = true;
static bool g_bFansRestored = false;

static void GetConfigPath(wchar_t* outPath, DWORD maxLen) {
    GetModuleFileNameW(NULL, outPath, maxLen);
    wchar_t* lastSlash = wcsrchr(outPath, L'\\');
    if (lastSlash) *(lastSlash + 1) = L'\0';
    lstrcatW(outPath, L"fan_config_split.txt");
}

static bool IsElevated() {
    BOOL fRet = FALSE;
    HANDLE hToken = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        TOKEN_ELEVATION Elevation = { 0 };
        DWORD cbSize = sizeof(TOKEN_ELEVATION);
        if (GetTokenInformation(hToken, TokenElevation, &Elevation, sizeof(Elevation), &cbSize)) {
            fRet = Elevation.TokenIsElevated;
        }
        CloseHandle(hToken);
    }
    return fRet;
}

static void RelaunchAsAdmin() {
    wchar_t szPath[MAX_PATH];
    if (GetModuleFileNameW(NULL, szPath, ARRAYSIZE(szPath))) {
        SHELLEXECUTEINFOW sei = { sizeof(sei) };
        sei.lpVerb = L"runas";
        sei.lpFile = szPath;
        sei.hwnd = NULL;
        sei.nShow = SW_NORMAL;
        if (!ShellExecuteExW(&sei)) ExitProcess(1);
    }
}

// Pure Integer Math Ceiling
static inline int PercentToSpeed(int percent) {
    if (percent <= 0) return 0;
    if (percent >= 100) return 229;
    return (percent * 229 + 99) / 100;
}

// Pure Integer Math Gradient
static int GetGradientTarget(int lastAppliedPercentage, int targetPercentage) {
    int gradientTarget = targetPercentage;
    if (targetPercentage > lastAppliedPercentage) {
        gradientTarget = lastAppliedPercentage + ((targetPercentage - lastAppliedPercentage + 1) / 2);
    }
    else if (targetPercentage < lastAppliedPercentage) {
        gradientTarget = lastAppliedPercentage - ((lastAppliedPercentage - targetPercentage + 1) / 2);
    }

    int diff = targetPercentage - gradientTarget;
    if (diff < 0) diff = -diff;

    return diff < 5 ? targetPercentage : gradientTarget;
}

// Piecewise Linear Interpolation for Smooth Curves (Zero Runtime CPU Cost)
static void InterpolateCurve(const CurvePoint* points, int count, int* lookup, int defaultSpeed) {
    if (count <= 0) {
        for (int i = 0; i < 150; i++) lookup[i] = defaultSpeed;
        return;
    }

    // Insertion sort points by temperature ascending
    CurvePoint sorted[32];
    for (int i = 0; i < count; i++) sorted[i] = points[i];
    for (int i = 1; i < count; i++) {
        CurvePoint key = sorted[i];
        int j = i - 1;
        while (j >= 0 && sorted[j].temp > key.temp) {
            sorted[j + 1] = sorted[j];
            j--;
        }
        sorted[j + 1] = key;
    }

    // Clamp below first defined temperature
    for (int t = 0; t < sorted[0].temp && t < 150; t++) {
        lookup[t] = sorted[0].speed;
    }

    // Piecewise linear interpolation between points
    for (int i = 0; i < count - 1; i++) {
        int t0 = sorted[i].temp;
        int t1 = sorted[i + 1].temp;
        int s0 = sorted[i].speed;
        int s1 = sorted[i + 1].speed;

        int rangeT = t1 - t0;
        int rangeS = s1 - s0;

        for (int t = t0; t <= t1 && t < 150; t++) {
            if (rangeT > 0) {
                int num = rangeS * (t - t0);
                int roundOffset = (num >= 0) ? (rangeT / 2) : -(rangeT / 2);
                lookup[t] = CLAMP(s0 + (num + roundOffset) / rangeT, 0, 100);
            }
            else {
                lookup[t] = CLAMP(s1, 0, 100);
            }
        }
    }

    // Clamp above last defined temperature
    for (int t = sorted[count - 1].temp; t < 150; t++) {
        lookup[t] = sorted[count - 1].speed;
    }
}

// Zero-Allocation Win32 File Parser with Piecewise Linear Curve Generation
static void BuildLookupTablesWin32(const wchar_t* filename) {
    HANDLE hFile = CreateFileW(filename, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

    if (hFile == INVALID_HANDLE_VALUE) {
        // Create Default Config
        hFile = CreateFileW(filename, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile != INVALID_HANDLE_VALUE) {
            const char* defaultConf = "# Aorus Fan Config (Ultra Optimized)\r\n[CPU]\r\n40:30\r\n50:45\r\n60:65\r\n70:85\r\n80:100\r\n\r\n[GPU]\r\n40:30\r\n50:45\r\n60:70\r\n75:90\r\n85:100\r\n";
            DWORD written;
            WriteFile(hFile, defaultConf, lstrlenA(defaultConf), &written, NULL);
            CloseHandle(hFile);
        }

        // Hardcode fallback arrays to match default curve
        CurvePoint defCpu[] = { {40, 30}, {50, 45}, {60, 65}, {70, 85}, {80, 100} };
        CurvePoint defGpu[] = { {40, 30}, {50, 45}, {60, 70}, {75, 90}, {85, 100} };
        InterpolateCurve(defCpu, 5, cpuLookup, 30);
        InterpolateCurve(defGpu, 5, gpuLookup, 30);
        return;
    }

    DWORD fileSize = GetFileSize(hFile, NULL);
    if (fileSize > 0 && fileSize < 65536) { // Max 64kb config
        char* buffer = (char*)VirtualAlloc(NULL, fileSize + 1, MEM_COMMIT, PAGE_READWRITE);
        if (buffer) {
            DWORD bytesRead = 0;
            if (ReadFile(hFile, buffer, fileSize, &bytesRead, NULL) && bytesRead <= fileSize) {
                buffer[bytesRead] = '\0';

                char* p = buffer;
                int currentSection = 0; // 1 = CPU, 2 = GPU

                CurvePoint cpuPoints[32]; int cpuPointCount = 0;
                CurvePoint gpuPoints[32]; int gpuPointCount = 0;

                while (*p) {
                    while (*p == ' ' || *p == '\r' || *p == '\n') p++;
                    if (!*p) break;

                    // Skip Comments
                    if (*p == '#') {
                        while (*p && *p != '\n') p++;
                        continue;
                    }

                    // Track Sections
                    if (*p == '[') {
                        if (p[1] == 'C' && p[2] == 'P' && p[3] == 'U' && p[4] == ']') {
                            currentSection = 1; p += 5;
                        }
                        else if (p[1] == 'G' && p[2] == 'P' && p[3] == 'U' && p[4] == ']') {
                            currentSection = 2; p += 5;
                        }
                        else {
                            while (*p && *p != '\n') p++;
                        }
                        continue;
                    }

                    // Parse Numbers "Temp:Speed"
                    int temp = 0;
                    while (*p >= '0' && *p <= '9') { temp = temp * 10 + (*p - '0'); p++; }

                    if (*p == ':') {
                        p++;
                        int speed = 0;
                        while (*p >= '0' && *p <= '9') { speed = speed * 10 + (*p - '0'); p++; }

                        if (temp >= 0 && temp < 150) {
                            speed = CLAMP(speed, 0, 100);
                            if (currentSection == 1 && cpuPointCount < 32) {
                                cpuPoints[cpuPointCount++] = { temp, speed };
                            }
                            else if (currentSection == 2 && gpuPointCount < 32) {
                                gpuPoints[gpuPointCount++] = { temp, speed };
                            }
                        }
                    }
                    while (*p && *p != '\n') p++;
                }

                InterpolateCurve(cpuPoints, cpuPointCount, cpuLookup, 30);
                InterpolateCurve(gpuPoints, gpuPointCount, gpuLookup, 30);
            }
            VirtualFree(buffer, 0, MEM_RELEASE);
        }
    }
    CloseHandle(hFile);
}

// NVAPI Types and Interfaces for GPU Hotspot Polling
typedef void* (*NvAPI_QueryInterface_t)(unsigned int offset);
typedef int (*NvAPI_Initialize_t)();
typedef void* NvPhysicalGpuHandle;
typedef int (*NvAPI_EnumPhysicalGPUs_t)(NvPhysicalGpuHandle gpuHandles[64], int* pGpuCount);

#pragma pack(push, 8)
struct NvThermalSensors {
    unsigned int version;
    unsigned int mask;
    int reserved[8];
    int temperatures[32];
};
#pragma pack(pop)

typedef int (*NvAPI_GPU_ThermalGetSensors_t)(NvPhysicalGpuHandle gpuHandle, NvThermalSensors* pThermalSensors);

static NvAPI_GPU_ThermalGetSensors_t g_NvAPI_GPU_ThermalGetSensors = nullptr;
static NvPhysicalGpuHandle g_hGpu = nullptr;
static bool g_nvApiInitialized = false;
static int s_nvapiBackoffCounter = 0;

static bool InitNvApi() {
    if (g_nvApiInitialized) return (g_hGpu != nullptr);

    HMODULE hNvApi = LoadLibraryA(sizeof(void*) == 8 ? "nvapi64.dll" : "nvapi.dll");
    if (!hNvApi) return false;

    NvAPI_QueryInterface_t NvAPI_QueryInterface = (NvAPI_QueryInterface_t)GetProcAddress(hNvApi, "nvapi_QueryInterface");
    if (!NvAPI_QueryInterface) return false;

    NvAPI_Initialize_t NvAPI_Initialize = (NvAPI_Initialize_t)NvAPI_QueryInterface(0x0150E828);
    NvAPI_EnumPhysicalGPUs_t NvAPI_EnumPhysicalGPUs = (NvAPI_EnumPhysicalGPUs_t)NvAPI_QueryInterface(0xE5AC921F);
    g_NvAPI_GPU_ThermalGetSensors = (NvAPI_GPU_ThermalGetSensors_t)NvAPI_QueryInterface(0x65FE3AAD);

    if (!NvAPI_Initialize || !NvAPI_EnumPhysicalGPUs || !g_NvAPI_GPU_ThermalGetSensors) return false;
    if (NvAPI_Initialize() != 0) return false;

    NvPhysicalGpuHandle gpuHandles[64] = { 0 };
    int gpuCount = 0;
    if (NvAPI_EnumPhysicalGPUs(gpuHandles, &gpuCount) != 0 || gpuCount == 0) return false;

    g_hGpu = gpuHandles[0];
    g_nvApiInitialized = true;
    return true;
}

// Low-overhead, DPC-safe Hotspot polling
static int GetGpuHotspotTemp() {
    if (!g_nvApiInitialized) {
        InitNvApi();
    }
    if (!g_nvApiInitialized || !g_NvAPI_GPU_ThermalGetSensors || !g_hGpu) {
        return 0;
    }

    if (s_nvapiBackoffCounter > 0) {
        s_nvapiBackoffCounter--;
        return 0;
    }

    NvThermalSensors ts = { 0 };
    ts.version = (unsigned int)(sizeof(NvThermalSensors) | (2 << 16));
    ts.mask = 0x2; // Strictly bit 1 (Hotspot diode only)

    int status = g_NvAPI_GPU_ThermalGetSensors(g_hGpu, &ts);
    if (status == 0 && ts.temperatures[1] > 0) {
        return ts.temperatures[1] / 256;
    }

    // GPU is in low-power D3 state or temporarily powered down; back off for 5 cycles
    s_nvapiBackoffCounter = 5;
    return 0;
}

static HRESULT FastWMISet(IWbemServices* pSvc, BSTR instancePath, BSTR methodName, IWbemClassObject* pClassInstance, int dataValue) {
    VARIANT var;
    VariantInit(&var);
    V_VT(&var) = VT_I4;
    V_I4(&var) = dataValue;

    pClassInstance->Put(L"Data", 0, &var, 0);
    HRESULT hres = pSvc->ExecMethod(instancePath, methodName, 0, nullptr, pClassInstance, nullptr, nullptr);

    VariantClear(&var);
    return hres;
}

static int FastWMIGet(IWbemServices* pSvc, BSTR instancePath, BSTR methodName) {
    IWbemClassObject* pOutParams = nullptr;
    HRESULT hres = pSvc->ExecMethod(instancePath, methodName, 0, nullptr, nullptr, &pOutParams, nullptr);

    if (FAILED(hres) || !pOutParams) return 0;

    VARIANT var;
    VariantInit(&var);
    hres = pOutParams->Get(L"Data", 0, &var, 0, 0);
    int value = (SUCCEEDED(hres) && V_VT(&var) == VT_I4) ? V_I4(&var) : 0;

    VariantClear(&var);
    pOutParams->Release();
    return value;
}

static BSTR GetWMIInstancePath(IWbemServices* pSvc, BSTR className) {
    wchar_t query[256];
    wsprintfW(query, L"SELECT * FROM %s", className);
    BSTR bstrQuery = SysAllocString(query);
    BSTR bstrWQL = SysAllocString(L"WQL");

    IEnumWbemClassObject* pEnum = nullptr;
    HRESULT hres = pSvc->ExecQuery(bstrWQL, bstrQuery, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &pEnum);

    SysFreeString(bstrQuery);
    SysFreeString(bstrWQL);

    BSTR path = nullptr;
    if (SUCCEEDED(hres) && pEnum) {
        IWbemClassObject* pInstance = nullptr;
        ULONG uRet = 0;
        if (pEnum->Next(WBEM_INFINITE, 1, &pInstance, &uRet) == WBEM_S_NO_ERROR) {
            VARIANT vtPath;
            VariantInit(&vtPath);
            pInstance->Get(L"__PATH", 0, &vtPath, 0, 0);
            if (V_VT(&vtPath) == VT_BSTR) path = SysAllocString(vtPath.bstrVal);
            VariantClear(&vtPath);
            pInstance->Release();
        }
        pEnum->Release();
    }
    return path;
}

// Emergency Safe Fan Processor
static void ProcessFanCycle(FanState& state, int targetPercent, int currentTemp, BSTR wmiMethodName, IWbemServices* pSvc, IWbemClassObject* pPreSpawnedInstance, BSTR setInstancePath) {
    if (state.appliedPercent == targetPercent) {
        state.rampDownCycle = 1;
        state.rampUpCycle = 1;
        return;
    }

    // Safety Emergency Bypass:
    // If temp is in critical territory (>= 85°C) or large positive delta (>= 35%),
    // immediately set fan speed without waiting for ramp-up cycles or gradient half-stepping!
    if (currentTemp >= EMERGENCY_TEMP_THRESHOLD || (targetPercent - state.appliedPercent >= 35)) {
        FastWMISet(pSvc, setInstancePath, wmiMethodName, pPreSpawnedInstance, PercentToSpeed(targetPercent));
        state.appliedPercent = targetPercent;
        state.rampUpCycle = 1;
        state.rampDownCycle = 1;
        return;
    }

    int gradientTarget;

    if (state.appliedPercent < targetPercent) {
        if (state.rampUpCycle >= WAIT_RAMP_UP_CYCLES) {
            gradientTarget = GetGradientTarget(state.appliedPercent, targetPercent);
            FastWMISet(pSvc, setInstancePath, wmiMethodName, pPreSpawnedInstance, PercentToSpeed(gradientTarget));
            state.rampDownCycle = 1;
            state.rampUpCycle = 1;
            state.appliedPercent = gradientTarget;
        }
        else {
            state.rampUpCycle++;
        }
    }
    else if (targetPercent < state.appliedPercent) {
        if (state.rampDownCycle >= WAIT_RAMP_DOWN_CYCLES) {
            gradientTarget = GetGradientTarget(state.appliedPercent, targetPercent);
            FastWMISet(pSvc, setInstancePath, wmiMethodName, pPreSpawnedInstance, PercentToSpeed(gradientTarget));
            state.rampDownCycle = 1;
            state.rampUpCycle = 1;
            state.appliedPercent = gradientTarget;
        }
        else {
            state.rampDownCycle++;
        }
    }
}

// Hardware Protection: Revert Embedded Controller (EC) back to BIOS/Auto Fan Control
static void RestoreAutoFans() {
    if (g_bFansRestored) return;
    g_bFansRestored = true;

    if (g_wmi.pSvc && g_wmi.setInstancePath && g_wmi.pTempInst && g_wmi.methodSetFixedFanStatus && g_wmi.methodSetAutoFanStatus) {
        FastWMISet(g_wmi.pSvc, g_wmi.setInstancePath, g_wmi.methodSetFixedFanStatus, g_wmi.pTempInst, 0);
        FastWMISet(g_wmi.pSvc, g_wmi.setInstancePath, g_wmi.methodSetAutoFanStatus, g_wmi.pTempInst, 1);
    }
}

// Clean Resource Deallocation
static void CleanupAllResources() {
    RestoreAutoFans();

    if (g_wmi.pCPUFanInst) { g_wmi.pCPUFanInst->Release(); g_wmi.pCPUFanInst = nullptr; }
    if (g_wmi.pGPUFanInst) { g_wmi.pGPUFanInst->Release(); g_wmi.pGPUFanInst = nullptr; }
    if (g_wmi.pTempInst) { g_wmi.pTempInst->Release(); g_wmi.pTempInst = nullptr; }

    if (g_wmi.setInstancePath) { SysFreeString(g_wmi.setInstancePath); g_wmi.setInstancePath = nullptr; }
    if (g_wmi.getInstancePath) { SysFreeString(g_wmi.getInstancePath); g_wmi.getInstancePath = nullptr; }

    if (g_wmi.methodSetSuperQuiet) SysFreeString(g_wmi.methodSetSuperQuiet);
    if (g_wmi.methodSetAutoFanStatus) SysFreeString(g_wmi.methodSetAutoFanStatus);
    if (g_wmi.methodSetStepFanStatus) SysFreeString(g_wmi.methodSetStepFanStatus);
    if (g_wmi.methodSetFixedFanStatus) SysFreeString(g_wmi.methodSetFixedFanStatus);
    if (g_wmi.methodSetFixedFanSpeed) SysFreeString(g_wmi.methodSetFixedFanSpeed);
    if (g_wmi.methodSetGPUFanDuty) SysFreeString(g_wmi.methodSetGPUFanDuty);
    if (g_wmi.methodGetCpuTemp) SysFreeString(g_wmi.methodGetCpuTemp);
    if (g_wmi.methodGetGpuTemp1) SysFreeString(g_wmi.methodGetGpuTemp1);
    if (g_wmi.methodGetGpuTemp2) SysFreeString(g_wmi.methodGetGpuTemp2);
    if (g_wmi.bstrGetClass) SysFreeString(g_wmi.bstrGetClass);
    if (g_wmi.bstrSetClass) SysFreeString(g_wmi.bstrSetClass);

    if (g_wmi.pSvc) { g_wmi.pSvc->Release(); g_wmi.pSvc = nullptr; }

    CoUninitialize();
}

// Console & Signal Handler for Safe Termination
static BOOL WINAPI ConsoleCtrlHandler(DWORD fdwCtrlType) {
    switch (fdwCtrlType) {
    case CTRL_C_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        RestoreAutoFans();
        return TRUE;
    default:
        return FALSE;
    }
}

// Hidden Window Procedure for Session & Power Events (Invisible Daemon)
static LRESULT CALLBACK WindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_POWERBROADCAST: {
        if (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND) {
            // Re-apply fan settings when system resumes from sleep/hibernate
            if (g_wmi.pSvc && g_wmi.setInstancePath && g_wmi.pTempInst) {
                FastWMISet(g_wmi.pSvc, g_wmi.setInstancePath, g_wmi.methodSetSuperQuiet, g_wmi.pTempInst, 0);
                FastWMISet(g_wmi.pSvc, g_wmi.setInstancePath, g_wmi.methodSetAutoFanStatus, g_wmi.pTempInst, 0);
                FastWMISet(g_wmi.pSvc, g_wmi.setInstancePath, g_wmi.methodSetStepFanStatus, g_wmi.pTempInst, 0);
                FastWMISet(g_wmi.pSvc, g_wmi.setInstancePath, g_wmi.methodSetFixedFanStatus, g_wmi.pTempInst, 1);
            }
        }
        return TRUE;
    }

    case WM_QUERYENDSESSION:
    case WM_ENDSESSION:
        RestoreAutoFans();
        return TRUE;

    case WM_CLOSE:
        DestroyWindow(hWnd);
        return 0;

    case WM_DESTROY:
        RestoreAutoFans();
        g_bRunning = false;
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

int WINAPI WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPSTR lpCmdLine, _In_ int nShowCmd) {
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);
    UNREFERENCED_PARAMETER(nShowCmd);

    if (!IsElevated()) { RelaunchAsAdmin(); return 0; }

    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"Global\\AorusFanControlMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(hMutex); return 0; }

    // Register console control handler for safe termination
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    // Background priority without thread starvation risk
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);

    wchar_t configPath[MAX_PATH] = { 0 };
    GetConfigPath(configPath, MAX_PATH);
    BuildLookupTablesWin32(configPath);

    // Register invisible top-level message window class to receive broadcast session/power messages
    const wchar_t CLASS_NAME[] = L"AorusFanControlWindowClass";
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    RegisterClassExW(&wc);

    // Completely invisible top-level unowned window (no WS_VISIBLE, no tray icon, no taskbar item)
    g_hWnd = CreateWindowExW(0, CLASS_NAME, L"Aorus Fan Control", 0, 0, 0, 0, 0, NULL, NULL, hInstance, NULL);

    if (FAILED(CoInitializeEx(0, COINIT_MULTITHREADED))) {
        CloseHandle(hMutex);
        return 1;
    }
    (void)CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE, nullptr);

    IWbemLocator* pLoc = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WbemLocator, 0, CLSCTX_INPROC_SERVER, IID_IWbemLocator, (LPVOID*)&pLoc))) {
        CleanupAllResources();
        CloseHandle(hMutex);
        return 1;
    }

    IWbemServices* pSvc = nullptr;
    BSTR bstrRoot = SysAllocString(L"ROOT\\WMI");
    HRESULT hrConnect = pLoc->ConnectServer(bstrRoot, nullptr, nullptr, 0, 0, 0, 0, &pSvc);
    SysFreeString(bstrRoot);
    pLoc->Release(); // Locator is no longer needed

    if (FAILED(hrConnect) || !pSvc) {
        CleanupAllResources();
        CloseHandle(hMutex);
        return 1;
    }
    g_wmi.pSvc = pSvc;

    (void)CoSetProxyBlanket(pSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);

    g_wmi.bstrGetClass = SysAllocString(L"GB_WMIACPI_Get");
    g_wmi.bstrSetClass = SysAllocString(L"GB_WMIACPI_Set");

    IWbemClassObject* pSetClass = nullptr;
    pSvc->GetObject(g_wmi.bstrSetClass, 0, nullptr, &pSetClass, nullptr);
    if (!pSetClass) {
        CleanupAllResources();
        CloseHandle(hMutex);
        return 1;
    }

    g_wmi.setInstancePath = GetWMIInstancePath(pSvc, g_wmi.bstrSetClass);
    g_wmi.getInstancePath = GetWMIInstancePath(pSvc, g_wmi.bstrGetClass);

    // Pre-allocated method names
    g_wmi.methodSetSuperQuiet = SysAllocString(L"SetSuperQuiet");
    g_wmi.methodSetAutoFanStatus = SysAllocString(L"SetAutoFanStatus");
    g_wmi.methodSetStepFanStatus = SysAllocString(L"SetStepFanStatus");
    g_wmi.methodSetFixedFanStatus = SysAllocString(L"SetFixedFanStatus");
    g_wmi.methodSetFixedFanSpeed = SysAllocString(L"SetFixedFanSpeed");
    g_wmi.methodSetGPUFanDuty = SysAllocString(L"SetGPUFanDuty");
    g_wmi.methodGetCpuTemp = SysAllocString(L"getCpuTemp");
    g_wmi.methodGetGpuTemp1 = SysAllocString(L"getGpuTemp1");
    g_wmi.methodGetGpuTemp2 = SysAllocString(L"getGpuTemp2");

    IWbemClassObject* pCPUFanDef = nullptr;
    if (SUCCEEDED(pSetClass->GetMethod(g_wmi.methodSetFixedFanSpeed, 0, &pCPUFanDef, nullptr)) && pCPUFanDef) {
        pCPUFanDef->SpawnInstance(0, &g_wmi.pCPUFanInst);
        pCPUFanDef->Release();
    }

    IWbemClassObject* pGPUFanDef = nullptr;
    if (SUCCEEDED(pSetClass->GetMethod(g_wmi.methodSetGPUFanDuty, 0, &pGPUFanDef, nullptr)) && pGPUFanDef) {
        pGPUFanDef->SpawnInstance(0, &g_wmi.pGPUFanInst);
        pGPUFanDef->Release();
    }

    IWbemClassObject* pTempDef = nullptr;
    if (SUCCEEDED(pSetClass->GetMethod(g_wmi.methodSetFixedFanStatus, 0, &pTempDef, nullptr)) && pTempDef) {
        pTempDef->SpawnInstance(0, &g_wmi.pTempInst);
        pTempDef->Release();
    }

    pSetClass->Release(); // Class definition no longer needed after spawning instances

    if (!g_wmi.pCPUFanInst || !g_wmi.pGPUFanInst || !g_wmi.pTempInst || !g_wmi.setInstancePath || !g_wmi.getInstancePath) {
        CleanupAllResources();
        CloseHandle(hMutex);
        return 1;
    }

    // Initialize WMI fan control state
    FastWMISet(pSvc, g_wmi.setInstancePath, g_wmi.methodSetSuperQuiet, g_wmi.pTempInst, 0);
    FastWMISet(pSvc, g_wmi.setInstancePath, g_wmi.methodSetAutoFanStatus, g_wmi.pTempInst, 0);
    FastWMISet(pSvc, g_wmi.setInstancePath, g_wmi.methodSetStepFanStatus, g_wmi.pTempInst, 0);
    FastWMISet(pSvc, g_wmi.setInstancePath, g_wmi.methodSetFixedFanStatus, g_wmi.pTempInst, 1);

    FanState cpuState = { cpuLookup[0], 1, 1 };
    FanState gpuState = { gpuLookup[0], 1, 1 };

    FastWMISet(pSvc, g_wmi.setInstancePath, g_wmi.methodSetFixedFanSpeed, g_wmi.pCPUFanInst, PercentToSpeed(cpuState.appliedPercent));
    FastWMISet(pSvc, g_wmi.setInstancePath, g_wmi.methodSetGPUFanDuty, g_wmi.pGPUFanInst, PercentToSpeed(gpuState.appliedPercent));

    HANDLE hTimer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
    if (!hTimer) {
        CleanupAllResources();
        CloseHandle(hMutex);
        return 1;
    }
    LARGE_INTEGER dueTime = { 0 };
    dueTime.QuadPart = -20000000LL;
    SetWaitableTimerEx(hTimer, &dueTime, 2000, NULL, NULL, NULL, 500);

    // Initialize NVAPI for direct GPU hotspot polling
    InitNvApi();

    // Event-driven loop: wake on 2-second timer tick or system power/session messages with 0% CPU idle
    while (g_bRunning) {
        DWORD waitResult = MsgWaitForMultipleObjectsEx(1, &hTimer, INFINITE, QS_ALLINPUT, MWMO_ALERTABLE | MWMO_INPUTAVAILABLE);

        if (waitResult == WAIT_OBJECT_0) {
            int cpuTemp = CLAMP(FastWMIGet(pSvc, g_wmi.getInstancePath, g_wmi.methodGetCpuTemp), 0, 149);

            // Check GPU hotspot temperature (falls back to standard WMI temp if hotspot is unavailable)
            int gpuHotspotTemp = GetGpuHotspotTemp();
            if (gpuHotspotTemp <= 0) {
                gpuHotspotTemp = MAX(FastWMIGet(pSvc, g_wmi.getInstancePath, g_wmi.methodGetGpuTemp1), FastWMIGet(pSvc, g_wmi.getInstancePath, g_wmi.methodGetGpuTemp2));
            }
            gpuHotspotTemp = CLAMP(gpuHotspotTemp, 0, 149);

            int targetCpuPercent = cpuLookup[cpuTemp];
            int targetGpuPercent = gpuLookup[gpuHotspotTemp];

            ProcessFanCycle(cpuState, targetCpuPercent, cpuTemp, g_wmi.methodSetFixedFanSpeed, pSvc, g_wmi.pCPUFanInst, g_wmi.setInstancePath);
            ProcessFanCycle(gpuState, targetGpuPercent, gpuHotspotTemp, g_wmi.methodSetGPUFanDuty, pSvc, g_wmi.pGPUFanInst, g_wmi.setInstancePath);
        }
        else if (waitResult == (WAIT_OBJECT_0 + 1)) {
            MSG msg;
            while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    g_bRunning = false;
                    break;
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }

    CloseHandle(hTimer);
    CleanupAllResources();
    CloseHandle(hMutex);

    return 0;
}