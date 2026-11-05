@echo off
setlocal DisableDelayedExpansion
set "PYTHONUTF8=1"
cd /d "%~dp0"
where py.exe >nul 2>&1
if errorlevel 1 goto python
py -3 -c "import sys; sys.exit(sys.version_info < (3,10))" >nul 2>&1
if errorlevel 1 goto python
py -3 server.py %*
goto end
:python
python server.py %*
:end
set "result=%errorlevel%"
if not "%result%"=="0" pause
exit /b %result%
