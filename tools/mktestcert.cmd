@echo off
setlocal enabledelayedexpansion
rem ----------------------------------------------------------------------
rem mktestcert.cmd - make the test-signing certificate. RUN THIS ONCE.
rem
rem Produces two files under src_drv\build\sign\, which is inside the
rem gitignored build tree because one of them holds a PRIVATE KEY:
rem
rem     adaptoid-test.pfx   the key, used by tools\signdriver.cmd
rem     adaptoid-test.cer   the public half, to install on the test VM
rem
rem THIS IS A SELF-SIGNED TEST CERTIFICATE AND NOTHING ELSE. It will not
rem satisfy a production machine; it exists so that a VM with test signing
rem turned on will load the driver. Do not treat it as a signing identity.
rem
rem New-SelfSignedCertificate rather than the WDK's makecert.exe: makecert
rem is deprecated and its 2009 build defaults to SHA-1, which Windows 10
rem does not accept for a kernel-mode signature.
rem ----------------------------------------------------------------------

set "OUT=%~dp0..\src_drv\build\sign"
set "SUBJECT=CN=Adaptoid Test Signing"
set "PFXPASS=adaptoid"

if not "%~1"=="" set "PFXPASS=%~1"

if not exist "%OUT%" mkdir "%OUT%" >nul 2>&1

if exist "%OUT%\adaptoid-test.pfx" (
    echo.
    echo   A certificate already exists:
    echo     !OUT!\adaptoid-test.pfx
    echo.
    echo   Delete it first if you really want a new one. Replacing it means
    echo   re-installing the .cer on every test machine that trusts it.
    exit /b 1
)

echo Creating a code-signing certificate, SHA-256, valid ten years...

powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$ErrorActionPreference='Stop';" ^
  "$c = New-SelfSignedCertificate -Type CodeSigningCert" ^
  " -Subject '%SUBJECT%' -CertStoreLocation Cert:\CurrentUser\My" ^
  " -HashAlgorithm SHA256 -KeyExportPolicy Exportable" ^
  " -NotAfter (Get-Date).AddYears(10);" ^
  "$p = ConvertTo-SecureString -String '%PFXPASS%' -Force -AsPlainText;" ^
  "Export-PfxCertificate -Cert $c -FilePath '%OUT%\adaptoid-test.pfx'" ^
  " -Password $p | Out-Null;" ^
  "Export-Certificate -Cert $c -FilePath '%OUT%\adaptoid-test.cer'" ^
  " | Out-Null;" ^
  "Write-Host ('  thumbprint  ' + $c.Thumbprint)"

if errorlevel 1 (
    echo.
    echo   FAILED. New-SelfSignedCertificate needs Windows 8 or later and
    echo   an elevated shell is not required, but an execution policy that
    echo   blocks -Command is.
    exit /b 1
)

echo.
echo   wrote  %OUT%\adaptoid-test.pfx   (private key - never commit this)
echo   wrote  %OUT%\adaptoid-test.cer   (public half - copy to the VM)
echo.
echo   NEXT, ON THE TEST MACHINE, and all three are needed:
echo.
echo     1. Trust the certificate. From an ELEVATED prompt, with the .cer
echo        copied over:
echo.
echo          certutil -addstore -f Root           adaptoid-test.cer
echo          certutil -addstore -f TrustedPublisher adaptoid-test.cer
echo.
echo        Root alone is not enough: the driver loads but the INF install
echo        still prompts, because PnP checks TrustedPublisher.
echo.
echo     2. Allow test-signed drivers, then REBOOT:
echo.
echo          bcdedit /set testsigning on
echo.
echo     3. Sign each build with tools\signdriver.cmd before copying it.
echo.
endlocal
exit /b 0
