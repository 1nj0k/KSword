@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b %errorlevel%
cl /nologo /O2 /W4 /WX /utf-8 /DUNICODE /D_UNICODE tools\hvm_ctl\hvm_ctl.c /Fe:tools\hvm_ctl\hvm_ctl.exe /Fo:tools\hvm_ctl\hvm_ctl.obj /link advapi32.lib
exit /b %errorlevel%
