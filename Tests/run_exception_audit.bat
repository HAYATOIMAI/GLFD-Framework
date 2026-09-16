@echo off
rem ---------------------------------------------------------------------------
rem  GLFD exception audit (ECS 2-2) -- acceptance condition 2, made mechanical.
rem
rem  Two checks, because NEITHER ONE CONTAINS THE OTHER:
rem
rem    (1) C4530   -- compile with no /EH and look for
rem                   "C++ exception handler used". This sees try / catch.
rem                   **It does NOT see throw.** Measured on four TUs:
rem                   try/catch -> C4530; "throw 1" -> nothing;
rem                   "throw std::bad_alloc()" -> nothing;
rem                   DynamicArray::PushBack -> nothing.
rem                   From JSON phase 1 until ECS 2-2 this single warning was
rem                   treated as "mechanical proof that we use no exceptions".
rem                   It never was. See ExceptionAudit\control_throw.cpp.
rem
rem    (2) _CxxThrowException -- dumpbin /symbols on the object file. This sees
rem                   throw, including the one N-2 violation we actually write by
rem                   accident: calling PushBack / EmplaceBack instead of Try*.
rem                   It does NOT see a try / catch that never throws.
rem
rem  TWO LISTS, because a probe cannot audit a function it cannot compile:
rem    - ExceptionAudit\*.cpp        : probes. They INCLUDE a header and CALL
rem                                    what they mean to check, so templates and
rem                                    inline functions are actually generated
rem    - ExceptionAudit\translation_units.txt : real .cpp files from Source\,
rem                                    compiled as themselves. This is the only
rem                                    way to reach a function whose definition
rem                                    lives in a .cpp (SceneManager is the
rem                                    example: every probe row for it is CLEAN
rem                                    while the .cpp row is dirty)
rem
rem  What this CANNOT check -- state it rather than imply coverage:
rem    - a template that nothing instantiates is never compiled, so never audited
rem    - code behind an #ifdef that these flags do not set
rem    - a .cpp that is in neither list
rem
rem  Expectations live next to the thing they describe:
rem      // EXPECT: CLEAN | C4530 | THROW | C4530+THROW | NOCODE
rem  NOCODE means the object holds no defined function symbol, i.e. nothing
rem  was compiled and the row proves nothing. Only a control may expect it.
rem      // WHY: <one line>
rem  (for translation units, the same two fields are columns in the .txt).
rem  A row whose measured result differs from EXPECT fails the run. Rows that
rem  expect something other than CLEAN are listed again at the end as accepted
rem  debt, so a passing run still says how much is left and why.
rem
rem  usage:  run_exception_audit.bat
rem  exit:   0 = every row matched its expectation, 1 = at least one did not
rem
rem  note: saved as cp932 so cmd.exe can read it, like build_and_run.bat
rem ---------------------------------------------------------------------------
setlocal enabledelayedexpansion

set "ROOT=%~dp0.."
set "SRCDIR=%~dp0ExceptionAudit"
set "OUTDIR=%~dp0build\exception_audit"
set "TULIST=%SRCDIR%\translation_units.txt"

rem  The GAME flags, minus /EH. Not the test flags: /W4 /WX rejects code the
rem  shipping build accepts, and then we would be auditing a different program (5.4).
set "AUDIT_FLAGS=/nologo /c /std:c++20 /permissive- /W3 /sdl /GR- /Zc:__cplusplus"

if not exist "%OUTDIR%" mkdir "%OUTDIR%"

if not defined VSINSTALLDIR call :setup_msvc
if errorlevel 1 exit /b 1

echo === GLFD exception audit (ECS 2-2) ===
echo flags: %AUDIT_FLAGS%   (no /EH on purpose)
echo.
echo row                                    expected      measured      result
echo ---------------------------------------------------------------------------

set /a PASS=0
set /a FAIL=0
set /a DEBT=0
set "FAILED="

for %%F in ("%SRCDIR%\*.cpp") do call :probe_row "%%~fF" "%%~nF"

echo.
echo real translation units from Source\ (definitions a probe cannot reach)
echo ---------------------------------------------------------------------------
for /f "usebackq tokens=1,2,* delims=|" %%A in ("%TULIST%") do call :tu_row "%%A" "%%B" "%%C"

echo ---------------------------------------------------------------------------
echo passed !PASS!, failed !FAIL!, accepted debt !DEBT!
if !FAIL! GTR 0 (
  echo.
  echo FAILED rows:
  for %%R in (!FAILED!) do echo   %%R
  echo.
  echo [AUDIT FAILED]
  exit /b 1
)
echo.
echo [AUDIT OK]
exit /b 0

rem ---------------------------------------------------------------------------
rem  A probe: expectation and reason are comments inside the file itself.
:probe_row
set "SRC=%~1"
set "NAME=%~2"

set "EXPECT="
for /f "tokens=2 delims=:" %%E in ('findstr /b /c:"// EXPECT:" "%SRC%"') do set "EXPECT=%%E"
set "EXPECT=%EXPECT: =%"
if "%EXPECT%"=="" (
  echo %NAME% -- no "// EXPECT:" line. every probe must declare one.
  set /a FAIL+=1
  set "FAILED=!FAILED! %NAME%^(no-expectation^)"
  goto :eof
)
set "REASON="
for /f "tokens=1,* delims=:" %%A in ('findstr /b /c:"// WHY:" "%SRC%"') do set "REASON=%%B"

call :measure "%SRC%" "%NAME%" || goto :eof
call :verdict "%NAME%" "%EXPECT%" "!REASON!"
goto :eof

rem ---------------------------------------------------------------------------
rem  A real translation unit: expectation and reason are columns in the list.
:tu_row
set "REL=%~1"
:trim_rel
if "%REL%"=="" goto :eof
if "%REL:~-1%"==" " set "REL=%REL:~0,-1%" & goto :trim_rel
if "%REL:~0,1%"=="#" goto :eof
set "EXPECT=%~2"
set "EXPECT=%EXPECT: =%"
set "REASON=%~3"

set "SRC=%ROOT%\%REL%"
if not exist "%SRC%" (
  echo %REL% -- listed in translation_units.txt but not on disk.
  set /a FAIL+=1
  set "FAILED=!FAILED! %REL%^(missing^)"
  goto :eof
)
for %%N in ("%SRC%") do set "NAME=tu_%%~nN"

call :measure "%SRC%" "%NAME%" || goto :eof
call :verdict "%REL%" "%EXPECT%" "!REASON!"
goto :eof

rem ---------------------------------------------------------------------------
rem  Compile one file and set HAS4530 / HASTHROW. Returns 1 if it could not be
rem  measured at all -- **an empty error is not a finding** (4.3).
:measure
set "MSRC=%~1"
set "MNAME=%~2"
set "LOG=%OUTDIR%\%MNAME%.log"
set "OBJ=%OUTDIR%\%MNAME%.obj"

rem  Never read a stale object as this run's result.
if exist "%OBJ%" del /q "%OBJ%"

cl %AUDIT_FLAGS% /I "%ROOT%\Source" /I "%ROOT%\Tests" "%MSRC%" /Fo"%OBJ%" > "%LOG%" 2>&1

findstr /c:"error C" "%LOG%" >nul 2>&1
if not errorlevel 1 (
  echo %MNAME% -- did not compile. see %LOG%
  set /a FAIL+=1
  set "FAILED=!FAILED! %MNAME%^(build-error^)"
  exit /b 1
)
if not exist "%OBJ%" (
  echo %MNAME% -- produced no object file. see %LOG%
  set /a FAIL+=1
  set "FAILED=!FAILED! %MNAME%^(no-object^)"
  exit /b 1
)

rem  **A row that compiled nothing is not a CLEAN row** - it is a row that
rem  checked nothing. This already happened once: a probe whose only function
rem  took a parameter of internal-linkage type emitted an object with zero
rem  defined function symbols, and the audit reported CLEAN. See
rem  ExceptionAudit\control_no_code.cpp, which is the tooth for this counter.
set "DEFINED=0"
for /f %%C in ('dumpbin /nologo /symbols "%OBJ%" 2^>nul ^| findstr /c:"External" ^| findstr /c:"()" ^| findstr /v /c:"UNDEF" ^| find /c /v ""') do set "DEFINED=%%C"
if "%DEFINED%"=="0" (
  set "ACTUAL=NOCODE"
  exit /b 0
)

set "HAS4530=0"
findstr /c:"C4530" "%LOG%" >nul 2>&1
if not errorlevel 1 set "HAS4530=1"

set "HASTHROW=0"
dumpbin /nologo /symbols "%OBJ%" 2>nul | findstr /c:"_CxxThrowException" >nul 2>&1
if not errorlevel 1 set "HASTHROW=1"

if "%HAS4530%%HASTHROW%"=="00" set "ACTUAL=CLEAN"
if "%HAS4530%%HASTHROW%"=="10" set "ACTUAL=C4530"
if "%HAS4530%%HASTHROW%"=="01" set "ACTUAL=THROW"
if "%HAS4530%%HASTHROW%"=="11" set "ACTUAL=C4530+THROW"
exit /b 0

rem ---------------------------------------------------------------------------
:verdict
set "VNAME=%~1"
set "VEXPECT=%~2"
set "VREASON=%~3"

set "RESULT=ok"
if not "%ACTUAL%"=="%VEXPECT%" set "RESULT=MISMATCH"

if "%RESULT%"=="ok" set /a PASS+=1
if "%RESULT%"=="MISMATCH" set /a FAIL+=1
if "%RESULT%"=="MISMATCH" set "FAILED=!FAILED! %VNAME%^(expected-%VEXPECT%-got-%ACTUAL%^)"

call :pad COL1 "%VNAME%"   38
call :pad COL2 "%VEXPECT%" 14
call :pad COL3 "%ACTUAL%"  14
echo !COL1!!COL2!!COL3!%RESULT%

rem  **The reason is printed on the row, not collected into a footnote.** A count
rem  at the bottom is easy to stop reading; a line under the row is not.
rem  A control is SUPPOSED to be dirty - that is its tooth. Anything else that is
rem  dirty is debt we are carrying, and it says so every single run.
if "%VEXPECT%"=="CLEAN" goto :eof
set "LABEL=accepted debt:"
if "%VNAME:~0,8%"=="control_" set "LABEL=tooth:"
if "%LABEL%"=="accepted debt:" set /a DEBT+=1
echo        %LABEL%!VREASON!
goto :eof

rem ---------------------------------------------------------------------------
rem  Fixed-width columns without PowerShell, so this runs in a bare cmd window.
:pad
set "PADTEXT=%~2                                                            "
if "%~3"=="38" set "%~1=!PADTEXT:~0,38!"
if "%~3"=="14" set "%~1=!PADTEXT:~0,14!"
goto :eof

rem ---------------------------------------------------------------------------
:setup_msvc
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo vswhere.exe not found. run this from a Developer Command Prompt.
  exit /b 1
)
for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%I"
if "%VSPATH%"=="" (
  echo no MSVC toolset found.
  exit /b 1
)
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
where cl.exe >nul 2>&1
if errorlevel 1 (
  echo cl.exe not on PATH after vcvars64.
  exit /b 1
)
rem  **Do not run half the audit.** With no dumpbin, every throw-only row would
rem  come back CLEAN, which is the exact failure this tool exists to end.
where dumpbin.exe >nul 2>&1
if errorlevel 1 (
  echo dumpbin.exe not on PATH. the throw check cannot run and the C4530-only
  echo result would be a false CLEAN. stopping.
  exit /b 1
)
exit /b 0
