@echo off
setlocal

:: Check for Administrator privileges
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Administrator permissions required. Right-click and select "Run as administrator".
    pause
    exit /b 1
)

echo ========================================================
echo           Uninstalling Aorus Fan Control
echo ========================================================
echo.

:: 1. Terminate running Fan-Control process
echo [1/5] Terminating running Fan-Control process...
taskkill /f /im Fan-Control.exe >nul 2>&1

:: 2. Remove Scheduled Task
echo [2/5] Removing scheduled task 'FanControl'...
schtasks /delete /tn "FanControl" /f >nul 2>&1

:: 3. Remove Registry Key
echo [3/5] Removing WmiAcpi Registry MofImagePath...
reg delete "HKLM\SYSTEM\CurrentControlSet\Services\WmiAcpi" /v MofImagePath /f >nul 2>&1

:: 4. Remove acpimof.dll from SysWOW64
echo [4/5] Removing acpimof.dll from SysWOW64...
if exist "C:\Windows\SysWOW64\acpimof.dll" (
    del /f /q "C:\Windows\SysWOW64\acpimof.dll" >nul 2>&1
)

:: 5. Remove Program Files directory
echo [5/5] Removing installation directory...
set "INSTALL_DIR=%ProgramFiles%\Fan-Control"
if exist "%INSTALL_DIR%" (
    rmdir /s /q "%INSTALL_DIR%" >nul 2>&1
)

echo.
echo ========================================================
echo            Uninstallation complete!
echo ========================================================
echo.
echo Note: A system reboot is recommended to completely unload the WmiAcpi driver.
echo.
pause
