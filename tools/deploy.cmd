@echo off
setlocal enabledelayedexpansion
rem ----------------------------------------------------------------------
rem deploy.cmd - clean-slate reinstall of the Adaptoid driver. GUEST side,
rem MUST BE ELEVATED. No reboot.
rem
rem     deploy.cmd [package-folder]
rem
rem Default package folder is B:\pkg, what tools\package.cmd stages.
rem
rem WHY IT DELETES FIRST. Every install stages a copy into the driver
rem store under its own hash and publishes a new oemNN.inf; the old copies
rem stay forever. Windows then RANKS all of them, so a stale package can
rem outrank a freshly installed one and the new driver silently does not
rem load. Deleting every copy of ours before installing is what keeps a
rem tweak loop honest.
rem
rem YOU CANNOT SHORTCUT THIS BY OVERWRITING THE .SYS. The INF sets
rem PnpLockdown=1, so %windir%\System32\drivers\wishk300.sys is protected
rem and a hand copy is refused. Package reinstall is the only path.
rem
rem ENGLISH WINDOWS ONLY. pnputil's field labels are localised and this
rem parses "Published Name" and "Original Name" literally.
rem ----------------------------------------------------------------------

set "PKG=%~1"
if "%PKG%"=="" set "PKG=B:\pkg"
set "INFNAME=adaptoid.inf"

net session >nul 2>&1
if errorlevel 1 (
    echo   NOT ELEVATED. Run this from an Administrator command prompt.
    exit /b 1
)
if not exist "%PKG%\%INFNAME%" (
    echo   No %INFNAME% in %PKG%
    echo   Run tools\package.cmd on the host first.
    exit /b 1
)

echo ============================================================
echo == 1. REMOVING EVERY STAGED COPY OF OUR PACKAGE
echo ============================================================
set "TMPF=%TEMP%\adaptoid-drv.txt"
pnputil /enum-drivers > "%TMPF%" 2>nul

set "LAST="
set "REMOVED=0"
for /f "usebackq delims=" %%L in ("%TMPF%") do (
    set "LINE=%%L"
    if /i "!LINE:~0,15!"=="Published Name:" (
        for /f "tokens=2 delims=:" %%V in ("!LINE!") do (
            set "LAST=%%V"
            set "LAST=!LAST: =!"
        )
    )
    if /i "!LINE:~0,14!"=="Original Name:" (
        for /f "tokens=2 delims=:" %%V in ("!LINE!") do (
            set "ORIG=%%V"
            set "ORIG=!ORIG: =!"
            if /i "!ORIG!"=="%INFNAME%" (
                if defined LAST (
                    echo   deleting !LAST!
                    pnputil /delete-driver !LAST! /uninstall /force
                    set /a REMOVED+=1
                    set "LAST="
                )
            )
        )
    )
)
del "%TMPF%" >nul 2>&1
echo   %REMOVED% stale package^(s^) removed.
echo.

echo ============================================================
echo == 2. INSTALLING THE NEW PACKAGE
echo ============================================================
pnputil /add-driver "%PKG%\%INFNAME%" /install
if errorlevel 1 (
    echo.
    echo   INSTALL FAILED. Usual causes, in order of likelihood:
    echo     - the catalogue is unsigned, or the test certificate is not
    echo       in BOTH Trusted Root and Trusted Publishers
    echo     - test signing is off:  bcdedit /set testsigning on  + reboot
    echo     - on Win10/11, Secure Boot or Memory Integrity is on
    exit /b 1
)
echo.

echo ============================================================
echo == 3. RESCANNING SO A PRESENT DEVICE PICKS IT UP
echo ============================================================
pnputil /scan-devices
echo.

echo ============================================================
echo == 4. RESULT
echo ============================================================
sc query wishk300 2>nul | findstr /i "SERVICE_NAME STATE"
reg query "HKLM\SYSTEM\CurrentControlSet\Enum\USB\VID_06F7&PID_0001" /s /v Service 2>nul | findstr /i "Service"
echo.
echo   For the full picture run state.cmd.
echo   If the adapter was plugged in through this, unplug and replug it -
echo   the accessory probe only runs on device arrival.
endlocal
