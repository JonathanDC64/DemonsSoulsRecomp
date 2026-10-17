#!/usr/bin/env bash
# kabsum.sh <out>: mean average fps of the A and B windows in an exeab.sh or
# knobab.sh result file (lines "A <profile summary>", "B ...").
awk '/^[AB] /{for(i=1;i<=NF;i++) if($i=="fps"){s[$1]+=$(i-1); n[$1]++; break}} END{for(k in s) printf "%s %.2f fps over %d windows\n", k, s[k]/n[k], n[k]}' "$1" | sort
