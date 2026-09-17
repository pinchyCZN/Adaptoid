@echo off
setlocal enabledelayedexpansion
rem ----------------------------------------------------------------------
rem undeploy.cmd - remove the Adaptoid driver completely. GUEST side, MUST
rem BE ELEVATED. For when you want a genuinely clean slate without
rem restoring a snapshot.
rem
rem     undeploy.cmd
rem
rem Removes, in this order: the device nodes, then every staged copy of
rem our package, then the service if anything left it behind.
rem
rem ORDER MATTERS. Deleting the package while a device is still bound to
rem it leaves the devnode pointing at a service whose binary is gone, and
rem the next enumeration can bind Microsoft's HidUsb on the compatible-ID
rem match instead. Removing the devices first avoids that.
rem
rem AFTER THIS THE ADAPTER STILL WORKS - Windows falls back to HidUsb and
rem it enumerates as a plain HID game controller. That is the state the
rem original driver's failed install produced, so do not read a working
rem game controller as evidence that our driver is installed. Run
rem state.cmd and read the Service value.
rem
rem ENGLISH WINDOWS ONLY, same pnputil label caveat as deploy.cmd.
rem ----------------------------------------------------------------------

set "INFNAME=adaptoid.inf"
set "HWID=USB\VID_06F7&PID_0001"

net session >nul 2>&1
if errorlevel 1 (
    echo   NOT ELEVATED. Run this from an Administrator command prompt.
    exit /b 1
)

echo ============================================================
echo == 1. REMOVING DEVICE NODES
echo ============================================================
set "GONE=0"
for /f "usebackq tokens=*" %%K in (
    `reg query "HKLM\SYSTEM\CurrentControlSet\Enum\%HWID%" 2^>nul`
) do (
    set "KEY=%%K"
    rem The instance id is the last path component of the enum key.
    for %%I in ("!KEY!") do set "INST=%%~nxI"
    if defined INST (
        echo   removing %HWID%\!INST!
        pnputil /remove-device "%HWID%\!INST!" 2>nul
        if errorlevel 1 (
            echo     pnputil /remove-device unavailable or refused;
            echo     remove it from Device Manager instead.
        ) else (
            set /a GONE+=1
        )
    )
)
echo   %GONE% device node^(s^) removed.
echo.

echo ============================================================
echo == 2. REMOVING EVERY STAGED COPY OF OUR PACKAGE
echo ============================================================
set "TMPF=%TEMP%\adaptoid-undrv.txt"
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
echo   %REMOVED% package^(s^) removed.
echo.

echo ============================================================
echo == 3. THE SERVICE, IF ANYTHING LEFT IT BEHIND
echo ============================================================
sc query wishk300 >nul 2>&1
if errorlevel 1 (
    echo   no wishk300 service, which is the expected end state.
) else (
    echo   service still registered; deleting.
    sc stop wishk300 >nul 2>&1
    sc delete wishk300
)
echo.

echo ============================================================
echo == 4. WHAT IS LEFT
echo ============================================================
reg query "HKLM\SYSTEM\CurrentControlSet\Enum\%HWID%" /s /v Service 2>nul
echo.
echo   Expect Service = HidUsb, or the key to be gone entirely.
echo   Unplug and replug the adapter to settle the enumeration.
endlocal
