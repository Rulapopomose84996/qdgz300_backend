# NUMA Affinity Benchmark

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
```

## Run (Linux target host)

```bash
numactl --cpunodebind=1 --membind=1 ./build/benches/numa_affinity --iterations 200000 --threads 8
```

## Output fields

- `throughput_pps`: packet pool allocate/deallocate throughput.
- `p99_latency_ms`: sampled P99 allocation/deallocation latency.
- `cross_node_access_pct`: reserved output slot for external `numastat/perf` sampling pipeline.

For acceptance, run under fixed CPU/memory binding and archive the console output plus `numastat` snapshot.
