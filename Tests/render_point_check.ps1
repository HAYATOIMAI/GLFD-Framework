# ---------------------------------------------------------------------------
#  GLFD: are ALL the points drawn? (ECS 2-3)
#
#  Until ECS 2-3 the point-list topology was never set, and only one vertex in
#  three reached the screen (placing 8 white points in a row read "1 0 0 1 0 0 1 0").
#  Unit tests cannot see this (DX11). This script is the check on the screen:
#
#   1. TEMPORARILY edits Source\Game\SurvivorRender.h: 8 white points at the START of
#      the vertex list and 8 at the END, at fixed NDC positions (so the world scale does
#      not matter), size written to Pos.z (ignored while the GS uses a fixed size)
#   2. -Mutant also removes the IASetPrimitiveTopology line from DX11Renderer.cpp
#   3. builds Release, starts the game, switches to Survivor with '2' (confirmed from
#      Game.log), captures 3 shots and reads the 16 points
#   4. ALWAYS restores both files, checks they are byte-identical to the originals,
#      and rebuilds Release so no probe exe is left behind
#
#  Needs the flat round points of ECS 2-3 stage (2) or later: a point is read as drawn
#  when all its channels are above 200. (Before stage (2) the texture darkened every
#  point to ~12% brightness, and this reading would say "not drawn" for all of them.)
#
#  Expectation: normal -> every point drawn in every shot. -Mutant -> NOT every point
#  drawn (the tooth: removing the line must be seen). Exit code 0 = expectation met.
#
#  usage:  powershell -File Tests\render_point_check.ps1 [-Mutant]
#  The window must be able to come to the front (a background window is not held by
#  vsync and the '2' key may not land); the script says so if it could not.
#
#  NOTE: names differ in spelling, not only in case (PowerShell names are
#        case-insensitive: ECS 2-4 $B/$b, ECS 2-7 $Configs/$configs).
# ---------------------------------------------------------------------------
param([switch]$Mutant, [int]$ShotCount = 3)
$ErrorActionPreference = "Stop"
$repoRoot   = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$renderHdr  = Join-Path $repoRoot "Source\Game\SurvivorRender.h"
$rendererCpp = Join-Path $repoRoot "Source\Graphics\DX11Renderer.cpp"
$exePath    = Join-Path $repoRoot "x64\Release\GameLib_conteinar.exe"
$gameLog    = Join-Path $repoRoot "Game.log"
$workDir    = Join-Path $env:TEMP ("glfd_point_check_" + (Get-Date -Format "yyyyMMdd_HHmmss"))
New-Item -ItemType Directory -Force -Path $workDir | Out-Null     # a NEW record every run

# --- the probe points (NDC). start row y = -0.84, end row y = -0.96, x = -0.70 + 0.20 i
#     **Keep them where nothing of the game goes.** The start row is drawn FIRST, i.e. under
#     everything: a bullet flying over a probe point hides it and reads as "not drawn" (seen:
#     the first version used y = -0.65 and a bullet covered one point). Bullets reach at most
#     24 world units from the origin (12 units/s x 2 s), y = -24/34 = -0.71 in NDC.
$probeCount = 8
$startY = -0.84; $endY = -0.96
function ProbeX([int]$i) { return -0.70 + 0.20 * $i }

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$msbuild = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe" | Select-Object -First 1
if (-not $msbuild) { throw "MSBuild was not found" }

function Build-Release([string]$tag) {
  $startedAt = Get-Date
  $buildLog = Join-Path $workDir "build_$tag.log"
  & $msbuild (Join-Path $repoRoot "GameLib_conteinar.sln") /nologo /m /v:minimal /t:Build /p:Configuration=Release /p:Platform=x64 *> $buildLog
  $fresh = (Test-Path $exePath) -and ((Get-Item $exePath).LastWriteTime -ge $startedAt)
  if ($LASTEXITCODE -ne 0 -or -not $fresh) { throw "build '$tag' failed or did not write the exe (see $buildLog)" }
}

# --- back up; refuse to run over a leftover backup (a previous run died before restoring)
$backupHdr = Join-Path $workDir "SurvivorRender.h.orig"
$backupCpp = Join-Path $workDir "DX11Renderer.cpp.orig"
Copy-Item $renderHdr $backupHdr
Copy-Item $rendererCpp $backupCpp
$hashHdr = (Get-FileHash $renderHdr).Hash
$hashCpp = (Get-FileHash $rendererCpp).Hash

$verdict = "NOT RUN"
$exitWith = 1
try {
  # --- 1. probe points (UTF-8 with BOM, CRLF: the header's own encoding)
  $utf8Bom = New-Object System.Text.UTF8Encoding($true)
  $hdrText = [IO.File]::ReadAllText($renderHdr, $utf8Bom)
  $capAnchor   = "pickups.BaseSize() + 1u;"
  $firstAnchor = "    for (auto [e, pos, hp] : enemies) {"
  $lastAnchor  = "    (void)vertices.TryResize(count);"
  foreach ($a in @($capAnchor, $firstAnchor, $lastAnchor)) {
    if (([regex]::Matches($hdrText, [regex]::Escape($a))).Count -ne 1) { throw "SurvivorRender.h: anchor not found exactly once: $a" }
  }
  $emitRow = {
    param([double]$y)
    "    for (int probeI = 0; probeI < $probeCount; ++probeI) {   // PROBE (render_point_check.ps1)`r`n" +
    "      vertices[count].Pos   = DirectX::XMFLOAT4(-0.70f + 0.20f * probeI, ${y}f, 0.03f, 1.0f);`r`n" +
    "      vertices[count].Color = DirectX::XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);`r`n" +
    "      ++count;`r`n" +
    "    }`r`n"
  }
  $hdrText = $hdrText.Replace($capAnchor, "pickups.BaseSize() + 1u + " + (2 * $probeCount) + "u;")
  $hdrText = $hdrText.Replace($firstAnchor, (& $emitRow $startY) + $firstAnchor)
  $hdrText = $hdrText.Replace($lastAnchor, (& $emitRow $endY) + $lastAnchor)
  [IO.File]::WriteAllText($renderHdr, $hdrText, $utf8Bom)

  # --- 2. the tooth: remove the topology line (cp932)
  if ($Mutant) {
    $cp932 = [Text.Encoding]::GetEncoding(932)
    $cppText = [IO.File]::ReadAllText($rendererCpp, $cp932)
    $topo = "    m_deviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);"
    if (([regex]::Matches($cppText, [regex]::Escape($topo))).Count -ne 1) { throw "DX11Renderer.cpp: topology line not found exactly once" }
    [IO.File]::WriteAllText($rendererCpp, $cppText.Replace($topo, "    // (removed by render_point_check.ps1 -Mutant)"), $cp932)
  }

  # --- 3. build, run, capture, read
  Build-Release "probe"
  Add-Type -AssemblyName System.Drawing
  if (-not ("PointCheckApi" -as [type])) {
    Add-Type -Namespace "" -Name PointCheckApi -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hWnd, out RECT r);
[DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hWnd, ref POINT p);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
[DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr hWnd);
[DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool f);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, IntPtr pid);
[DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
[DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, IntPtr extra);
[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
public struct RECT { public int Left, Top, Right, Bottom; }
public struct POINT { public int X, Y; }
'@
  }
  [void][PointCheckApi]::SetProcessDPIAware()
  if (Test-Path $gameLog) { Remove-Item $gameLog -Force }
  $proc = Start-Process -FilePath $exePath -WorkingDirectory $repoRoot -PassThru
  $hwnd = [IntPtr]::Zero
  for ($i = 0; $i -lt 60 -and $hwnd -eq [IntPtr]::Zero; ++$i) {
    Start-Sleep -Milliseconds 500; $proc.Refresh()
    if ($proc.HasExited) { throw "the game exited during startup (code $($proc.ExitCode))" }
    $hwnd = $proc.MainWindowHandle
  }
  if ($hwnd -eq [IntPtr]::Zero) { throw "no main window after 30 s" }
  $frontGame = {
    $target = [PointCheckApi]::GetWindowThreadProcessId($hwnd, [IntPtr]::Zero); $mine = [PointCheckApi]::GetCurrentThreadId()
    for ($try = 0; $try -lt 20; ++$try) {
      if ([PointCheckApi]::GetForegroundWindow() -eq $hwnd) { return $true }
      [void][PointCheckApi]::AttachThreadInput($mine, $target, $true); [void][PointCheckApi]::ShowWindow($hwnd, 9)
      [void][PointCheckApi]::BringWindowToTop($hwnd); [void][PointCheckApi]::SetForegroundWindow($hwnd)
      [void][PointCheckApi]::AttachThreadInput($mine, $target, $false); Start-Sleep -Milliseconds 200
    }
    return ([PointCheckApi]::GetForegroundWindow() -eq $hwnd)
  }
  $countChanges = { if (-not (Test-Path $gameLog)) { return 0 }; return @(Get-Content $gameLog | Select-String -Pattern "pressed. changing to").Count }
  Start-Sleep -Seconds 2
  $before = & $countChanges; $landed = $false
  for ($attempt = 1; $attempt -le 3 -and -not $landed; ++$attempt) {
    if (-not (& $frontGame)) { continue }
    [PointCheckApi]::keybd_event([byte][char]'2', 0, 0, [IntPtr]::Zero); Start-Sleep -Milliseconds 60
    [PointCheckApi]::keybd_event([byte][char]'2', 0, 2, [IntPtr]::Zero)
    for ($w = 0; $w -lt 40 -and -not $landed; ++$w) { Start-Sleep -Milliseconds 50; if ((& $countChanges) -gt $before) { $landed = $true } }
  }
  $rowsSeen = @()
  if ($landed) {
    for ($n = 1; $n -le $ShotCount; ++$n) {
      Start-Sleep -Milliseconds 700
      $fronted = & $frontGame
      $cr = New-Object PointCheckApi+RECT; [void][PointCheckApi]::GetClientRect($hwnd, [ref]$cr)
      $org = New-Object PointCheckApi+POINT; [void][PointCheckApi]::ClientToScreen($hwnd, [ref]$org)
      $cw = $cr.Right - $cr.Left; $ch = $cr.Bottom - $cr.Top
      $bmp = New-Object System.Drawing.Bitmap $cw, $ch
      $gfx = [System.Drawing.Graphics]::FromImage($bmp)
      $gfx.CopyFromScreen($org.X, $org.Y, 0, 0, (New-Object System.Drawing.Size $cw, $ch)); $gfx.Dispose()
      $bmp.Save((Join-Path $workDir ("shot_{0:D2}.png" -f $n)), [System.Drawing.Imaging.ImageFormat]::Png)
      $readRow = {
        param([double]$y)
        $bits = @()
        for ($i = 0; $i -lt $probeCount; ++$i) {
          $px = [int][math]::Round(((ProbeX $i) + 1.0) / 2.0 * $cw); $py = [int][math]::Round((1.0 - $y) / 2.0 * $ch)
          $c = $bmp.GetPixel($px, $py)
          $bits += $(if ([math]::Min([math]::Min($c.R, $c.G), $c.B) -gt 200) { "1" } else { "0" })
        }
        return ($bits -join " ")
      }
      $startRow = & $readRow $startY; $endRow = & $readRow $endY
      $bmp.Dispose()
      $rowsSeen += "shot $n (fronted=$fronted): start $startRow | end $endRow"
    }
  }
  [void]$proc.CloseMainWindow()
  $selfExit = if ($proc.WaitForExit(15000)) { "exited by itself, code $($proc.ExitCode)" } else { $proc | Stop-Process -Force; "HUNG (killed)" }

  $allOnes = ("1 " * $probeCount).Trim()
  if (-not $landed) { $verdict = "NO VERDICT: the '2' key did not land, no Survivor shot was taken"; $exitWith = 2 }
  else {
    $everyPointDrawn = @($rowsSeen | Where-Object { $_ -notmatch "start $allOnes \| end $allOnes$" }).Count -eq 0
    if ($Mutant) {
      $verdict = if (-not $everyPointDrawn) { "OK: with the topology line removed, points are missing (the check has teeth)" } else { "FAILED: the line was removed but every point was still drawn" }
      $exitWith = if (-not $everyPointDrawn) { 0 } else { 1 }
    } else {
      $verdict = if ($everyPointDrawn) { "OK: every point was drawn in every shot" } else { "FAILED: some points were not drawn" }
      $exitWith = if ($everyPointDrawn) { 0 } else { 1 }
    }
  }
  $rowsSeen | ForEach-Object { Write-Host "  $_" }
  Write-Host "  game: $selfExit"
}
finally {
  # --- 4. ALWAYS restore, check, rebuild
  Copy-Item $backupHdr $renderHdr -Force
  Copy-Item $backupCpp $rendererCpp -Force
  # Copy-Item keeps the OLD timestamp, so MSBuild would think nothing changed and keep the
  # probe exe. Touch both so the rebuild below really compiles the restored sources
  (Get-Item $renderHdr).LastWriteTime = Get-Date
  (Get-Item $rendererCpp).LastWriteTime = Get-Date
  $restored = ((Get-FileHash $renderHdr).Hash -eq $hashHdr) -and ((Get-FileHash $rendererCpp).Hash -eq $hashCpp)
  Write-Host "  restored byte-identical: $restored"
  if (-not $restored) { Write-Host "  RESTORE FAILED. originals are in $workDir"; $exitWith = 3 }
  else {
    try { Build-Release "restored"; Write-Host "  Release rebuilt from the restored sources" }
    catch { Write-Host "  REBUILD FAILED after restore: $_"; $exitWith = 3 }
  }
}
Write-Host ("render_point_check" + $(if ($Mutant) { " -Mutant" } else { "" }) + ": $verdict  (shots in $workDir)")
exit $exitWith
