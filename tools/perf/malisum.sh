#!/usr/bin/env bash
# usage: tools/perf/malisum.sh <dir with .spv from GTA4_XENIA_SPIRV_DUMP>
# Runs the Mali Offline Compiler (Mali-G615, one process per core) on every game pixel shader of the dump, writes
# <dir>/mali/<hash>.txt and <dir>/mali_rows.txt (hash, then A CVT LS T for total, shortest and longest path, work
# registers) and prints the sums. malioc comes from Arm Performance Studio (free): the Windows .msi from
# artifacts.tools.arm.com/arm-performance-studio/<version>/, unpacked with 7-Zip into build/malioc (README "Fase 4").
# Static estimates only: on the phone uniform-only code costs much less than malioc says (README).
P=$(cd "$(dirname "$0")/../.." && pwd)
M=${MALIOC:-$P/build/malioc/malioc.exe}
T=$(ls -d /c/Users/*/AppData/Local/Android/Sdk/ndk/*/shader-tools/*/ | tail -1)
D=$1; mkdir -p $D/mali $D/txt
one() { h=$(basename $1 .spv)
  grep -aq "gta4port-guest" $1 || return
  [ -f $D/txt/$h.txt ] || $T/spirv-dis.exe $1 -o $D/txt/$h.txt
  grep -q "OpEntryPoint Fragment" $D/txt/$h.txt || return
  [ -f $D/mali/$h.txt ] || $M -d --core Mali-G615 --fragment $1 > $D/mali/$h.txt 2>&1
}
export -f one; export M T D
ls $D/*.spv | xargs -P $(nproc) -I{} bash -c 'one {}'
cd $D/mali
for m in *.txt; do awk -v n=${m%.txt} '/^Total instruction/{t=$4" "$6" "$8" "$10} /^Shortest path/{a=$4" "$6" "$8" "$10} /^Longest path/{b=$4" "$6" "$8" "$10} /Work registers:/{w=$3} END{print n, t, a, b, w}' $m; done > ../mali_rows.txt
awk '{n++; if ($6=="N/A" || $10=="N/A") {loops++; for(i=2;i<=5;i++) L[i]+=$i; next} for(i=2;i<=13;i++) x[i]+=$i; if ($14>32) half++} END {
 printf "%d pixel shaders (%d with loops, %d of the rest at half occupancy). Loop-free: total A %.0f CVT %.0f LS %.0f T %.0f | shortest A %.0f CVT %.0f LS %.0f T %.0f | longest A %.0f CVT %.0f LS %.0f T %.0f\n", n, loops, half, x[2],x[3],x[4],x[5],x[6],x[7],x[8],x[9],x[10],x[11],x[12],x[13];
 printf "With loops (total): A %.0f CVT %.0f LS %.0f T %.0f\n", L[2],L[3],L[4],L[5]}' ../mali_rows.txt
