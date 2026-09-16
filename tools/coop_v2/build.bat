@echo off
setlocal EnableExtensions

if not defined GFORCE_VCVARSALL (
	for %%P in (
		"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
		"C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat"
		"C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat"
		"C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
		"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat"
	) do (
		if not defined GFORCE_VCVARSALL if exist "%%~P" set "GFORCE_VCVARSALL=%%~P"
	)
)
if not defined GFORCE_VCVARSALL (
	echo ERROR: Visual Studio 2022 C++ Build Tools ^(v143^) were not found.
	echo Set GFORCE_VCVARSALL to a vcvarsall.bat path to use another compiler.
	exit /b 1
)
if not exist "%GFORCE_VCVARSALL%" (
	echo ERROR: GFORCE_VCVARSALL does not exist: "%GFORCE_VCVARSALL%"
	exit /b 1
)
call "%GFORCE_VCVARSALL%" x86
if errorlevel 1 exit /b 1

cd /d "%~dp0"
if not exist build mkdir build

if not defined GFORCE_RUNTIME_ROOT if defined GFORCE_DEPLOY_ROOT set "GFORCE_RUNTIME_ROOT=%GFORCE_DEPLOY_ROOT%"
if not defined GFORCE_RUNTIME_ROOT set "GFORCE_RUNTIME_ROOT=C:\GamesAndSource\SHOC\xr_build\bin\Win32\Release"
for %%F in (GameNetworkingSockets.dll steam_api.dll libprotobuf.dll libcrypto-3.dll abseil_dll.dll) do (
	if not exist "%GFORCE_RUNTIME_ROOT%\%%F" (
		echo ERROR: missing runtime dependency "%GFORCE_RUNTIME_ROOT%\%%F"
		exit /b 1
	)
)

set "GnsRuntimeRoot=%GFORCE_RUNTIME_ROOT%"
set "GFORCE_BUILD_OUTPUT=build\Release"
where msbuild.exe >nul 2>nul
if errorlevel 1 (
	echo ERROR: MSBuild was not initialized by "%GFORCE_VCVARSALL%".
	exit /b 1
)
rem A final deploy must compile every translation unit, not merely relink an
rem incremental object cache left by a previous experiment.
msbuild.exe GForceCoop.sln /t:Rebuild "/p:Configuration=Release;Platform=Win32;GnsRuntimeRoot=%GnsRuntimeRoot%" /nologo /v:minimal
if errorlevel 1 exit /b 1
for %%F in (coop_dll.dll winmm.dll) do (
	if not exist "%GFORCE_BUILD_OUTPUT%\%%F" (
		echo ERROR: solution build did not produce "%GFORCE_BUILD_OUTPUT%\%%F"
		exit /b 1
	)
)

if defined GFORCE_DEPLOY_ROOT (
	if not exist "%GFORCE_DEPLOY_ROOT%\GForce.exe" (
		echo ERROR: GFORCE_DEPLOY_ROOT is not a G-Force game directory: "%GFORCE_DEPLOY_ROOT%"
		exit /b 1
	)
	tasklist /FI "IMAGENAME eq GForce.exe" /NH | findstr /I /C:"GForce.exe" >nul
	if not errorlevel 1 (
		echo ERROR: GForce.exe is running. Close it before deploying DLLs.
		exit /b 1
	)
	for %%F in (coop_dll.dll winmm.dll) do (
		copy /Y "%GFORCE_BUILD_OUTPUT%\%%F" "%GFORCE_DEPLOY_ROOT%\%%F" >nul
		if errorlevel 1 exit /b 1
	)
	echo DEPLOY_OK: coop_dll.dll and winmm.dll copied to "%GFORCE_DEPLOY_ROOT%"
	echo NOTE: GForce.ini is deliberately left untouched; mod settings live in its [window]/[language] sections.
) else (
	echo DEPLOY_SKIPPED: set GFORCE_DEPLOY_ROOT to copy the two built mod DLLs to a closed game directory.
)
echo BUILD_OK: GForceCoop.sln Release^|Win32
