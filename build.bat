@echo off
setlocal

set DIST=dist
set PYINST=pyinstaller

echo Building test_bridge.exe (writer + TCP server)...
%PYINST% --onefile --console --name test_bridge --distpath %DIST% test_bridge.py
if errorlevel 1 goto :err

echo.
echo Building tcp_monitor.exe (TCP server only)...
%PYINST% --onefile --console --name tcp_monitor --distpath %DIST% tcp_monitor.py
if errorlevel 1 goto :err

echo.
echo Done.
echo   dist\test_bridge.exe  - simulator + server
echo   dist\tcp_monitor.exe  - server only
echo.
echo Usage:
echo   test_bridge.exe [drive]     e.g. test_bridge.exe D
echo   tcp_monitor.exe [port]      e.g. tcp_monitor.exe 2000
goto :eof

:err
echo Build failed.
exit /b 1
