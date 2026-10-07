#!/bin/sh
# Runs an ABI test binary as two parties and reports party 1's result.
#   tests/run_abi_test.sh build/bin/abi_test_gmw [group]
# group limits the run to one test group (e.g. add, icmp, misc, reduce,
# oblivious, float, binops).
set -u
exe=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
group=${2:-}
port=${PORT:-15700}
dir=$(mktemp -d)
mkdir -p "$dir/data"   # emp-aby stores pre-OT data here
cd "$dir"
"$exe" 1 "$port" $group > p1.log 2>&1 &
p1=$!
sleep 0.3
"$exe" 2 "$port" $group > p2.log 2>&1
r2=$?
wait $p1
r1=$?
cat p1.log | grep -v "^simd circ\|Depth\|time used" 
[ $r2 -ne 0 ] && echo "party 2 exited with $r2" && tail -3 p2.log
rm -rf "$dir"
[ $r1 -eq 0 ] && [ $r2 -eq 0 ]
