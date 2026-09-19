@echo off
setlocal enabledelayedexpansion
rem ----------------------------------------------------------------------
rem package.cmd - build, stamp, sign and stage a driver package. HOST side.
rem
rem     tools\package.cmd [dest] [nostamp]
rem
rem Default dest is B:\pkg, the share the guest reads. One command turns a
rem source tree into a folder the guest's deploy.cmd can install.
rem
rem WHY IT STAMPS DriverVer. Windows ranks driver packages and a stale
rem staged copy with an equal or higher version can win, so a rebuilt
rem driver silently does not deploy. deploy.cmd deletes our old packages
rem first, which also solves it, but the two together mean a partial
rem delete cannot leave an old build in charge. The stamp is written into
rem src_drv\adaptoid.inf IN PLACE, because the version shipped has to be
rem the version in the source - pass nostamp to leave it alone.
rem
rem The version is 3.0.MMdd.HHmm. That rises through a development period
rem but resets at a year boundary; if that ever matters, widen it.
rem ----------------------------------------------------------------------

set "DEST=%~1"
if "%DEST%"=="" set "DEST=B:\pkg"
set "STAMP=1"
if /i "%~2"=="nostamp" set "STAMP="
if /i "%~1"=="nostamp" (set "STAMP=" & set "DEST=B:\pkg")

set "ROOT=%~dp0.."
set "INF=%ROOT%\src_drv\adaptoid.inf"
set "PKG=%ROOT%\src_drv\build\x64\Release\package"

rem ---- 1. the version stamp ------------------------------------------
if defined STAMP (
    rem THE SLASHES HAVE TO BE FORCED. In a .NET format string "/" is the
    rem CULTURE'S date separator, not a literal - on a machine whose short
    rem date uses dashes, -Format 'MM/dd/yyyy' yields 09-16-2026 and
    rem Inf2Cat rejects it with "DriverVer missing or in incorrect format".
    rem InvariantCulture is what pins it to slashes.
    for /f "usebackq delims=" %%D in (
        `powershell -NoProfile -Command "(Get-Date).ToString('MM/dd/yyyy',[Globalization.CultureInfo]::InvariantCulture)"`
    ) do set "DVDATE=%%D"
    for /f "usebackq delims=" %%V in (
        `powershell -NoProfile -Command "Get-Date -Format 'MMdd.HHmm'"`
    ) do set "DVVER=%%V"
    set "DRIVERVER=!DVDATE!,3.0.!DVVER!"
    echo   stamping DriverVer = !DRIVERVER!
    powershell -NoProfile -Command ^
      "$p='%INF%'; $t=[IO.File]::ReadAllText($p);" ^
      "$t=[Text.RegularExpressions.Regex]::Replace($t," ^
      "'(?m)^DriverVer\s*=.*$','DriverVer   = !DRIVERVER!');" ^
      "[IO.File]::WriteAllText($p,$t)"
    if errorlevel 1 (
        echo   FAILED to stamp the INF.
        exit /b 1
    )
)

rem ---- 2. build --------------------------------------------------------
set "MSBUILD="
for %%E in (Professional Enterprise Community) do (
    if not defined MSBUILD (
        set "C=C:\Program Files (x86)\Microsoft Visual Studio\2017\%%E\MSBuild\15.0\Bin\MSBuild.exe"
        if exist "!C!" set "MSBUILD=!C!"
    )
)
if not defined MSBUILD (
    echo   No VS2017 MSBuild found.
    exit /b 1
)

echo   building x64 Release ...
"%MSBUILD%" "%ROOT%\src_drv\adaptoid.sln" /p:Configuration=Release ^
    /p:Platform=x64 /v:minimal /nologo
if errorlevel 1 (
    echo   BUILD FAILED.
    exit /b 1
)

rem ---- 3. sign, which also assembles %PKG% and builds the catalogue ----
echo   signing ...
call "%~dp0signdriver.cmd" x64 Release
if errorlevel 1 (
    echo   SIGNING FAILED.
    exit /b 1
)

rem ---- 4. stage for the guest -----------------------------------------
rem Wipe the destination rather than copying over it, so a file that a
rem build stopped producing cannot linger and get installed.
if exist "%DEST%" rmdir /s /q "%DEST%"
mkdir "%DEST%" 2>nul
copy /y "%PKG%\*" "%DEST%\" >nul
if errorlevel 1 (
    echo   could not stage to !DEST!
    exit /b 1
)

rem ---- 5. the guest-side scripts ---------------------------------------
rem The guest cannot see the source tree, only the share, so the scripts
rem it has to run have to travel with the package. They go BESIDE the
rem package folder rather than inside it, to keep that folder to exactly
rem the files the catalogue covers.
for %%S in (deploy.cmd state.cmd undeploy.cmd trustcert.cmd) do (
    if exist "%~dp0%%S" copy /y "%~dp0%%S" "%DEST%\..\" >nul
)
rem THE CERTIFICATE HAS TO TRAVEL TOO. Without it in the guest's Root and
rem TrustedPublisher stores the package stages but reports
rem "Signer Name: Unknown", the driver is refused as unsigned, and the
rem device falls through to HidUsb looking like a plain game controller.
copy /y "%ROOT%\src_drv\build\sign\adaptoid-test.cer" "%DEST%\..\" >nul 2>&1
if errorlevel 1 echo   WARNING: no adaptoid-test.cer to stage.
if exist "%ROOT%\src_drv\build\x64\Release\pakread.exe" (
    copy /y "%ROOT%\src_drv\build\x64\Release\pakread.exe" "%DEST%\..\" >nul
)

echo.
echo   staged to %DEST%
dir /b "%DEST%"
echo.
echo   In the guest, from an ELEVATED prompt:
echo       %DEST%\..\deploy.cmd
echo       %DEST%\..\state.cmd
endlocal
