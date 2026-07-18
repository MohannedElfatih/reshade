# D3D12 asynchronous pipeline implementation history

This is the chronological record formerly embedded in [plan.md](plan.md). The current architecture, safety contract, and release goals live there; active discussion items live in [todo.md](todo.md).

## Phase 0–1: initial proxy prototype

- Located classic graphics creation, pipeline binding, command-list reset/clear, initial-state, and residency interception points.
- Added `d3d12_async_pipeline.cpp` and `.hpp` with a unique `ID3D12PipelineState` COM proxy.
- Added deep copies for asynchronously used graphics descriptor data.
- Implemented shared fallback PSOs, worker-side real creation, atomic fallback-to-real publication, and `GetCachedBlob` instrumentation.
- Added synchronous eligibility fallbacks for unsupported interfaces, MSAA, root-signature, topology, stream-output, geometry/tessellation, and multi-adapter cases.
- Added the new sources to the CMake and Visual Studio project files.

## Phase 2: diagnostics

- Added creation-path, eligibility, fallback-cache, queue, compile-time, resolve, residency, and cached-blob counters.
- Added classic-versus-stream and `ID3D12PipelineState1` counters.
- Added periodic diagnostics because shutdown-only summaries mostly described temporary devices.

## Phase 3: stream creation and fallback experiments

- Runtime evidence showed target games using `ID3D12Device2::CreatePipelineState`, so a stream-to-classic converter and asynchronous stream path were added.
- Tested fixed fallback-bind publication delays and compile gating.
- Tested a single global fallback, command suppression, keyed fallbacks, fallback execution, pipeline-library deferral, and compute-only operation.
- Established that a shared global fallback cannot safely execute arbitrary application work and initially restored command suppression.

## Phase 4.0–4.7: optimization, promotion, and residency

- Reduced descriptor-copy overhead, shader storage, logging, and redundant worker wakeups.
- Restored sentinel fallback mode with one graphics and one compute fallback.
- Added just-in-time promotion before draw, indexed draw, dispatch, and indirect commands so command lists recorded with fallback can bind the real PSO before useful work.
- Removed fixed bind-count compilation/publication gates and moved to eager background creation.
- Added deferred residency intent and a separate residency worker.
- Expanded from one compiler worker to multiple workers.

## Phase 4.8–4.14: add-on lifecycle experiments

- Moved real-pipeline `create_pipeline` and `init_pipeline` handling onto compiler workers.
- Tested keeping asynchronous creation enabled for lifecycle-only and creation-mutating add-ons.
- Iterated between suppressing fallback commands, executing fallback commands, hiding fallback handles, and publishing fallback lifecycle events.
- Settled temporarily on null pipeline handles for fallback binds and real handles after promotion, but the final add-on timing and concurrency contract remains open.

## Phase 4.15–4.18: lossless stream preservation and mesh support

- Added `CopiedPipelineStateStream`, preserving raw stream shape and deep-copying nested shader, cached-PSO, input-layout, stream-output, view-instancing, and serialized-root-signature data.
- Kept classic conversion only for support checks and fallback construction; real creation uses the copied original stream.
- Added fallback-only downgrades for `DEPTH_STENCIL1/2` and `RASTERIZER1/2` while retaining the original state for real creation.
- Added view instancing, stream output, serialized root signatures, amplification shaders, and mesh shaders to supported stream copying.
- Added a manager-owned dummy empty root signature for sentinel fallback PSOs.
- Added `DispatchMesh` fallback handling. A classic fallback remains unsuitable for actually executing mesh work.

## Phase 4.19: runtime configuration and crash diagnostics

- Added `[ASYNC]` configuration for minidumps, detailed diagnostics, sentinel versus keyed fallbacks, pending-command behavior, compile pacing, and maximum queued jobs.
- Added consistent `[ASYNC]` log prefixes and millisecond timing output.

## Phase 4.20: proportional present-time pacing

- Added one pacing source per D3D12 swapchain with short recent and slow baseline frame-time estimates.
- Added device-local aggregation for multiple swapchains.
- Added 75%, 50%, and 25% hardware-thread worker tiers with confirmation samples and gradual tier changes.
- Restored the maximum tier whenever pending work fit the maximum worker pool.
- Review identified that maximum-first startup and the pending-count bypass do not protect the initial compiler burst; the current plan replaces this with conservative ramp-up.

## Phase 4.21: real-PSO cached-blob synchronization

- Added `[ASYNC] WaitForCachedBlob=1`, enabled by default.
- `GetCachedBlob` now sleeps the calling thread until the normally queued real PSO finishes, then forwards the request to the real PSO without changing compile-queue priority.
- Native creation failure and manager shutdown cancellation now record a terminal result and wake all cached-blob waiters.
- Added cached-blob wait count, average duration, and maximum duration diagnostics.

## Phase 4.22: debug-only counters and mesh sentinel

- Routed global diagnostics increments and maxima through `[ASYNC] Debug`, eliminating their atomic writes when detailed diagnostics are disabled.
- Removed unconditional per-proxy bind-counter writes and compile-gate notifications while the bind thresholds remain zero.
- Added embedded DXIL for a minimal `ms_6_5` shader that emits zero vertices/primitives and a no-output `ps_6_0` shader.
- Added a separate global mesh sentinel PSO created through `ID3D12Device2::CreatePipelineState` and selected for pipeline streams containing a mesh shader.
- Mesh sentinel commands remain subject to fallback command suppression because the shared dummy root signature is not compatible with arbitrary application root state.

## Validation record

- Repeated per-file Clang syntax validation succeeded for the touched asynchronous pipeline, device, command-list, swapchain, and DLL sources after removing the generated `-fPIC` option and suppressing the pre-existing HRESULT narrowing diagnostic.
- The existing full CMake build configuration has failed before reaching these sources because Clang targeting MSVC rejects the generated `-fPIC` option.
- Runtime validation has been performed incrementally using target-game `ReShade.log` diagnostics, but the safety and release gates in [plan.md](plan.md) remain incomplete.
