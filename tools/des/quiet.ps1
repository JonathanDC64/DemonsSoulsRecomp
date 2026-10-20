# quiet.ps1 [-Secs N] [-Ours name,name,...]: CPU use over N seconds (default 5), for perflib.sh's perf_quiet.
# Prints one line:
#   cpu <total>% [outside <share>%] ; top: name(ms) name(ms) ...
# <total> is the whole machine's busy share from GetSystemTimes (kernel + user - idle, all logical CPUs), so it does
# not depend on the performance counters' names, which Windows translates. With -Ours, <share> is the CPU of every
# process whose name is not in that list (the batch's own shells and tools, and whatever runs the batch), over all
# logical CPUs. top: the processes that used the most CPU time meanwhile. Read-only.
param([int]$Secs = 5, [string]$Ours = "")
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class QuietCpu {
    [DllImport("kernel32.dll")] public static extern bool GetSystemTimes(out long idle, out long kernel, out long user);
}
"@
$inv = [Globalization.CultureInfo]::InvariantCulture
$ours = @($Ours -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
function Snap {
    $h = @{}
    foreach ($p in Get-Process) {
        $t = $null
        try { $t = $p.TotalProcessorTime } catch {}
        if ($t -ne $null -and $p.Id -ne 0) { $h[$p.Id] = @($p.ProcessName, $t.TotalMilliseconds) }
    }
    $h
}
$a = Snap
$i0 = 0L; $k0 = 0L; $u0 = 0L
[void][QuietCpu]::GetSystemTimes([ref]$i0, [ref]$k0, [ref]$u0)
$sw = [Diagnostics.Stopwatch]::StartNew()
Start-Sleep -Seconds $Secs
$el = $sw.Elapsed.TotalMilliseconds
$i1 = 0L; $k1 = 0L; $u1 = 0L
[void][QuietCpu]::GetSystemTimes([ref]$i1, [ref]$k1, [ref]$u1)
$b = Snap
$all = ($k1 - $k0) + ($u1 - $u0)   # kernel time includes the idle time
$busy = 0.0
if ($all -gt 0) { $busy = 100.0 * ($all - ($i1 - $i0)) / $all }
$rows = foreach ($id in $b.Keys) {
    if ($a.ContainsKey($id)) {
        [pscustomobject]@{ n = $b[$id][0]; ms = [int]($b[$id][1] - $a[$id][1]); out = ($ours -notcontains $b[$id][0]) }
    }
}
$line = 'cpu ' + $busy.ToString('F1', $inv) + '%'
if ($ours.Count -gt 0) {
    $outMs = ($rows | Where-Object { $_.out } | Measure-Object ms -Sum).Sum
    if ($outMs -eq $null) { $outMs = 0 }
    $share = 100.0 * $outMs / ($el * [Environment]::ProcessorCount)
    $line += ' outside ' + $share.ToString('F1', $inv) + '%'
}
$top = $rows | Where-Object { $_.ms -gt 0 } | Sort-Object ms -Descending | Select-Object -First 5
$line + ' ; top: ' + (($top | ForEach-Object { $_.n + '(' + $_.ms + 'ms)' }) -join ' ')
