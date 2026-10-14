# Runs one headless session of several passes. Each pass: teleport to the flat strip, mount, then the pass's commands.
#   pass.ps1 -Tag e1 -Passes 'tricks airpose last off;KICKCASPER', 'KICKCASPER'
# KICKCASPER / KICKPRIMO expand to a kickflip with the stick swept to the casper / primo place and held.
# Prints the physics, node and catch lines of this run only (dev\last-run.log holds them all).
param([string]$Tag = 'p', [string[]]$Passes, [string]$Setup = 'tricks slide 5', [int]$MaxSeconds = 420,
    [string]$Level = 'levels/game/dingolevel_reskate_thps_training/dingolevel_reskate_thps_training',
    [string]$Mount = 'trainer tp -5 -6.3 -9.5;wait 4;tricks pad ly=-1:500 n:600 b:150 n:1200 y:200 n:2500;wait 7')
$mount = $Mount
$macros = @{
    KICKCASPER = 'tricks pad ls:500 ls+a:200 ls:300 ls+ry=-1:250 ls+rx=0.7+ry=0.7:100 ls+rx=0.2+ry=-1:2400 n:2000;wait 7'
    KICKCASPERLONG = 'tricks pad ls:500 ls+a:200 ls:300 ls+ry=-1:250 ls+rx=0.7+ry=0.7:100 ls+rx=0.2+ry=-1:6000 n:2000'
    KICKPRIMOLONG  = 'tricks pad ls:500 ls+a:200 ls:300 ls+ry=-1:250 ls+rx=0.7+ry=0.7:100 ls+rx=0.85+ry=-0.5:6000 n:2000'
    FASTCASPER = 'tricks pad a:200 n:500 a:200 n:500 a:200 n:500 a:200 n:600 ls:300 ls+ry=-1:250 ls+rx=0.7+ry=0.7:100 ls+rx=0.2+ry=-1:4000 n:2500;wait 11;trainer where'
    FASTPRIMO  = 'tricks pad a:200 n:500 a:200 n:500 a:200 n:500 a:200 n:600 ls:300 ls+ry=-1:250 ls+rx=0.7+ry=0.7:100 ls+rx=0.85+ry=-0.5:4000 n:2500;wait 11;trainer where'
    KICKPRIMO  = 'tricks pad ls:500 ls+a:200 ls:300 ls+ry=-1:250 ls+rx=0.7+ry=0.7:100 ls+rx=0.85+ry=-0.5:2400 n:2000;wait 7'
}
$all = @($Setup)
for ($i = 0; $i -lt $Passes.Count; $i++) {
    $p = $Passes[$i]
    foreach ($k in ($macros.Keys | Sort-Object Length -Descending)) { $p = $p.Replace($k, $macros[$k]) }
    $all += "echo pass-$Tag-$i-begin;$mount;$p"
}
$log = 'C:\Program Files (x86)\Steam\steamapps\common\Skate\logs\ReSkate.log'
$out = & "$PSScriptRoot\run.ps1" -Commands ($all -join ';') -Level $Level -KeepInstalled -MaxSeconds $MaxSeconds -Pattern 'ZZZNOMATCH'
$out | Select-Object -First 2
$out | Select-Object -Last 2
$lines = Get-Content $log
$start = ($lines | Select-String -SimpleMatch "pass-$Tag-0-begin" | Select-Object -Last 1).LineNumber
if (-not $start) { 'No pass marker in the log.'; exit 1 }
$mine = $lines[($start - 1)..($lines.Count - 1)] | Where-Object { $_ -match 'pass-|Tricks: (physics|nodes|caught|catch|slide rocking|pose in|ground|bounce|shuv|board|skater root|primo|let go|flicked|the )|TRICK|error:|Way-in|osition|speed' } |
    ForEach-Object { $_ -replace '^\[[^\]]+\] \[[^\]]+\] ', '' }
$mine | Set-Content "$PSScriptRoot\last-run.log"
$mine
