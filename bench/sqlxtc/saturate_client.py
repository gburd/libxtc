#!/usr/bin/env python3
"""Drive N clients * M queries against sqlxtc.  Reports total queries,
errors, and rejected (connect failures)."""
import json
import socket
import sys
import threading
import time
import random


def main():
    port = int(sys.argv[1])
    n_clients = int(sys.argv[2])
    n_queries = int(sys.argv[3])

    counts = [0]
    errors = [0]
    rejected = [0]
    lock = threading.Lock()

    def worker(idx):
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=10)
        except Exception:
            with lock:
                rejected[0] += 1
            return
        f = s.makefile('rwb', buffering=0)
        # Banner.
        line = f.readline()
        if not line:
            with lock:
                rejected[0] += 1
            return
        m = json.loads(line)
        if 'err' in m:
            with lock:
                rejected[0] += 1
            s.close()
            return

        ok = 0
        err = 0
        for q in range(n_queries):
            r = random.random()
            if r < 0.6:
                msg = b'{"q":"SELECT COUNT(*) FROM t"}\n'
            elif r < 0.85:
                msg = (b'{"q":"INSERT INTO t(k,v) VALUES(' +
                       str(idx*n_queries + q).encode() +
                       b",'c'+'X')\"}\n")
            else:
                msg = b'{"q":"SELECT id,k FROM t LIMIT 5"}\n'
            try:
                f.write(msg)
            except Exception:
                err += 1
                break

            while True:
                line = f.readline()
                if not line:
                    err += 1; break
                jm = json.loads(line)
                if 'cols' in jm or 'row' in jm:
                    continue
                if 'done' in jm:
                    ok += 1; break
                if 'err' in jm:
                    err += 1; break
            else:
                err += 1
        try:
            f.write(b'{"quit":1}\n')
        except Exception:
            pass
        s.close()
        with lock:
            counts[0] += ok
            errors[0] += err

    t0 = time.monotonic()
    threads = [threading.Thread(target=worker, args=(i,))
               for i in range(n_clients)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    elapsed = time.monotonic() - t0

    print(f"clients {n_clients}")
    print(f"per-client {n_queries}")
    print(f"total {counts[0]}")
    print(f"errors {errors[0]}")
    print(f"rejected {rejected[0]}")
    print(f"elapsed {elapsed:.2f}")
    if elapsed > 0:
        print(f"qps {counts[0]/elapsed:.0f}")


if __name__ == "__main__":
    main()
