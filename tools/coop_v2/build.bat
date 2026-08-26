@echo off
setlocal

call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x86
if errorlevel 1 exit /b 1

cd /d "%~dp0"
if not exist build mkdir build

cl.exe /nologo /W4 /O2 /GS /sdl /guard:cf /MT /EHsc /LD /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /I. coop_application.cpp coop_dll.cpp coop_runtime.cpp coop_netgame.cpp ip_connect_dialog.cpp player2.cpp save_sync.cpp window_hook.cpp world_sync.cpp ServerClient\MClient.cpp ServerClient\MClientONLINE.cpp ServerClient\MServer.cpp ServerClient\MServerONLINE.cpp ServerClient\MStandalone.cpp ServerClient\MTypes.cpp ServerClient\SteamManager.cpp /Fo:build\ /Fe:build\coop_dll.dll /link /DEF:coop_dll.def /LIBPATH:"%~dp0SteamWorksSDK" GameNetworkingSockets.lib steam_api.lib bcrypt.lib user32.lib /PDB:build\coop_dll.pdb /MAP:build\coop_dll.map
if errorlevel 1 exit /b 1

cl.exe /nologo /W4 /O2 /GS /sdl /guard:cf /MT /EHsc /LD /DUNICODE /D_UNICODE winmm_proxy.cpp /Fo:build\winmm_proxy.obj /Fe:build\winmm.dll /link /DEF:winmm_proxy.def /PDB:build\winmm.pdb
if errorlevel 1 exit /b 1

cl.exe /nologo /W4 /O2 /GS /sdl /guard:cf /MT /EHsc proxy_smoke.cpp /Fo:build\proxy_smoke.obj /Fe:build\proxy_smoke.exe /link /PDB:build\proxy_smoke.pdb
if errorlevel 1 exit /b 1

copy /Y coop.ini build\coop.ini >nul
copy /Y "C:\GamesAndSource\SHOC\xr_build\bin\Win32\Release\GameNetworkingSockets.dll" build\GameNetworkingSockets.dll >nul
copy /Y "C:\GamesAndSource\SHOC\xr_build\bin\Win32\Release\steam_api.dll" build\steam_api.dll >nul
copy /Y "C:\GamesAndSource\SHOC\xr_build\bin\Win32\Release\libprotobuf.dll" build\libprotobuf.dll >nul
copy /Y "C:\GamesAndSource\SHOC\xr_build\bin\Win32\Release\libcrypto-3.dll" build\libcrypto-3.dll >nul
copy /Y "C:\GamesAndSource\SHOC\xr_build\bin\Win32\Release\abseil_dll.dll" build\abseil_dll.dll >nul
echo BUILD_OK
