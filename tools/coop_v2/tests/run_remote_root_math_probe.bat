@echo off
setlocal
if not defined GFORCE_VCVARSALL (
    for %%P in (
        "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
        "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
        "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat"
    ) do (
        if not defined GFORCE_VCVARSALL if exist "%%~P" set "GFORCE_VCVARSALL=%%~P"
    )
)
if not defined GFORCE_VCVARSALL exit /b 1
call "%GFORCE_VCVARSALL%" x86
if errorlevel 1 exit /b 1
cd /d "%~dp0.."
if not exist build mkdir build
cl.exe /nologo /EHsc /std:c++17 /Fo:build\remote_root_math_probe.obj tests\remote_root_math_probe.cpp /Fe:build\remote_root_math_probe.exe
if errorlevel 1 exit /b 1
build\remote_root_math_probe.exe
