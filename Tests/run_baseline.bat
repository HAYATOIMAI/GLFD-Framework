@echo off
rem ---------------------------------------------------------------------------
rem  GLFD performance baseline (ECS 2-5): build -> measure -> record, one command.
rem
rem  usage:  run_baseline.bat [options passed to run_baseline.ps1]
rem          run_baseline.bat                                   main run (20 rounds)
rem          run_baseline.bat -Rounds 2 -SlowRuns 1 -WakeRuns 1  trial
rem          run_baseline.bat -Inject crash -Rounds 1 -SlowRuns 0 -WakeRuns 0
rem                                                             teeth check
rem                          (-Inject: build, launch, crash, hang, noend, exitcode)
rem
rem  The record goes to Docs\Baseline\<date>_<commit>\baseline.md (+ runs.csv,
rem  rows.csv, raw\). Teeth checks go to %TEMP%\glfd_baseline_teeth\.
rem  Exit code: 0 only when the build and every run succeeded.
rem
rem  Do not run anything else while this runs. The numbers move (ECS 2-4).
rem
rem  This file is ASCII only, so it reads the same as cp932.
rem ---------------------------------------------------------------------------
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0run_baseline.ps1" %*
exit /b %errorlevel%
