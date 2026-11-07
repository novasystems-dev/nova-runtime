# Nova-Runtime — High-Performance Perceus Region Allocator & Bare-Metal FFI

> **Sub-Microsecond Memory Recycling | POSIX / Winsock Sockets | Lock-Free Atomics | Cross-Platform C99 Native Engine**

Nova-Runtime is the minimal, ultra-portable C99 native runtime engine underpinning the **Nova Systems Programming Language**.

---

## Architectural Pillars

- **Perceus Region Allocator**: 1MB default chunk arenas with sub-nanosecond bump allocation and deterministic recycling.
- **POSIX & Winsock Sockets**: Unified non-blocking high-throughput network transport.
- **Lock-Free Concurrency**: Lock-free atomic load, store, add, and CAS primitives using standard compiler intrinsics.
- **Zero Heavy Dependencies**: Pure C99, linking only standard system libraries (`pthread`, `m`, `ws2_32`).

---

## License

PolyForm Noncommercial License 1.0.0. Copyright (c) 2026 Fyew999 (Nova Systems).
See [LICENSE](LICENSE) for details.
