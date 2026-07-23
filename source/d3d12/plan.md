# D3D12 asynchronous pipeline plan

This document describes the current architecture, compatibility contract, and
remaining validation goals for asynchronous D3D12 pipeline creation. Detailed
scheduler control flow is documented in [flow.md](flow.md).

## Goal

Reduce application-thread pipeline creation stalls by immediately returning a
unique `ID3D12PipelineState` proxy, compiling the requested native PSO in the
background, and promoting command lists from a temporary native fallback to the
real pipeline as soon as compilation finishes.

This remains an experimental compatibility layer. Returning success before
native creation completes cannot exactly preserve D3D12's synchronous failure
contract, so failures are recorded as explicit terminal proxy states.

## Current scope

- Classic graphics and compute descriptors.
- Graphics, compute, and mesh/amplification pipeline-state streams, including
  deep copies of caller-owned stream state.
- Unique COM identity and proxy-owned metadata for every application request.
- Deferred pipeline-library stores and residency-intent replay against the real
  pipeline.
- Synchronous fallback for cached input PSOs, unsupported interfaces, MSAA, and
  descriptors that cannot safely be copied or represented.
- Ray-tracing state objects remain out of scope.

## Creation and promotion architecture

1. Intercept classic or stream pipeline creation.
2. Validate async eligibility and deep-copy all pointer-bearing descriptor data.
3. Create or retrieve an appropriate native fallback PSO.
4. Return a unique proxy whose current native pointer initially targets that fallback.
5. Publish a shared compile job to the parallel job registry and lock-free normal
   queue, with one lightweight-semaphore token per physical queue entry.
6. Compile workers check the urgent queue before the normal queue and use atomic
   job-state transitions as the sole authority to claim work.
7. A first pending bind promotes `queued → promoting → priority_queued` and
   publishes the same job to the urgent queue. The original normal entry becomes
   harmless stale work; no backlog scan or arbitrary queue removal occurs.
8. Publish the real native PSO atomically after successful creation, add-on
   initialization, metadata replay, deferred store replay, and residency replay.
9. Resolve proxies at command-list creation, `Reset`, `ClearState`, and
   `SetPipelineState`. Immediately before draw or dispatch, attempt just-in-time
   promotion if the command list still records a fallback.

The scheduler uses separate normal and urgent
`moodycamel::ConcurrentQueue<CompileJobPtr>` instances, per-worker consumer
tokens, a 64-submap parallel node map for job lookup, and atomic states
`queued`, `promoting`, `priority_queued`, `claimed`, and `finished`. A shared
lifecycle mutex gates enqueue and promotion against exclusive shutdown; it is not
a queue lock.

## Ownership and shutdown contract

- The map, physical queue entries, workers, and stale entries share stable jobs
  through `shared_ptr` ownership.
- The scheduler holds one COM reference on each proxy until completion or
  cancellation. `TryAddRef` cannot resurrect a proxy whose count reached zero.
- Every successful physical publication has one work-semaphore token, including
  stale normal entries left by promotion.
- Identity-checked map erasure prevents one completion from removing a different job.
- Shutdown closes lifecycle admission, wakes and joins workers, drains physical
  queues, cancels all remaining logical jobs, and only then unregisters debug
  callbacks or destroys scheduler state.

## Fallback modes and native command safety

Two fallback strategies exist:

- **Sentinel fallback:** shared classic-graphics, mesh-graphics, and compute
  placeholders use dummy state. This minimizes fallback creation but is not
  structurally compatible with arbitrary application state.
- **Keyed fallback:** selected compatibility fields and the application root
  signature are retained. This creates more PSOs and is still not complete
  emulation of the requested pipeline.

`FallbackMode=skip` is the safe default. If just-in-time promotion cannot resolve
the real pipeline, native draw, indexed-draw, dispatch, indirect, and mesh
dispatch commands are suppressed. Their add-on events are still emitted so
add-ons can maintain or clear command-list state.

Sentinel fallbacks must never execute application commands. `FallbackMode=execute`
is only appropriate for keyed pipeline classes that have been separately proven
compatible. A mesh pipeline cannot execute through a classic VS/PS substitute.

## Add-on compatibility contract

- Every fallback PSO receives a complete create/init/destroy add-on lifecycle.
- Bind events publish the actual native fallback handle while a proxy is pending.
- Promotion emits another bind event with the real native pipeline handle.
- Add-ons currently observe but cannot modify fallback descriptors.
- Real pipeline `create_pipeline` and `init_pipeline` callbacks run on compile
  workers and may therefore be delayed and concurrent.
- Suppressing a native command does not suppress its draw/dispatch add-on event;
  callback return values are ignored when execution is already skipped.

Publishing fallback handles without their lifecycle is forbidden because add-ons
may index metadata by pipeline handle.

## Cached pipelines and pipeline libraries

- With `[ASYNC] WaitForCachedBlob=1` (default), `GetCachedBlob` waits for the real
  PSO and returns its driver-generated blob. It does not reprioritize the job.
- Terminal creation failure or shutdown cancellation wakes waiters with the
  recorded failure.
- With `WaitForCachedBlob=0`, legacy experimental behavior may forward to the
  current fallback and expose fallback cached data.
- `StorePipeline` is deferred until the real PSO exists. Pipeline-library names
  are immutable bindings: an occupied name cannot be overwritten with an
  incompatible descriptor, so replay failure is reported rather than retried as
  replacement.

## Compile pacing

Present only publishes coalesced per-source timestamps and counts. A dedicated
pacing thread consumes the latest sample from each pending source and adjusts the
compile-slot limit, so Present never waits on the controller or compile workers.

The worker pool is 75% of hardware threads. Adaptive limits are:

- full: 75%;
- lower: 50%;
- middle: the midpoint between full and lower.

This produces 12/10/8 active native compilers on a 16-thread processor. Control
runs at 250 ms intervals with confirmation samples and asymmetric smoothing.
There is no bounded-queue policy, one-or-two-worker ramp-up, or 100-job override;
logical backlog may be large while active driver compilation remains paced.

## Diagnostics policy

Detailed counters and hot-path timing are enabled only by `[ASYNC] Debug=1`.
Periodic tables and repetitive event summaries use a 512-event cadence without
an initial burst. Deferred `StorePipeline` failures follow the same policy, while
rare native creation failures remain visible.

Queue diagnostics time only direct concurrent-queue enqueue/dequeue calls and
warn at 10 ms. Queue age is expected backlog, and compile-slot waiting is
intentional pacing, so neither is treated as queue contention. Debug-off queue
operations avoid diagnostic clock reads.

## Validation gates

1. Build or syntax-check all touched D3D12, DXGI, dependency, and project-file configurations.
2. Run with the D3D12 debug layer and treat root-signature, pipeline-type, or
   descriptor-lifetime errors as failures.
3. Exercise classic graphics/compute, graphics/compute streams, mesh streams,
   command-list creation/reset/clear, bundles, indirect commands, and mesh dispatch.
4. Verify urgent promotion never duplicates native creation or add-on lifecycle
   events, including promotion/dequeue/shutdown races and stale entries.
5. Verify fallback handles always receive lifecycle events before bind events,
   and promotion publishes the real handle afterward.
6. Verify skip mode emits add-on command events while recording no native work.
7. Force native creation failures and shutdown cancellation; verify terminal
   state, waiter wakeup, descriptor cleanup, and scheduler-reference release.
8. Confirm cached fallback data cannot enter normal application caches or
   pipeline libraries with the default configuration.
9. Compare frame-time percentiles and active-worker limits during large startup
   bursts; do not infer contention from backlog age alone.
10. Test add-ons with lifecycle, bind, command, and creation-mutating callbacks,
    including add-ons that retain pipeline metadata.

## Current priorities

1. Validate the fallback lifecycle and bind-handle contract across all classic,
   stream, mesh, keyed, and sentinel creation paths.
2. Stress native-skip/add-on-event behavior for draw, indexed draw, dispatch,
   indirect, and mesh dispatch.
3. Stress promotion and shutdown races under very large startup backlogs.
4. Measure 12/10/8 pacing across multiple games and Present sources.
5. Investigate add-ons that are not safe under concurrent worker-side creation
   callbacks; keep add-on-specific races separate from scheduler correctness.

## Relevant files

- `d3d12_async_pipeline.cpp` / `.hpp` — proxy, descriptor copies, fallbacks,
  scheduler, pacing, residency, and diagnostics.
- `d3d12_device.cpp` / `.hpp` — creation interception, stream handling,
  residency, and command-list initialization.
- `d3d12_command_list.cpp` / `.hpp` — proxy resolution, fallback tracking,
  promotion, command suppression, and add-on command events.
- `../dxgi/dxgi_swapchain.cpp` / `.hpp` — Present pacing-source publication.
- `../../deps/concurrentqueue` — lock-free queues and lightweight semaphores.
- `../../deps/parallel-hashmap` — synchronized sharded registries and caches.