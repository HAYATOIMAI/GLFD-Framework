# ---------------------------------------------------------------------------
#  GLFD: does the game end through the normal shutdown however its console is closed? (ECS 2-10, T-ECS-46)
#
#  Before 2-10, closing the console, Ctrl+C and Ctrl+Break ended the process in 25-240 ms with 0xC000013A:
#  no "job system stopped ... every job ran" line and no "=== Engine Shutdown ===" (the default handler calls
#  ExitProcess). Only the game window's X ended it properly.
#
#  Cases (each starts the game fresh, waits, acts, then waits up to 15 s for the process to end):
#   C1 X on the game window (the reference)          C5 minimised (2-8 pause), then close the console
#   C2 close the console                             C6 in the window-move modal loop (SC_MOVE), then close
#   C3 Ctrl+C                                        C7 while the 2-9 message box is up (probe build), then close
#   C4 Ctrl+Break                                    C8 Ctrl+C right after launch, before the window exists
#                                                    C9 close the console right after launch (during Initialize)
#   C10 (conhost only) select text in the console (Edit > Select All), then close the console
#  Expected: the log ends with the join line and "=== Engine Shutdown ===" (C2-C6 and C8 also have exactly one
#  "console: ..." line); the process ends by itself within 15 s. C7: ends within 15 s (the log was already
#  complete when the box came up). Exit codes are RECORDED, not asserted (a close races main's own return).
#  C6 is only meaningful if the game thread was really in the move loop (GetGUIThreadInfo GUI_INMOVESIZE),
#  and C8 only if the signal arrived before the frame loop started ("frame rate" line absent): both are recorded.
#  C10: while conhost has a selection, a write to the console waits until the selection ends. The shutdown lines are
#  written after the close, so without the console silence they wait on std::cout until the handler's limit and the
#  OS ends the process (a build without the silence: 4030 ms, 0xC000013A, no Engine Shutdown). C10 is only
#  meaningful if a selection was up (GetConsoleSelectionInfo through the helper); skipped without -UseConhost.
#
#  usage:  powershell -File Tests\console_close_check.ps1 [-Config Release|Debug] [-UseConhost] [-ExePath path]
#          [-Cases C1,C2,...] [-OutName name]
#   -ExePath runs another build of the game (the pre-fix build for the teeth). Its working folder must hold
#   Resource\ and Source\Shaders\ (a repository root). -UseConhost starts the game under conhost.exe instead of
#   the default terminal (Windows Terminal on this machine).
#  C7 needs the probe build x64\ProbeRelease\ (Tests\render_fault_check.ps1 builds it); skipped if missing.
#  Output: a NEW folder Tests\build\console_close\<OutName>\ (result.txt + each case's Game.log).
#  Exit codes: 0 every case as expected / 1 at least one not / 2 could not build the helper
#  SAFETY: only a console window that appeared after the launch and belongs to this game (its title is the exe
#  path, or it is owned by the conhost this script started) is ever closed.
#  Every wait has an upper bound. Names differ in spelling, not only in case.
# ---------------------------------------------------------------------------
param([ValidateSet("Release", "Debug")][string]$Config = "Release", [switch]$UseConhost, [string]$ExePath = "",
      [string[]]$Cases = @("C1", "C2", "C3", "C4", "C5", "C6", "C7", "C8", "C9", "C10"),
      [string]$OutName = ("run_" + (Get-Date -Format "yyyyMMdd_HHmmss")))
$ErrorActionPreference = "Stop"
# `powershell -File ... -Cases C3,C4` passes ONE string "C3,C4": split it, and refuse names that are not cases
$Cases = @($Cases | ForEach-Object { $_ -split "," } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
$unknownCases = @($Cases | Where-Object { $_ -notin @("C1", "C2", "C3", "C4", "C5", "C6", "C7", "C8", "C9", "C10") })
if ($unknownCases.Count -gt 0) { Write-Host "unknown case(s): $($unknownCases -join ', ')"; exit 2 }
$repoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$gameExe  = if ($ExePath) { $ExePath } else { Join-Path $repoRoot "x64\$Config\GameLib_conteinar.exe" }
$workDir  = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $gameExe))   # <root>\x64\<cfg>\exe -> <root>
$probeExe = Join-Path $repoRoot "x64\ProbeRelease\GameLib_conteinar.exe"
$toolDir  = Join-Path $repoRoot "Tests\build\console_close"
$outDir   = Join-Path $toolDir $OutName
if (-not (Test-Path $gameExe)) { Write-Host "no game exe at $gameExe"; exit 2 }
if (Test-Path $outDir) { Remove-Item -Recurse -Force $outDir }
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$result = Join-Path $outDir "result.txt"
"game $gameExe (written $((Get-Item $gameExe).LastWriteTime.ToString('s')))  host $(if ($UseConhost) { 'conhost' } else { 'default terminal' })" | Set-Content $result

# ---- the Ctrl+C / Ctrl+Break helper
$ctrlSend = Join-Path $toolDir "ConsoleCtrlSend.exe"
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$bat = Join-Path $toolDir "build_ctrlsend.bat"
"@echo off`r`ncall `"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul 2>&1`r`ncl /nologo /W4 /WX /EHsc /std:c++20 /Fo`"$toolDir\\`" /Fe`"$ctrlSend`" `"$repoRoot\Tests\ConsoleCtrlSend.cpp`" >`"$toolDir\build_ctrlsend.log`" 2>&1`r`n" | Set-Content $bat -Encoding ascii
cmd /c "`"$bat`" < nul"
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $ctrlSend)) { Write-Host "could not build ConsoleCtrlSend (see $toolDir\build_ctrlsend.log)"; exit 2 }
# warm-up: run the just-built helper once against no process (it only fails to attach). The first Ctrl send of a
# run failed to attach (error 5) in three runs before this was added
$warm = Start-Process -FilePath $ctrlSend -ArgumentList "0 0 `"$toolDir\warmup.txt`"" -WindowStyle Hidden -PassThru
[void]$warm.WaitForExit(10000)

if (-not ("GlfdCloseCheck" -as [type])) {
  Add-Type -Namespace "" -Name GlfdCloseCheck -MemberDefinition @'
public delegate bool EnumProc(IntPtr h, IntPtr l);
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
[DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int l, t, r, b; }
[StructLayout(LayoutKind.Sequential)] public struct GUITHREADINFO { public int cbSize; public int flags; public IntPtr hwndActive, hwndFocus, hwndCapture, hwndMenuOwner, hwndMoveSize, hwndCaret; public RECT rcCaret; }
[DllImport("user32.dll")] public static extern bool GetGUIThreadInfo(uint thread, ref GUITHREADINFO info);
public static bool InMoveSize(IntPtr gameWindow) {
  uint pid; uint tid = GetWindowThreadProcessId(gameWindow, out pid);
  var info = new GUITHREADINFO(); info.cbSize = Marshal.SizeOf(info);
  return GetGUIThreadInfo(tid, ref info) && (info.flags & 0x2) != 0;   // GUI_INMOVESIZE
}
public static System.Collections.Generic.List<string> ConsoleWindows() {
  var list = new System.Collections.Generic.List<string>();
  EnumWindows((h, l) => {
    if (!IsWindowVisible(h)) return true;
    var cls = new System.Text.StringBuilder(128); GetClassName(h, cls, 128);
    var c = cls.ToString();
    if (c == "ConsoleWindowClass" || c == "CASCADIA_HOSTING_WINDOW_CLASS") {
      uint pid; GetWindowThreadProcessId(h, out pid);
      var t = new System.Text.StringBuilder(512); GetWindowText(h, t, 512);
      list.Add(h.ToInt64() + "|" + c + "|" + pid + "|" + t.ToString());
    }
    return true;
  }, IntPtr.Zero);
  return list;
}
public static IntPtr FindGameWindow(uint pid) {
  IntPtr found = IntPtr.Zero;
  EnumWindows((h, l) => {
    uint p; GetWindowThreadProcessId(h, out p);
    if (p != pid) return true;
    var sb = new System.Text.StringBuilder(64); GetClassName(h, sb, 64);
    if (sb.ToString() == "GameLibWindow") { found = h; return false; }
    return true;
  }, IntPtr.Zero);
  return found;
}
public static IntPtr FindDialog(uint pid) {
  IntPtr found = IntPtr.Zero;
  EnumWindows((h, l) => {
    uint p; GetWindowThreadProcessId(h, out p);
    if (p != pid || !IsWindowVisible(h)) return true;
    var sb = new System.Text.StringBuilder(64); GetClassName(h, sb, 64);
    if (sb.ToString() == "#32770") { found = h; return false; }
    return true;
  }, IntPtr.Zero);
  return found;
}
'@
}

function Wait-Until([scriptblock]$cond, [double]$seconds) {
  $until = (Get-Date).AddSeconds($seconds)
  while ((Get-Date) -lt $until) { if (& $cond) { return $true }; Start-Sleep -Milliseconds 50 }
  return (& $cond)
}

function Start-Game([string]$exe, [hashtable]$env) {
  foreach ($k in $env.Keys) { Set-Item "env:$k" $env[$k] }
  try {
    if ($UseConhost) {
      $hostProc = Start-Process -FilePath "$env:SystemRoot\System32\conhost.exe" -ArgumentList "`"$exe`"" -WorkingDirectory $workDir -PassThru
      $game = $null
      [void](Wait-Until { $script:child = Get-CimInstance Win32_Process -Filter "ParentProcessId=$($hostProc.Id) AND Name='GameLib_conteinar.exe'"; $null -ne $script:child } 15)
      if ($script:child) { $game = Get-Process -Id $script:child.ProcessId; $null = $game.Handle }
      return @{ game = $game; host = $hostProc }
    }
    $game = Start-Process -FilePath $exe -WorkingDirectory $workDir -PassThru
    $null = $game.Handle
    return @{ game = $game; host = $null }
  }
  finally { foreach ($k in $env.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue } }
}

function Find-GameConsole($started, [string[]]$before) {
  $own = @("$($started.game.Id)"); if ($started.host) { $own += "$($started.host.Id)" }
  return @([GlfdCloseCheck]::ConsoleWindows() | Where-Object {
    $before -notcontains $_ -and ((($_ -split "\|", 4)[3] -like "*GameLib_conteinar*") -or ($own -contains ($_ -split "\|", 4)[2])) })
}

function Close-Console($started, [string[]]$before) {
  $w = @(Find-GameConsole $started $before)     # @(): a one-element result is unrolled to a string (first run)
  if ($w.Count -ne 1) { return "NOT_RUN ($($w.Count) console windows of this game)" }
  [void][GlfdCloseCheck]::PostMessage([IntPtr][long](($w[0] -split "\|")[0]), 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
  return "closed the console window ($((($w[0] -split '\|')[1])))"
}

# the selection flags of the game's console (0 = none), or -1 when the helper could not tell
function Get-SelectionFlags($game, [string]$tag) {
  $r = Join-Path $outDir "$tag.selection.txt"
  $h = Start-Process -FilePath $ctrlSend -ArgumentList "$($game.Id) 9 `"$r`"" -WindowStyle Hidden -PassThru
  if (-not $h.WaitForExit(10000)) { $h.Kill(); return -1 }
  $text = (Get-Content $r -ErrorAction SilentlyContinue) -join " "
  if ($text -match "^selection flags 0x([0-9a-fA-F]+)") { return [Convert]::ToInt32($Matches[1], 16) }
  return -1
}

function Send-Ctrl($game, [int]$kind, [string]$tag) {
  $r = Join-Path $outDir "$tag.ctrlsend.txt"
  # its own hidden console: run from this script's console, AttachConsole failed with 5 (already attached)
  # right after a case that closed a console (twice, rel_wt / close1)
  $h = Start-Process -FilePath $ctrlSend -ArgumentList "$($game.Id) $kind `"$r`"" -WindowStyle Hidden -PassThru
  if (-not $h.WaitForExit(10000)) { $h.Kill(); return "NOT_RUN (ConsoleCtrlSend did not end in 10 s)" }
  $sent = (Get-Content $r -ErrorAction SilentlyContinue) -join " "
  $what = "ConsoleCtrlSend $(if ($kind -eq 0) { 'CTRL_C' } else { 'CTRL_BREAK' }): $sent"
  # the helper could not deliver it: do not judge the case on whatever happens next (C3 in the first run
  # ended on a Ctrl+C this tool did not send; the sender is unknown)
  if ($sent -notlike "generate ok*") { return "NOT_RUN ($what)" }
  return $what
}

$allOk = $true
$caseIndex = 0
foreach ($case in $Cases) {
  ++$caseIndex
  $tag = "{0:D2}_{1}" -f $caseIndex, $case
  $exe = $gameExe; $envs = @{}
  if ($case -eq "C7") {
    if (-not (Test-Path $probeExe)) { "C7  SKIPPED  (no probe build at $probeExe; run Tests\render_fault_check.ps1 first)" | Tee-Object -FilePath $result -Append | Write-Host; continue }
    $exe = $probeExe; $envs = @{ GLFD_RENDER_FAULT = "present=0x887A0005@432,reason=0x887A0007@0" }
  }
  if ($case -eq "C10" -and -not $UseConhost) { "C10 SKIPPED  (a selection that holds the output is conhost's; run with -UseConhost)" | Tee-Object -FilePath $result -Append | Write-Host; continue }
  $log = Join-Path $workDir "Game.log"
  Remove-Item $log -ErrorAction SilentlyContinue
  $before = @([GlfdCloseCheck]::ConsoleWindows())
  $started = Start-Game $exe $envs
  $game = $started.game
  $notes = @(); $action = ""; $t0 = $null; $pre = $true; $hwnd = [IntPtr]::Zero
  if ($null -eq $game) { "$case  LAUNCH_FAILED" | Tee-Object -FilePath $result -Append | Write-Host; $allOk = $false; continue }
  try {

  if ($case -in @("C8", "C9")) {
    # right after launch: wait only for the console window, not for the game window
    [void](Wait-Until { @(Find-GameConsole $started $before).Count -ge 1 -or $game.HasExited } 10)
    $windowAtSignal = [GlfdCloseCheck]::FindGameWindow([uint32]$game.Id) -ne [IntPtr]::Zero
    $installedLine = (Test-Path $log) -and (@(Get-Content $log) -like "*go through the normal shutdown. a close waits*").Count -gt 0
    $t0 = Get-Date
    if ($case -eq "C8") { $action = Send-Ctrl $game 0 $case } else { $action = Close-Console $started $before }
    $notes += "game window existed when the signal was sent: $windowAtSignal"
    $notes += "window handle already published (Initialize finished) when the signal was sent: $installedLine"
  }
  else {
    # the GAME window by its class: under conhost, MainWindowHandle can be the console window (full_dbg_con)
    [void](Wait-Until { $game.HasExited -or [GlfdCloseCheck]::FindGameWindow([uint32]$game.Id) -ne [IntPtr]::Zero } 30)
    $hwnd = [GlfdCloseCheck]::FindGameWindow([uint32]$game.Id)
    if ($hwnd -eq [IntPtr]::Zero) { $notes += "NO GAME WINDOW (class GameLibWindow) found" }
    [void](Wait-Until { (Test-Path $log) -and (@(Get-Content $log) -match "frame rate:").Count -gt 0 } 20)
    switch ($case) {
      "C1" { $t0 = Get-Date; [void][GlfdCloseCheck]::PostMessage($hwnd, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero); $action = "WM_CLOSE to the game window" }
      "C2" { $t0 = Get-Date; $action = Close-Console $started $before }
      "C3" { $t0 = Get-Date; $action = Send-Ctrl $game 0 $case }
      "C4" { $t0 = Get-Date; $action = Send-Ctrl $game 1 $case }
      "C5" {
        [void][GlfdCloseCheck]::ShowWindow($hwnd, 6)
        [void](Wait-Until { (@(Get-Content $log) -match "paused \(window minimized\)").Count -gt 0 } 10)
        $pre = [GlfdCloseCheck]::IsIconic($hwnd) -and (@(Get-Content $log) -match 'paused \(window minimized\)').Count -gt 0
        $notes += "minimised and paused before the close: $pre"
        $t0 = Get-Date; $action = Close-Console $started $before
      }
      "C6" {
        [void][GlfdCloseCheck]::PostMessage($hwnd, 0x0112, [IntPtr]0xF010, [IntPtr]::Zero)   # WM_SYSCOMMAND SC_MOVE
        $pre = Wait-Until { [GlfdCloseCheck]::InMoveSize($hwnd) } 10
        $notes += "game thread in the move/size loop before the close: $pre"
        $t0 = Get-Date; $action = Close-Console $started $before
      }
      "C10" {
        $w = @(Find-GameConsole $started $before)
        if ($w.Count -eq 1 -and ($w[0] -split "\|")[1] -eq "ConsoleWindowClass") {
          # WM_SYSCOMMAND 0xFFF5 = Edit > Select All of the console window's system menu
          [void][GlfdCloseCheck]::PostMessage([IntPtr][long](($w[0] -split "\|")[0]), 0x0112, [IntPtr]0xFFF5, [IntPtr]::Zero)
        }
        [void](Wait-Until { (Get-SelectionFlags $game $tag) -gt 0 } 5)
        $flags = Get-SelectionFlags $game $tag
        $pre = $flags -gt 0
        $notes += "selection up before the close: $pre (flags $flags)"
        $t0 = Get-Date; $action = Close-Console $started $before
      }
      "C7" {
        $boxUp = Wait-Until { $game.Refresh(); $game.HasExited -or ([GlfdCloseCheck]::FindDialog([uint32]$game.Id) -ne [IntPtr]::Zero) } 20
        $pre = [GlfdCloseCheck]::FindDialog([uint32]$game.Id) -ne [IntPtr]::Zero
        $notes += "2-9 box up before the close: $pre"
        $t0 = Get-Date; $action = Close-Console $started $before
      }
    }
  }
  $ended = $game.WaitForExit(15000)
  $ms = ((Get-Date) - $t0).TotalMilliseconds
  if (-not $ended) { $game.Kill(); [void]$game.WaitForExit(5000); $notes += "DID NOT END within 15 s (killed)" }
  $code = if ($ended) { "0x{0:X8}" -f $game.ExitCode } else { "killed" }
  Start-Sleep -Milliseconds 300
  $lines = if (Test-Path $log) { @(Get-Content $log) } else { @() }
  Copy-Item $log (Join-Path $outDir "$tag.Game.log") -ErrorAction SilentlyContinue
  foreach ($w in @(Find-GameConsole $started $before)) {        # a console left open after the game ended
    [void][GlfdCloseCheck]::PostMessage([IntPtr][long](($w -split "\|")[0]), 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
  }

  $joined   = @($lines | Where-Object { $_ -like "*every worker joined*" }).Count
  $lastLine = if ($lines.Count -gt 0) { $lines[-1] } else { "" }
  $console  = @($lines | Where-Object { $_ -like "*console: *stopping the game through the normal shutdown*" }).Count
  $ok = $ended -and $action -notlike "NOT_RUN*"
  if ($case -notin @("C8", "C9") -and $hwnd -eq [IntPtr]::Zero) { $ok = $false }   # C8 / C9 act before the window exists
  # C5 / C6 / C7 test a state; if the game was not in it, the case did not test what it is for
  if ($case -in @("C5", "C6", "C7", "C10") -and -not $pre) { $ok = $false; $notes += "NOT_RUN as meant: the precondition did not hold" }
  if ($joined -ne 1) { $ok = $false; $notes += "join line x$joined" }
  if ($lastLine -notlike "*=== Engine Shutdown ===*") { $ok = $false; $notes += "last line is not Engine Shutdown: $lastLine" }
  if ($case -in @("C2", "C3", "C4", "C5", "C6", "C8", "C9", "C10") -and $console -ne 1) { $ok = $false; $notes += "console line x$console" }
  if ($case -in @("C9", "C10")) {
    $limit = @($lines | Where-Object { $_ -match "a close waits up to (\d+) ms" } | ForEach-Object { [int]$Matches[1] })
    $notes += "close -> end {0:N0} ms against the handler's limit {1} ms" -f $ms, ($(if ($limit.Count) { $limit[0] } else { "?" }))
  }
  if ($case -in @("C8", "C9")) {
    $frameRate = @($lines | Where-Object { $_ -like "*frame rate:*" }).Count
    $notes += "frame loop started before the signal was seen ('frame rate' line): $($frameRate -gt 0)"
    # the case only tests Run's flag check if the signal came before the handle was published
    if ($installedLine) { $ok = $false; $notes += "NOT_RUN as meant: the signal came after Initialize (only the flag path is the point of C8)" }
  }
  if (-not $ok) { $allOk = $false }
  $row = "{0,-3} {1,-16} ended {2} in {3,6:N0} ms  exit {4}  | {5} | {6}" -f $case, $(if ($ok) { "OK" } else { "NOT AS EXPECTED" }), $ended, $ms, $code, $action, ($notes -join "; ")
  $row | Add-Content $result
  Write-Host $row
  }
  finally {
    # never leave a game running (it would keep writing to Game.log under the next case)
    if (-not $game.HasExited) { $game.Kill(); [void]$game.WaitForExit(5000); Write-Host "${tag}: killed a game left running" }
  }
  Start-Sleep -Milliseconds 500
}
if ($allOk) { Write-Host "all cases as expected. details: $result"; exit 0 }
Write-Host "at least one case NOT as expected. details: $result"
exit 1
