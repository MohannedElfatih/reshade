# Decisions

## 2026-07-18 — `GetCachedBlob` waits without reprioritizing

`[ASYNC] WaitForCachedBlob=1` is enabled by default. If a proxy receives `GetCachedBlob` before native creation completes, only the calling thread sleeps. The compile job retains its normal queue order so engines that request blobs for every PSO do not continuously reorder the queue. Successful completion forwards to the real PSO; compilation failure or manager shutdown wakes the caller with the terminal failure.

Setting `WaitForCachedBlob=0` restores the legacy experimental behavior of forwarding to the currently bound native PSO, which may be a fallback and therefore may return cached data that does not represent the requested pipeline.

## 2026-07-18 — Dedicated bind-only mesh sentinel

Mesh pipeline streams use a separate global sentinel created from embedded minimal `ms_6_5` and `ps_6_0` DXIL. The mesh shader calls `SetMeshOutputCounts(0, 0)`. The sentinel is type-correct for mesh pipeline binding, but its shared dummy root signature is not compatible with arbitrary application root state, so safe mode continues to suppress `DispatchMesh` until just-in-time promotion binds the real PSO.

Detailed global diagnostics counters are updated only when `[ASYNC] Debug=1`. Synchronization and scheduling atomics required for proxy correctness remain enabled.
