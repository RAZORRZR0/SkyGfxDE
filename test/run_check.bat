@echo off
:: Real-binary check. Usage: test\run_check.bat <path\to\SanAndreas.exe> <path\to\timecyc.dat>
:: Writes test\check_result.txt (repeatable artifact).
setlocal
cd /d "%~dp0\.."
if not defined DevEnvDir (
    if exist "C:\BuildTools\VC\Auxiliary\Build\vcvars64.bat" (
        call "C:\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
    ) else (
        call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
    )
)
if not exist "obj\test" mkdir "obj\test"
cl.exe /nologo /O2 /W3 /MD /EHsc /std:c++20 /D_CRT_SECURE_NO_WARNINGS /Iminhook /Fo:obj\test\ /Fe:obj\test\offline_check.exe ^
    test\offline_check.cpp minhook\buffer.c minhook\hook.c minhook\trampoline.c minhook\hde\hde64.c user32.lib || exit /b 1
obj\test\offline_check.exe %1 %2 test\check_result.txt
