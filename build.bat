@echo off
setlocal enabledelayedexpansion
:: Builds bin\SkyGfxDE.asi (x64). Usage: build.bat [nopause]
cd /d "%~dp0"

if not defined DevEnvDir (
    set "VCVARS="
    for %%P in (
        "C:\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
        "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
        "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
        "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
    ) do if not defined VCVARS if exist %%P set "VCVARS=%%~P"
    if not defined VCVARS (
        echo [ERROR] Visual Studio x64 build tools not found. Run from an x64 Developer Command Prompt.
        if not "%1"=="nopause" pause
        exit /b 1
    )
    call "!VCVARS!" >nul
)

if not exist "bin" mkdir "bin"
if not exist "obj" mkdir "obj"
cl.exe /nologo /O2 /W3 /MD /EHsc /std:c++20 /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /Iminhook /Fo:obj\ ^
    src\dllmain.cpp src\core.cpp src\overlay.cpp src\postfx.cpp src\tools.cpp src\peds.cpp src\coronas.cpp src\streetlights.cpp ^
    imgui\imgui.cpp imgui\imgui_draw.cpp imgui\imgui_tables.cpp imgui\imgui_widgets.cpp imgui\imgui_impl_dx11.cpp imgui\imgui_impl_win32.cpp ^
    minhook\buffer.c minhook\hook.c minhook\trampoline.c minhook\hde\hde64.c ^
    /link /DLL /OUT:bin\SkyGfxDE.asi user32.lib d3d11.lib dxgi.lib
if errorlevel 1 (
    echo [ERROR] build failed
    if not "%1"=="nopause" pause
    exit /b 1
)
copy /y SkyGfxDE.ini bin\SkyGfxDE.ini >nul
copy /y data\SALodLights.dat bin\SALodLights.dat >nul
copy /y data\timecyc_ps2.dat bin\timecyc_ps2.dat >nul
echo Built bin\SkyGfxDE.asi
