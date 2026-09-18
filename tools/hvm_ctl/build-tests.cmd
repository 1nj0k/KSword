@echo off
setlocal
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=D:\Software\VS\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" exit /b 2
call "%VCVARS%" >nul
if errorlevel 1 exit /b %errorlevel%
cl /nologo /O2 /W4 /WX /utf-8 tools\hvm_ctl\test_query_json.c /Fe:tools\hvm_ctl\test_query_json.exe /Fo:tools\hvm_ctl\test_query_json.obj /link advapi32.lib
if errorlevel 1 exit /b %errorlevel%
python tools\hvm_ctl\test_json_output.py
exit /b %errorlevel%
