@echo off
rem ---------------------------------------------------------------------------
rem  JobSystem 停止の再現ハーネス (ECS 2-4) のビルド ^& 実行
rem
rem  usage:  run_jobsystem_stop_harness.bat <debug^|release> <scenario> <trials> <timeout_ms> [--widen site:kind:N]
rem          scenario = idle ^| immediate ^| jobs
rem
rem  **GLFD_JOBSYSTEM_PROBE を定義するのはこのビルドだけ。** JobSystem.cpp の
rem  差し込み点 (JobSystemProbe.h) が有効になり、窓を広げる / 原子カウンタで
rem  観測することができる。ゲーム本体とテストスイートはこれを定義しない
rem
rem  フラグはテストスイート (build_and_run.bat) と同じにする
rem  注: 本ファイルは cmd.exe が解釈できるよう cp932 (Shift-JIS) で保存する
rem ---------------------------------------------------------------------------
setlocal enabledelayedexpansion

rem  shift は %0 もずらすので、スクリプトの場所は先に取っておく
set "HERE=%~dp0"

set "CONFIG=%~1"
if "%CONFIG%"=="" (
  echo usage: run_jobsystem_stop_harness.bat ^<debug^|release^> ^<scenario^> ^<trials^> ^<timeout_ms^> [--widen site:kind:N]
  exit /b 2
)
shift
set "ARGS="
:collect
if "%~1"=="" goto :collected
set "ARGS=!ARGS! %1"
shift
goto :collect
:collected

set "ROOT=%HERE%.."
set "OUTDIR=%HERE%build"

if /i "%CONFIG%"=="release" (
  set "CFG_FLAGS=/O2 /MD /DNDEBUG"
) else (
  set "CFG_FLAGS=/Od /MDd /D_DEBUG /RTC1"
)
set "COMMON_FLAGS=/nologo /std:c++20 /permissive- /W4 /WX /EHsc /GR- /Zc:__cplusplus /wd4324"

if not defined VSINSTALLDIR call :setup_msvc
if errorlevel 1 exit /b 1

set "NAME=JobSystemStopHarness_%CONFIG%"
set "OBJDIR=%OUTDIR%\%NAME%"
if not exist "%OBJDIR%" mkdir "%OBJDIR%"

echo ----- building %NAME% -----
cl %COMMON_FLAGS% %CFG_FLAGS% /DGLFD_JOBSYSTEM_PROBE ^
   /I "%ROOT%\Source" /I "%HERE%." ^
   /Fo"%OBJDIR%\\" /Fd"%OBJDIR%\%NAME%.pdb" ^
   "%HERE%JobSystemStopHarness.cpp" "%ROOT%\Source\Threading\JobSystem.cpp" ^
   /Fe"%OUTDIR%\%NAME%.exe"
if errorlevel 1 (
  echo [ERROR] build failed: %NAME%
  exit /b 1
)

if "%ARGS%"=="" exit /b 0
"%OUTDIR%\%NAME%.exe"%ARGS%
rem  0 以外(ハング / クラッシュ / 負の終了コード)をすべて失敗として返す
if not "!ERRORLEVEL!"=="0" exit /b 1
exit /b 0


rem ---------------------------------------------------------------------------
:setup_msvc
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo [ERROR] vswhere.exe was not found.
  exit /b 1
)
set "VSPATH="
set "VSPATH_TMP=%TEMP%\glfd_vspath.txt"
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
