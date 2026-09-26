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

rem  2 番目の引数でスイートを 1 本に絞れる(変異テストが 1 本だけ回すため)。
rem  **フラグの表はここ 1 つのまま**にしたいので、別のランナーを作らない
set "ONLY=%~2"

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

for %%f in ("%~dp0*Tests.cpp") do call :maybe_build "%%~ff" "%%~nf"

echo ===========================================================
if not "%FAILED%"=="0" (
  echo [ERROR] %FAILED% of %SUITES% suite^(s^) failed:%FAILED_NAMES%
  exit /b 1
)
echo [OK] all %SUITES% suite^(s^) passed.
exit /b 0


rem ---------------------------------------------------------------------------
:maybe_build
if not "%ONLY%"=="" if /i not "%~2"=="%ONLY%" goto :eof
call :build_and_run "%~1" "%~2"
goto :eof

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
set "EXTRA_FLAGS="
if /i "%NAME%"=="TemplateInstantiationTests" set "EXTRA_SOURCES=%ROOT%\Source\Core\Logger.cpp"
rem  EcsDiagnosticsTests (1-6) は SpatialHashGrid の確保失敗を直接見るので実装が要る
if /i "%NAME%"=="EcsDiagnosticsTests" set "EXTRA_SOURCES=%ROOT%\Source\Physics\SpatialHashGrid.cpp"
rem  EcsCollisionTests (1-7) は本番の GridBuildSystem / CollisionSystem を呼ぶ
if /i "%NAME%"=="EcsCollisionTests" set "EXTRA_SOURCES=%ROOT%\Source\Physics\SpatialHashGrid.cpp %ROOT%\Source\Threading\JobSystem.cpp %ROOT%\Source\Threading\ThreadPool.cpp %ROOT%\Source\Core\StackAllocator.cpp"
rem  C4324 (alignas による詰め物の通知) は情報提供の警告で、ゲーム本体の
rem  /W3 では出ない。LockFreeQueue / JobCounter の alignas は偽共有を
rem  避けるための意図的なものなので、スレッド系を建てるスイートだけ抑制する
if /i "%NAME%"=="EcsCollisionTests" set "EXTRA_FLAGS=/wd4324"
rem  EcsSurvivorTests (1-8) は本番の RunSurvivorFrame を回す。EcsCollisionTests と同じ形
if /i "%NAME%"=="EcsSurvivorTests" set "EXTRA_SOURCES=%ROOT%\Source\Physics\SpatialHashGrid.cpp %ROOT%\Source\Threading\JobSystem.cpp %ROOT%\Source\Threading\ThreadPool.cpp %ROOT%\Source\Core\StackAllocator.cpp"
if /i "%NAME%"=="EcsSceneTransitionTests" set "EXTRA_SOURCES=%ROOT%\Source\Scene\SceneManager.cpp %ROOT%\Source\Core\StackAllocator.cpp"
if /i "%NAME%"=="EcsSceneTransitionTests" set "EXTRA_FLAGS=/wd4324"
if /i "%NAME%"=="EcsSurvivorTests" set "EXTRA_FLAGS=/wd4324"
rem  EcsGuideTests (1-8) は README の並列の例で JobSystem を使う
if /i "%NAME%"=="EcsGuideTests" set "EXTRA_SOURCES=%ROOT%\Source\Threading\JobSystem.cpp %ROOT%\Source\Threading\ThreadPool.cpp"
if /i "%NAME%"=="EcsGuideTests" set "EXTRA_FLAGS=/wd4324"
rem  JobSystemStopTests (2-4) は差し込み点を有効にした本物の JobSystem を使う。
rem  **GLFD_JOBSYSTEM_PROBE を定義するのはこのスイートとハーネスだけ。** ゲーム本体と
rem  他のスイートは定義しないので、差し込み点は ((void)0) に消える
if /i "%NAME%"=="JobSystemStopTests" set "EXTRA_SOURCES=%ROOT%\Source\Threading\JobSystem.cpp"
if /i "%NAME%"=="JobSystemStopTests" set "EXTRA_FLAGS=/wd4324 /DGLFD_JOBSYSTEM_PROBE"
rem  ParallelForTests (2-6) は本物の JobSystem で分け方と積み方を確かめる
if /i "%NAME%"=="ParallelForTests" set "EXTRA_SOURCES=%ROOT%\Source\Threading\JobSystem.cpp"
if /i "%NAME%"=="ParallelForTests" set "EXTRA_FLAGS=/wd4324 /DGLFD_JOBSYSTEM_PROBE"
rem  RenderDiagnosticsTests (2-3) は本物の ReportRenderStep と Logger で診断の行を読む。
rem  EcsDiagnosticsLog.h が GameContext.h 経由で EcsSurvivorTests と同じものを引き込む
if /i "%NAME%"=="RenderDiagnosticsTests" set "EXTRA_SOURCES=%ROOT%\Source\Core\Logger.cpp %ROOT%\Source\Physics\SpatialHashGrid.cpp %ROOT%\Source\Threading\JobSystem.cpp %ROOT%\Source\Threading\ThreadPool.cpp %ROOT%\Source\Core\StackAllocator.cpp"
if /i "%NAME%"=="RenderDiagnosticsTests" set "EXTRA_FLAGS=/wd4324"
set "OBJDIR=%OUTDIR%\%NAME%"
if not exist "%OBJDIR%" mkdir "%OBJDIR%"

echo.
echo ----- building %NAME% (%CONFIG%) -----
cl %COMMON_FLAGS% %CFG_FLAGS% %EXTRA_FLAGS% ^
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

rem  **`if errorlevel 1` ではクラッシュを取り逃がす。** アクセス違反で落ちた
rem  プロセスの終了コードは 0xC0000005 = 負の数で、`errorlevel 1` は
rem  「1 以上か」なので **偽になる**。実際に踏んだ: 2-1 の変異 M5 で、
rem  スイートがケース 3 で落ちて要約すら出していないのに、ランナーは
rem  `[OK] all 1 suite(s) passed` と報告した。§4.4「ハングを合格と読まない」の
rem  クラッシュ版である。**0 でないかを見る**
if not "!ERRORLEVEL!"=="0" (
  echo [ERROR] suite exited with !ERRORLEVEL!: %NAME%
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
