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
echo            Installing Aorus Fan Control
echo ========================================================
echo.

set "SOURCE_DIR=%~dp0"
set "INSTALL_DIR=%ProgramFiles%\Fan-Control"

:: Locate Fan-Control.exe
set "EXE_SRC="
if exist "%SOURCE_DIR%Fan-Control.exe" (
    set "EXE_SRC=%SOURCE_DIR%Fan-Control.exe"
) else if exist "%SOURCE_DIR%x64\Release\Fan-Control.exe" (
    set "EXE_SRC=%SOURCE_DIR%x64\Release\Fan-Control.exe"
) else (
    echo Error: Fan-Control.exe was not found. Please compile the Release build first.
    pause
    exit /b 1
)

:: Locate acpimof.dll
if not exist "%SOURCE_DIR%acpimof.dll" (
    echo Error: "acpimof.dll" was not found in "%SOURCE_DIR%".
    pause
    exit /b 1
)

:: Locate fan_config_split.txt
if not exist "%SOURCE_DIR%fan_config_split.txt" (
    echo Error: "fan_config_split.txt" was not found in "%SOURCE_DIR%".
    pause
    exit /b 1
)

:: 1. Terminate any currently running instance
echo [1/6] Stopping any running Fan-Control process...
taskkill /f /im Fan-Control.exe >nul 2>&1

:: 2. Create installation directory
echo [2/6] Creating installation directory at "%INSTALL_DIR%"...
if not exist "%INSTALL_DIR%" mkdir "%INSTALL_DIR%"

:: 3. Copy program files to installation directory
echo [3/6] Copying executable and configuration...
copy /y "%EXE_SRC%" "%INSTALL_DIR%\Fan-Control.exe" >nul
copy /y "%SOURCE_DIR%fan_config_split.txt" "%INSTALL_DIR%\fan_config_split.txt" >nul
if exist "%SOURCE_DIR%Uninstall.cmd" (
    copy /y "%SOURCE_DIR%Uninstall.cmd" "%INSTALL_DIR%\Uninstall.cmd" >nul
)

:: Grant users modify permission on the config file so it can be edited without admin prompt
icacls "%INSTALL_DIR%\fan_config_split.txt" /grant Users:(M) >nul 2>&1

:: 4. Copy acpimof.dll to SysWOW64
echo [4/6] Installing acpimof.dll into C:\Windows\SysWOW64...
copy /y "%SOURCE_DIR%acpimof.dll" "C:\Windows\SysWOW64\acpimof.dll" >nul
if %errorlevel% neq 0 (
    echo Failed to copy acpimof.dll to SysWOW64.
    pause
    exit /b 1
)

:: 5. Register WmiAcpi MOF image path in Registry
echo [5/6] Registering WmiAcpi MofImagePath in Registry...
reg add "HKLM\SYSTEM\CurrentControlSet\Services\WmiAcpi" /v MofImagePath /t REG_SZ /d "C:\Windows\SysWOW64\acpimof.dll" /f >nul
if %errorlevel% neq 0 (
    echo Failed to write to the Registry.
    pause
    exit /b 1
)

:: 6. Create Task Scheduler task (Run at logon with highest privileges)
echo [6/6] Creating Task Scheduler auto-start task 'FanControl'...
schtasks /create /tn "FanControl" /tr "\"%INSTALL_DIR%\Fan-Control.exe\"" /sc onlogon /rl highest /f /delay 0000:05 >nul
if %errorlevel% neq 0 (
    echo Failed to create scheduled task.
    pause
    exit /b 1
)

echo.
echo ========================================================
echo            Installation Succeeded!
echo ========================================================
echo.
echo Program installed to: "%INSTALL_DIR%"
echo Auto-start task:      'FanControl' (Runs at user logon, elevated)
echo.
echo NOTE: If this is the first time installing acpimof.dll,
echo a system reboot is required for WmiAcpi to load the driver.
echo.
choice /c YN /m "Do you want to reboot your computer now? (Y/N)"
if %errorlevel% equ 1 (
    echo Rebooting in 5 seconds...
    shutdown /r /t 5
) else (
    echo.
    echo Starting Fan-Control now...
    start "" "%INSTALL_DIR%\Fan-Control.exe"
    echo Done. You can close this window.
    pause
)