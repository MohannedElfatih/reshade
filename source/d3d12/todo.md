# Async D3D12 Pipeline Prototype TODO

Current architecture and release goals: [plan.md](plan.md). Chronological implementation record: [plan_history.md](plan_history.md).

## Discussion items for improvement ideas

- [x] Add a risk-score heuristic for graphics/compute PSO proxy eligibility.
  - Defer for now; revisit later once the current fallback/proxy path is more stable.
- [x] Improve fallback compatibility keys to reduce mismatches without exploding cache size.
  - Defer for now; revisit later once we have a better view of which fields actually matter in practice.
- [x] Add compile backpressure, queue limits, and worker budgeting.
  - Expand this into: cap the number of queued compile jobs, limit concurrent worker threads, and throttle compilation when the game is under heavy load or during frame-time-sensitive periods.
  - The goal is to prevent a burst of async PSO work from creating new hitching spikes while still allowing the system to compile useful pipelines in the background.
  - Initial pacing compares a short D3D12 present-time average to a slowly learned baseline: only a material regression limits real PSO creation to one worker.
  - `MaxQueuedJobs` defaults to `0` (unlimited). With a nonzero limit, a full queue forces synchronous creation for that request, avoiding permanently pending fallback proxies while bounding async memory and backlog.
- [x] Replace fixed bind-count gating with a more adaptive real-PSO promotion strategy.
  - Remove the fixed bind-count approach entirely.
  - Instead, rely on more natural triggers such as: publish the real PSO once compilation has completed and the pipeline is likely to be used soon, or allow promotion based on broader runtime context rather than a magic bind threshold.
  - This should help avoid missing loading-screen work and reduce the chance of leaving useful PSOs pending too long.
- [ ] Improve fallback shader quality and make the fallback path more robust.
  - Expand this into: use a tiny, well-tested fallback shader pair that is as harmless as possible when bound, avoid invalid resource usage, and keep the fallback path compatible with a wider range of render targets and depth/stencil combinations.
  - The fallback should be conservative and predictable rather than trying to emulate the real pipeline too closely.
- [x] Add clearer per-proxy lifecycle state tracking and diagnostics.
  - Defer for now because excessive logging would hammer disk and make runtime traces noisy.
- [x] Deduplicate repeated real PSO compiles when the same descriptor is seen again.
  - Clarify this: if the game calls Create*PipelineState repeatedly with the same effective description, we may be able to avoid compiling the same pipeline more than once.
  - That said, this is probably not common for the current target workload, so it remains a low-priority optimization unless profiling shows repeated churn.
- [x] Add richer per-proxy observability for binds, publication, promotion, and failures.
  - Combine this with the earlier diagnostics backlog under a single item: "more debug logs".
  - Keep this lightweight and opt-in so we do not flood the log file during normal runs.
- [x] Add opt-in crash dumps and runtime diagnostics configuration.
  - `ReShade.ini` now supports `[ASYNC] EnableMiniDump=1` (default disabled), `[ASYNC] Debug=1`, `[ASYNC] Sentinel=1` (default enabled), and `FallbackMode` (`0` skips pending commands, `1` executes fallback shaders).
- [x] Wait for the real PSO when `GetCachedBlob` is called.
  - `[ASYNC] WaitForCachedBlob=1` is enabled by default. The calling thread sleeps until the normally queued real PSO finishes; the job is not reprioritized.
  - Compilation failure and manager shutdown wake waiters with a terminal failure instead of leaving them blocked.
- [x] Make detailed diagnostics counters debug-only.
  - `[ASYNC] Debug=0` no longer performs global diagnostic atomic increments/max updates or unconditional per-proxy bind-count writes.
- [x] Add a global mesh sentinel fallback.
  - Mesh streams now use a separate pipeline-state-stream sentinel with embedded minimal `ms_6_5`/`ps_6_0` DXIL. The mesh shader calls `SetMeshOutputCounts(0, 0)`.
  - `DispatchMesh` remains suppressed in the safe fallback mode because the global sentinel still uses the dummy root signature.

## Notes

- Keep this file as a lightweight discussion and implementation tracker for the prototype ideas.
- Update it as decisions are made or work is started.
