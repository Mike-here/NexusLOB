# NexusLOB

NexusLOB is a C++17, Linux-oriented limit-order book and price-time matching engine designed around predictable memory access rather than general-purpose STL containers. It provides a realistic integration surface—fixed binary TCP messages, an `epoll` I/O loop, isolated matching, and a NumPy workload/validator—without pretending that a userspace demo alone has exchange-grade latency.

## Design

| Concern | Implementation |
| --- | --- |
| Resting-order allocation | One startup `std::vector<Order>` inside `OrderPool`; a `uint32_t` free-list is O(1). |
| Queue layout | `Order` embeds previous/next pool indices. Every price level is FIFO without `std::list`. |
| Price discovery | Bid and ask `std::vector<PriceLevel>` arrays, plus a preallocated hierarchical bitmap to refresh the best price without scanning the range. |
| Cancellation | Dense `order_id -> pool_index` vector and intrusive unlinking are O(1). |
| Concurrency | One network producer and one matching consumer connected by an acquire/release SPSC ring; no mutexes or condition variables. |
| Output safety | A second fixed SPSC ring returns execution reports. If full, the matcher retains the in-flight aggressor residual before changing its next fill. |

All vectors, bitmaps, ID slots, price levels, and SPSC entries are sized during construction. The matching loop performs array access and integer-index manipulation only; it never calls `new` or `delete`.

For a configured, bounded price domain, executing the queue head is O(1). When a level becomes empty, the hierarchical bitmap finds the next non-empty price in a fixed number of 64-way levels (at most six over the full 32-bit tick domain), rather than walking the price array.

## Layout

```text
include/nexuslob/
  types.h              Core book and message types
  memory_pool.h        Preallocated intrusive order pool
  ring_buffer.h        Cache-line-separated SPSC queue
  order_book.h         Matching and bounded price structures
  wire_protocol.h      Fixed-size, network-byte-order binary protocol
  network_listener.h   epoll TCP listener API
src/
  order_book.cpp       Price-time matching / cancel logic
  network_listener.cpp epoll ingestion and execution output
  main.cpp             Matching-core pinning and busy poll loop
tools/backtest_momentum.py  NumPy load generator and validator
```

## Build and test

The server is Linux-only because it uses `epoll` and `sched_setaffinity`. Core unit tests build on non-Linux hosts too.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Optional `-DNEXUSLOB_NATIVE_ARCH=ON` enables `-march=native` for a pinned deployment host.

## Docker run and workload replay

```bash
docker build -t nexuslob .
docker run --rm --cpuset-cpus=0 -p 9000:9000 nexuslob
python3 tools/backtest_momentum.py --host 127.0.0.1 --port 9000
```

The Python client first seeds 2,048 bid and ask levels, derives market-order direction from a NumPy fast-minus-slow momentum signal, sends a fixed-size binary barrage, then validates every execution's sequence, aggressor ID, side, and quantity before calculating marked P&L.

## Wire protocol

The TCP stream has no framing allocation: it is a sequence of fixed-size, big-endian records.

| Record | Size | Format |
| --- | ---: | --- |
| Order | 32 B | `magic:u32, message:u8, side:u8, type:u8, reserved:u8, order_id:u64, price_tick:u32, quantity:u32, client_sequence:u64` |
| Execution | 40 B | `magic:u32, message=0x80:u8, aggressor_side:u8, reserved:u16, execution_sequence:u64, aggressor_id:u64, resting_id:u64, price_tick:u32, quantity:u32` |

`magic` is `0x4E4C4F42` (`NLOB`). New order messages use `1`; cancellation messages use `2`. Order types are GTC limit (`1`), IOC limit (`2`), and market (`3`). Prices are integer ticks, eliminating floating-point work from the book.

## Deployment notes

This is intentionally single-writer for book state—the standard way to preserve deterministic price-time order without locks. For a production deployment, place the matching core on an isolated CPU, pre-fault/lock its preallocated memory where policy permits, tune NIC interrupt affinity, use a kernel-bypass feed handler if appropriate, add risk/self-trade controls ahead of the ingress ring, and measure p50/p99/p99.9 latency under a realistic load generator.
