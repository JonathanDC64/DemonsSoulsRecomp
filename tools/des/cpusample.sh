#!/usr/bin/env bash
# cpusample.sh <runlog> [sec]: des_boot's total CPU (in cores) and its busiest
# host threads over <sec> seconds, mapped to guest threads via the run log.
LOG="$1"; S="${2:-4}"
powershell -NoProfile -Command "
\$p=Get-Process des_boot -ErrorAction Stop; \$a=@{}; \$p.Threads|%{\$a[\$_.Id]=\$_.TotalProcessorTime.TotalMilliseconds}
\$c=(Get-Counter '\Process(des_boot)\% Processor Time' -SampleInterval $S -MaxSamples 1).CounterSamples[0].CookedValue
\$p=Get-Process des_boot; 'cores busy: ' + [math]::Round(\$c/100,2) + '  threads: ' + \$p.Threads.Count + '  priority: ' + \$p.PriorityClass
\$p.Threads|%{[pscustomobject]@{id=\$_.Id;ms=[int](\$_.TotalProcessorTime.TotalMilliseconds-\$a[\$_.Id])}}|sort ms -desc|select -first 6|%{ \"\$(\$_.id) \$(\$_.ms)\" }
" | tr -d '\r' | while read -r id ms; do
  case "$id" in cores*) echo "$id $ms"; continue;; esac
  g=$(grep -a "hosttid=$id\b" "$LOG" | grep -o "THREAD [0-9]*" | head -1)
  echo "  host $id: ${ms} ms/${S}s ${g:-(runtime/SPU thread)}"
done
