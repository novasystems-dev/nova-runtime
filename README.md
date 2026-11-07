<div align="center">

# ⚙️ Nova Runtime Engine

**High-Performance Perceus Region Allocator & Bare-Metal FFI Engine**

<br />

[![License](https://img.shields.io/badge/license-PolyForm-a855f7?style=flat&labelColor=0f172a&logo=open-source-initiative&logoColor=white)](LICENSE)
[![Nova](https://img.shields.io/badge/language-Nova_0.31.00-38bdf8?style=flat&labelColor=0f172a&logo=starship&logoColor=white)](https://github.com/novasystems-dev/nova)
[![Engine](https://img.shields.io/badge/engine-Perceus_C99-10b981?style=flat&labelColor=0f172a&logo=c&logoColor=white)](src/)
[![Ecosystem](https://img.shields.io/badge/ecosystem-10%20repos-f59e0b?style=flat&labelColor=0f172a&logo=github&logoColor=white)](https://github.com/novasystems-dev)

<br />

</div>

---

## ⚙️ Architectural Pillars

- **Perceus Region Allocator**: 1MB default chunk arenas with sub-nanosecond bump allocation and deterministic recycling.
- **POSIX & Winsock Sockets**: Unified non-blocking high-throughput network transport.
- **Lock-Free Concurrency**: Lock-free atomic load, store, add, and CAS primitives using standard compiler intrinsics.
- **Zero Heavy Dependencies**: Pure C99, linking only standard system libraries (`pthread`, `m`, `ws2_32`).

---

## ⚖️ License

PolyForm Noncommercial License 1.0.0. Copyright (c) 2026 Fyew999 (Nova Systems).
See [LICENSE](LICENSE) for details.
