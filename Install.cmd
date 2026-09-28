@echo off
:: Check for Administrator privileges
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Administrator permissions required. Right-click and select "Run as administrator".
    pause
    exit /b 1
)

:: 1. Copy DLL from the current folder to SysWOW64
if exist "%~dp0acpimof.dll" (
    copy /y "%~dp0acpimof.dll" "C:\Windows\SysWOW64\acpimof.dll"
) else (
    echo Error: "acpimof.dll" was not found in the script folder.
    pause
    exit /b 1
)

:: 2. Create the string value (REG_SZ)
reg add "HKLM\SYSTEM\CurrentControlSet\Services\WmiAcpi" /v MofImagePath /t REG_SZ /d "C:\Windows\SysWOW64\acpimof.dll" /f
if %errorlevel% neq 0 (
    echo Failed to write to the Registry.
    pause
    exit /b 1
)

:: 3. Reboot the computer immediately
shutdown /r /t 0