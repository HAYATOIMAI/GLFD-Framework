# GLFD (since ECS 2-14): shoot an exe built with screen_marker_probe.py. Input gate: 60 s idle (exit 3).
# Warms the exe up once first (the first run of a fresh exe is slow).
# 3 shots in Boid, '2', 3 shots in Survivor, then WM_CLOSE (what the
# title bar's x sends). Shots: <OutDir>\boids_N.png / survivor_N.png. Exit 0 shot / 2 key did not land / 3 not idle / 4 game
param([Parameter(Mandatory)][string]$GameRoot, [Parameter(Mandatory)][string]$OutDir)
$ErrorActionPreference = "Stop"
$exe = Join-Path $GameRoot "x64\Release\GameLib_conteinar.exe"
$log = Join-Path $GameRoot "Game.log"
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
if (-not ("ShotApi" -as [type])) {
  Add-Type -AssemblyName System.Drawing
  Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices;
public static class ShotApi {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [StructLayout(LayoutKind.Sequential)] public struct LASTINPUTINFO { public uint cbSize; public uint dwTime; }
  [DllImport("user32.dll")] public static extern bool GetLastInputInfo(ref LASTINPUTINFO p);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindowAsync(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
  [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool f);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr pid);
  [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, IntPtr extra);
  public static uint IdleMs() { var i = new LASTINPUTINFO(); i.cbSize = 8; GetLastInputInfo(ref i); return (uint)Environment.TickCount - i.dwTime; }
}
'@
}
[void][ShotApi]::SetProcessDPIAware()
if ([ShotApi]::IdleMs() -lt 60000) { Write-Host "NOT IDLE"; exit 3 }
function Front($h) {
  $mine = [ShotApi]::GetCurrentThreadId()
  for ($t = 0; $t -lt 20; ++$t) {
    $f = [ShotApi]::GetForegroundWindow(); if ($f -eq $h) { return $true }
    $ft = [ShotApi]::GetWindowThreadProcessId($f, [IntPtr]::Zero)
    $att = ($ft -ne 0) -and ($ft -ne $mine) -and [ShotApi]::AttachThreadInput($mine, $ft, $true)
    [void][ShotApi]::ShowWindowAsync($h, 9); [void][ShotApi]::BringWindowToTop($h); [void][ShotApi]::SetForegroundWindow($h)
    if ($att) { [void][ShotApi]::AttachThreadInput($mine, $ft, $false) }
    Start-Sleep -Milliseconds 200
  }
  return ([ShotApi]::GetForegroundWindow() -eq $h)
}
function Shot($h, $name) {
  [void](Front $h)
  $cr = New-Object ShotApi+RECT; [void][ShotApi]::GetClientRect($h, [ref]$cr)
  $org = New-Object ShotApi+POINT; [void][ShotApi]::ClientToScreen($h, [ref]$org)
  $bmp = New-Object System.Drawing.Bitmap $cr.Right, $cr.Bottom; $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($org.X, $org.Y, 0, 0, (New-Object System.Drawing.Size $cr.Right, $cr.Bottom)); $g.Dispose()
  $bmp.Save((Join-Path $OutDir "$name.png"), [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
}
function StartGame {
  if (Test-Path $log) { Remove-Item $log -Force }
  $p = Start-Process -FilePath $exe -WorkingDirectory $GameRoot -PassThru
  $h = [IntPtr]::Zero
  for ($i = 0; $i -lt 60 -and $h -eq [IntPtr]::Zero; ++$i) { Start-Sleep -Milliseconds 500; $p.Refresh(); if ($p.HasExited) { break }; $h = $p.MainWindowHandle }
  return @($p, $h)
}
$w = StartGame; Start-Sleep -Seconds 3; [void]$w[0].CloseMainWindow(); [void]$w[0].WaitForExit(15000)   # warm-up
$g = StartGame; $proc = $g[0]; $hwnd = $g[1]
if ($hwnd -eq [IntPtr]::Zero) { Write-Host "no window"; exit 4 }
$exitWith = 0
try {
  Start-Sleep -Seconds 3
  for ($n = 1; $n -le 3; ++$n) { Shot $hwnd "boids_$n"; Start-Sleep -Milliseconds 700 }
  $landed = $false
  for ($a = 0; $a -lt 3 -and -not $landed; ++$a) {
    if (-not (Front $hwnd)) { continue }
    [ShotApi]::keybd_event([byte][char]'2', 0, 0, [IntPtr]::Zero); Start-Sleep -Milliseconds 60
    [ShotApi]::keybd_event([byte][char]'2', 0, 2, [IntPtr]::Zero)
    for ($q = 0; $q -lt 40 -and -not $landed; ++$q) { Start-Sleep -Milliseconds 50; $landed = Select-String -Path $log -Pattern "pressed. changing to" -SimpleMatch -Quiet }
  }
  if (-not $landed) { Write-Host "the '2' key did not land"; $exitWith = 2 }
  else {
    Start-Sleep -Seconds 2
    for ($n = 1; $n -le 3; ++$n) { Shot $hwnd "survivor_$n"; Start-Sleep -Milliseconds 700 }
  }
}
finally {
  [void]$proc.CloseMainWindow()
  $self = if ($proc.WaitForExit(15000)) { "exited by itself, code $($proc.ExitCode)" } else { $proc | Stop-Process -Force; "HUNG (killed)" }
  Write-Host "game: $self"
  Copy-Item $log (Join-Path $OutDir "Game.log") -ErrorAction SilentlyContinue
}
exit $exitWith
