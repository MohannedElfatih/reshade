# D3D12 asynchronous pipeline plan

This document describes the current architecture, safety contract, and release goals. The chronological prototype record has moved to [plan_history.md](plan_history.md). Current implementation work is tracked in [todo.md](todo.md).

## Goal

Reduce application-thread D3D12 pipeline creation stalls by returning a unique `ID3D12PipelineState` proxy immediately, compiling the real native pipeline in the background, and promoting command lists from a temporary fallback to the real pipeline as soon as it is available.

This is intentionally an experimental compatibility layer: returning success before native creation finishes cannot exactly preserve the normal D3D12 failure contract.

## Current scope

- Classic graphics and compute pipeline descriptors.
- Graphics and compute pipeline-state streams, including deep copies of stream-only state.
- Mesh/amplification pipeline streams for real asynchronous creation.
- Unique COM identity and proxy-owned metadata for each application request.
- Pipeline-library storage deferral and residency-intent replay.
- Ray-tracing state objects remain out of scope.
- Cached input PSOs, unsupported interfaces, MSAA, and risky descriptors may remain synchronous.

## Architecture

1. Intercept classic or stream pipeline creation.
2. Validate asynchronous eligibility and deep-copy all caller-owned descriptor data.
3. Obtain a temporary fallback PSO.
4. Return a unique proxy whose atomic current-native pointer initially targets the fallback.
5. Queue real native creation on background workers.
6. Publish the real PSO atomically after successful creation and add-on initialization.
7. Resolve proxies at `SetPipelineState`, command-list initialization, `Reset`, and `ClearState`.
8. If a command list recorded a fallback bind, attempt just-in-time promotion immediately before draw, dispatch, indirect, or mesh-dispatch commands.
9. Defer pipeline-library stores and replay residency intent against the real PSO.

## Fallback modes and safety contract

Two fallback strategies exist:

- Sentinel fallback: separate shared classic-graphics, mesh-graphics, and compute placeholders use a dummy empty root signature. This minimizes fallback creation, but the sentinels are not structurally compatible with arbitrary application state. The mesh sentinel uses a minimal `ms_6_5` shader that calls `SetMeshOutputCounts(0, 0)`.
- Keyed fallback: fallback PSOs retain selected compatibility fields and the application root signature. This costs more fallback creation and is still not a complete emulation of the real pipeline.

Pending commands are skipped by default. The following constraints are release requirements:

- Sentinel fallback must never execute draw, dispatch, indirect, or mesh-dispatch work. Binding is only a placeholder while commands are suppressed or until just-in-time promotion succeeds.
- `FallbackMode=execute` may only be enabled for pipeline classes whose keyed fallback has been proven compatible.
- Mesh pipelines are created from pipeline-state streams and execute through `DispatchMesh`; a classic VS/PS fallback is not a valid executable substitute for a mesh pipeline.
- A real compilation failure must become an explicit terminal proxy state. It must not silently leave a proxy skipping commands forever.

## Cached pipeline behavior

`ID3D12PipelineState::GetCachedBlob` returns driver-generated cached pipeline data that can be supplied through `D3D12_CACHED_PIPELINE_STATE` on a later matching creation request. It is not shader bytecode and is generally tied to the relevant adapter, driver, and pipeline description.

Proxy policy:

- With `[ASYNC] WaitForCachedBlob=1` (the default), wait on the calling thread until normal queued creation produces the real PSO, then forward to that PSO. Do not reprioritize the job.
- If real creation fails or manager shutdown cancels the job, wake the caller and return the terminal failure.
- With `WaitForCachedBlob=0`, retain the experimental legacy behavior of forwarding to the current native PSO, which may expose fallback cached data.
- Keep `StorePipeline` deferred until the real PSO exists.

## Compile scheduling and backpressure

The scheduler should prevent compiler work from becoming a new source of hitches.

- Maintain a bounded configurable queue and report queue-limit fallbacks.
- Transfer queued PSOs that resolve to a fallback into a dedicated urgent queue, which workers drain before normal FIFO work without scanning the full compile backlog.
- Keep a worker pool, but begin with a conservative active compiler limit of one or two.
- Increase concurrency only after sustained frame-time headroom.
- Reduce concurrency immediately when present timing regresses.
- Do not bypass pacing merely because pending jobs fit the maximum worker pool; concurrent native creation is itself the pressure being controlled.
- Restore full concurrency only when the queue is empty or an explicit loading-screen policy permits it.

## Add-on compatibility

Worker-side `create_pipeline` and `init_pipeline` callbacks change callback timing and may increase callback concurrency. Before general use, choose and document one of these contracts:

- Preserve synchronous add-on callback ordering and proxy only when no creation-mutating add-on is registered.
- Make delayed worker-side callbacks an explicit opt-in capability.
- Serialize add-on callbacks independently from native PSO compilation.

Fallback handles must not be presented to add-ons as if they were the requested real pipelines unless the fallback receives a complete and internally consistent lifecycle.

## Release goals

### 1. Correct proxy semantics

- Preserve COM identity, reference counts, private data, names, device lookup, and supported interfaces.
- Deep-copy every pointer-bearing descriptor or stream subobject used asynchronously.
- Define terminal states for queued, compiling, published, and failed proxies.
- Never return fallback cached data for the requested real pipeline.

### 2. Safe pending behavior

- Enforce the sentinel command-suppression rule in code rather than relying on configuration guidance.
- Restrict executable fallback mode to validated keyed pipeline classes.
- Define compile-failure recovery and diagnostics.
- Validate command lists, bundles, indirect commands, graphics, compute, and mesh separately.

### 3. Hitch-resistant scheduling

- Prevent the initial creation burst from launching the maximum worker tier.
- Bound queued memory and active driver compilation.
- Measure active workers as well as queued and pending jobs.
- Add an explicit loading-screen or manual-unthrottled mode only if runtime evidence justifies it.

### 4. Measurable compatibility

Track and evaluate:

- synchronous and asynchronous creation counts;
- compile successes, failures, average latency, and maximum latency;
- queue high-water mark and active-worker high-water mark;
- fallback resolves, skipped commands, promotions, and terminal failures;
- pipeline-library and cached-blob use;
- debug-layer errors, device removals, and crash dumps;
- frame-time percentiles before, during, and after compile bursts.

Detailed counters are active only with `[ASYNC] Debug=1`; release-mode hot paths retain only synchronization and control state required for correct behavior.

## Validation gates

1. Syntax-check every touched D3D12, DXGI, and DLL source configuration.
2. Run with the D3D12 debug layer and treat root-signature or pipeline-type mismatches as failures.
3. Test classic graphics, classic compute, graphics streams, compute streams, mesh streams, command-list reset/clear, bundles, and indirect commands.
4. Confirm no fallback blob can enter an application cache or pipeline library.
5. Force native creation failures and verify explicit terminal behavior.
6. Compare one, two, and adaptive active compiler limits using frame-time percentiles rather than average FPS.
7. Test with add-ons that register `create_pipeline`, lifecycle-only events, and no pipeline events.

## Relevant files

- `d3d12_async_pipeline.cpp` / `.hpp` — proxy, descriptor copies, fallback creation, workers, pacing, residency, and diagnostics.
- `d3d12_device.cpp` / `.hpp` — creation interception, stream handling, residency, and command-list initialization.
- `d3d12_command_list.cpp` / `.hpp` — proxy resolution, fallback tracking, promotion, and command suppression.
- `../dxgi/dxgi_swapchain.cpp` / `.hpp` — present-time pacing source.
- `../dll_main.cpp` — optional crash diagnostics.

## Current priorities

1. Make sentinel execution impossible, including mesh dispatch.
2. Implement a terminal compilation-failure state and policy.
3. Validate default `GetCachedBlob` waiting and measure whether callers lie on a frame-critical path.
4. Change pacing from reactive maximum-first behavior to conservative ramp-up.
5. Decide the worker-side add-on callback contract.
6. Improve and validate keyed fallback shaders only after the safety contract is fixed.
