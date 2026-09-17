@echo off
setlocal
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=D:\Software\VS\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" exit /b 2
call "%VCVARS%" >nul
if errorlevel 1 exit /b %errorlevel%
cl /nologo /W4 /WX /O2 tools\hvm_lab\svm_unit_tests.c /Fe:tools\hvm_lab\svm_unit_tests.exe /Fo:tools\hvm_lab\svm_unit_tests.obj
if errorlevel 1 exit /b %errorlevel%
tools\hvm_lab\svm_unit_tests.exe
if errorlevel 1 exit /b %errorlevel%
cl /nologo /W4 /WX /O2 tools\hvm_unit_tests\hvm_unit_tests.c /Fe:tools\hvm_lab\vmx_unit_tests.exe /Fo:tools\hvm_lab\vmx_unit_tests.obj
if errorlevel 1 exit /b %errorlevel%
tools\hvm_lab\vmx_unit_tests.exe
exit /b %errorlevel%
