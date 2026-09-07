#!/usr/bin/env bash
# worst.sh — run diag_poles, print the worst (max Re) eigenvalues below the
# artifact cutoff (Re < 1.0). Usage: worst.sh <label> [diag_poles args...]
label="$1"; shift
out=$("$@" 2>&1)
vals=$(echo "$out" | awk '
  /^[ ]+[a-z_][a-zA-Z0-9_\/~]*:/ {
    n=NF; re=$(n-5)
    if (re < 1.0 && re != "-inf" && re != "nan") print re
  }
' | sort -g -r | head -3 | paste -sd' ')
echo "$label  worst<1rad/s Re: $vals"
echo "$out" | grep -m1 "trim:" | sed 's/^/    /'
