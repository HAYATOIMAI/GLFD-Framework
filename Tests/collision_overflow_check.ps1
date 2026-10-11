# ---------------------------------------------------------------------------
#  GLFD: does Boid run 3 minutes without the event queue overflowing? (ECS 2-12, T-ECS-49)
#
#  Before 2-12, the Boid demo published one CollisionEvent per detected contact (up to about 120,000 in one step in a
#  dense flock) for a subscriber that only counted them. The queue (16,384) overflowed from step 3,400-4,300 on and
#  kept overflowing (the game logged it 151 times in 3 minutes). 2-12: CollisionSystem asks once per step whether
#  anyone subscribes, and publishes only then; the Boid scene does not subscribe and observes by the count.
#
#  Headless: EcsBenchmark runs the same systems in the same order as the scene (1 frame = 1 step), with no subscriber.
#  Judged (all must hold):
#   - the run completed (exit 0, "@end", the "overflow:" line covers every measured frame)
#   - frames_with_drops = 0 and dropped_total = 0 (no event was dropped in any frame)
#   - contacts_min > 0 (collisions were detected in every frame: the check is not passing because nothing happens)
#  Default 11,300 frames = 3 minutes of steps at 62.5 steps/s (11,250) plus the warm-up.
#
#  usage:  powershell -File Tests\collision_overflow_check.ps1 [-Frames 11300] [-Warmup 30] [-SkipBuild]
#          [-ExePath path -WorkDir root] [-OutName name]
#   -ExePath runs another build (the pre-fix build for the teeth); -WorkDir is the folder it reads
#   Resource\GameConfig.jsonc from (a repository root). Without -ExePath the benchmarks are built first.
#  Output: a NEW folder Tests\build\collision_overflow\<OutName>\ (result.txt + the benchmark output).
#  Exit codes: 0 as expected / 1 not as expected / 2 could not build or run.
#  No window is opened. Every wait has an upper bound (20 minutes for the run).
# ---------------------------------------------------------------------------
param([int]$Frames = 11300, [int]$Warmup = 30, [switch]$SkipBuild, [string]$ExePath = "", [string]$WorkDir = "",
      [string]$OutName = ("run_" + (Get-Date -Format "yyyyMMdd_HHmmss")))
$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$outDir = Join-Path $repoRoot "Tests\build\collision_overflow\$OutName"
if (Test-Path $outDir) { Remove-Item -Recurse -Force $outDir }
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$result = Join-Path $outDir "result.txt"

if ($ExePath) {
  $exe = $ExePath
  $runDir = if ($WorkDir) { $WorkDir } else { Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $ExePath)) }
}
else {
  $exe = Join-Path $repoRoot "Tests\build\EcsBenchmark.exe"
  $runDir = $repoRoot
  if (-not $SkipBuild) {
    cmd /c "`"$repoRoot\Tests\build_benchmarks.bat`" < nul" > (Join-Path $outDir "build.txt") 2>&1
    if ($LASTEXITCODE -ne 0) { "could not build the benchmarks (see build.txt)" | Tee-Object -FilePath $result | Write-Host; exit 2 }
  }
}
if (-not (Test-Path $exe)) { "no benchmark at $exe" | Tee-Object -FilePath $result | Write-Host; exit 2 }
if (-not (Test-Path (Join-Path $runDir "Resource\GameConfig.jsonc"))) { "no Resource\GameConfig.jsonc under $runDir" | Tee-Object -FilePath $result | Write-Host; exit 2 }
"benchmark $exe (written $((Get-Item $exe).LastWriteTime.ToString('s'))), run from $runDir, frames $Frames warmup $Warmup" | Set-Content $result

$raw = Join-Path $outDir "benchmark.txt"
$t0 = Get-Date
$p = Start-Process -FilePath $exe -ArgumentList "$Frames $Warmup" -WorkingDirectory $runDir -NoNewWindow -RedirectStandardOutput $raw -PassThru
$null = $p.Handle                                   # so that ExitCode is available after the exit
if (-not $p.WaitForExit(1200000)) { $p.Kill(); [void]$p.WaitForExit(5000); "the benchmark did not end within 20 minutes (killed)" | Tee-Object -FilePath $result -Append | Write-Host; exit 2 }
$sec = ((Get-Date) - $t0).TotalSeconds
$lines = @(Get-Content $raw)
$code = $p.ExitCode

$ok = $true
$notes = @()
if ($code -ne 0) { $ok = $false; $notes += "exit code $code" }
if (@($lines | Where-Object { $_ -eq "@end" -or $_.StartsWith("@end ") }).Count -eq 0) { $ok = $false; $notes += "no @end line (the output stopped early)" }
$ovf = @($lines | Where-Object { $_.StartsWith("overflow: ") })
if ($ovf.Count -ne 1) {
  $ok = $false; $notes += "found $($ovf.Count) 'overflow:' line(s), expected 1"
}
else {
  $kv = @{}
  foreach ($t in $ovf[0].Substring(10).Split(" ", [StringSplitOptions]::RemoveEmptyEntries)) {
    $i = $t.IndexOf("="); if ($i -gt 0) { $kv[$t.Substring(0, $i)] = [double]$t.Substring($i + 1) }
  }
  foreach ($k in @("frames_with_drops", "dropped_total", "published_total", "contacts_min", "measured")) {
    if (-not $kv.ContainsKey($k)) { $ok = $false; $notes += "the overflow line has no $k" }
  }
  if ($ok) {
    if ($kv["measured"] -lt ($Frames - $Warmup)) { $ok = $false; $notes += "measured $($kv['measured']) frame(s), expected $($Frames - $Warmup)" }
    if ($kv["frames_with_drops"] -ne 0 -or $kv["dropped_total"] -ne 0) { $ok = $false; $notes += "events were DROPPED: $($kv['frames_with_drops']) frame(s), $($kv['dropped_total']) event(s)" }
    if ($kv["contacts_min"] -le 0) { $ok = $false; $notes += "a frame detected no contact (contacts_min $($kv['contacts_min'])): the check would pass with nothing happening" }
    $notes += "frames with drops $($kv['frames_with_drops']), dropped $($kv['dropped_total']), published $($kv['published_total']), contacts min $($kv['contacts_min']), measured $($kv['measured'])"
  }
}
$row = "{0}  in {1:N1} s | {2}" -f $(if ($ok) { "OK (no overflow over the run)" } else { "NOT AS EXPECTED" }), $sec, ($notes -join "; ")
$row | Add-Content $result
Write-Host $row
if ($ok) { exit 0 }
exit 1
