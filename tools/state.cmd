@echo off
setlocal
rem ----------------------------------------------------------------------
rem state.cmd - what is actually installed and bound right now. GUEST side.
rem Read only. Answers "did my build even land" in one step.
rem
rem     state.cmd [output-file]
rem
rem With no argument it writes adaptoid-state.txt BESIDE THIS SCRIPT.
rem
rem THERE IS NO TEST HERE FOR \\.\Wish_NA1, deliberately. cmd's "if exist"
rem does not work on device paths: it reports ABSENT for a control device
rem that is present and openable, which is a false negative that has
rem already cost this project a wrong diagnosis. The only honest test is
rem to open the device, so run pakread.exe - it prints whether the open
rem succeeded before it does anything else.
rem ----------------------------------------------------------------------

rem ONE DESTINATION, WRITTEN DIRECTLY. %~dp0 is this script's own folder
rem and ALREADY ENDS IN A BACKSLASH, so %~dp0adaptoid-state.txt needs no
rem separator of its own. Run from a share and the report lands in the
rem same folder the other side reads.
rem
rem A READ-ONLY DESTINATION FAILS LOUDLY HERE RATHER THAN QUIETLY BELOW.
rem A VirtualBox shared folder is read-only unless it was explicitly made
rem writable, and redirecting onto one fails EVERY line in this script
rem with "The system cannot find the path specified" - dozens of identical
rem errors that read as the registry queries failing rather than as the
rem output file never having been created. One probe up front turns that
rem into a single sentence naming the real problem.
set "OUT=%~1"
if "%OUT%"=="" set "OUT=%~dp0adaptoid-state.txt"

rem QUOTE A PATH INSIDE A BLOCK. cmd expands %VAR% while it PARSES the
rem whole parenthesised block, before running any of it, so a value
rem holding a closing paren ends the block early and the rest becomes a
rem stray token. Under "C:\Program Files (x86)\Adaptoid\win64" that is
rem "\Adaptoid\win64\... was unexpected at this time", and it happens
rem even when the condition is FALSE, because parsing comes first.
break > "%OUT%" 2>nul
if not exist "%OUT%" (
    echo   Cannot write "%OUT%"
    echo.
    echo   That folder is read-only. Either make it writable, or give
    echo   this script a path that is not:
    echo       state.cmd %%TEMP%%\adaptoid-state.txt
    exit /b 1
)

echo Adaptoid install state > "%OUT%"
echo Collected %DATE% %TIME% >> "%OUT%"
echo. >> "%OUT%"

echo ============================================================ >> "%OUT%"
echo == 1. OUR STAGED DRIVER PACKAGES                          == >> "%OUT%"
echo ==    MORE THAN ONE means old copies are still ranked      == >> "%OUT%"
echo ==    against the new one. deploy.cmd clears them.         == >> "%OUT%"
echo ============================================================ >> "%OUT%"
pnputil /enum-drivers >> "%OUT%" 2>&1
echo. >> "%OUT%"

echo ============================================================ >> "%OUT%"
echo == 2. THE SERVICE                                         == >> "%OUT%"
echo ============================================================ >> "%OUT%"
sc query wishk300 >> "%OUT%" 2>&1
echo. >> "%OUT%"
sc qc wishk300 >> "%OUT%" 2>&1
echo. >> "%OUT%"

echo ============================================================ >> "%OUT%"
echo == 3. WHICH DRIVER OWNS THE DEVICE                        == >> "%OUT%"
echo ==    Service should read wishk300. HidUsb means our INF   == >> "%OUT%"
echo ==    did not win the ranking and Microsoft's driver took  == >> "%OUT%"
echo ==    the compatible-ID match instead.                     == >> "%OUT%"
echo ============================================================ >> "%OUT%"
reg query "HKLM\SYSTEM\CurrentControlSet\Enum\USB\VID_06F7&PID_0001" /s /v Service >> "%OUT%" 2>&1
echo. >> "%OUT%"
reg query "HKLM\SYSTEM\CurrentControlSet\Enum\USB\VID_06F7&PID_0001" /s /v ClassGUID >> "%OUT%" 2>&1
echo. >> "%OUT%"

echo ============================================================ >> "%OUT%"
echo == 4. THE HID CHILDREN hidclass CREATED                   == >> "%OUT%"
echo ==    With the mask at its default 7 the descriptor has    == >> "%OUT%"
echo ==    three top-level collections, so expect THREE:        == >> "%OUT%"
echo ==      Col01 mouse, Col02 keyboard, Col03 joystick        == >> "%OUT%"
echo ==    Fewer means the descriptor or the mask is wrong.     == >> "%OUT%"
echo ============================================================ >> "%OUT%"
reg query "HKLM\SYSTEM\CurrentControlSet\Enum\HID" /s /f "06F7" >> "%OUT%" 2>&1
echo. >> "%OUT%"

echo ============================================================ >> "%OUT%"
echo == 5. THE DRIVER'S OWN SETTING                            == >> "%OUT%"
echo ==    VirtualDevices ABSENT IS CORRECT - the driver falls  == >> "%OUT%"
echo ==    back to 7. Its presence is an override, not a        == >> "%OUT%"
echo ==    requirement.                                         == >> "%OUT%"
echo ============================================================ >> "%OUT%"
reg query "HKLM\Software\Wish Technologies\Adaptoid" /s >> "%OUT%" 2>&1
echo. >> "%OUT%"

echo ============================================================ >> "%OUT%"
echo == 6. THE DIRECTINPUT BUTTON NAMES                        == >> "%OUT%"
echo ==    Proves the INF's AddReg sections ran.                == >> "%OUT%"
echo ============================================================ >> "%OUT%"
reg query "HKLM\System\CurrentControlSet\Control\MediaProperties\PrivateProperties\Joystick\OEM\VID_06F7&PID_0001\Buttons" /s >> "%OUT%" 2>&1
echo. >> "%OUT%"

echo ============================================================ >> "%OUT%"
echo == 7. IS IT LOADED                                        == >> "%OUT%"
echo ============================================================ >> "%OUT%"
driverquery /v | findstr /i "wishk300" >> "%OUT%" 2>&1
echo. >> "%OUT%"

echo ============================================================ >> "%OUT%"
echo == 8. GHOST DEVICE NODES                                  == >> "%OUT%"
echo ==    Anything that is not the live instance is a leftover.== >> "%OUT%"
echo ============================================================ >> "%OUT%"
reg query "HKLM\SYSTEM\CurrentControlSet\Enum\USB" /s /f "06F7" /k >> "%OUT%" 2>&1
echo. >> "%OUT%"

echo.
echo ============================================================
echo == THE TWO ANSWERS THAT MATTER
echo ==   Service should read wishk300. HidUsb means our package
echo ==   lost the ranking, or our driver refused to start.
echo ==   Expect THREE HID children: Col01 Col02 Col03.
echo ============================================================
findstr /i /c:"SERVICE_NAME" /c:"STATE" "%OUT%"
findstr /i /c:"    Service    REG_SZ" "%OUT%"
findstr /i /c:"HID\VID_06F7" "%OUT%"
echo.

echo   Full report at %OUT%
endlocal
