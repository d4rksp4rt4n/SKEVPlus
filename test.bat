@echo off
rem Builds and runs test_evsm.exe (ShadowNoiseFix shader patch check, see test_evsm.c).
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
cl /nologo /O2 /W3 test_evsm.c /link d3d11.lib || exit /b 1
del test_evsm.obj 2>nul
test_evsm.exe %*
