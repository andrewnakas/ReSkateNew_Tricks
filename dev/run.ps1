# Installs this build over the game's ReSkate files (backing up what was there), boots a map, runs console commands
# (';' separated), captures the window at every `echo shot-<name>`, prints the matching log lines, closes the game and
# puts the previous ReSkate files back. Refuses when a game is already running.
#   run.ps1 -Commands "tricks status;tricks freestyle casper;wait 3;echo shot-casper"
param(
    [string]$Commands = 'tricks status',
    [string]$Level = 'levels/game/dingolevel_reskate_gravy_train/dingolevel_reskate_gravy_train',
    [string]$Shots = 'D:\ReSkateNew_Tricks\dev\shots',
    [string]$Pattern = 'Tricks|Freestyle|Casper|tricks |error:|\[ERROR\]|\[CRITICAL\]|Startup command',
    [int]$MaxSeconds = 420, [int]$Width = 1600, [int]$Height = 900,
    [switch]$KeepInstalled
)
$game = 'C:\Program Files (x86)\Steam\steamapps\common\Skate'
$build = 'D:\ReSkateNew_Tricks\build\vs2022-x64\Release'
$backup = 'D:\ReSkateNew_Tricks\dev\backup'
$files = 'ReSkate.dll', 'ReSkateLauncher.exe'
$log = Join-Path $game 'logs\ReSkate.log'
if (Get-Process Skate, ReSkateLauncher -ErrorAction SilentlyContinue) { 'REFUSED: a game is already running.'; exit 2 }
foreach ($f in $files) { if (-not (Test-Path (Join-Path $build $f))) { "Missing build output: $f"; exit 1 } }

# Back up the installed files once per run (never over an older backup of the same files).
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$saved = Join-Path $backup $stamp
New-Item -ItemType Directory -Force $saved | Out-Null
foreach ($f in $files) { Copy-Item (Join-Path $game $f) (Join-Path $saved $f) }
foreach ($f in $files) { Copy-Item (Join-Path $build $f) (Join-Path $game $f) -Force }
"Installed the Tricks build; the previous files are in dev\backup\$stamp."

New-Item -ItemType Directory -Force $Shots | Out-Null
$env:RESKATE_STARTUP_COMMANDS = "load $Level;wait 38;$Commands;wait 3;echo tricks-run-finished"
$env:RESKATE_CRASH_REPORTING = '0'
$started = Get-Date
Start-Process (Join-Path $game 'ReSkateLauncher.exe') -ArgumentList '--no-gui', '--no-update', '--windowed', "--width=$Width", "--height=$Height" -WorkingDirectory $game
$done = $false; $taken = @{}
while (((Get-Date) - $started).TotalSeconds -lt $MaxSeconds -and -not $done) {
    Start-Sleep -Milliseconds 600
    if ((Test-Path $log) -and (Get-Item $log).LastWriteTime -gt $started) {
        $text = Get-Content $log -Raw -ErrorAction SilentlyContinue
        if ($text) {
            foreach ($m in [regex]::Matches($text, '\] shot-(\w+)')) {
                $n = $m.Groups[1].Value
                if (-not $taken[$n]) { $taken[$n] = 1; & 'D:\n64work\reskate\trainer-dev\ui.ps1' shot (Join-Path $Shots "$n.png") $Width | Out-Null }
            }
            if ($text -match 'tricks-run-finished') { $done = $true }
        }
    }
    if (((Get-Date) - $started).TotalSeconds -gt 90 -and -not (Get-Process Skate -ErrorAction SilentlyContinue)) { break }
}
"finished=$done after $([int]((Get-Date) - $started).TotalSeconds) s; shots: $(($taken.Keys | Sort-Object) -join ' ')"
if (Test-Path $log) {
    Select-String -Path $log -Pattern $Pattern | ForEach-Object { $_.Line -replace '^\[[^\]]+\] \[[^\]]+\] ', '' } | Select-Object -Last 60
}
# Only close a game this script started and whose run finished.
if ($done) {
    Get-Process Skate -ErrorAction SilentlyContinue | Where-Object { $_.StartTime -ge $started } | Stop-Process -Force -Confirm:$false
    Start-Sleep -Seconds 3
    'Game closed.'
} else { 'Run did not finish; the game was left open (files stay installed until it is closed and restore.ps1 runs).'; exit 3 }
if (-not $KeepInstalled) {
    foreach ($f in $files) { Copy-Item (Join-Path $saved $f) (Join-Path $game $f) -Force }
    'Previous ReSkate files restored.'
}
