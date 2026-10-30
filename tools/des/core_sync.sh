#!/usr/bin/env bash
# core_sync.sh <ps3recomp checkout> [<copy_manifest.txt> [<copy_synced.txt>]]:
# what of today's renderer has changed since the portable core copied it
# (docs/120). The core (libs/video/rsxcore/, libs/video/gpu/) holds copies of
# line ranges of libs/video/rsx_d3d12_backend.c, which stays as it is; a fix
# to the original after the copy has to be ported to the copy, or marked as
# not applying.
#
# copy_manifest.txt (default <checkout>/libs/video/rsxcore/copy_manifest.txt):
#   base=<commit>                      the commit the ranges are counted at
#   <new file> <start>-<end> <names>   one copied range: lines of the original
#                                      at base, and what they hold (free text)
# A file can have several range lines; '#' starts a comment.
# copy_synced.txt (default beside the manifest; may be missing):
#   <commit> ported|n/a <note>         a commit dealt with: its entry is left out
#
# It lists the commits since base that change the original (git rev-list
# --first-parent base..CORE_SYNC_REF, default HEAD: one entry per merged pull
# request or direct commit; CORE_SYNC_ALL=1 lists every non-merge commit
# instead), takes each one's changed lines, counts them back to base (through
# the changes between base and that commit's parent), and prints the copies
# each one touches, with the base lines:
#   1a2b3c4d 2026-11-02 d3d12: the fog wall's alpha
#       rsxc_post.c  base 9600-9612  (in 9511-9840: post chain)
#       not copied   base 2390-2391
# "not copied" lines are outside every range (D3D12-only code, or code the
# copy left out): look at them too, a gpu_d3d12* file may hold them.
# CORE_SYNC_FILE changes the original's path (default
# libs/video/rsx_d3d12_backend.c).
# Exit status: 0 nothing left to port, 1 commits left to port, 2 bad
# arguments or the manifest cannot be read.
set -uo pipefail
P=${1:-}
case "$P" in
  -h|--help) sed -n '2,/^set -uo/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
  ''|-*) echo "usage: core_sync.sh <ps3recomp checkout> [<copy_manifest.txt> [<copy_synced.txt>]]  (--help)"; exit 2 ;;
esac
[ -d "$P" ] && git -C "$P" rev-parse --git-dir > /dev/null 2>&1 || { echo "[sync] $P is not a git checkout"; exit 2; }
MAN=${2:-$P/libs/video/rsxcore/copy_manifest.txt}
SYN=${3:-$(dirname "$MAN")/copy_synced.txt}
FILE=${CORE_SYNC_FILE:-libs/video/rsx_d3d12_backend.c}
REF=${CORE_SYNC_REF:-HEAD}
[ -f "$MAN" ] || { echo "[sync] no manifest $MAN"; exit 2; }
g() { git -C "$P" "$@"; }

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
# The manifest as "<start>\t<end>\t<new file>\t<names>".
tr -d '\r' < "$MAN" | sed 's/#.*//' | awk -v out="$TMP/ranges.tsv" '
  NF == 0 { next }
  /^base=/ { sub(/^base=/, ""); base = $1; next }
  { if (NF < 2 || $2 !~ /^[0-9]+-[0-9]+$/) { printf "[sync] line %d: want <new file> <start>-<end> <names>\n", NR; bad = 1; next }
    split($2, r, "-"); if (r[1] + 0 > r[2] + 0) { printf "[sync] line %d: range %s runs backwards\n", NR, $2; bad = 1; next }
    names = ""; for (i = 3; i <= NF; i++) names = names (i > 3 ? " " : "") $i
    print r[1] "\t" r[2] "\t" $1 "\t" names > out }
  END { if (base == "") { print "[sync] no base=<commit> line"; bad = 1 }
        else print base > "/dev/stderr"
        exit bad }' 2> "$TMP/base" || exit 2
BASE_IN=$(cat "$TMP/base")
BASE=$(g rev-parse --verify --quiet "$BASE_IN^{commit}") || { echo "[sync] base $BASE_IN is not a commit in $P"; exit 2; }
[ -s "$TMP/ranges.tsv" ] || { echo "[sync] the manifest has no ranges"; exit 2; }
TIP=$(g rev-parse --verify --quiet "$REF^{commit}") || { echo "[sync] $REF is not a commit in $P"; exit 2; }
g merge-base --is-ancestor "$BASE" "$TIP" || echo "[sync] note: base ${BASE:0:8} is not an ancestor of $REF; counting from where they meet"

# Commits dealt with (full hashes); an entry that names no commit is reported.
: > "$TMP/done"
if [ -f "$SYN" ]; then
  while read -r c st _; do
    c="${c%$'\r'}"; st="${st%$'\r'}"
    case "$c" in ''|'#'*) continue ;; esac
    case "$st" in ported|n/a) ;; *) echo "[sync] $SYN: $c: the status is ported or n/a, not '$st'"; exit 2 ;; esac
    full=$(g rev-parse --verify --quiet "$c^{commit}") || { echo "[sync] $SYN: $c is not a commit here (left out)"; continue; }
    echo "$full" >> "$TMP/done"
  done < "$SYN"
fi

if [ "${CORE_SYNC_ALL:-0}" = 1 ]; then LIST=(--no-merges); else LIST=(--first-parent); fi
mapfile -t COMMITS < <(g rev-list --reverse "${LIST[@]}" "$BASE..$TIP" -- "$FILE")

# "@@ -a,b +c,d @@" headers of a diff of FILE as "a b c d".
hunks() {
  g diff -U0 --no-color --no-ext-diff "$1" "$2" -- "$FILE" | awk '
    /^@@ / { o = $2; n = $3; sub(/^-/, "", o); sub(/^\+/, "", n)
             split(o, a, ","); split(n, b, ",")
             print a[1], (a[2] == "" ? 1 : a[2]), b[1], (b[2] == "" ? 1 : b[2]) }'
}

todo=0; done_n=0
for c in "${COMMITS[@]}"; do
  if grep -qx "$c" "$TMP/done"; then done_n=$((done_n + 1)); continue; fi
  par=$(g rev-parse --verify --quiet "$c^1") || continue
  hunks "$BASE" "$par" > "$TMP/h1"   # base -> parent
  hunks "$par" "$c" > "$TMP/h2"      # parent -> the commit
  [ -s "$TMP/h2" ] || continue
  todo=$((todo + 1))
  g log -1 --format='%h %ad %s' --date=short "$c"
  awk -v h1="$TMP/h1" -v rg="$TMP/ranges.tsv" '
    BEGIN {
      while ((getline l < h1) > 0) { split(l, f, " "); n1++; bo[n1] = f[1]; bn[n1] = f[2]; po[n1] = f[3]; pn[n1] = f[4] }
      FS = "\t"
      while ((getline l < rg) > 0) { split(l, f, "\t"); nr++; rs[nr] = f[1]; re[nr] = f[2]; rf[nr] = f[3]; rn[nr] = f[4] }
      FS = " "
    }
    # A line of the parent, counted back to base: blo and bhi, the base lines
    # it stands for. A line that base -> parent added stands for the base
    # lines it replaced, or for the two it went in between.
    function back(L,   i, d, s, e) {
      d = 0
      for (i = 1; i <= n1; i++) {
        s = po[i]; e = po[i] + pn[i] - 1
        if (pn[i] > 0 && L >= s && L <= e) { blo = bo[i]; bhi = bo[i] + (bn[i] ? bn[i] - 1 : 1); return }
        # A hunk wholly before L shifts it (for a deletion, s is the line
        # the deleted ones followed).
        if ((pn[i] > 0 && e < L) || (pn[i] == 0 && s < L)) d += pn[i] - bn[i]
      }
      blo = bhi = L - d
    }
    # Spans per copied range (key r) or outside them all (key 0), merged when
    # they are under 8 lines apart.
    function add(k, x, y) {
      if (!(k in ns)) order[++nk] = k
      if (ns[k] && x <= he[k, ns[k]] + 8) { if (y > he[k, ns[k]]) he[k, ns[k]] = y; return }
      ns[k]++; hs[k, ns[k]] = x; he[k, ns[k]] = y
    }
    { lo = $1; hi = $1 + $2 - 1; if ($2 == 0) { hi = $1 + 1; if (lo < 1) lo = 1 }
      back(lo); a = blo; back(hi); b = bhi; if (b < a) b = a
      hit = 0
      for (r = 1; r <= nr; r++) if (a <= re[r] + 0 && b >= rs[r] + 0) {
        add(r, a > rs[r] + 0 ? a : rs[r] + 0, b < re[r] + 0 ? b : re[r] + 0); hit = 1 }
      if (!hit) add(0, a, b) }
    END {
      for (i = 1; i <= nk; i++) {
        k = order[i]; s = ""
        for (j = 1; j <= ns[k]; j++) s = s (j > 1 ? ", " : "") (hs[k, j] == he[k, j] ? hs[k, j] : hs[k, j] "-" he[k, j])
        if (k) printf "    %-22s base %s  (in %s-%s%s)\n", rf[k], s, rs[k], re[k], rn[k] != "" ? ": " rn[k] : ""
        else   printf "    %-22s base %s\n", "not copied", s
      } }' "$TMP/h2"
done
echo "core_sync: base ${BASE:0:8}, ${#COMMITS[@]} commit(s) change $FILE up to $REF: $done_n dealt with, $todo to port"
[ $todo = 0 ]
