@echo off
setlocal
rem ----------------------------------------------------------------------
rem trustcert.cmd - install the test certificate so a test-signed driver
rem will load. GUEST side, MUST BE ELEVATED. Run once per VM.
rem
rem     trustcert.cmd [path-to-adaptoid-test.cer]
rem
rem BOTH STORES ARE REQUIRED, and this is the usual reason a correctly
rem signed driver is still refused:
rem
rem   Root             makes the chain verifiable at all. Without it the
rem                    signature terminates in an untrusted root.
rem   TrustedPublisher makes Windows willing to LOAD it without asking.
rem                    Without it the chain verifies and the driver is
rem                    still blocked.
rem
rem The symptom of missing either is the same and it is not an error
rem message: pnputil /enum-drivers reports the package with
rem
rem     Signer Name:    Unknown
rem
rem while every healthy package names its signer. An unsigned kernel
rem driver cannot load on x64, so the device silently falls through to
rem Microsoft's HidUsb on the compatible-ID match and looks like a plain
rem game controller.
rem
rem TEST SIGNING MUST ALSO BE ON, and on Win10/11 Secure Boot must be off
rem for it to take effect. This script checks and reports both; it does
rem not change them, because both need a reboot and that is your call.
rem ----------------------------------------------------------------------

set "CER=%~1"
if "%CER%"=="" (
    if exist "Z:\adaptoid-test.cer" set "CER=Z:\adaptoid-test.cer"
)
if "%CER%"=="" (
    if exist "B:\adaptoid-test.cer" set "CER=B:\adaptoid-test.cer"
)
if "%CER%"=="" (
    if exist "%~dp0adaptoid-test.cer" set "CER=%~dp0adaptoid-test.cer"
)

net session >nul 2>&1
if errorlevel 1 (
    echo   NOT ELEVATED. Run this from an Administrator command prompt.
    exit /b 1
)
if not exist "%CER%" (
    echo   No certificate found. Pass its path, or stage it with
    echo   tools\package.cmd on the host.
    exit /b 1
)

echo   using %CER%
echo.
echo ============================================================
echo == INSTALLING INTO BOTH REQUIRED STORES
echo ============================================================
certutil -addstore -f Root "%CER%"
certutil -addstore -f TrustedPublisher "%CER%"
echo.

echo ============================================================
echo == VERIFYING
echo ============================================================
set "OK=1"
certutil -store Root | findstr /i "Adaptoid" >nul
if errorlevel 1 (
    echo   MISSING from Root
    set "OK="
) else (
    echo   present in Root
)
certutil -store TrustedPublisher | findstr /i "Adaptoid" >nul
if errorlevel 1 (
    echo   MISSING from TrustedPublisher
    set "OK="
) else (
    echo   present in TrustedPublisher
)
echo.

echo ============================================================
echo == THE OTHER TWO PRECONDITIONS
echo ============================================================
bcdedit | findstr /i "testsigning"
if errorlevel 1 (
    echo   testsigning is NOT set. Turn it on and reboot:
    echo       bcdedit /set testsigning on
)
rem SECURE BOOT, READ FROM THE REGISTRY rather than msinfo32: the value
rem only exists when the machine booted UEFI, so its ABSENCE is the
rem answer "legacy BIOS, Secure Boot cannot be in the way" - which is the
rem default for a VirtualBox VM.
set "SBKEY=HKLM\SYSTEM\CurrentControlSet\Control\SecureBoot\State"
reg query "%SBKEY%" /v UEFISecureBootEnabled >nul 2>&1
if errorlevel 1 (
    echo   Secure Boot: not present, legacy BIOS boot - nothing to disable.
) else (
    for /f "tokens=3" %%V in (
        'reg query "%SBKEY%" /v UEFISecureBootEnabled ^| findstr /i UEFISecureBootEnabled'
    ) do (
        if /i "%%V"=="0x0" (
            echo   Secure Boot: OFF
        ) else (
            echo   Secure Boot: ON - testsigning does nothing until it is off.
        )
    )
)
echo.

if defined OK (
    echo   Certificate is in place. Re-run deploy.cmd, then replug the
    echo   adapter, then state.cmd - Signer Name should no longer read
    echo   Unknown.
) else (
    echo   Certificate did NOT install into both stores. Nothing else
    echo   will work until it does.
)
endlocal
