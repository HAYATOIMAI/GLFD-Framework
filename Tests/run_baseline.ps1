<#
 GLFD 性能の基準線 (ECS 2-5)

 1 つのコマンドで ビルド -> 計測 -> 記録 まで行う。入口は run_baseline.bat。

   run_baseline.bat                     本計測(既定: 20 周 + 遅い区間 5 回 + 起こす費用 5 回)
   run_baseline.bat -Rounds 2 -SlowRuns 1 -WakeRuns 1        試走
   run_baseline.bat -Inject crash -Rounds 1 -SlowRuns 0 -WakeRuns 0   歯の確認

 ## 何を守っているか
  - **失敗を数字と取り違えない**(開発手法 §4.14a)。ビルド失敗、起動失敗、時間切れ、
    0 以外の終了コード、`@end` の無い出力、引数と違う設定で走った回は、すべて FAILED の行として
    残し、**集計には入れない**。1 回でも失敗があればスクリプトは 1 で終わる。
    2-4 では起動に失敗した回を「HUNG」と記録した。状態は起きたことの名前で書く。
  - **回ごとに値を作り直す。** 前の回の値が残ると、別の回の数字に化ける(2-5 の探索で踏んだ)。
  - **古い exe を測らない。** ビルドの前に exe を消し、ビルドの後に更新時刻を確かめる。
  - **環境は自動で取る。** 手で書き写すと抜ける。

 ## 基準線は A/B 比較の代わりにならない
  記録は「この条件でこの数字だった」という参照。変更の効果は同じ機械で交互に測って比べる
  (開発手法 §5.2)。条件が違う数字と並べないこと。

 このファイルは UTF-8 BOM 付き(Windows PowerShell 5.1 が日本語を正しく読むため)。
#>
[CmdletBinding()]
param(
  [int]$Rounds = 20,
  [int]$SlowRuns = 5,
  [int]$WakeRuns = 5,
  [int]$TimeoutSec = 120,
  [int]$BackgroundSec = 10,
  [string]$OutRoot = "",
  [string]$Label = "",
  [ValidateSet("", "build", "launch", "crash", "hang", "noend", "exitcode")]
  [string]$Inject = ""
)

Set-StrictMode -Version 2
$ErrorActionPreference = "Stop"
$Inv = [Globalization.CultureInfo]::InvariantCulture

$RepoRoot = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $PSScriptRoot "build"
Set-Location -LiteralPath $RepoRoot

# ----------------------------------------------------------------- 設定
# Boid は定常状態が無い(群れが固まり、衝突が増え続ける)。区間を固定して明記する
$BoidArgs     = @("300", "30")
$BoidSlowArgs = @("3300", "3000")   # 長く走らせたときの参照(比較には使わない)
$SmallArgs    = @("3000", "900", "small")
$LargeArgs    = @("3000", "900", "large")
$WakeArgs     = @("2000", "200")

$Stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$Commit = (& git rev-parse HEAD).Trim()
$Short = $Commit.Substring(0, 7)
if ($OutRoot -eq "") {
  if ($Inject -ne "") { $OutRoot = Join-Path $env:TEMP "glfd_baseline_teeth" }
  else                { $OutRoot = Join-Path $RepoRoot "Docs\Baseline" }
}
$Name = "{0}_{1}" -f $Stamp, $Short
if ($Label -ne "") { $Name = "{0}_{1}" -f $Name, $Label }
if ($Inject -ne "") { $Name = "{0}_inject-{1}" -f $Name, $Inject }
$OutDir = Join-Path $OutRoot $Name
$RawDir = Join-Path $OutDir "raw"
New-Item -ItemType Directory -Force -Path $RawDir | Out-Null

function Write-Utf8Bom([string]$path, [string[]]$lines) {
  $text = ($lines -join "`r`n") + "`r`n"
  [IO.File]::WriteAllText($path, $text, (New-Object Text.UTF8Encoding($true)))
}

function Fmt([double]$v, [int]$digits = 1) { return $v.ToString("F$digits", $Inv) }

function Get-Median([double[]]$values) {
  if ($values.Count -eq 0) { return [double]::NaN }
  $sorted = [double[]]($values | Sort-Object)
  return $sorted[[int][Math]::Floor($sorted.Count / 2)]
}

# ----------------------------------------------------------------- 背景負荷
function Measure-Background([int]$seconds) {
  $before = @{}
  foreach ($p in Get-Process) {
    try { $before[$p.Id] = $p.TotalProcessorTime.TotalSeconds } catch { }
  }
  $samples = New-Object System.Collections.Generic.List[double]
  $sw = [Diagnostics.Stopwatch]::StartNew()
  for ($i = 0; $i -lt $seconds; ++$i) {
    Start-Sleep -Seconds 1
    $cpu = Get-CimInstance -ClassName Win32_PerfFormattedData_PerfOS_Processor -Filter "Name='_Total'"
    $samples.Add([double]$cpu.PercentProcessorTime)
  }
  $elapsed = $sw.Elapsed.TotalSeconds
  $deltas = New-Object System.Collections.Generic.List[object]
  foreach ($p in Get-Process) {
    if (-not $before.ContainsKey($p.Id)) { continue }
    try {
      $d = $p.TotalProcessorTime.TotalSeconds - $before[$p.Id]
      if ($d -gt 0) { $deltas.Add([pscustomobject]@{ Name = $p.ProcessName; Cores = $d / $elapsed }) }
    } catch { }
  }
  $top = $deltas | Group-Object Name | ForEach-Object {
    [pscustomobject]@{ Name = $_.Name; Cores = ($_.Group | Measure-Object Cores -Sum).Sum }
  } | Sort-Object Cores -Descending | Select-Object -First 5
  $topText = ($top | ForEach-Object { "{0} {1}" -f $_.Name, (Fmt $_.Cores 2) }) -join ", "
  $mean = ($samples | Measure-Object -Average).Average
  $max = ($samples | Measure-Object -Maximum).Maximum
  return [pscustomobject]@{
    Mean = [double]$mean; Max = [double]$max; Seconds = $seconds; Top = $topText
  }
}

# ----------------------------------------------------------------- 環境
function Get-Environment {
  $cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
  $os = Get-CimInstance Win32_OperatingSystem
  $cs = Get-CimInstance Win32_ComputerSystem
  $battery = @(Get-CimInstance Win32_Battery).Count
  $plan = (& powercfg /getactivescheme | Out-String).Trim()
  return [ordered]@{
    "CPU"            = $cpu.Name.Trim()
    "物理 / 論理コア(WMI)" = "{0} / {1}" -f $cpu.NumberOfCores, $cpu.NumberOfLogicalProcessors
    "最大クロック"   = "{0} MHz" -f $cpu.MaxClockSpeed
    "電源プラン"     = $plan
    "OS"             = "{0} {1} (build {2})" -f $os.Caption, $os.Version, $os.BuildNumber
    "メモリ"         = "{0} GB(開始時の空き {1} GB)" -f (Fmt ($cs.TotalPhysicalMemory / 1GB) 1), (Fmt ($os.FreePhysicalMemory * 1KB / 1GB) 1)
    "機種"           = "{0} {1}" -f $cs.Manufacturer.Trim(), $cs.Model.Trim()
    "バッテリー"     = $(if ($battery -eq 0) { "なし" } else { "あり ($battery)" })
  }
}

# ----------------------------------------------------------------- 1 回の実行
function Invoke-Bench([int]$seq, [int]$round, [string]$bench, [string]$exeName,
                      [string[]]$argList, [hashtable]$expect) {
  # **回ごとに作り直す。** 前の回の値を引き継がない
  $rec = [ordered]@{
    Seq = $seq; Round = $round; Bench = $bench; Args = ($argList -join " ")
    Status = "NOT_RUN"; ExitCode = ""; WallSec = 0.0; Note = ""
    Rows = (New-Object System.Collections.Generic.List[object])
    Ratios = (New-Object System.Collections.Generic.List[object])
    Meta = @{}; Cpu = @{}; Problems = @{}
    Raw = ("raw/{0:D3}_{1}.txt" -f $seq, $bench)
  }
  $rawPath = Join-Path $OutDir $rec.Raw
  $errPath = $rawPath + ".stderr"
  $exe = Join-Path $BuildDir $exeName
  Write-Host ("[{0,3}] round {1,2} {2,-10} {3}" -f $seq, $round, $bench, $rec.Args) -NoNewline

  $sw = [Diagnostics.Stopwatch]::StartNew()
  $proc = $null
  try {
    $proc = Start-Process -FilePath $exe -ArgumentList $argList -WorkingDirectory $RepoRoot `
              -NoNewWindow -PassThru -RedirectStandardOutput $rawPath -RedirectStandardError $errPath
  } catch {
    $rec.Note = $_.Exception.Message
  }
  if ($null -eq $proc) {
    $rec.Status = "LAUNCH_FAILED"
    Write-Host "  -> LAUNCH_FAILED"
    return [pscustomobject]$rec
  }
  $null = $proc.Handle   # 終了後も ExitCode を読めるように握っておく

  if (-not $proc.WaitForExit($TimeoutSec * 1000)) {
    try { $proc.Kill() } catch { }
    $null = $proc.WaitForExit(10000)
    $rec.Status = "TIMEOUT"
    $rec.Note = "killed after $TimeoutSec s"
    $rec.WallSec = $sw.Elapsed.TotalSeconds
    Write-Host "  -> TIMEOUT"
    return [pscustomobject]$rec
  }
  $proc.WaitForExit()
  $rec.WallSec = $sw.Elapsed.TotalSeconds
  $code = $proc.ExitCode
  $rec.ExitCode = $code

  $sawEnd = $false
  $lines = @()
  if (Test-Path -LiteralPath $rawPath) { $lines = @(Get-Content -LiteralPath $rawPath) }
  foreach ($line in $lines) {
    if (-not $line.StartsWith("@")) { continue }
    $tokens = $line.Substring(1).Split(" ", [StringSplitOptions]::RemoveEmptyEntries)
    $kind = $tokens[0]
    $kv = @{}
    foreach ($t in @($tokens | Select-Object -Skip 1)) {
      $i = $t.IndexOf("=")
      if ($i -gt 0) { $kv[$t.Substring(0, $i)] = $t.Substring($i + 1) }
    }
    switch ($kind) {
      "end"      { $sawEnd = $true }
      "meta"     { foreach ($k in $kv.Keys) { $rec.Meta[$k] = $kv[$k] } }
      "cpu"      { $rec.Cpu = $kv }
      "problems" { $rec.Problems = $kv }
      "ratio"    { $rec.Ratios.Add($kv) }
      default    { $kv["kind"] = $kind; $rec.Rows.Add($kv) }
    }
  }

  # 状態は起きたことの名前で書く。最初に当たったものを採る
  if ($code -ne 0) {
    if ($code -lt 0 -or $code -gt 255) { $rec.Status = "EXIT_0x{0:X8}" -f $code }
    else                               { $rec.Status = "EXIT_$code" }
  } elseif (-not $sawEnd) {
    $rec.Status = "NO_END"
    $rec.Note = "exit code 0 but no @end line (the output stopped early)"
  } else {
    $rec.Status = "OK"
    foreach ($k in $expect.Keys) {
      $got = $rec.Meta[$k]
      if ("$got" -ne "$($expect[$k])") {
        $rec.Status = "PARAM_MISMATCH"
        $rec.Note += "$k expected $($expect[$k]) got $got; "
      }
    }
    if ($rec.Status -eq "OK" -and $rec.Meta["fail"] -ne "none") {
      $rec.Status = "INJECTED"
      $rec.Note = "a failure was injected (--fail=$($rec.Meta["fail"]))"
    }
    if ($rec.Status -eq "OK" -and $rec.Meta["bench"] -eq "survivor") {
      if ($rec.Meta["scrambled"] -ne "yes") {
        $rec.Status = "NOT_SCRAMBLED"
        $rec.Note = "dense order was not scrambled when measuring started (§5.8)"
      }
      foreach ($k in $rec.Problems.Keys) {
        if ($rec.Problems[$k] -ne "0") {
          $rec.Status = "PROBLEMS"
          $rec.Note += "$k=$($rec.Problems[$k]); "
        }
      }
    }
    if ($rec.Status -eq "OK" -and $rec.Meta["bench"] -ne "wake" -and $rec.Cpu.Count -eq 0) {
      $rec.Status = "NO_CPU"
      $rec.Note = "no @cpu line"
    }
  }
  Write-Host ("  -> {0} ({1:F1}s)" -f $rec.Status, $rec.WallSec)
  return [pscustomobject]$rec
}

function Get-Row($run, [string]$kind, [string]$name) {
  foreach ($r in $run.Rows) { if ($r["kind"] -eq $kind -and $r["name"] -eq $name) { return $r } }
  return $null
}

function Num($table, [string]$key) {
  return [double]::Parse($table[$key], $Inv)
}

# =================================================================== 本体
Write-Host "=== GLFD baseline (ECS 2-5) -> $OutDir"
if ($Inject -ne "") { Write-Host "*** teeth check: injecting '$Inject' ***" }

$SourceCommit = (& git log -1 --format=%H -- Source).Trim()
$SourceDirty = @(& git status --porcelain -- Source)
$TrackedDirty = @(& git status --porcelain --untracked-files=no)
$StartTime = Get-Date
$Envr = Get-Environment

Write-Host "background load before ($BackgroundSec s)..."
$BgBefore = Measure-Background $BackgroundSec

# ----------------------------------------------------------------- ビルド
$BuildLog = Join-Path $OutDir "build.log"
$Exes = @("EcsBenchmark.exe", "EcsSurvivorBenchmark.exe", "JobWakeBenchmark.exe")
foreach ($e in $Exes) {
  $p = Join-Path $BuildDir $e
  if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Force }   # 古い exe を測らない
}
$BuildStart = Get-Date
if ($Inject -eq "build") { $env:GLFD_BENCH_INJECT_BUILD_FAILURE = "1" }
Write-Host "building..."
& cmd.exe /c "`"$PSScriptRoot\build_benchmarks.bat`" > `"$BuildLog`" 2>&1"
$BuildExit = $LASTEXITCODE
$env:GLFD_BENCH_INJECT_BUILD_FAILURE = $null
$BuildStatus = "OK"
if ($BuildExit -ne 0) { $BuildStatus = "BUILD_FAILED (exit $BuildExit)" }
else {
  foreach ($e in $Exes) {
    $p = Join-Path $BuildDir $e
    if (-not (Test-Path -LiteralPath $p)) { $BuildStatus = "BUILD_FAILED ($e missing)"; break }
    if ((Get-Item -LiteralPath $p).LastWriteTime -lt $BuildStart) { $BuildStatus = "STALE_EXE ($e)"; break }
  }
}
$Warnings = @(Select-String -LiteralPath $BuildLog -Pattern "warning C\d+" | ForEach-Object { $_.Line.Trim() })
Write-Host "build: $BuildStatus"

# ----------------------------------------------------------------- 計測
$Runs = New-Object System.Collections.Generic.List[object]
if ($BuildStatus -eq "OK") {
  $seq = 0
  # 建てたばかりの exe の初回は起動が遅い(試走で 2.8 s。ウイルス対策の走査と見ている【推測】)。
  # 各 exe を 1 回ずつ走らせて捨てる。**失敗すれば失敗として数える**(集計には入れない)
  $Runs.Add((Invoke-Bench (++$seq) 0 "prime-boid" "EcsBenchmark.exe" @("40", "10") @{ bench = "boid" }))
  $Runs.Add((Invoke-Bench (++$seq) 0 "prime-survivor" "EcsSurvivorBenchmark.exe" @("100", "900", "small") @{ bench = "survivor" }))
  $Runs.Add((Invoke-Bench (++$seq) 0 "prime-wake" "JobWakeBenchmark.exe" @("20", "0") @{ bench = "wake" }))
  for ($r = 1; $r -le $Rounds; ++$r) {
    $boidExe = "EcsBenchmark.exe"
    if ($Inject -eq "launch" -and $r -eq 1) { $boidExe = "EcsBenchmark_injected_missing.exe" }
    $smallArgs = $SmallArgs
    if ($r -eq 1 -and @("crash", "hang", "noend", "exitcode") -contains $Inject) {
      $smallArgs = $SmallArgs + @("--fail=$Inject")
    }
    $Runs.Add((Invoke-Bench (++$seq) $r "boid" $boidExe $BoidArgs @{ bench = "boid"; frames = $BoidArgs[0]; warmup = $BoidArgs[1] }))
    $Runs.Add((Invoke-Bench (++$seq) $r "small" "EcsSurvivorBenchmark.exe" $smallArgs @{ bench = "survivor"; preset = "small"; frames = $SmallArgs[0]; warmup = $SmallArgs[1] }))
    $Runs.Add((Invoke-Bench (++$seq) $r "large" "EcsSurvivorBenchmark.exe" $LargeArgs @{ bench = "survivor"; preset = "large"; frames = $LargeArgs[0]; warmup = $LargeArgs[1] }))
  }
  for ($i = 1; $i -le $SlowRuns; ++$i) {
    $Runs.Add((Invoke-Bench (++$seq) $i "boid-slow" "EcsBenchmark.exe" $BoidSlowArgs @{ bench = "boid"; frames = $BoidSlowArgs[0]; warmup = $BoidSlowArgs[1] }))
  }
  for ($i = 1; $i -le $WakeRuns; ++$i) {
    $Runs.Add((Invoke-Bench (++$seq) $i "wake" "JobWakeBenchmark.exe" $WakeArgs @{ bench = "wake"; iterations = $WakeArgs[0]; gap_us = $WakeArgs[1] }))
  }
}

Write-Host "background load after ($BackgroundSec s)..."
$BgAfter = Measure-Background $BackgroundSec
$EndTime = Get-Date

$Failed = @($Runs | Where-Object { $_.Status -ne "OK" })
$Ok = @($Runs | Where-Object { $_.Status -eq "OK" })
$AllOk = ($BuildStatus -eq "OK") -and ($Failed.Count -eq 0) -and ($Runs.Count -gt 0)

# ----------------------------------------------------------------- CSV
$csv = New-Object System.Collections.Generic.List[string]
$csv.Add("seq,round,bench,args,status,exit_code,wall_s,window_s,cores_by_times,cores_by_cycles,frame_median_us,frame_mean_us,frame_min_us,frame_p95_us,raw")
foreach ($run in $Runs) {
  $f = Get-Row $run "frame" "frame"
  $fm = ""; $fa = ""; $fn = ""; $fp = ""
  if ($null -ne $f) { $fm = $f["median"]; $fa = $f["mean"]; $fn = $f["min"]; $fp = $f["p95"] }
  $ws = ""; $ct = ""; $cc = ""
  if ($run.Cpu.Count -gt 0) { $ws = $run.Cpu["wall_s"]; $ct = $run.Cpu["cores_by_times"]; $cc = $run.Cpu["cores_by_cycles"] }
  $csv.Add(("{0},{1},{2},""{3}"",{4},{5},{6},{7},{8},{9},{10},{11},{12},{13},{14}" -f $run.Seq, $run.Round, $run.Bench, $run.Args,
            $run.Status, $run.ExitCode, (Fmt $run.WallSec 3), $ws, $ct, $cc, $fm, $fa, $fn, $fp, $run.Raw))
}
Write-Utf8Bom (Join-Path $OutDir "runs.csv") $csv

$rowsCsv = New-Object System.Collections.Generic.List[string]
$rowsCsv.Add("seq,round,bench,status,kind,name,median,mean,min,p95")
foreach ($run in $Runs) {
  foreach ($row in $run.Rows) {
    $rowsCsv.Add(("{0},{1},{2},{3},{4},{5},{6},{7},{8},{9}" -f $run.Seq, $run.Round, $run.Bench, $run.Status,
                  $row["kind"], $row["name"], $row["median"], $row["mean"], $row["min"], $row["p95"]))
  }
}
Write-Utf8Bom (Join-Path $OutDir "rows.csv") $rowsCsv

# ----------------------------------------------------------------- Markdown
$md = New-Object System.Collections.Generic.List[string]
$title = "性能の基準線 (ECS 2-5)"
if ($Inject -ne "") { $title = "歯の確認の試走 (inject=$Inject)" }
$md.Add("# $title — $($StartTime.ToString('yyyy-MM-dd HH:mm:ss'))")
$md.Add("")
$md.Add("> ``Tests/run_baseline.bat`` が生成した。**手で書き換えないこと。** 各回の生の出力は ``raw/``、各回の数字は ``runs.csv`` / ``rows.csv``。")
$md.Add("")
$md.Add("## 結果の状態")
$md.Add("")
if ($AllOk) {
  $md.Add("**OK** — ビルド成功、$($Runs.Count) 回すべて OK。")
} else {
  $md.Add("**FAILED** — ビルド: $BuildStatus。失敗した回: $($Failed.Count) / $($Runs.Count)。**失敗した回は集計に入れていない。**")
}
$md.Add("")
if ($BgBefore.Mean -gt 8.0 -or $BgAfter.Mean -gt 8.0) {
  $md.Add("**注意: 背景負荷が高い**(計測の前 $(Fmt $BgBefore.Mean 1)% / 後 $(Fmt $BgAfter.Mean 1)%)。上位: $($BgBefore.Top)。数字が動いている可能性がある。")
  $md.Add("")
}
if ($Failed.Count -gt 0) {
  $md.Add("### 失敗した回(先に出す)")
  $md.Add("")
  $md.Add("| # | 周 | ベンチ | 引数 | 状態 | 終了コード | メモ | 生の出力 |")
  $md.Add("|---|---|---|---|---|---|---|---|")
  foreach ($run in $Failed) {
    $md.Add(("| {0} | {1} | {2} | ``{3}`` | **{4}** | {5} | {6} | ``{7}`` |" -f $run.Seq, $run.Round, $run.Bench, $run.Args, $run.Status, $run.ExitCode, $run.Note, $run.Raw))
  }
  $md.Add("")
}

$md.Add("## 測定条件")
$md.Add("")
$md.Add("| 項目 | 値 |")
$md.Add("|---|---|")
$md.Add("| 先頭のコミット | ``$Commit`` |")
$md.Add("| Source/ を最後に変えたコミット | ``$SourceCommit`` |")
if ($SourceDirty.Count -eq 0) { $md.Add("| Source/ の未コミットの変更 | なし |") }
else { $md.Add("| Source/ の未コミットの変更 | **あり**: $(($SourceDirty | ForEach-Object { $_.Trim() }) -join ', ') |") }
if ($TrackedDirty.Count -eq 0) { $md.Add("| 追跡済みファイルの未コミットの変更 | なし |") }
else { $md.Add("| 追跡済みファイルの未コミットの変更 | $(($TrackedDirty | ForEach-Object { $_.Trim() }) -join ', ') |") }
$md.Add("| 日時 | $($StartTime.ToString('yyyy-MM-dd HH:mm:ss')) 〜 $($EndTime.ToString('HH:mm:ss')) |")
$md.Add("| 回数 | $Rounds 周(1 周 = Boid -> small -> large を 1 回ずつ)+ Boid の遅い区間 $SlowRuns 回 + 起こす費用 $WakeRuns 回。その前に各 exe を 1 回ずつ走らせて捨てる(prime-*) |")
foreach ($k in $Envr.Keys) { $md.Add("| $k | $($Envr[$k] -replace '\r?\n', ' ') |") }
$firstMeta = $null
foreach ($run in $Runs) { if ($run.Meta.ContainsKey("p_cores")) { $firstMeta = $run.Meta; break } }
if ($null -ne $firstMeta) {
  $md.Add("| コアの内訳(EfficiencyClass) | 性能コア $($firstMeta['p_cores'])(論理 $($firstMeta['p_logical']))+ 効率コア $($firstMeta['e_cores'])(論理 $($firstMeta['e_logical']))= 物理 $($firstMeta['physical_cores']) / 論理 $($firstMeta['logical_processors']) |")
  $md.Add("| ``hardware_concurrency()`` | $($firstMeta['hardware_concurrency']) |")
  $md.Add("| ``JobSystem`` のワーカー | $($firstMeta['workers']) 本(メインを足すと $([int]$firstMeta['workers'] + 1) 本) |")
  $md.Add("| コンパイラ(``_MSC_FULL_VER``) | $($firstMeta['msc_full_ver']) |")
}
$md.Add("| ビルド | x64 Release、ゲームと同じフラグ ``/std:c++20 /permissive- /W3 /sdl /EHsc /GR- /O2 /MD /DNDEBUG``(``Tests/build_benchmarks.bat``) |")
$md.Add("| ビルドの警告 | $(if ($Warnings.Count -eq 0) { 'なし' } else { ($Warnings | Select-Object -Unique) -join '<br>' }) |")
$md.Add("| 背景負荷(計測の前 $($BgBefore.Seconds) 秒) | CPU 全体で平均 $(Fmt $BgBefore.Mean 1)%、最大 $(Fmt $BgBefore.Max 1)%。上位: $($BgBefore.Top) |")
$md.Add("| 背景負荷(計測の後 $($BgAfter.Seconds) 秒) | CPU 全体で平均 $(Fmt $BgAfter.Mean 1)%、最大 $(Fmt $BgAfter.Max 1)%。上位: $($BgAfter.Top) |")
$md.Add("| 時間切れ | 1 回 $TimeoutSec 秒 |")
$md.Add("")
$md.Add("| ベンチ | 引数 | 計測する区間 |")
$md.Add("|---|---|---|")
$md.Add("| boid | ``$($BoidArgs -join ' ')`` | $($BoidArgs[1])〜$([int]$BoidArgs[0] - 1) フレーム目($([int]$BoidArgs[0] - [int]$BoidArgs[1]) フレーム)。**定常状態は無い** |")
$md.Add("| small | ``$($SmallArgs -join ' ')`` | $($SmallArgs[1]) フレーム捨てて $($SmallArgs[0]) フレーム |")
$md.Add("| large | ``$($LargeArgs -join ' ')`` | $($LargeArgs[1]) フレーム捨てて $($LargeArgs[0]) フレーム |")
$md.Add("| boid-slow | ``$($BoidSlowArgs -join ' ')`` | $($BoidSlowArgs[1])〜$([int]$BoidSlowArgs[0] - 1) フレーム目。**長く走らせたときの参照。比較には使わない** |")
$md.Add("| wake | ``$($WakeArgs -join ' ')`` | 2000 回、各回の前に 200 µs 空回りしてワーカーを眠らせる |")
$md.Add("")
$md.Add("## 読むときの注意")
$md.Add("")
$md.Add("- **基準線は A/B 比較の代わりにならない。** 変更の効果は同じ機械で交互に測って比べる(開発手法 §5.2)。")
$md.Add("- **最小値は標本数で動く**(極値の統計量)。計測フレーム数の違う最小値どうしは比べない。")
$md.Add("- **Boid の数字は区間で決まる。** 群れが固まり、衝突イベントが増え続ける。区間の違う数字どうしは比べない。")
$md.Add("- p95 は並べ替えて ``values[count*95/100]``。")
$md.Add("- 平均使用コア数は**計測区間だけ**(準備と助走を含まない)。``by times`` は ``GetProcessTimes``(15.625 ms 単位で数える)、``by cycles`` は ``QueryProcessCycleTime`` / TSC(性能コアと効率コアで周波数が違い、ターボで変動するので近似)。")
$md.Add("- ワーカー 20 本 + メイン = 21 本が論理 20 を上回る。``WaitFor`` は一番遅いワーカーを待つので、効率コアに載った仕事がフレームの長さを決め得る【推測】。今回は変えていない。")
$md.Add("")

function Add-BenchSummary([string]$bench, [string]$title) {
  $runs = @($Ok | Where-Object { $_.Bench -eq $bench })
  $md.Add("## $title")
  $md.Add("")
  if ($runs.Count -eq 0) { $md.Add("OK の回が無い。"); $md.Add(""); return }
  $md.Add("OK の回: $($runs.Count)。値は「回ごとの値の中央値」と「回ごとの値の最小〜最大」。")
  $md.Add("")
  $md.Add("| 指標 | 回ごとの中央値 | 最小〜最大 |")
  $md.Add("|---|---|---|")
  foreach ($stat in @("median", "mean", "min", "p95")) {
    $vals = [double[]]@($runs | ForEach-Object { Num (Get-Row $_ "frame" "frame") $stat })
    $md.Add(("| フレームの {0} | {1} µs | {2}〜{3} µs |" -f $stat, (Fmt (Get-Median $vals)), (Fmt ($vals | Measure-Object -Minimum).Minimum), (Fmt ($vals | Measure-Object -Maximum).Maximum)))
  }
  foreach ($key in @("cores_by_times", "cores_by_cycles", "wall_s")) {
    $vals = [double[]]@($runs | ForEach-Object { Num $_.Cpu $key })
    $unit = ""; $digits = 2
    if ($key -eq "wall_s") { $unit = " s"; $digits = 3 }
    $md.Add(("| {0} | {1}{4} | {2}〜{3}{4} |" -f $key, (Fmt (Get-Median $vals) $digits), (Fmt ($vals | Measure-Object -Minimum).Minimum $digits), (Fmt ($vals | Measure-Object -Maximum).Maximum $digits), $unit))
  }
  # **区間を長くする代わりに、同じ区間を回の数だけ足し合わせる。** Boid は区間を延ばすと
  # 仕事量が変わるので、1 回の区間は延ばせない。GetProcessTimes の丸めは足し合わせで相対的に小さくなる
  $sumWall = ($runs | ForEach-Object { Num $_.Cpu "wall_s" } | Measure-Object -Sum).Sum
  $sumCpu = ($runs | ForEach-Object { Num $_.Cpu "cpu_s" } | Measure-Object -Sum).Sum
  $sumCyc = ($runs | ForEach-Object { (Num $_.Cpu "cores_by_cycles") * (Num $_.Cpu "wall_s") } | Measure-Object -Sum).Sum
  $md.Add(("| **足し合わせたコア数**(Σ CPU 時間 / Σ 区間、{0} 回で {1} s) | by times **{2}** / by cycles {3} | — |" -f $runs.Count, (Fmt $sumWall 2), (Fmt ($sumCpu / $sumWall) 2), (Fmt ($sumCyc / $sumWall) 2)))
  $md.Add("")
  $md.Add("段ごと(各回の中央値 / p95 の、回ごとの中央値と範囲):")
  $md.Add("")
  $md.Add("| 段 | 中央値 | 中央値の範囲 | p95 | p95 の範囲 |")
  $md.Add("|---|---|---|---|---|")
  foreach ($row in $runs[0].Rows) {
    if ($row["kind"] -ne "stage") { continue }
    $name = $row["name"]
    $meds = [double[]]@($runs | ForEach-Object { Num (Get-Row $_ "stage" $name) "median" })
    $p95s = [double[]]@($runs | ForEach-Object { Num (Get-Row $_ "stage" $name) "p95" })
    $md.Add(("| {0} | {1} µs | {2}〜{3} | {4} µs | {5}〜{6} |" -f $name, (Fmt (Get-Median $meds)),
             (Fmt ($meds | Measure-Object -Minimum).Minimum), (Fmt ($meds | Measure-Object -Maximum).Maximum),
             (Fmt (Get-Median $p95s)), (Fmt ($p95s | Measure-Object -Minimum).Minimum), (Fmt ($p95s | Measure-Object -Maximum).Maximum)))
  }
  $md.Add("")
  $md.Add("件数(1 フレームあたり、各回の中央値の、回ごとの中央値と範囲):")
  $md.Add("")
  $md.Add("| 件数 | 中央値 | 範囲 |")
  $md.Add("|---|---|---|")
  foreach ($row in $runs[0].Rows) {
    if ($row["kind"] -ne "count") { continue }
    $name = $row["name"]
    $meds = [double[]]@($runs | ForEach-Object { Num (Get-Row $_ "count" $name) "median" })
    $md.Add(("| {0} | {1} | {2}〜{3} |" -f $name, (Fmt (Get-Median $meds) 0), (Fmt ($meds | Measure-Object -Minimum).Minimum 0), (Fmt ($meds | Measure-Object -Maximum).Maximum 0)))
  }
  $md.Add("")
  $md.Add("各回:")
  $md.Add("")
  $md.Add("| # | 周 | 中央値 | 平均 | 最小 | p95 | コア (times) | コア (cycles) | 区間 |")
  $md.Add("|---|---|---|---|---|---|---|---|---|")
  foreach ($run in $runs) {
    $f = Get-Row $run "frame" "frame"
    $md.Add(("| {0} | {1} | {2} | {3} | {4} | {5} | {6} | {7} | {8} s |" -f $run.Seq, $run.Round, $f["median"], $f["mean"], $f["min"], $f["p95"],
             $run.Cpu["cores_by_times"], $run.Cpu["cores_by_cycles"], $run.Cpu["wall_s"]))
  }
  $md.Add("")
}

Add-BenchSummary "boid" "Boid(300 / 30)"
Add-BenchSummary "small" "Survivor small(3000 / 900)"
Add-BenchSummary "large" "Survivor large(3000 / 900)"
Add-BenchSummary "boid-slow" "Boid の遅い区間(3300 / 3000、参照のみ)"

$wake = @($Ok | Where-Object { $_.Bench -eq "wake" })
$md.Add("## 起こす費用(空のジョブ)")
$md.Add("")
if ($wake.Count -eq 0) { $md.Add("OK の回が無い。") }
else {
  $md.Add("OK の回: $($wake.Count)。値は各回の値の、回ごとの中央値と範囲。**原因の見立て(``KickJob`` ごとの ``notify_one``)は【推測・未確認】。**")
  $md.Add("")
  $md.Add("| 計測 | 中央値 | 中央値の範囲 | p95 | p95 の範囲 |")
  $md.Add("|---|---|---|---|---|")
  foreach ($row in $wake[0].Rows) {
    $name = $row["name"]
    $meds = [double[]]@($wake | ForEach-Object { Num (Get-Row $_ "wake" $name) "median" })
    $p95s = [double[]]@($wake | ForEach-Object { Num (Get-Row $_ "wake" $name) "p95" })
    $md.Add(("| {0} | {1} µs | {2}〜{3} | {4} µs | {5}〜{6} |" -f $name, (Fmt (Get-Median $meds) 2),
             (Fmt ($meds | Measure-Object -Minimum).Minimum 2), (Fmt ($meds | Measure-Object -Maximum).Maximum 2),
             (Fmt (Get-Median $p95s) 2), (Fmt ($p95s | Measure-Object -Minimum).Minimum 2), (Fmt ($p95s | Measure-Object -Maximum).Maximum 2)))
  }
  $md.Add("")
  $md.Add("| 割合 | 各回 |")
  $md.Add("|---|---|")
  foreach ($ratio in $wake[0].Ratios) {
    $name = $ratio["name"]
    $each = @($wake | ForEach-Object {
      $x = $null
      foreach ($q in $_.Ratios) { if ($q["name"] -eq $name) { $x = $q } }
      "{0}/{1}" -f $x["part"], $x["whole"]
    }) -join ", "
    $md.Add("| $name | $each |")
  }
}
$md.Add("")

Write-Utf8Bom (Join-Path $OutDir "baseline.md") $md

Write-Host ""
Write-Host "record: $(Join-Path $OutDir 'baseline.md')"
if ($AllOk) { Write-Host "RESULT: OK"; exit 0 }
Write-Host "RESULT: FAILED (build: $BuildStatus, failed runs: $($Failed.Count) / $($Runs.Count))"
exit 1
