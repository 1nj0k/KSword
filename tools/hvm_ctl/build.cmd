@echo off
setlocal
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=D:\Software\VS\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" exit /b 2
call "%VCVARS%" >nul
if errorlevel 1 exit /b %errorlevel%
cl /nologo /O2 /W4 /WX /utf-8 /DUNICODE /D_UNICODE tools\hvm_ctl\hvm_ctl.c /Fe:tools\hvm_ctl\hvm_ctl.exe /Fo:tools\hvm_ctl\hvm_ctl.obj /link advapi32.lib
exit /b %errorlevel%
