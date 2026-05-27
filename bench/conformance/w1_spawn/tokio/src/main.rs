/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * bench/conformance/w1_spawn/tokio/src/main.rs
 *   M17 W1 — spawn-N-await-all, Tokio runtime.
 *
 *   Spawns N tokio tasks; each task does trivial work (increments an
 *   atomic counter).  Waits for all tasks via join_all, then emits one
 *   M17 result line on stdout.
 *
 * Build:
 *   cd bench/conformance/w1_spawn/tokio && cargo build --release
 *   Binary: target/release/bench  (wrapped by ./bench shell script)
 *
 * Usage:
 *   ./bench                  # N=10000 (default)
 *   ./bench --N=50000
 *   ./bench --params=N=50000
 */

use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;
use std::time::Instant;

/// Parse N from argv.  Accepts --N=<int> or --params=N=<int>.
fn parse_n(args: &[String]) -> u64 {
    for a in args.iter().skip(1) {
        if let Some(v) = a.strip_prefix("--N=") {
            if let Ok(n) = v.parse::<u64>() {
                return n;
            }
        }
        if let Some(v) = a.strip_prefix("--params=N=") {
            if let Ok(n) = v.parse::<u64>() {
                return n;
            }
        }
        if let Some(rest) = a.strip_prefix("--params=") {
            // params may be KEY=VALUE:KEY=VALUE
            for kv in rest.split(':') {
                if let Some(v) = kv.strip_prefix("N=") {
                    if let Ok(n) = v.parse::<u64>() {
                        return n;
                    }
                }
            }
        }
    }
    10_000
}

/// Read peak RSS in KiB from /proc/self/status (Linux).
/// Returns 0 on any parse error or non-Linux platform.
fn peak_rss_kb() -> u64 {
    #[cfg(target_os = "linux")]
    {
        if let Ok(data) = std::fs::read_to_string("/proc/self/status") {
            for line in data.lines() {
                if let Some(rest) = line.strip_prefix("VmPeak:") {
                    let kb: u64 = rest
                        .split_whitespace()
                        .next()
                        .and_then(|s| s.parse().ok())
                        .unwrap_or(0);
                    return kb;
                }
            }
        }
        0
    }
    #[cfg(not(target_os = "linux"))]
    {
        0
    }
}

/// CPU time (user + sys) in microseconds from /proc/self/stat.
/// Returns 0 on any error.
fn cpu_us() -> u64 {
    #[cfg(target_os = "linux")]
    {
        if let Ok(data) = std::fs::read_to_string("/proc/self/stat") {
            let fields: Vec<&str> = data.split_whitespace().collect();
            // fields[13] = utime (jiffies), fields[14] = stime (jiffies)
            if fields.len() > 14 {
                let utime: u64 = fields[13].parse().unwrap_or(0);
                let stime: u64 = fields[14].parse().unwrap_or(0);
                // jiffies → microseconds (assume HZ=100 → 10 ms per jiffy)
                let hz: u64 = 100;
                return (utime + stime) * 1_000_000 / hz;
            }
        }
        0
    }
    #[cfg(not(target_os = "linux"))]
    {
        0
    }
}

#[tokio::main]
async fn main() {
    let args: Vec<String> = std::env::args().collect();
    let n = parse_n(&args);

    let counter = Arc::new(AtomicU64::new(0));

    let t0 = Instant::now();

    // Spawn N tasks; each does trivial work.
    let handles: Vec<_> = (0..n)
        .map(|_| {
            let c = Arc::clone(&counter);
            tokio::spawn(async move {
                c.fetch_add(1, Ordering::Relaxed);
            })
        })
        .collect();

    // Await all.
    for h in handles {
        let _ = h.await;
    }

    let elapsed_ns = t0.elapsed().as_nanos() as u64;
    let rss        = peak_rss_kb();
    let cpu        = cpu_us();

    println!(
        "workload=W1 runtime=tokio params=N={n} \
         elapsed_ns={elapsed_ns} cpu_us={cpu} rss_kb={rss} \
         p50_ns=0 p95_ns=0 p99_ns=0 p999_ns=0"
    );
}
