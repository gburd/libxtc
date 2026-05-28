// Copyright (c) 2026, The XTC Project
// Use of this source code is governed by the ISC License.
//
// W4 mutex contention: parking_lot::Mutex variant.
//
// The default tokio bench uses tokio::sync::Mutex which is an
// async mutex and therefore much slower than a raw OS mutex on
// short critical sections (the lock-acquire path goes through the
// task scheduler, not directly to the OS).  This binary uses
// parking_lot::Mutex -- a raw OS mutex with the std API shape --
// for the *fair* comparison against xtc_lwlock and __os_mutex.

use hdrhistogram::Histogram;
use parking_lot::Mutex;
use std::env;
use std::fs::File;
use std::io::{BufRead, BufReader};
use std::sync::Arc;
use std::thread;
use std::time::{Duration, Instant};

fn rss_kb() -> u64 {
    File::open("/proc/self/status").ok().and_then(|f| {
        BufReader::new(f).lines().filter_map(|l| l.ok()).find_map(|l| {
            l.strip_prefix("VmRSS:").and_then(|rest| {
                rest.split_whitespace().next()?.parse::<u64>().ok()
            })
        })
    }).unwrap_or(0)
}

fn cpu_us() -> u64 {
    let mut ts: libc::timespec = unsafe { std::mem::zeroed() };
    let rc = unsafe {
        libc::clock_gettime(libc::CLOCK_PROCESS_CPUTIME_ID, &mut ts)
    };
    if rc != 0 { return 0; }
    (ts.tv_sec as u64) * 1_000_000 + (ts.tv_nsec as u64 / 1000)
}

fn main() {
    let args: Vec<String> = env::args().collect();
    let mut n_threads: usize = 8;
    let mut ops: u64 = 100_000;
    for a in &args[1..] {
        if let Some(v) = a.strip_prefix("--threads=") {
            n_threads = v.parse().unwrap_or(8);
        } else if let Some(v) = a.strip_prefix("--ops=") {
            ops = v.parse().unwrap_or(100_000);
        }
    }
    let per_task = ops / (n_threads as u64);
    let total = per_task * (n_threads as u64);

    let counter = Arc::new(Mutex::new(0u64));
    let cpu0 = cpu_us();
    let t0 = Instant::now();

    let handles: Vec<_> = (0..n_threads)
        .map(|_| {
            let counter = counter.clone();
            thread::spawn(move || {
                let mut h = Histogram::<u64>::new_with_bounds(1, 60_000_000_000, 2)
                    .unwrap();
                for i in 0..per_task {
                    let do_sample = (i % 1000) == 0;
                    let t = if do_sample { Some(Instant::now()) } else { None };
                    {
                        let mut g = counter.lock();
                        *g += 1;
                    }
                    if let Some(t) = t {
                        let _ = h.record(t.elapsed().as_nanos() as u64);
                    }
                }
                h
            })
        })
        .collect();

    let mut merged = Histogram::<u64>::new_with_bounds(1, 60_000_000_000, 2).unwrap();
    for h in handles {
        merged.add(&h.join().unwrap()).unwrap();
    }
    let elapsed = t0.elapsed();
    let cpu1 = cpu_us();
    let val = *counter.lock();
    assert_eq!(val, total);

    println!(
        "workload=W4 runtime=tokio_pl_mutex params=threads={}|ops={} elapsed_ns={} cpu_us={} rss_kb={} p50_ns={} p95_ns={} p99_ns={} p999_ns={}",
        n_threads, total,
        elapsed.as_nanos(),
        cpu1.saturating_sub(cpu0),
        rss_kb(),
        merged.value_at_quantile(0.50),
        merged.value_at_quantile(0.95),
        merged.value_at_quantile(0.99),
        merged.value_at_quantile(0.999),
    );
    let _ = Duration::from_secs(0);     /* silence unused-import */
}
