# bench

Latency and throughput harness. Measures both matching strategies through the same
`MatchingStrategy` interface on one identical, pre-recorded action stream.

Build and run it in a Release configuration with engine assertions off, otherwise the
numbers are not worth quoting:

```sh
cmake -B build-bench -DCMAKE_BUILD_TYPE=Release -DMATCHBOX_ENABLE_ASSERTS=OFF
cmake --build build-bench
./build-bench/bench/matchbox_bench
```

Results and their interpretation live in the root README.
