@echo off
setlocal

set FQBN=rp2040:rp2040:waveshare_rp2350_plus:usbstack=tinyusb
set SKETCH=RP2350_ETH_MSC.ino
set OUTDIR=build

echo Building firmware (pure MSC, no CDC)...
echo FQBN: %FQBN%
echo.

arduino-cli compile ^
  --fqbn "%FQBN%" ^
  --build-property "build.extra_flags=-DCFG_TUD_CDC=0" ^
  --output-dir %OUTDIR% ^
  %SKETCH%

if errorlevel 1 (
  echo.
  echo Build FAILED.
  exit /b 1
)

echo.
echo Build OK.
echo   Flash: copy %OUTDIR%\%SKETCH%.uf2  to RP2350 BOOTSEL drive
echo.
