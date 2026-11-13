@echo off
rem Builds dinput8.dll (64-bit) with the MSVC Build Tools and copies it to the game folder (skmod.ini only if missing there).
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
ml64 /nologo /c /Fostubs.obj stubs.asm || exit /b 1
rc /nologo /fo dinput8.res version.rc || exit /b 1
cl /nologo /O2 /W3 /GS- /LD /MT skmod.c stubs.obj dinput8.res /link /OUT:dinput8.dll kernel32.lib user32.lib || exit /b 1
del *.obj *.res *.lib *.exp 2>nul
copy /y dinput8.dll "%~dp0..\" >nul && echo Copied dinput8.dll to the game folder.
if not exist "%~dp0..\skmod.ini" copy skmod.ini "%~dp0..\" >nul && echo Copied skmod.ini to the game folder.
