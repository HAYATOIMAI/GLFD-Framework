@echo off
rem ---------------------------------------------------------------------------
rem  GLFD テストのビルド ^& 実行
rem
rem  Tests\*Tests.cpp を 1 本ずつ独立した exe としてビルドし、順に実行する。
rem  ゲーム本体 (GameLib_conteinar.vcxproj) には main() が既にあるため、
rem  テストは vcxproj に含めず cl.exe で直接ビルドする。
rem
rem  usage:  build_and_run.bat [debug^|release]      (既定: debug)
rem
rem  注: 本ファイルは cmd.exe が解釈できるよう cp932 (Shift-JIS) で保存する
rem ---------------------------------------------------------------------------
setlocal enabledelayedexpansion

set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=debug"

set "ROOT=%~dp0.."
set "OUTDIR=%~dp0build"

if /i "%CONFIG%"=="release" (
  set "CFG_FLAGS=/O2 /MD /DNDEBUG"
) else (
  set "CFG_FLAGS=/Od /MDd /D_DEBUG /RTC1"
)

rem  /W4 /WX                 : N-5「警告ゼロを維持する」に合わせ警告をエラー扱いにする
rem  /permissive- /std:c++20 : vcxproj の ConformanceMode / LanguageStandard と同一
rem  /GR-                    : RTTI を無効化し、未使用をコンパイラに強制させる
rem  /Zc:__cplusplus         : __cplusplus を正しい値にする
set "COMMON_FLAGS=/nologo /std:c++20 /permissive- /W4 /WX /EHsc /GR- /Zc:__cplusplus"

rem  各テストには JSON サブシステムの実装をまとめてリンクする。
rem  未使用のものが混ざってもリンク時間が増えるだけで害は無い
set "ENGINE_SOURCES=%ROOT%\Source\Core\Json\JsonArena.cpp %ROOT%\Source\Core\Json\JsonReader.cpp %ROOT%\Source\Core\Json\JsonValue.cpp %ROOT%\Source\Core\Json\JsonFileIO.cpp %ROOT%\Source\Core\Json\JsonDocument.cpp %ROOT%\Source\Core\Json\JsonWriter.cpp"

rem --- MSVC 環境の読み込み ---------------------------------------------------
if not defined VSINSTALLDIR call :setup_msvc
if errorlevel 1 exit /b 1

if not exist "%OUTDIR%" mkdir "%OUTDIR%"

set "SUITES=0"
set "FAILED=0"
set "FAILED_NAMES="

for %%f in ("%~dp0*Tests.cpp") do call :build_and_run "%%~ff" "%%~nf"

echo ===========================================================
if not "%FAILED%"=="0" (
  echo [ERROR] %FAILED% of %SUITES% suite^(s^) failed:%FAILED_NAMES%
  exit /b 1
)
echo [OK] all %SUITES% suite^(s^) passed.
exit /b 0


rem ---------------------------------------------------------------------------
rem  %1 = テストのソースファイル(フルパス) / %2 = 名前(拡張子なし)
rem ---------------------------------------------------------------------------
:build_and_run
set /a SUITES+=1
set "NAME=%~2"

rem  スイート個別の追加ソース。既定は無し。
rem  TemplateInstantiationTests (E-1) だけは ResourceStorage<T>::Load が
rem  LOG_ERROR を呼ぶため Logger.cpp が要る。全スイートへ足すと Json 側の
rem  スイートにまで Logger を持ち込むことになるので、個別に足す
set "EXTRA_SOURCES="
if /i "%NAME%"=="TemplateInstantiationTests" set "EXTRA_SOURCES=%ROOT%\Source\Core\Logger.cpp"
rem  EcsDiagnosticsTests (1-6) は SpatialHashGrid の確保失敗を直接見るので実装が要る
if /i "%NAME%"=="EcsDiagnosticsTests" set "EXTRA_SOURCES=%ROOT%\Source\Physics\SpatialHashGrid.cpp"
set "OBJDIR=%OUTDIR%\%NAME%"
if not exist "%OBJDIR%" mkdir "%OBJDIR%"

echo.
echo ----- building %NAME% (%CONFIG%) -----
cl %COMMON_FLAGS% %CFG_FLAGS% ^
   /I "%ROOT%\Source" /I "%~dp0." ^
   /Fo"%OBJDIR%\\" /Fd"%OBJDIR%\%NAME%.pdb" ^
   "%~1" %ENGINE_SOURCES% %EXTRA_SOURCES% ^
   /Fe"%OUTDIR%\%NAME%.exe"

if errorlevel 1 (
  echo [ERROR] build failed: %NAME%
  set /a FAILED+=1
  set "FAILED_NAMES=!FAILED_NAMES! %NAME%(build)"
  exit /b 0
)

echo.
"%OUTDIR%\%NAME%.exe"
if errorlevel 1 (
  set /a FAILED+=1
  set "FAILED_NAMES=!FAILED_NAMES! %NAME%"
)
exit /b 0


rem ---------------------------------------------------------------------------
:setup_msvc
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo [ERROR] vswhere.exe was not found.
  exit /b 1
)
rem for /f のバッククォート内でパスを引用符で括ると解釈が崩れるため、
rem いったんファイルへ落としてから読み取る
set "VSPATH="
set "VSPATH_TMP=%TEMP%\glfd_vspath.txt"
"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%VSPATH_TMP%"
if exist "%VSPATH_TMP%" set /p VSPATH=<"%VSPATH_TMP%"
del "%VSPATH_TMP%" >nul 2>&1
if not defined VSPATH (
  echo [ERROR] Visual Studio with the C++ toolset was not found.
  exit /b 1
)
rem vcvars64.bat は内部で vswhere を呼ぶ際に無害な警告を出すことがあるため
rem stderr ごと捨てる(初期化の成否は直後の cl.exe の有無で判定する)
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
where cl.exe >nul 2>&1
if errorlevel 1 (
  echo [ERROR] failed to initialize the MSVC environment.
  exit /b 1
)
exit /b 0
