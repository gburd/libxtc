// Copyright (c) 2026, The XTC Project — All rights reserved.
// Use of this source code is governed by the ISC License.
//
// bench/conformance/w2_echo/tokio/src/main.rs
//   M17 W2 — TCP echo server, Tokio runtime.
//
//   The server runs a tokio::net::TcpListener::accept loop, spawning a
//   task per accepted connection that echoes data back.  Clients are also
//   spawned as Tokio tasks (not OS threads) — they use
//   tokio::net::TcpStream::connect + write_all + read_exact.
//
//   Latency: per-RTT, recorded in an HDRHistogram.
//
// Usage:
//   cargo run --release -- [--clients=N] [--msgs=M]
//   cargo run --release -- --params=clients=N:msgs=M
//   ./bench --clients=1000 --msgs=10

use std::sync::Arc;
use std::time::Instant;

use hdrhistogram::Histogram;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::{TcpListener, TcpStream};
use tokio::sync::{Barrier, Mutex};

// ---------------------------------------------------------------------------
// Argument parsing
// ---------------------------------------------------------------------------

fn parse_args(args: &[String]) -> (u64, u64) {
    let mut clients: u64 = 1000;
    let mut msgs: u64 = 10;

    for a in args {
        if let Some(v) = a.strip_prefix("--clients=") {
            if let Ok(n) = v.parse::<u64>() {
                clients = n;
            }
        } else if let Some(v) = a.strip_prefix("--msgs=") {
            if let Ok(n) = v.parse::<u64>() {
                msgs = n;
            }
        } else if let Some(rest) = a.strip_prefix("--params=") {
            for kv in rest.split(':') {
                if let Some(v) = kv.strip_prefix("clients=") {
                    if let Ok(n) = v.parse::<u64>() {
                        clients = n;
                    }
                } else if let Some(v) = kv.strip_prefix("msgs=") {
                    if let Ok(n) = v.parse::<u64>() {
                        msgs = n;
                    }
                }
            }
        }
    }

    (clients, msgs)
}

// ---------------------------------------------------------------------------
// Resource helpers
// ---------------------------------------------------------------------------

fn cpu_us() -> u64 {
    // Read from /proc/self/stat: fields 14 (utime) + 15 (stime) in clock ticks.
    if let Ok(s) = std::fs::read_to_string("/proc/self/stat") {
        let fields: Vec<&str> = s.split_whitespace().collect();
        if fields.len() > 15 {
            let utime: u64 = fields[13].parse().unwrap_or(0);
            let stime: u64 = fields[14].parse().unwrap_or(0);
            let ticks_per_sec = unsafe { libc::sysconf(libc::_SC_CLK_TCK) } as u64;
            if ticks_per_sec > 0 {
                return (utime + stime) * 1_000_000 / ticks_per_sec;
            }
        }
    }
    0
}

fn rss_kb() -> u64 {
    // /proc/self/status has VmRSS in kB
    if let Ok(s) = std::fs::read_to_string("/proc/self/status") {
        for line in s.lines() {
            if let Some(rest) = line.strip_prefix("VmRSS:") {
                let trimmed = rest.trim();
                // "12345 kB"
                if let Some(num) = trimmed.split_whitespace().next() {
                    return num.parse().unwrap_or(0);
                }
            }
        }
    }
    0
}

// ---------------------------------------------------------------------------
// Echo server connection handler
// ---------------------------------------------------------------------------

async fn handle_connection(mut stream: TcpStream) {
    let mut buf = vec![0u8; 64];
    loop {
        let n = match stream.read(&mut buf).await {
            Ok(0) | Err(_) => break,
            Ok(n) => n,
        };
        if stream.write_all(&buf[..n]).await.is_err() {
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

#[tokio::main]
async fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let (n_clients, n_msgs) = parse_args(&args);

    // Bind the server to an ephemeral port on loopback.
    let listener = TcpListener::bind("127.0.0.1:0")
        .await
        .expect("TcpListener::bind failed");
    let addr = listener.local_addr().expect("local_addr failed");

    // Shared histogram (Mutex-wrapped for concurrent updates).
    let hist: Arc<Mutex<Histogram<u64>>> = Arc::new(Mutex::new(
        Histogram::new_with_bounds(1, 60_000_000_000, 2)
            .expect("Histogram::new failed"),
    ));

    // Barrier so all clients start at approximately the same time.
    let barrier = Arc::new(Barrier::new(n_clients as usize + 1));

    // ---- Start wall clock ----
    let t0 = Instant::now();

    // Spawn server task.
    tokio::spawn(async move {
        loop {
            match listener.accept().await {
                Ok((stream, _)) => {
                    tokio::spawn(handle_connection(stream));
                }
                Err(_) => break,
            }
        }
    });

    // Spawn client tasks.
    let mut client_handles = Vec::with_capacity(n_clients as usize);
    for _ in 0..n_clients {
        let hist_clone = Arc::clone(&hist);
        let barrier_clone = Arc::clone(&barrier);
        let n_msgs_copy = n_msgs;

        let handle = tokio::spawn(async move {
            barrier_clone.wait().await;

            let mut stream = match TcpStream::connect(addr).await {
                Ok(s) => s,
                Err(_) => return,
            };
            stream.set_nodelay(true).ok();

            let msg = b"xtcping!";
            let mut rxbuf = [0u8; 8];

            for _ in 0..n_msgs_copy {
                let t0_msg = Instant::now();

                if stream.write_all(msg).await.is_err() {
                    break;
                }
                if stream.read_exact(&mut rxbuf).await.is_err() {
                    break;
                }

                let elapsed_ns = t0_msg.elapsed().as_nanos() as u64;
                let mut h = hist_clone.lock().await;
                let _ = h.record(elapsed_ns.max(1));
            }
        });
        client_handles.push(handle);
    }

    // Release all clients simultaneously.
    barrier.wait().await;

    // Wait for all clients to finish.
    for h in client_handles {
        let _ = h.await;
    }

    let elapsed_ns = t0.elapsed().as_nanos() as u64;
    let cpu_micros = cpu_us();
    let peak_rss = rss_kb();

    let h = hist.lock().await;
    let p50  = h.value_at_quantile(0.500);
    let p95  = h.value_at_quantile(0.950);
    let p99  = h.value_at_quantile(0.990);
    let p999 = h.value_at_quantile(0.999);

    println!(
        "workload=W2 runtime=tokio params=clients={}:msgs={} \
         elapsed_ns={} cpu_us={} rss_kb={} \
         p50_ns={} p95_ns={} p99_ns={} p999_ns={}",
        n_clients, n_msgs,
        elapsed_ns, cpu_micros, peak_rss,
        p50, p95, p99, p999,
    );
}

// ---------------------------------------------------------------------------
// libc shim for cpu_us()
// ---------------------------------------------------------------------------
mod libc {
    extern "C" {
        pub fn sysconf(name: i32) -> i64;
    }
    pub const _SC_CLK_TCK: i32 = 2;
}
