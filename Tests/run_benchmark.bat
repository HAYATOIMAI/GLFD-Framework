@echo off
rem ---------------------------------------------------------------------------
rem  GLFD JSON の計測ツール (N-6)
rem
rem  CI ではなく手動実行するもの。
rem  絶対値の目標は無く、最適化の前後で比較できればよい。
rem
rem  usage:  run_benchmark.bat [debug^|release] [iterations]   (既定: release 2000)
rem
rem  注: 本ファイルは cmd.exe が解釈できるよう cp932 で保存する
rem ---------------------------------------------------------------------------
setlocal

set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=release"
set "ITER=%~2"
if "%ITER%"=="" set "ITER=2000"

set "ROOT=%~dp0.."
set "OUTDIR=%~dp0build"
set "OBJDIR=%OUTDIR%\JsonBenchmark"

if /i "%CONFIG%"=="debug" (
  set "CFG_FLAGS=/Od /MDd /D_DEBUG /RTC1"
) else (
  set "CFG_FLAGS=/O2 /MD /DNDEBUG"
)

set "COMMON_FLAGS=/nologo /std:c++20 /permissive- /W4 /WX /EHsc /GR- /Zc:__cplusplus"
set "ENGINE_SOURCES=%ROOT%\Source\Core\Json\JsonArena.cpp %ROOT%\Source\Core\Json\JsonReader.cpp %ROOT%\Source\Core\Json\JsonValue.cpp %ROOT%\Source\Core\Json\JsonFileIO.cpp %ROOT%\Source\Core\Json\JsonDocument.cpp %ROOT%\Source\Core\Json\JsonWriter.cpp"

if not defined VSINSTALLDIR call :setup_msvc
if errorlevel 1 exit /b 1

if not exist "%OUTDIR%" mkdir "%OUTDIR%"
if not exist "%OBJDIR%" mkdir "%OBJDIR%"

echo ----- building JsonBenchmark (%CONFIG%) -----
cl %COMMON_FLAGS% %CFG_FLAGS% ^
   /I "%ROOT%\Source" /I "%~dp0." ^
   /Fo"%OBJDIR%\\" /Fd"%OBJDIR%\JsonBenchmark.pdb" ^
   "%~dp0JsonBenchmark.cpp" %ENGINE_SOURCES% ^
   /Fe"%OUTDIR%\JsonBenchmark.exe"
if errorlevel 1 (
  echo [ERROR] build failed
  exit /b 1
)

echo.
"%OUTDIR%\JsonBenchmark.exe" %ITER%
exit /b %errorlevel%


rem ---------------------------------------------------------------------------
:setup_msvc
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo [ERROR] vswhere.exe was not found.
  exit /b 1
)
set "VSPATH="
set "VSPATH_TMP=%TEMP%\glfd_bench_vspath.txt"
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
