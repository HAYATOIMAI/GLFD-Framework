@echo off
rem ---------------------------------------------------------------------------
rem  GLFD: build the three baseline benchmarks (ECS 2-5)
rem
rem  usage:  build_benchmarks.bat
rem          builds Tests\build\EcsBenchmark.exe, EcsSurvivorBenchmark.exe and
rem          JobWakeBenchmark.exe, Release, with the GAME flags (/W3 /sdl).
rem          Called by run_baseline.ps1. Exit code is non-zero if any build fails.
rem
rem  NOTE: the GAME flags, not the test flags (/W4 /WX): measuring code built
rem        with different flags than the game would measure a different program.
rem
rem  NOTE: GLFD_BENCH_INJECT_BUILD_FAILURE=1 force-includes a header that does
rem        not exist, so the build fails. run_baseline.ps1 uses it to check that
rem        a failed build is recorded as a failure (the teeth check).
rem
rem  NOTE: GLFD_BENCH_INJECT_SKIP_BUILD=1 builds NOTHING and exits 0, i.e. a
rem        build that "succeeded" without producing an exe. run_baseline.ps1
rem        uses it to check that an old exe is never measured (STALE_EXE).
rem
rem  This file is ASCII only, so it reads the same as cp932.
rem ---------------------------------------------------------------------------
setlocal

if "%GLFD_BENCH_INJECT_SKIP_BUILD%"=="1" (
  echo [INJECTED] build skipped, exiting with 0
  exit /b 0
)

set "ROOT=%~dp0.."
set "OUTDIR=%~dp0build"

set "CFG_FLAGS=/O2 /MD /DNDEBUG"
rem  the game's flags, from CL.command.1.tlog (same as run_ecs_benchmark.bat)
set "COMMON_FLAGS=/nologo /std:c++20 /permissive- /W3 /sdl /EHsc /GR- /Zc:__cplusplus"
set "INJECT="
if "%GLFD_BENCH_INJECT_BUILD_FAILURE%"=="1" set "INJECT=/FIglfd_injected_build_failure_does_not_exist.h"

set "ENGINE_SOURCES=%ROOT%\Source\Core\Json\JsonArena.cpp %ROOT%\Source\Core\Json\JsonReader.cpp %ROOT%\Source\Core\Json\JsonValue.cpp %ROOT%\Source\Core\Json\JsonFileIO.cpp %ROOT%\Source\Core\Json\JsonDocument.cpp %ROOT%\Source\Core\Json\JsonWriter.cpp %ROOT%\Source\Core\StackAllocator.cpp %ROOT%\Source\Core\Logger.cpp %ROOT%\Source\Physics\SpatialHashGrid.cpp %ROOT%\Source\Threading\JobSystem.cpp %ROOT%\Source\Threading\ThreadPool.cpp"

if not defined VSINSTALLDIR call :setup_msvc
if errorlevel 1 exit /b 1

if not exist "%OUTDIR%" mkdir "%OUTDIR%"

call :build EcsBenchmark "%~dp0EcsBenchmark.cpp %ENGINE_SOURCES%"
if errorlevel 1 exit /b 1
call :build EcsSurvivorBenchmark "%~dp0EcsSurvivorBenchmark.cpp %ENGINE_SOURCES%"
if errorlevel 1 exit /b 1
call :build JobWakeBenchmark "%~dp0JobWakeBenchmark.cpp %ROOT%\Source\Threading\JobSystem.cpp"
if errorlevel 1 exit /b 1

echo [OK] all three benchmarks built
exit /b 0


rem ---------------------------------------------------------------------------
:build
set "NAME=%~1"
set "OBJDIR=%OUTDIR%\%NAME%"
if not exist "%OBJDIR%" mkdir "%OBJDIR%"
echo ----- building %NAME% (release, game flags) -----
cl %COMMON_FLAGS% %CFG_FLAGS% %INJECT% ^
   /I "%ROOT%\Source" /I "%~dp0." ^
   /Fo"%OBJDIR%\\" /Fd"%OBJDIR%\%NAME%.pdb" ^
   %~2 "%~dp0BenchMeasure.cpp" ^
   /Fe"%OUTDIR%\%NAME%.exe"
if errorlevel 1 (
  echo [ERROR] build failed: %NAME%
  exit /b 1
)
exit /b 0


rem ---------------------------------------------------------------------------
:setup_msvc
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo [ERROR] vswhere.exe was not found.
  exit /b 1
)
set "VSPATH="
set "VSPATH_TMP=%TEMP%\glfd_buildbench_vspath.txt"
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
