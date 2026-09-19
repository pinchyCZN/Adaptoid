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
if "%PKG%"=="" set "PKG=%~dp0pkg"
set "INFNAME=adaptoid.inf"
set "HWID=USB\VID_06F7&PID_0001"
for %%H in ("USB\VID_06F7&PID_0001") do set "HWLEAF=%%~nxH"

net session >nul 2>&1
if errorlevel 1 (
    echo   NOT ELEVATED. Run this from an Administrator command prompt.
    exit /b 1
)

rem THE CONFIGURATOR PINS THE OLD DRIVER IN MEMORY, and this is the one
rem failure mode of this script that reports complete success and changes
rem nothing. It is worth refusing rather than warning.
rem
rem wishk300 only leaves memory when its last device object goes, and the
rem control device is deleted only when the adapter count AND the open
rem handle count are both zero. The configurator holds two handles on
rem \\.\Wish_NA1 for as long as it runs, so unplugging the adapter is not
rem enough: the control device survives, the driver object survives, and
rem Windows will not map a second copy of an image that is still resident.
rem
rem Everything below then succeeds - the file is staged, the package
rem installs, the rescan binds - while the kernel goes on running the
rem previous build. Measured as an unchanged load address and an unchanged
rem PDB GUID across a deploy, with the bug the deploy was meant to fix
rem still visibly accumulating in the device extension.
tasklist /fi "imagename eq wishd201.exe" 2>nul | find /i "wishd201.exe" >nul
if not errorlevel 1 (
    echo.
    echo   THE CONFIGURATOR IS RUNNING. Deploying now would report success
    echo   and leave the OLD driver running.
    echo.
    echo   wishd201.exe keeps \\.\Wish_NA1 open, which keeps the control
    echo   device alive, which keeps wishk300.sys resident. A resident
    echo   image is never replaced.
    echo.
    echo   Exit it from its tray icon, then run this again. If it is
    echo   already closed and this still fires, reboot instead - something
    echo   else holds a handle.
    echo.
    echo   To deploy anyway, knowing it may not take effect:
    echo       deploy.cmd "!PKG!" force
    echo.
    if /i not "%~2"=="force" exit /b 1
    echo   FORCED. Verify the load address actually changed afterwards.
    echo.
)

rem WAS THE OLD IMAGE ALREADY RESIDENT WHEN WE STARTED? That, not the
rem state afterwards, is what decides whether this deploy can take. A
rem running service after the install is normal and expected - the device
rem was rebound and the driver loaded. A running service BEFORE it means
rem the previous image is still mapped, and Windows does not replace a
rem mapped image, so the install below will stage the new file and leave
rem the kernel executing the old one.
rem
rem wishk300 stays mapped until its last device object goes, and the
rem control device is deleted only when the adapter count and the open
rem handle count are BOTH zero AT THE SAME MOMENT. Closing the
rem configurator while the adapter is plugged does not do it, and
rem unplugging the adapter while the configurator runs does not either.
rem Section 7.2 of ..\src_drv\README.txt has the detail.
set "WASRESIDENT="
sc query wishk300 2>nul | find /i "RUNNING" >nul
if not errorlevel 1 set "WASRESIDENT=1"
rem USE !VAR! INSIDE A BLOCK, NOT %VAR%. cmd expands %VAR% while it
rem PARSES the whole parenthesised block, before running any of it, so a
rem value containing a closing paren ends the block early and the rest of
rem the path becomes a stray token. Installed under "C:\Program Files
rem (x86)\Adaptoid\win64" that failed with "\Adaptoid\win64\pkg was
rem unexpected at this time" - and it failed even though the condition was
rem FALSE, because parsing happens first. Quoting works too, and is why
rem the if below is fine; !PKG! keeps quotes out of the message.
if not exist "%PKG%\%INFNAME%" (
    echo   No !INFNAME! in !PKG!
    echo   Run tools\package.cmd on the host first.
    exit /b 1
)

echo ============================================================
echo == 1. REMOVING THE DEVICE NODE
echo ==    DRIVER SELECTION IS STICKY. Once a device node has a
echo ==    driver bound, replugging does NOT reconsider it - PnP
echo ==    just restarts whatever was chosen the first time.
echo ==    Staging a better package changes nothing on its own, so
echo ==    a rebuild silently keeps running the old driver, or
echo ==    HidUsb. Deleting the node is what makes the rescan in
echo ==    step 4 run a real selection.
echo ============================================================
set "GONE=0"
for /f "usebackq tokens=*" %%K in (
    `reg query "HKLM\SYSTEM\CurrentControlSet\Enum\%HWID%" 2^>nul`
) do (
    set "KEY=%%K"
    rem SKIP THE PARENT KEY. reg query prints the queried key itself as
    rem well as its subkeys, and taking its last component yields the
    rem hardware id rather than an instance id - a removal that always
    rem fails and reports itself as pnputil being unavailable.
    set "INST="
    for %%I in ("!KEY!") do set "INST=%%~nxI"
    if /i "!INST!"=="!HWLEAF!" set "INST="
    if defined INST (
        echo   removing !HWID!\!INST!
        pnputil /remove-device "%HWID%\!INST!" >nul 2>&1
        if not errorlevel 1 set /a GONE+=1
    )
)
echo   %GONE% device node^(s^) removed.
echo.

echo ============================================================
echo == 2. REMOVING EVERY STAGED COPY OF OUR PACKAGE
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
echo == 3. INSTALLING THE NEW PACKAGE
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
echo == 4. RESCANNING SO A PRESENT DEVICE PICKS IT UP
echo ============================================================
rem RE-ENABLE THE SERVICE FIRST. pnputil /delete-driver /uninstall
rem sets our service's start type to 4, DISABLED, as part of detaching
rem it from its devices - and nothing puts it back. A disabled service
rem can never be selected for a device, so the freshly installed
rem package loses to HidUsb every time and the symptom is a correctly
rem staged, correctly signed driver that simply never binds. sc config
rem on a service that does not exist is harmless, so this is
rem unconditional.
sc config wishk300 start= demand >nul 2>&1

pnputil /scan-devices
echo.

echo ============================================================
echo == 5. RESULT
echo ============================================================
sc query wishk300 2>nul | findstr /i "SERVICE_NAME STATE"
reg query "HKLM\SYSTEM\CurrentControlSet\Enum\USB\VID_06F7&PID_0001" /s /v Service 2>nul | findstr /i "Service"
echo.
if defined WASRESIDENT (
    echo   ------------------------------------------------------------
    echo   THE OLD DRIVER WAS ALREADY LOADED WHEN THIS STARTED, so the
    echo   new file is staged but the kernel is almost certainly still
    echo   running the previous build. Everything above still reports
    echo   success; that is what makes this worth saying out loud.
    echo.
    echo   REBOOT THE GUEST. That is the only step that reliably maps
    echo   the new image, and it costs less than diagnosing a fix that
    echo   appears not to work.
    echo.
    echo   To confirm afterwards, from the host debugger:
    echo       lm vm wishk300
    echo   The load address and the PDB GUID both change on every build.
    echo   If neither moved, the old image is still running.
    echo   ------------------------------------------------------------
    echo.
)
echo   For the full picture run state.cmd.
echo   If the adapter was plugged in through this, unplug and replug it -
echo   the accessory probe only runs on device arrival.
endlocal
