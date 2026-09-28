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

#define CLAMP(x, low, high) (((x) > (high)) ? (high) : (((x) < (low)) ? (low) : (x)))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))

const int WAIT_RAMP_UP_CYCLES = 2;
const int WAIT_RAMP_DOWN_CYCLES = 5;

struct FanState {
    int appliedPercent;
    int rampUpCycle = 1;
    int rampDownCycle = 1;
};

// Global O(1) Lookup Tables (0-149 Celsius)
int cpuLookup[150] = { 0 };
int gpuLookup[150] = { 0 };

bool IsElevated() {
    BOOL fRet = FALSE;
    HANDLE hToken = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        TOKEN_ELEVATION Elevation;
        DWORD cbSize = sizeof(TOKEN_ELEVATION);
        if (GetTokenInformation(hToken, TokenElevation, &Elevation, sizeof(Elevation), &cbSize)) {
            fRet = Elevation.TokenIsElevated;
        }
        CloseHandle(hToken);
    }
    return fRet;
}

void RelaunchAsAdmin() {
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
inline int PercentToSpeed(int percent) {
    if (percent <= 0) return 0;
    if (percent >= 100) return 229;
    return (percent * 229 + 99) / 100;
}

// Pure Integer Math Gradient
int GetGradientTarget(int lastAppliedPercentage, int targetPercentage) {
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

// Custom Zero-Allocation Win32 File Parser
void BuildLookupTablesWin32(const char* filename) {
    HANDLE hFile = CreateFileA(filename, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

    // Safety Baselines
    for (int i = 0; i < 150; i++) { cpuLookup[i] = 30; gpuLookup[i] = 30; }

    if (hFile == INVALID_HANDLE_VALUE) {
        // Create Default Config
        hFile = CreateFileA(filename, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile != INVALID_HANDLE_VALUE) {
            const char* defaultConf = "# Aorus Fan Config (Ultra Optimized)\n[CPU]\n40:30\n50:45\n60:65\n70:85\n80:100\n\n[GPU]\n40:30\n50:45\n60:70\n75:90\n85:100\n";
            DWORD written;
            WriteFile(hFile, defaultConf, lstrlenA(defaultConf), &written, NULL);
            CloseHandle(hFile);
        }

        // Hardcode fallback arrays to match default text
        for (int i = 40; i < 50; i++) { cpuLookup[i] = 30; gpuLookup[i] = 30; }
        for (int i = 50; i < 60; i++) { cpuLookup[i] = 45; gpuLookup[i] = 45; }
        for (int i = 60; i < 70; i++) { cpuLookup[i] = 65; gpuLookup[i] = 70; }
        for (int i = 70; i < 75; i++) { cpuLookup[i] = 85; gpuLookup[i] = 70; }
        for (int i = 75; i < 80; i++) { cpuLookup[i] = 85; gpuLookup[i] = 90; }
        for (int i = 80; i < 85; i++) { cpuLookup[i] = 100; gpuLookup[i] = 90; }
        for (int i = 85; i < 150; i++) { cpuLookup[i] = 100; gpuLookup[i] = 100; }
        return;
    }

    DWORD fileSize = GetFileSize(hFile, NULL);
    if (fileSize > 0 && fileSize < 65536) { // Max 64kb config
        char* buffer = (char*)VirtualAlloc(NULL, fileSize + 1, MEM_COMMIT, PAGE_READWRITE);
        DWORD bytesRead;
        if (ReadFile(hFile, buffer, fileSize, &bytesRead, NULL)) {
            buffer[bytesRead] = '\0';

            char* p = buffer;
            int currentSection = 0; // 1 = CPU, 2 = GPU

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
                        for (int i = temp; i < 150; i++) {
                            if (currentSection == 1) cpuLookup[i] = speed;
                            else if (currentSection == 2) gpuLookup[i] = speed;
                        }
                    }
                }
                while (*p && *p != '\n') p++;
            }
        }
        VirtualFree(buffer, 0, MEM_RELEASE);
    }
    CloseHandle(hFile);
}

HRESULT FastWMISet(IWbemServices* pSvc, BSTR instancePath, BSTR methodName, IWbemClassObject* pClassInstance, int dataValue) {
    VARIANT var;
    VariantInit(&var);
    V_VT(&var) = VT_I4;
    V_I4(&var) = dataValue;

    pClassInstance->Put(L"Data", 0, &var, 0);
    HRESULT hres = pSvc->ExecMethod(instancePath, methodName, 0, nullptr, pClassInstance, nullptr, nullptr);

    VariantClear(&var);
    return hres;
}

int FastWMIGet(IWbemServices* pSvc, BSTR instancePath, BSTR methodName) {
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

BSTR GetWMIInstancePath(IWbemServices* pSvc, BSTR className) {
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

void ProcessFanCycle(FanState& state, int targetPercent, BSTR wmiMethodName, IWbemServices* pSvc, IWbemClassObject* pPreSpawnedInstance, BSTR setInstancePath) {
    if (state.appliedPercent == targetPercent) {
        state.rampDownCycle = 1;
        state.rampUpCycle = 1;
        return;
    }

    int gradientTarget;

    if (state.appliedPercent < targetPercent) {
        if (state.rampUpCycle == WAIT_RAMP_UP_CYCLES) {
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
        if (state.rampDownCycle == WAIT_RAMP_DOWN_CYCLES) {
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

// Entry Point replacing main() to completely bypass the C++ CRT Initialization
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd) {
    if (!IsElevated()) { RelaunchAsAdmin(); return 0; }

    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"Global\\AorusFanControlMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(hMutex); return 0; }

    // 1. DPC Latency Optimization: Lock the process execution solely to CPU Core 0 
    SetProcessAffinityMask(GetCurrentProcess(), 1);

    // Background task scheduling
    SetPriorityClass(GetCurrentProcess(), IDLE_PRIORITY_CLASS);

    BuildLookupTablesWin32("fan_config_split.txt");

    CoInitializeEx(0, COINIT_MULTITHREADED);
    CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE, nullptr);

    IWbemLocator* pLoc = nullptr;
    CoCreateInstance(CLSID_WbemLocator, 0, CLSCTX_INPROC_SERVER, IID_IWbemLocator, (LPVOID*)&pLoc);

    IWbemServices* pSvc = nullptr;
    BSTR bstrRoot = SysAllocString(L"ROOT\\WMI");
    if (FAILED(pLoc->ConnectServer(bstrRoot, nullptr, nullptr, 0, 0, 0, 0, &pSvc))) {
        SysFreeString(bstrRoot);
        pLoc->Release(); CoUninitialize(); CloseHandle(hMutex); return 1;
    }
    SysFreeString(bstrRoot);

    CoSetProxyBlanket(pSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);

    BSTR bstrGetClass = SysAllocString(L"GB_WMIACPI_Get");
    BSTR bstrSetClass = SysAllocString(L"GB_WMIACPI_Set");

    IWbemClassObject* pSetClass = nullptr;
    pSvc->GetObject(bstrSetClass, 0, nullptr, &pSetClass, nullptr);

    BSTR setInstancePath = GetWMIInstancePath(pSvc, bstrSetClass);
    BSTR getInstancePath = GetWMIInstancePath(pSvc, bstrGetClass);

    // Pre-allocated method names
    BSTR methodSetSuperQuiet = SysAllocString(L"SetSuperQuiet");
    BSTR methodSetAutoFanStatus = SysAllocString(L"SetAutoFanStatus");
    BSTR methodSetStepFanStatus = SysAllocString(L"SetStepFanStatus");
    BSTR methodSetFixedFanStatus = SysAllocString(L"SetFixedFanStatus");
    BSTR methodSetFixedFanSpeed = SysAllocString(L"SetFixedFanSpeed");
    BSTR methodSetGPUFanDuty = SysAllocString(L"SetGPUFanDuty");
    BSTR methodGetCpuTemp = SysAllocString(L"getCpuTemp");
    BSTR methodGetGpuTemp1 = SysAllocString(L"getGpuTemp1");
    BSTR methodGetGpuTemp2 = SysAllocString(L"getGpuTemp2");

    IWbemClassObject* pCPUFanDef = nullptr, * pCPUFanInst = nullptr;
    pSetClass->GetMethod(methodSetFixedFanSpeed, 0, &pCPUFanDef, nullptr);
    pCPUFanDef->SpawnInstance(0, &pCPUFanInst);

    IWbemClassObject* pGPUFanDef = nullptr, * pGPUFanInst = nullptr;
    pSetClass->GetMethod(methodSetGPUFanDuty, 0, &pGPUFanDef, nullptr);
    pGPUFanDef->SpawnInstance(0, &pGPUFanInst);

    IWbemClassObject* pTempDef = nullptr, * pTempInst = nullptr;
    pSetClass->GetMethod(methodSetFixedFanStatus, 0, &pTempDef, nullptr);
    pTempDef->SpawnInstance(0, &pTempInst);

    FastWMISet(pSvc, setInstancePath, methodSetSuperQuiet, pTempInst, 0);
    FastWMISet(pSvc, setInstancePath, methodSetAutoFanStatus, pTempInst, 0);
    FastWMISet(pSvc, setInstancePath, methodSetStepFanStatus, pTempInst, 0);
    FastWMISet(pSvc, setInstancePath, methodSetFixedFanStatus, pTempInst, 1);

    pTempDef->Release(); pTempInst->Release();

    FanState cpuState = { cpuLookup[0], 1, 1 };
    FanState gpuState = { gpuLookup[0], 1, 1 };

    FastWMISet(pSvc, setInstancePath, methodSetFixedFanSpeed, pCPUFanInst, PercentToSpeed(cpuState.appliedPercent));
    FastWMISet(pSvc, setInstancePath, methodSetGPUFanDuty, pGPUFanInst, PercentToSpeed(gpuState.appliedPercent));

    HANDLE hTimer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
    LARGE_INTEGER dueTime;
    dueTime.QuadPart = -20000000LL;
    SetWaitableTimerEx(hTimer, &dueTime, 2000, NULL, NULL, NULL, 500);

    // 2. RAM Optimization: Purge all the setup variables, buffers, and COM metadata from physical memory
    EmptyWorkingSet(GetCurrentProcess());

    while (true) {
        WaitForSingleObject(hTimer, INFINITE);

        int cpuTemp = CLAMP(FastWMIGet(pSvc, getInstancePath, methodGetCpuTemp), 0, 149);
        int maxGpuTemp = CLAMP(MAX(FastWMIGet(pSvc, getInstancePath, methodGetGpuTemp1), FastWMIGet(pSvc, getInstancePath, methodGetGpuTemp2)), 0, 149);

        int targetCpuPercent = cpuLookup[cpuTemp];
        int targetGpuPercent = gpuLookup[maxGpuTemp];

        ProcessFanCycle(cpuState, targetCpuPercent, methodSetFixedFanSpeed, pSvc, pCPUFanInst, setInstancePath);
        ProcessFanCycle(gpuState, targetGpuPercent, methodSetGPUFanDuty, pSvc, pGPUFanInst, setInstancePath);
    }

    return 0;
}