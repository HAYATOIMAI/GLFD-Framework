# ---------------------------------------------------------------------------
#  GLFD: does the game stop correctly when drawing can no longer continue? (ECS 2-9)
#
#  A lost graphics device cannot be caused on demand (on this machine `dxcap -forcetdr` did
#  not remove the device). [CORRECTED] It can, but not every time: at 17:14 drawing went on after
#  dxcap -forcetdr (no driver event was recorded; whether it was a real TDR is unknown), at 18:53
#  the same command removed the device (Present -> DXGI_ERROR_DEVICE_REMOVED,
#  reason DXGI_ERROR_DEVICE_RESET). Not on demand, so the probe is still needed.
#  The probe build (GLFD_RENDER_FAULT_PROBE) replaces the results
#  of Present / Present(TEST) / Map / GetDeviceRemovedReason from the environment variable
#  GLFD_RENDER_FAULT (format: Tests\RenderFaultProbe.cpp). The sources are NOT edited: the
#  probe build is a separate MSBuild output (x64\ProbeRelease\), and the game build never
#  contains Tests\RenderFaultProbe.cpp (ECS 2-9 decision 2, 開発手法 §4.14b).
#
#  Cases:
#   H1 no fault                    -> runs; closing the window ends it with 0; no box
#   H2 Present lost                -> ONE failure line "at Present", box AFTER "Engine Shutdown", exit != 0
#                                     (reason DXGI_ERROR_DEVICE_RESET: what the real loss at 18:53 returned)
#   H3 Map lost                    -> the same, "at Map"
#   H4 Map fails, device not lost  -> keeps running; ONE pair of skipped-draw lines; closing ends with 0
#   H5 OCCLUDED, then TEST lost    -> the TDR order: stops at "Present(TEST)", no "resumed", exit != 0
#   H6 startup failure             -> the GAME build started from x64\Release (the wrong folder):
#                                     box with the working folder, exit != 0
#  The box is found by the script (a #32770 window of the game process), its text is
#  recorded, and it is closed with IDOK. The game has no switch to hide the box.
#
#  usage:  powershell -File Tests\render_fault_check.ps1 [-SkipBuild] [-OutName name]
#  Output: a NEW folder Tests\build\render_fault\<OutName>\ per run (result.txt + each Game.log).
#  Exit codes: 0 every case as expected / 1 at least one case not / 2 build failed
#  Every wait has an upper bound. Names differ in spelling, not only in case.
# ---------------------------------------------------------------------------
param([switch]$SkipBuild, [string]$OutName = ("run_" + (Get-Date -Format "yyyyMMdd_HHmmss")))
$ErrorActionPreference = "Stop"
$repoRoot  = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$probeDir  = Join-Path $repoRoot "x64\ProbeRelease\"
$probeExe  = Join-Path $probeDir "GameLib_conteinar.exe"
$gameExe   = Join-Path $repoRoot "x64\Release\GameLib_conteinar.exe"
$outDir    = Join-Path $repoRoot "Tests\build\render_fault\$OutName"
if (Test-Path $outDir) { Remove-Item -Recurse -Force $outDir }
New-Item -ItemType Directory -Path $outDir | Out-Null

if (-not $SkipBuild) {
  $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
  $msbuild = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe" | Select-Object -First 1
  if (-not $msbuild) { throw "MSBuild was not found" }
  $buildLog = Join-Path $outDir "build_probe.log"
  & $msbuild (Join-Path $repoRoot "GameLib_conteinar.vcxproj") /nologo /m /v:minimal /t:Rebuild `
      /p:Configuration=Release /p:Platform=x64 /p:GLFDProbeDefines=GLFD_RENDER_FAULT_PROBE `
      "/p:OutDir=$probeDir" "/p:IntDir=$($probeDir)obj\" *> $buildLog
  if ($LASTEXITCODE -ne 0) { Write-Host "BUILD FAILED (see $buildLog)"; exit 2 }
}
if (-not (Test-Path $probeExe)) { Write-Host "no probe exe at $probeExe"; exit 2 }
if (-not (Test-Path $gameExe)) { Write-Host "no game exe at $gameExe (build Release first)"; exit 2 }

if (-not ("GlfdFaultWin" -as [type])) {
  Add-Type -Namespace "" -Name GlfdFaultWin -MemberDefinition @'
public delegate bool EnumProc(IntPtr h, IntPtr l);
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
[DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc cb, IntPtr l);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
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
[DllImport("user32.dll")] public static extern IntPtr SendMessageTimeout(IntPtr h, uint msg, IntPtr w, IntPtr l, uint flags, uint ms, out IntPtr res);
public static IntPtr FindButton(IntPtr dlg) {
  IntPtr found = IntPtr.Zero;
  EnumChildWindows(dlg, (h, l) => {
    var cls = new System.Text.StringBuilder(64); GetClassName(h, cls, 64);
    if (cls.ToString() == "Button") { found = h; return false; }
    return true;
  }, IntPtr.Zero);
  return found;
}
public static string DialogText(IntPtr dlg) {
  var all = new System.Text.StringBuilder();
  EnumChildWindows(dlg, (h, l) => {
    var cls = new System.Text.StringBuilder(64); GetClassName(h, cls, 64);
    if (cls.ToString() == "Static") { var t = new System.Text.StringBuilder(2048); GetWindowText(h, t, 2048); all.Append(t.ToString()); }
    return true;
  }, IntPtr.Zero);
  return all.ToString();
}
'@
}

function Wait-Until([scriptblock]$cond, [double]$seconds) {
  $until = (Get-Date).AddSeconds($seconds)
  while ((Get-Date) -lt $until) { if (& $cond) { return $true }; Start-Sleep -Milliseconds 100 }
  return (& $cond)
}

function Run-Case([string]$caseName, [string]$exePath, [string]$workDir, [string]$fault,
                  [bool]$expectBox, [string[]]$mustLog, [string[]]$mustNotLog, [int]$runSeconds) {
  $logPath = Join-Path $workDir "Game.log"
  Remove-Item $logPath -ErrorAction SilentlyContinue
  $env:GLFD_RENDER_FAULT = $fault
  $proc = Start-Process -FilePath $exePath -WorkingDirectory $workDir -PassThru
  $env:GLFD_RENDER_FAULT = $null
  $notes = @()
  $box = [IntPtr]::Zero
  $boxText = ""
  $lastLineAtBox = ""
  # wait for either a dialog, the exit, or the run time
  [void](Wait-Until { $proc.Refresh(); $proc.HasExited -or ([GlfdFaultWin]::FindDialog([uint32]$proc.Id) -ne [IntPtr]::Zero) } $runSeconds)
  if (-not $proc.HasExited) { $box = [GlfdFaultWin]::FindDialog([uint32]$proc.Id) }
  if ($box -ne [IntPtr]::Zero) {
    $boxText = [GlfdFaultWin]::DialogText($box)
    if (Test-Path $logPath) { $lastLineAtBox = (Get-Content $logPath | Select-Object -Last 1) }
    $proc.Refresh(); $cpuAtBox = $proc.TotalProcessorTime.TotalSeconds
    # press the box's only button (BM_CLICK). WM_COMMAND IDOK posted to the box did not close it (first run)
    $button = [GlfdFaultWin]::FindButton($box)
    $res = [IntPtr]::Zero
    if ($button -ne [IntPtr]::Zero) { [void][GlfdFaultWin]::SendMessageTimeout($button, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero, 2, 2000, [ref]$res) }
    else { $notes += "no button found in the box"; [void][GlfdFaultWin]::PostMessage($box, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) }
    $notes += ("box text: " + ($boxText -replace "`r?`n", " | "))
    $notes += "last log line when the box was up: $lastLineAtBox"
    $notes += ("cpu seconds used by then: {0:N2}" -f $cpuAtBox)
  }
  elseif (-not $proc.HasExited) {
    $proc.Refresh()
    if ($proc.MainWindowHandle -ne 0) { [void][GlfdFaultWin]::PostMessage($proc.MainWindowHandle, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) }   # WM_CLOSE
  }
  $ended = $proc.WaitForExit(15000)
  if (-not $ended) { $proc.Kill(); [void]$proc.WaitForExit(5000); $notes += "DID NOT EXIT within 15 s (killed)" }
  $code = if ($ended) { $proc.ExitCode } else { -1 }
  $log = if (Test-Path $logPath) { Get-Content $logPath } else { @() }
  Copy-Item $logPath (Join-Path $outDir "$caseName.Game.log") -ErrorAction SilentlyContinue

  $ok = $ended
  if ($expectBox) {
    if ($box -eq [IntPtr]::Zero) { $ok = $false; $notes += "EXPECTED a box, none appeared" }
    elseif ($lastLineAtBox -notmatch "=== Engine Shutdown ===") { $ok = $false; $notes += "the box came BEFORE 'Engine Shutdown'" }
    if ($code -eq 0) { $ok = $false; $notes += "EXPECTED a non-zero exit code" }
  }
  else {
    if ($box -ne [IntPtr]::Zero) { $ok = $false; $notes += "UNEXPECTED box" }
    if ($code -ne 0) { $ok = $false; $notes += "EXPECTED exit code 0" }
  }
  foreach ($pattern in $mustLog) {
    $hits = @($log | Where-Object { $_ -like "*$pattern*" }).Count
    if ($hits -ne 1) { $ok = $false; $notes += "EXPECTED exactly 1 line with '$pattern', found $hits" }
  }
  foreach ($pattern in $mustNotLog) {
    $hits = @($log | Where-Object { $_ -like "*$pattern*" }).Count
    if ($hits -ne 0) { $ok = $false; $notes += "EXPECTED no line with '$pattern', found $hits" }
  }
  if (@($log | Where-Object { $_ -like "*every worker joined*" }).Count -ne 1) { $ok = $false; $notes += "no 'every worker joined' line" }
  $verdict = if ($ok) { "OK" } else { "NOT AS EXPECTED" }
  $line = "{0,-3} {1,-16} exit {2,3}  box {3,-5} {4}" -f $caseName, $verdict, $code, ($box -ne [IntPtr]::Zero), (($notes | Where-Object { $_ -notlike "box text*" -and $_ -notlike "last log*" -and $_ -notlike "cpu*" }) -join "; ")
  $line | Add-Content (Join-Path $outDir "result.txt")
  $notes | ForEach-Object { "      $_" } | Add-Content (Join-Path $outDir "result.txt")
  Write-Host $line
  return $ok
}

$lost    = "the graphics device was lost"
$results = @()
$results += Run-Case "H1" $probeExe $repoRoot "" $false @() @("render:") 6
$results += Run-Case "H2" $probeExe $repoRoot "present=0x887A0005@432,reason=0x887A0007@0" $true @("$lost at Present: DXGI_ERROR_DEVICE_REMOVED") @("at Present(TEST)", "resumed after") 20
$results += Run-Case "H3" $probeExe $repoRoot "map=0x8007000E@432,reason=0x887A0005@0" $true @("$lost at Map: E_OUTOFMEMORY") @("not drawing this frame") 20
$results += Run-Case "H4" $probeExe $repoRoot "map=0x8007000E@432+3" $false @("not drawing this frame", "drawing again after 3 frame(s)") @("stopping the game") 8
$results += Run-Case "H5" $probeExe $repoRoot "present=0x087A0001@432+1,test=0x887A0005@0,reason=0x887A0006@0" $true @("$lost at Present(TEST): DXGI_ERROR_DEVICE_REMOVED") @("resumed after") 20
$results += Run-Case "H6" $gameExe (Split-Path -Parent $gameExe) "" $true @("DX11 Init Failed!") @() 20

$failed = @($results | Where-Object { -not $_ }).Count
Write-Host ("{0} of {1} cases as expected. details: {2}" -f ($results.Count - $failed), $results.Count, (Join-Path $outDir "result.txt"))
if ($failed -ne 0) { exit 1 }
exit 0
