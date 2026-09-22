@echo off
rem ---------------------------------------------------------------------------
rem  GLFD ECS の計測ツール (1-5 の性能基準線)
rem
rem  usage:  run_survivor_benchmark.bat [debug^|release] [frames] [warmup] [small^|large]
rem          (default: release 600 900 small)
rem
rem  NOTE: this builds with the GAME flags (/W3 /sdl), not the test flags
rem        (/W4 /WX). The game systems are compiled at /W3 in the vcxproj and
rem        do not survive /W4 /WX today. Measuring code that is built with
rem        different flags than the game would measure a different program.
rem
rem  NOTE: it must run from the repository root (it reads Resource/GameConfig.jsonc).
rem
rem  This file is saved as cp932 so cmd.exe can read it.
rem ---------------------------------------------------------------------------
setlocal

set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=release"
set "FRAMES=%~2"
if "%FRAMES%"=="" set "FRAMES=600"
set "WARMUP=%~3"
if "%WARMUP%"=="" set "WARMUP=900"
set "PRESET=%~4"
if "%PRESET%"=="" set "PRESET=small"

set "ROOT=%~dp0.."
set "OUTDIR=%~dp0build"
set "OBJDIR=%OUTDIR%\EcsSurvivorBenchmark"

if /i "%CONFIG%"=="debug" (
  set "CFG_FLAGS=/Od /MDd /D_DEBUG /RTC1"
) else (
  set "CFG_FLAGS=/O2 /MD /DNDEBUG"
)

rem  the game's flags, from CL.command.1.tlog
set "COMMON_FLAGS=/nologo /std:c++20 /permissive- /W3 /sdl /EHsc /GR- /Zc:__cplusplus"

set "ENGINE_SOURCES=%ROOT%\Source\Core\Json\JsonArena.cpp %ROOT%\Source\Core\Json\JsonReader.cpp %ROOT%\Source\Core\Json\JsonValue.cpp %ROOT%\Source\Core\Json\JsonFileIO.cpp %ROOT%\Source\Core\Json\JsonDocument.cpp %ROOT%\Source\Core\Json\JsonWriter.cpp %ROOT%\Source\Core\StackAllocator.cpp %ROOT%\Source\Core\Logger.cpp %ROOT%\Source\Physics\SpatialHashGrid.cpp %ROOT%\Source\Threading\JobSystem.cpp %ROOT%\Source\Threading\ThreadPool.cpp"

if not defined VSINSTALLDIR call :setup_msvc
if errorlevel 1 exit /b 1

if not exist "%OUTDIR%" mkdir "%OUTDIR%"
if not exist "%OBJDIR%" mkdir "%OBJDIR%"

echo ----- building EcsSurvivorBenchmark (%CONFIG%) -----
cl %COMMON_FLAGS% %CFG_FLAGS% ^
   /I "%ROOT%\Source" /I "%~dp0." ^
   /Fo"%OBJDIR%\\" /Fd"%OBJDIR%\EcsSurvivorBenchmark.pdb" ^
   "%~dp0EcsSurvivorBenchmark.cpp" "%~dp0BenchMeasure.cpp" %ENGINE_SOURCES% ^
   /Fe"%OUTDIR%\EcsSurvivorBenchmark.exe"
if errorlevel 1 (
  echo [ERROR] build failed
  exit /b 1
)

echo.
pushd "%ROOT%"
"%OUTDIR%\EcsSurvivorBenchmark.exe" %FRAMES% %WARMUP% %PRESET%
set "RC=%errorlevel%"
popd
exit /b %RC%


rem ---------------------------------------------------------------------------
:setup_msvc
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo [ERROR] vswhere.exe was not found.
  exit /b 1
)
set "VSPATH="
set "VSPATH_TMP=%TEMP%\glfd_survbench_vspath.txt"
"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%VSPATH_TMP%"
if exist "%VSPATH_TMP%" set /p VSPATH=<"%VSPATH_TMP%"
del "%VSPATH_TMP%" >nul 2>&1
if not defined VSPATH (
  echo [ERROR] Visual Studio with the C++ toolset was not found.
  exit /b 1
)
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
where cl.exe >nul 2>&1
if errorlevel 1 (
  echo [ERROR] failed to initialize the MSVC environment.
  exit /b 1
)
exit /b 0
