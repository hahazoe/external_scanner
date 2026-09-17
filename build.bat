@echo off
REM Build the 32-bit injected profiler and standalone DebugView.
setlocal
set "SRC=%~dp0"
set "OUT=%SRC%build"
set "GEN=%OUT%\generated"
set "LUA_INC=%SRC%..\Lua_5_1_4\src"
set "IMGUI=%SRC%third_party\imgui"
set "IMPLOT=%SRC%third_party\implot"

if not exist "%OUT%" mkdir "%OUT%"
if not exist "%GEN%" mkdir "%GEN%"
if not exist "%LUA_INC%\lua.h" (echo [X] Lua 5.1.4 headers not found: "%LUA_INC%" && exit /b 1)
if not exist "%IMGUI%\imgui.cpp" (echo [X] Dear ImGui sources not found && exit /b 1)
if not exist "%IMPLOT%\implot.cpp" (echo [X] ImPlot sources not found && exit /b 1)
where gcc >nul 2>&1 || (echo [X] gcc not found && exit /b 1)
where g++ >nul 2>&1 || (echo [X] g++ not found && exit /b 1)

set "CFLAGS=-m32 -O2 -Wall"
set "CXXFLAGS=-m32 -O2 -std=gnu++11 -Wall -DCOCOS_DEBUG_IMGUI_NO_DWMAPI -DIMGUI_IMPL_WIN32_DISABLE_GAMEPAD -include %SRC%debugview\mingw_compat.h"

echo.
echo === Step 1: embed headless Lua bootstrap ===
powershell -NoProfile -ExecutionPolicy Bypass -File "%SRC%injection\generate_embedded.ps1" -Mode Lua -SourceRoot "%SRC%." -Output "%GEN%\bootstrap_lua_data.h"
if errorlevel 1 (echo [X] FAILED && exit /b 1)
echo [OK]

echo === Step 2: injected profiler DLL ===
gcc %CFLAGS% -I"%LUA_INC%" -I"%SRC%common" -I"%SRC%injection" -shared ^
  -o "%GEN%\inject_exec.dll" ^
  "%SRC%injection\injected_runtime.c" "%SRC%injection\telemetry_writer.c" ^
  "%SRC%injection\luac_dump.c" "%SRC%injection\lua_profiler.c" ^
  "%SRC%injection\lua_api_hooks.c" ^
  -lpsapi -static-libgcc
if errorlevel 1 (echo [X] FAILED && exit /b 1)
echo [OK]

echo === Step 3: embed injected DLL ===
powershell -NoProfile -ExecutionPolicy Bypass -File "%SRC%injection\generate_embedded.ps1" -Mode Dll -SourceRoot "%SRC%." -Output "%GEN%\inject_dll_data.h"
if errorlevel 1 (echo [X] FAILED && exit /b 1)
echo [OK]

echo === Step 4: compile injector module ===
gcc %CFLAGS% -I"%SRC%common" -I"%SRC%injection" -I"%GEN%" ^
  -c "%SRC%injection\injector.c" -o "%GEN%\injector.o"
if errorlevel 1 (echo [X] FAILED && exit /b 1)
gcc %CFLAGS% -I"%SRC%common" -I"%SRC%injection" ^
  -c "%SRC%injection\lua_vm_scan.c" -o "%GEN%\lua_vm_scan.o"
if errorlevel 1 (echo [X] FAILED && exit /b 1)
echo [OK]

echo === Step 5: IPC self-test ===
gcc %CFLAGS% -I"%SRC%common" -I"%SRC%injection" ^
  -o "%GEN%\ipc_selftest.exe" "%SRC%tests\ipc_selftest.c" "%SRC%injection\telemetry_writer.c" ^
  -static-libgcc
if errorlevel 1 (echo [X] FAILED && exit /b 1)
"%GEN%\ipc_selftest.exe"
if errorlevel 1 (echo [X] FAILED && exit /b 1)
echo [OK]

echo === Step 6: precise Lua profiler core test ===
gcc %CFLAGS% -DCOCOS_DEBUG_LUA_PROFILER_TESTING -I"%LUA_INC%" -I"%SRC%common" -I"%SRC%injection" ^
  -o "%GEN%\lua_profiler_core_test.exe" "%SRC%tests\lua_profiler_core_test.c" ^
  "%SRC%injection\lua_profiler.c" -static-libgcc
if errorlevel 1 (echo [X] FAILED && exit /b 1)
"%GEN%\lua_profiler_core_test.exe"
if errorlevel 1 (echo [X] FAILED && exit /b 1)
echo [OK]

echo === Step 7: Lua 5.1 integration test ===
gcc %CFLAGS% -Wno-misleading-indentation -Wno-format -Wno-unused-function ^
  -DLUA_USE_WINDOWS -I"%LUA_INC%" -I"%SRC%common" -I"%SRC%injection" ^
  -o "%GEN%\lua_profiler_lua51_test.exe" ^
  "%SRC%tests\lua_profiler_lua51_test.c" "%SRC%injection\lua_profiler.c" ^
  "%SRC%injection\lua_api_hooks.c" ^
  "%LUA_INC%\lapi.c" "%LUA_INC%\lauxlib.c" "%LUA_INC%\lbaselib.c" ^
  "%LUA_INC%\lcode.c" "%LUA_INC%\ldblib.c" "%LUA_INC%\ldebug.c" ^
  "%LUA_INC%\ldo.c" "%LUA_INC%\ldump.c" "%LUA_INC%\lfunc.c" ^
  "%LUA_INC%\lgc.c" "%LUA_INC%\linit.c" "%LUA_INC%\liolib.c" ^
  "%LUA_INC%\llex.c" "%LUA_INC%\lmathlib.c" "%LUA_INC%\lmem.c" ^
  "%LUA_INC%\loadlib.c" "%LUA_INC%\lobject.c" "%LUA_INC%\lopcodes.c" ^
  "%LUA_INC%\loslib.c" "%LUA_INC%\lparser.c" "%LUA_INC%\lstate.c" ^
  "%LUA_INC%\lstring.c" "%LUA_INC%\lstrlib.c" "%LUA_INC%\ltable.c" ^
  "%LUA_INC%\ltablib.c" "%LUA_INC%\ltm.c" "%LUA_INC%\lundump.c" ^
  "%LUA_INC%\lvm.c" "%LUA_INC%\lzio.c" "%LUA_INC%\int64.c" ^
  -lm -static-libgcc
if errorlevel 1 (echo [X] FAILED && exit /b 1)
"%GEN%\lua_profiler_lua51_test.exe"
if errorlevel 1 (echo [X] FAILED && exit /b 1)
echo [OK]

echo === Step 8: recording format test ===
g++ %CXXFLAGS% -I"%SRC%common" -I"%SRC%injection" -I"%SRC%debugview" ^
  -o "%GEN%\recording_io_test.exe" "%SRC%tests\recording_io_test.cpp" ^
  "%SRC%debugview\recording_io.cpp" -static-libgcc -static-libstdc++
if errorlevel 1 (echo [X] FAILED && exit /b 1)
"%GEN%\recording_io_test.exe"
if errorlevel 1 (echo [X] FAILED && exit /b 1)
echo [OK]

echo === Step 9: DebugView (ImGui + ImPlot, cached .o) ===
if not exist "%GEN%\imgui_obj" mkdir "%GEN%\imgui_obj"
powershell -NoProfile -ExecutionPolicy Bypass -File "%SRC%compile_imgui_cache.ps1" ^
  -SrcRoot "%SRC%." -OutDir "%GEN%\imgui_obj" -ImguiDir "%IMGUI%" -ImplotDir "%IMPLOT%"
if errorlevel 1 (echo [X] FAILED imgui cache && exit /b 1)

set "IMGUI_OBJS=%GEN%\imgui_obj\imgui.o %GEN%\imgui_obj\imgui_draw.o %GEN%\imgui_obj\imgui_tables.o %GEN%\imgui_obj\imgui_widgets.o %GEN%\imgui_obj\imgui_impl_win32.o %GEN%\imgui_obj\imgui_impl_dx9.o %GEN%\imgui_obj\implot.o %GEN%\imgui_obj\implot_items.o"

g++ %CXXFLAGS% -I"%SRC%common" -I"%SRC%injection" -I"%SRC%debugview" ^
  -I"%IMGUI%" -I"%IMGUI%\backends" -I"%IMPLOT%" ^
  -o "%OUT%\debugview.exe" ^
  "%SRC%debugview\main.cpp" "%SRC%debugview\telemetry_client.cpp" ^
  "%SRC%debugview\frame_preview.cpp" "%SRC%debugview\recording_io.cpp" ^
  "%SRC%debugview\debug_list.cpp" ^
  %IMGUI_OBJS% ^
  "%GEN%\injector.o" "%GEN%\lua_vm_scan.o" -ld3d9 -lpsapi -lgdi32 -limm32 -luser32 -lcomdlg32 ^
  -static-libgcc -static-libstdc++ -mwindows
if errorlevel 1 (
  echo [!] debugview.exe busy or link failed, trying debugview_new.exe
  g++ %CXXFLAGS% -I"%SRC%common" -I"%SRC%injection" -I"%SRC%debugview" ^
    -I"%IMGUI%" -I"%IMGUI%\backends" -I"%IMPLOT%" ^
    -o "%OUT%\debugview_new.exe" ^
    "%SRC%debugview\main.cpp" "%SRC%debugview\telemetry_client.cpp" ^
    "%SRC%debugview\frame_preview.cpp" "%SRC%debugview\recording_io.cpp" ^
    "%SRC%debugview\debug_list.cpp" ^
    %IMGUI_OBJS% ^
    "%GEN%\injector.o" "%GEN%\lua_vm_scan.o" -ld3d9 -lpsapi -lgdi32 -limm32 -luser32 -lcomdlg32 ^
    -static-libgcc -static-libstdc++ -mwindows
  if errorlevel 1 (echo [X] FAILED && exit /b 1)
  echo [OK] wrote %OUT%\debugview_new.exe
) else (
  echo [OK]
)

if /I "%~1"=="ab" (
  echo === Optional: live A/B test against a running target process ===
  gcc %CFLAGS% -I"%SRC%common" -I"%SRC%injection" -I"%GEN%" ^
    -o "%GEN%\runtime_ab_test.exe" "%SRC%tests\runtime_ab_test.c" ^
    "%GEN%\injector.o" "%GEN%\lua_vm_scan.o" -lpsapi -static-libgcc
  if errorlevel 1 (echo [X] FAILED && exit /b 1)
  echo [OK] built %GEN%\runtime_ab_test.exe - run it with a live target PID
)

echo.
echo === Done: %OUT%\debugview.exe ===
endlocal
