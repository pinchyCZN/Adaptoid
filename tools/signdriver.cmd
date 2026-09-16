@echo off
setlocal enabledelayedexpansion
rem ----------------------------------------------------------------------
rem signdriver.cmd - sign a built driver with the test certificate.
rem
rem     tools\signdriver.cmd [Platform] [Configuration]
rem
rem defaulting to x64 and Release. Run tools\mktestcert.cmd once first.
rem
rem It does two things, and BOTH are needed for a clean install:
rem
rem   1. Embedded-signs wishk300.sys. This is what lets the kernel LOAD the
rem      driver at all once test signing is on.
rem   2. Builds adaptoid.cat from adaptoid.inf and signs that. This is what
rem      lets PnP INSTALL the package without declaring it unsigned.
rem
rem THE CATALOG STEP IS THE UNCERTAIN ONE, and it is worth knowing why
rem before you rely on it. The only Inf2Cat on this machine is WDK 7.1's,
rem whose newest /os value is 7_X64 - it predates Windows 8 and cannot
rem write a Windows 10 OS attribute. A catalog built that way is correctly
rem signed but claims the wrong platform, and whether a given Windows 10
rem build accepts it has NOT been tested here. If the install refuses,
rem section 7 of src_drv\README.txt lists what to try.
rem ----------------------------------------------------------------------

set "PLATFORM=%~1"
set "CONFIG=%~2"
if "%PLATFORM%"=="" set "PLATFORM=x64"
if "%CONFIG%"==""   set "CONFIG=Release"

set "ROOT=%~dp0.."
set "SIGNDIR=%ROOT%\src_drv\build\sign"
set "OUTDIR=%ROOT%\src_drv\build\%PLATFORM%\%CONFIG%"
set "PFX=%SIGNDIR%\adaptoid-test.pfx"
set "PFXPASS=adaptoid"
if not "%~3"=="" set "PFXPASS=%~3"

if not exist "%PFX%" (
    echo   No certificate. Run tools\mktestcert.cmd first.
    exit /b 1
)
if not exist "%OUTDIR%\wishk300.sys" (
    echo   No driver at %OUTDIR%\wishk300.sys
    echo   Build it first:
    echo     msbuild src_drv\adaptoid.sln /p:Configuration=%CONFIG% /p:Platform=%PLATFORM%
    exit /b 1
)

rem ---- find the newest signtool. The Windows 10 SDK's, NOT the WDK's:
rem      the 2009 build cannot produce a SHA-256 signature.
set "SIGNTOOL="
for /f "delims=" %%I in ('dir /b /o-n "C:\Program Files (x86)\Windows Kits\10\bin\10.*" 2^>nul') do (
    if not defined SIGNTOOL (
        if exist "C:\Program Files (x86)\Windows Kits\10\bin\%%I\x64\signtool.exe" (
            set "SIGNTOOL=C:\Program Files (x86)\Windows Kits\10\bin\%%I\x64\signtool.exe"
        )
    )
)
if not defined SIGNTOOL (
    echo   No Windows 10 SDK signtool found. The WDK 7.1 one is SHA-1 only
    echo   and Windows 10 will not accept its signature on a driver.
    exit /b 1
)

rem ---- stage the package: the INF and the .sys must sit together for
rem      Inf2Cat, and the catalog is named by the INF's CatalogFile.
set "PKG=%OUTDIR%\package"
if not exist "%PKG%" mkdir "%PKG%" >nul 2>&1
copy /y "%ROOT%\src_drv\adaptoid.inf" "%PKG%\" >nul
copy /y "%OUTDIR%\wishk300.sys"       "%PKG%\" >nul

echo Signing %PLATFORM% %CONFIG%
echo   signtool  %SIGNTOOL%

rem ---- 1. the driver binary
"%SIGNTOOL%" sign /fd SHA256 /f "%PFX%" /p "%PFXPASS%" ^
    /t http://timestamp.digicert.com "%PKG%\wishk300.sys"
if errorlevel 1 (
    echo.
    echo   Signing the .sys FAILED. Without a timestamp server reachable,
    echo   drop the /t argument - a test signature does not need one.
    exit /b 1
)

rem ---- 2. the catalog. See the note at the top about 7_X64.
set "INF2CAT=E:\DEV\WinDDK\bin\selfsign\Inf2Cat.exe"
if not exist "%INF2CAT%" (
    echo.
    echo   No Inf2Cat; skipping the catalog. The .sys is signed, so the
    echo   driver will LOAD, but the INF install will report the package
    echo   as unsigned.
    goto done
)

"%INF2CAT%" /driver:"%PKG%" /os:7_X64,7_X86 /verbose
if errorlevel 1 (
    echo.
    echo   Inf2Cat FAILED. Common cause: the INF names a CatalogFile that
    echo   does not match, or references a file not present in %PKG%.
    echo   The .sys is still signed and will load.
    goto done
)

"%SIGNTOOL%" sign /fd SHA256 /f "%PFX%" /p "%PFXPASS%" ^
    /t http://timestamp.digicert.com "%PKG%\adaptoid.cat"
if errorlevel 1 (
    echo   Signing the catalog FAILED. The .sys is still signed.
    goto done
)

:done
echo.
echo   package ready:  %PKG%
dir /b "%PKG%"
echo.
echo   Copy that folder to the test machine and install with:
echo     pnputil /add-driver adaptoid.inf /install
echo   or right-click the INF and choose Install.
echo.
rem ---- report what was actually applied.
rem
rem      AN UNTRUSTED-ROOT STATUS HERE IS THE EXPECTED ANSWER. This machine
rem      has not been told to trust the test root; the VM will be, by step 1
rem      of the instructions mktestcert.cmd prints. What matters below is
rem      that a signature EXISTS, names the test certificate, and is
rem      sha256RSA - signtool verify would just say "failed" and hide all
rem      three facts.
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "Get-ChildItem '%PKG%' -Include *.sys,*.cat -Recurse |" ^
  " Get-AuthenticodeSignature -ErrorAction SilentlyContinue |" ^
  " Format-Table @{n='file';e={Split-Path $_.Path -Leaf}}," ^
  " @{n='signer';e={$_.SignerCertificate.Subject}}," ^
  " @{n='algorithm';e={$_.SignerCertificate.SignatureAlgorithm.FriendlyName}}" ^
  " -AutoSize"
echo   An untrusted root on THIS machine is expected; the VM fixes it.
endlocal
exit /b 0
