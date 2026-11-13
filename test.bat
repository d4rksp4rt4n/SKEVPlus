@echo off
rem Builds and runs test_mipshader.exe (MipFix shader patch check, see test_mipshader.c).
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d "%~dp0"
cl /nologo /O2 /W3 test_mipshader.c /link d3d11.lib || exit /b 1
del test_mipshader.obj 2>nul
.\test_mipshader.exe %*
