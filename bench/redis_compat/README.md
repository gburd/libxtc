# Redis-Compatible Server Benchmark

Simple TCP benchmark client for measuring GET/SET throughput and latency.

## Building

```bash
make
```

## Usage

```bash
# Basic benchmark against xtc-redis
./bench --host localhost --port 6379 --clients 10 --requests 10000

# Compare with real Redis (must be running on port 6379)
./bench --port 6380 --compare

# Quiet mode (M17 format output only)
./bench --quiet
```

## Options

| Option | Description | Default |
|--------|-------------|---------|
| `--host HOST` | Server hostname | 127.0.0.1 |
| `--port PORT` | Server port | 6379 |
| `--clients N` | Parallel client count | 10 |
| `--requests N` | Total requests | 10000 |
| `--datasize N` | Value size in bytes | 64 |
| `--quiet` | M17 format output only | off |
| `--compare` | Also benchmark real Redis | off |

## Output Format

Results are printed in M17 conformance format:

```
bench.xtc_redis.ops_per_sec=12345.67
bench.xtc_redis.p50_us=45.23
bench.xtc_redis.p99_us=123.45
bench.xtc_redis.p999_us=456.78
bench.xtc_redis.completed=10000
bench.xtc_redis.errors=0
```

## Workload

The benchmark alternates between SET and GET operations:
- SET: stores a value of `--datasize` bytes
- GET: retrieves the previously stored value
- Keys are unique per client and iteration

## Notes

- This is a simple benchmark for basic throughput measurement
- For serious benchmarking, use `redis-benchmark` from Redis distribution
- Latency percentiles are computed from up to 1M samples
