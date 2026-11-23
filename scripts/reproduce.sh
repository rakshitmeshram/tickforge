#!/usr/bin/env bash
# Rebuild everything, run all tests, and regenerate every number and figure in
# the README from scratch. Deterministic: same seeds -> same research results
# (benchmark timings naturally vary by machine).
set -euo pipefail
cd "$(dirname "$0")/.."

echo "== build (Release) =="
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build -j

echo "== C++ tests =="
(cd build && ctest --output-on-failure)

echo "== Python tests =="
(cd python && python3 -m pytest -q)

mkdir -p data results
echo "== benchmark: typical book =="
./build/tf_bench --n 2000000 --seed 1 --runs 5 --json results/bench.json --samples results/latency_samples.csv
echo "== benchmark: deep book =="
./build/tf_bench --n 2000000 --seed 1 --runs 3 --live 200000 --decay_pct 2 --json results/bench_deep.json
python3 research/plot_latency.py results/latency_samples.csv results/bench.json results

echo "== order book imbalance study =="
./build/tf_gen --n 2000000 --seed 7 --out data/flow.bin
./build/tf_replay --in data/flow.bin --book data/book.csv --trades data/trades.csv
python3 research/obi_signal.py data/book.csv results | tee results/obi_stdout.txt

echo "== market-making backtest =="
python3 research/market_maker.py 200000 10 results | tee results/mm_stdout.txt
