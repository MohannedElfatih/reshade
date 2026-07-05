## Plan: Prototype D3D12 Graphics PSO Async Fallback System

Goal: Design a D3D12-only graphics-pipeline prototype that returns unique COM proxy PSO objects to the game, reuses a limited cache of compatible native fallback PSOs, compiles real graphics PSOs asynchronously, and resolves proxy binds through a single atomic current-native-PSO pointer initialized to fallback and later swapped to real.

**Scope**
- Include: graphics PSOs from `CreateGraphicsPipelineState` / graphics pipeline-state streams.
- Exclude for now: compute PSOs, ray tracing state objects, mesh shader pipelines unless encountered later, tessellation/geometry/stream-output initially.
- Ignore MSAA completely for now: do not include `SampleDesc.Count` or `SampleDesc.Quality` in fallback key and restrict/force-sync any PSO where MSAA is detected if needed.
- Command list recording caveat is accepted: already-recorded fallback binds will not be rewritten.

**Architecture**
1. Intercept graphics PSO creation.
2. Copy the original `D3D12_GRAPHICS_PIPELINE_STATE_DESC` deeply enough for async creation, including shader bytecode and input layout data if needed by real creation.
3. Compute a coarse `GraphicsFallbackKey`.
4. Get or lazily create a shared native fallback PSO for that key.
5. Create a unique `ID3D12PipelineState` COM proxy per original game PSO request.
6. Initialize `proxy.current_native_pso = fallback_pso`.
7. Return the proxy to the game immediately.
8. Queue real native PSO creation on worker thread.
9. When real creation succeeds, atomically swap `proxy.current_native_pso = real_pso`.
10. Intercept `SetPipelineState`.
11. If argument is a proxy, bind `proxy.current_native_pso.load()` with no ready/else branch; otherwise pass through unchanged.

**Initial graphics fallback key**
- `ID3D12RootSignature *root_signature`
- `D3D12_PRIMITIVE_TOPOLOGY_TYPE primitive_topology_type`
- `UINT num_render_targets`
- `DXGI_FORMAT rtv_formats[8]`
- `DXGI_FORMAT dsv_format`
- `bool depth_enabled`
- `bool stencil_enabled`

Do not include initially:
- `SampleDesc.Count` / `SampleDesc.Quality` because MSAA is ignored
- shader bytecode
- input layout
- blend state details
- rasterizer state details
- depth func / stencil ops
- render target write masks

**Fallback shader blobs**
- Compile/embed fallback shader bytecode once globally.
- Reuse the same fallback VS/PS blobs for every fallback PSO.
- Prefer offline/precompiled DXIL/DXBC byte arrays to avoid runtime shader compiler dependency and startup compilation hitches.
- Fallback VS should use `SV_VertexID` and no vertex input.
- Fallback PS should be no-output/no-op if validation accepts it.

**Fallback PSO design**
- Reuse original root signature.
- Use cached global fallback shader bytecode.
- Use empty input layout.
- Preserve key-compatible fields: topology type, RTV formats, DSV format, depth/stencil enabled bits.
- MSAA ignored: either set sample count to 1 for fallback-only supported path or force-sync real creation for any original PSO with `SampleDesc.Count != 1`.
- Use conservative default blend/rasterizer/depth settings, with depth writes disabled if testing shows this is safe.

**Proxy optimization**
- The game still receives a unique proxy object per requested PSO to preserve identity.
- Internally, the proxy exposes one stable COM handle to the game, but stores a `current_native_pso` pointer.
- `current_native_pso` starts as shared fallback and is swapped to the real native PSO once async compilation completes.
- Bind path does not need `if real_ready else fallback`; it simply loads and binds `current_native_pso`.

**GetCachedBlob instrumentation**
- Implement/forward `ID3D12PipelineState::GetCachedBlob` in the proxy.
- Log every call with proxy id and whether current pointer is fallback or real.
- Initial behavior options:
  - if real PSO is ready, forward to real `GetCachedBlob`;
  - if only fallback exists, either forward to fallback while logging or return a conservative failure after testing.
- Use this to determine whether the game relies on cached PSO blobs.

**Safety policy**
Async fallback only for common simple graphics PSOs:
- triangle/line/point graphics pipelines
- normal VS/PS style pipelines
- no stream output
- no tessellation initially
- no geometry shader initially
- no mesh shader initially
- no unusual node mask / multi-adapter flags
- no MSAA initially, or force-sync any `SampleDesc.Count != 1`

Synchronous real creation for unsupported/risky PSOs.

**Data structures**
- `PipelineProxy`: COM object implementing/wrapping `ID3D12PipelineState`; stores fallback native PSO, real native PSO, atomic current native PSO, original desc/key, ref counts.
- `fallback_cache`: `GraphicsFallbackKey -> ComPtr<ID3D12PipelineState>`.
- `fallback_shader_cache`: process-global precompiled fallback shader bytecode reused by all fallback PSOs.
- `compile_queue`: async jobs with deep-copied real graphics PSO descriptions.
- optional diagnostics: counts, creation latency, fallback bind count, real bind count, cache size, `GetCachedBlob` call count.

**Validation steps**
1. First log-only mode: count unique real graphics PSOs and candidate fallback keys.
2. Estimate fallback count before enabling proxy mode.
3. Add global fallback shader blob cache and verify no runtime shader compilation happens per fallback.
4. Add fallback cache creation and verify fallback PSOs compile.
5. Add proxy object return path for safe PSOs only.
6. Add bind-time resolution via `proxy.current_native_pso.load()`.
7. Add async real PSO worker and atomic pointer swap.
8. Instrument `GetCachedBlob`.
9. Log fallback vs real binds and real compile latency.
10. Add caps/kill-switch: max fallback PSOs, max queued jobs, max fallback frames/binds per proxy.

**Relevant ReShade files**
- `d:\GitHub\reshade\source\d3d12\d3d12_device.cpp` — `D3D12Device::CreateGraphicsPipelineState`, `CreatePipelineState`.
- `d:\GitHub\reshade\source\d3d12\d3d12_impl_device.cpp` — native graphics PSO creation helper.
- `d:\GitHub\reshade\source\d3d12\d3d12_command_list.cpp` — `D3D12GraphicsCommandList::SetPipelineState` bind interception.
- `d:\GitHub\reshade\source\d3d12\d3d12_impl_type_convert.hpp` — pipeline stream helpers.

**Open questions**
- Whether no-output PS is valid/reliable for all RTV bucket cases.
- Whether depth/stencil writes should be disabled or mirrored from original.
- Whether root-signature compatibility permits fallback shaders that use no resources with every original root signature.
- Whether `GetCachedBlob` is called by the game and what behavior it expects before real PSO is ready.
- Whether proxy `QueryInterface` needs to emulate newer `ID3D12PipelineState` interfaces beyond base behavior.


## Implementation progress

### Phase 0 — Codebase inspection
- Found graphics PSO interception at `D3D12Device::CreateGraphicsPipelineState` in `source/d3d12/d3d12_device.cpp`.
- Found bind interception at `D3D12GraphicsCommandList::SetPipelineState` in `source/d3d12/d3d12_command_list.cpp`.
- Found command-list `Reset` / `ClearState` also accept PSO handles and must resolve proxy handles before forwarding to native D3D12.
- Decided first implementation will be a native wrapper-layer prototype, not a ReShade add-on event implementation.


### Phase 1 — Initial implementation
- Added `source/d3d12/d3d12_async_pipeline.hpp` and `.cpp`.
- Implemented `D3D12AsyncPipelineProxy`, a unique COM proxy implementing `ID3D12PipelineState`.
- Proxy stores a single atomic `current_native_pso` pointer that starts as the shared fallback PSO and is swapped to the real PSO after async creation.
- Implemented `GetCachedBlob` instrumentation on the proxy; it logs whether the call happens while the proxy targets fallback or real PSO, then forwards to the current native PSO.
- Implemented a process/device-local fallback PSO cache keyed by root signature, primitive topology type, RTV formats/count, DSV format, and depth/stencil enabled bits. MSAA fields are not part of the key.
- Implemented global fallback shader blob reuse inside the manager: fallback VS/PS are compiled once and reused for all fallback PSO buckets.
- Current fallback shaders are minimal runtime-compiled HLSL (`SV_VertexID` VS + no-op PS). Future optimization: replace with embedded precompiled bytecode to avoid the first runtime `D3DCompile`.
- Implemented async worker queue for real `CreateGraphicsPipelineState` calls.
- Safety filter currently forces synchronous creation for MSAA (`SampleDesc.Count != 1` or `Quality != 0`), null root signatures, tessellation, geometry shader, stream output, patch/undefined topology, and nonzero node mask.
- Wired `D3D12Device::CreateGraphicsPipelineState` to use async proxy creation when no ReShade add-on event already handled creation.
- Wired `D3D12GraphicsCommandList::SetPipelineState`, `Reset`, `ClearState`, and `D3D12Device::CreateCommandList` to resolve proxy handles to the current native PSO before forwarding to native D3D12.
- Wired `MakeResident`, `Evict`, and `EnqueueMakeResident` to resolve proxy PSOs because PSOs are pageable objects.
- Added the new source/header to `CMakeLists.txt`, `ReShade.vcxproj`, and `ReShade.vcxproj.filters`.

### Phase 1 validation
- Full `cmake --build build --config Release --target ReShade` did not reach the changed source files because the existing CMake build configuration fails early with `clang++: error: unsupported option '-fPIC' for target 'x86_64-pc-windows-msvc'`.
- Per-file validation with `clang-cl` succeeded for:
  - `source/d3d12/d3d12_async_pipeline.cpp`
  - `source/d3d12/d3d12_device.cpp` with `RESHADE_ADDON=0` and `RESHADE_ADDON=2`
  - `source/d3d12/d3d12_command_list.cpp` with `RESHADE_ADDON=0` and `RESHADE_ADDON=2`

### Remaining implementation gaps
- `ID3D12Device2::CreatePipelineState` pipeline stream path is not async-proxied yet; it still uses the existing native/ReShade path.
- Fallback shaders are reused after first compilation, but are not embedded offline bytecode yet.
- No runtime config/kill-switch/caps yet.
- Diagnostics counters are now implemented for Phase 2; remaining diagnostics work is to add periodic/in-game dump controls instead of only shutdown summary logging.
- Proxy implements base `ID3D12PipelineState`; requests for `ID3D12PipelineState1` currently fall back to synchronous native creation.

### Phase 2 — Diagnostics counters
- Added global async PSO diagnostics counters in `source/d3d12/d3d12_async_pipeline.cpp`.
- Counters now track graphics create calls, async proxy creates, synchronous fallbacks by reason, fallback cache hits/misses, fallback buckets created, fallback creation failures, fallback shader blob compilation, queued jobs, queue high-water mark, async compile success/failure count, async compile total/max microseconds, proxy resolves to fallback/real, pageable proxy resolves, and `GetCachedBlob` calls.
- `resolve_async_pipeline_state` now counts proxy bind resolves and whether each resolve targets fallback or real native PSO.
- `resolve_async_pageable` now counts pageable proxy resolves separately so residency calls do not pollute bind counters.
- Async worker now measures real PSO compile time with `std::chrono::steady_clock` and updates average/max compile timing counters.
- Manager shutdown logs one summary line with all counters. This should clarify whether the game mostly uses supported classic graphics PSOs, whether fallback bucket count is reasonable, whether async compile keeps up, and whether `GetCachedBlob` matters.

### Phase 2 validation
- Per-file `clang-cl` validation succeeded for:
  - `source/d3d12/d3d12_async_pipeline.cpp`
  - `source/d3d12/d3d12_device.cpp` with `RESHADE_ADDON=0`


### Phase 2.1 — Path-specific diagnostics
- Added explicit counters for classic `CreateGraphicsPipelineState` requests that ask for `ID3D12PipelineState1` (`graphics_iid1`).
- Added explicit counters for `ID3D12Device2::CreatePipelineState` stream path calls (`stream_create`).
- Added explicit counters for stream path calls that ask for `ID3D12PipelineState1` (`stream_iid1`).
- Updated the shutdown summary line to include `graphics_iid1`, `stream_create`, and `stream_iid1`, so logs now directly show whether the game uses the stream path and whether either creation path requests `ID3D12PipelineState1`.
- Per-file `clang-cl` validation succeeded for `source/d3d12/d3d12_async_pipeline.cpp` and `source/d3d12/d3d12_device.cpp` after these changes.


### Phase 2.2 — Periodic diagnostics visibility
- Confirmed logs are present in the game `ReShade.log`, but only shutdown summaries appeared for early temporary D3D12 devices with all counters at zero.
- Added periodic diagnostics logging so counters are visible while the game is still running, not only when the D3D12 device is destroyed.
- Diagnostics now log on early classic graphics creation calls and early stream creation calls, then every 512 calls after that.
- Reused the same summary formatter for periodic `graphics-create`, periodic `stream-create`, and shutdown `summary` lines.
- Per-file `clang-cl` validation succeeded for `source/d3d12/d3d12_async_pipeline.cpp`.



### Phase 3 — Stream graphics PSO async path
- Real FF7 Rebirth logs showed `graphics_create=0`, `stream_create=16`, `stream_iid1=0`, and `async_proxies=0`, so the game is currently using `ID3D12Device2::CreatePipelineState` pipeline-state streams rather than classic `CreateGraphicsPipelineState`.
- Added `create_async_pipeline_state_stream` to `source/d3d12/d3d12_async_pipeline.hpp/.cpp`.
- Added a stream-to-`D3D12_GRAPHICS_PIPELINE_STATE_DESC` converter for common graphics stream subobjects: root signature, VS/PS/HS/DS/GS bytecode, stream output, blend, sample mask, rasterizer, depth/stencil, input layout, strip-cut, primitive topology, render-target formats, DSV format, sample desc, node mask, cached PSO, flags, and representable `DEPTH_STENCIL1` without depth-bounds testing.
- Unsupported stream subobjects currently force synchronous native creation: compute (`CS`), mesh/amplification (`MS`/`AS`), view instancing, serialized root signatures, `DEPTH_STENCIL2`, and `RASTERIZER1/2`.
- Wired `D3D12Device::CreatePipelineState` so ReShade add-on handling still wins; otherwise supported `ID3D12PipelineState` stream graphics requests go through the async proxy path, while validation calls or unsupported interface requests still call native stream creation.
- Validation: per-file syntax validation succeeded for `source/d3d12/d3d12_async_pipeline.cpp` and `source/d3d12/d3d12_device.cpp` after removing the known bad generated `-fPIC` option and suppressing the unrelated HRESULT narrowing diagnostic.
- Next runtime check: launch FF7 Rebirth and inspect `ReShade.log` for `stream_create > 0` with `async_proxies > 0`, `fallback_buckets > 0`, `queued > 0`, and later `compile_ok > 0` / `resolves_real > 0`. If `sync_desc` rises without proxies, add more detailed unsupported-stream-subobject counters/logging.


### Phase 3.1 — Force fallback-first publishing semantics
- Runtime symptom: the game still showed correct first-time effects and still stuttered, which means real PSOs can be created and published before their first meaningful bind. In that case the proxy architecture is working mechanically, but the fallback is losing the race.
- Changed `D3D12AsyncPipelineProxy` so successful async real PSO creation stores the real PSO as pending instead of immediately swapping `current_native_pso`.
- The proxy now keeps returning the fallback native PSO for at least 8 proxy bind resolves before publishing the pending real PSO to `current_native_pso`.
- `native_for_store` still force-publishes the pending real PSO after waiting, so pipeline-library storage does not store a fallback PSO.
- Validation: per-file `clang-cl` validation succeeded for `source/d3d12/d3d12_async_pipeline.cpp` after removing the known bad generated `-fPIC` option. The only remaining diagnostic was the existing unknown-warning-option warning for `-Wno-changes-meaning`.
- Next runtime check: `resolves_fallback` should now increase before `resolves_real`, and first-time rendering should visibly use fallback for proxied PSOs. If stutter remains, the next likely cause is worker-thread `CreatePipelineState` still blocking inside the driver/global PSO compiler, so add delayed/limited worker compilation or a kill-switch to test compile pacing independently from proxy binding.


### Phase 3.2 — Single fallback PSO and worker compile gating
- Runtime symptom remained: stutter persisted, suggesting that asynchronous worker calls into `CreatePipelineState` can still contend with the game/driver and cause hitches even though binds are fallback-first.
- Changed fallback creation from keyed bucket cache to one device-local global fallback PSO created from the first supported graphics pipeline. All proxies now reuse that same fallback PSO.
- Diagnostics still reuse the existing fallback counters: expected runtime behavior is `fallback_buckets=1`, first request as `fallback_misses=1`, and later requests as `fallback_hits > 0`.
- Changed the worker so it does not start real PSO creation immediately after a proxy is returned. It now waits until that proxy has resolved to fallback at least 8 times before calling the real `CreatePipelineState` / `CreateGraphicsPipelineState`.
- This should test whether the stutter is caused by immediate background driver compilation rather than fallback binding. It also avoids compiling real PSOs that are created but never actually bound.
- Validation: per-file `clang-cl` validation succeeded for `source/d3d12/d3d12_async_pipeline.cpp` after removing the known bad generated `-fPIC` option. The only remaining diagnostic was the existing unknown-warning-option warning for `-Wno-changes-meaning`; VS Code still reports the known generated `-fPIC` problem.
- Next runtime check: confirm `fallback_buckets=1`, `queued` can rise ahead of `compile_ok`, `compile_ok` should only rise after fallback resolves, and first-time rendering should visibly use fallback. If stutter still happens before `compile_ok` rises, fallback PSO creation/binding is the source; if stutter begins when `compile_ok` rises, worker-side driver compilation is still causing cross-thread contention and needs stronger pacing or a manual compile trigger.


### Phase 3.3 — Global fallback needs draw suppression
- Runtime log showed the global fallback path was active: fallback shaders compiled once, `fallback_buckets=1`, `fallback_misses=1`, `fallback_hits` increased, and several async proxies were queued.
- The log did not show any proxy bind resolves before stopping (`proxy_resolves=0`, `resolves_fallback=0`, `resolves_real=0`, `compile_ok=0`), so worker compilation was not yet the observed source in that run.
- A single global D3D12 graphics PSO cannot be render-target/root-signature/topology-compatible with every game draw. Binding it and then executing normal draw commands is unsafe. The prototype now treats the global fallback as a sentinel PSO: when a command list resolves an async proxy to fallback, subsequent draw, indexed draw, and execute-indirect calls are suppressed until a real PSO is bound.
- Added bind-time fallback status propagation through `resolve_async_pipeline_state(..., bool *resolved_to_fallback)`.
- Added command-list state tracking for fallback-bound async PSOs in `SetPipelineState`, `Reset`, `ClearState`, and `CreateCommandList` initial-state handling.
- Added a `fallback_draw_skips` diagnostic counter to confirm that fallback-bound draws are actually being skipped.
- Validation: per-file `clang-cl` validation succeeded for `source/d3d12/d3d12_async_pipeline.cpp`, `source/d3d12/d3d12_command_list.cpp`, and `source/d3d12/d3d12_device.cpp` after removing the known bad generated `-fPIC` option. Remaining warnings are pre-existing compiler warnings.
- Next runtime check: if global fallback works as sentinel mode, logs should show `proxy_resolves > 0`, `resolves_fallback > 0`, and `fallback_draw_skips > 0`. If it still fails before these counters rise, the issue is likely returning shared fallback-backed proxies or game-side PSO introspection before bind, not draw execution.


### Phase 3.4 — Revert global fallback PSO
- Removed the single global fallback PSO experiment and restored the original keyed fallback cache logic.
- Fallback PSOs are again cached by `GraphicsFallbackKey` with root signature, primitive topology type, RTV/DSV formats, and depth/stencil enabled bits.
- Kept the newer changes around fallback-first real publishing, worker compile gating, bind-time fallback status propagation, and fallback draw-skip diagnostics.
- Validation: per-file `clang-cl` validation succeeded for `source/d3d12/d3d12_async_pipeline.cpp`, `source/d3d12/d3d12_command_list.cpp`, and `source/d3d12/d3d12_device.cpp` after removing the known bad generated `-fPIC` option. Remaining warnings are pre-existing compiler warnings.
- Next runtime check: logs should return to multiple fallback buckets if the game uses multiple incompatible render signatures. `fallback_draw_skips` may still show whether fallback-bound work is being intentionally suppressed before real PSO publication.


### Phase 3.5 — Avoid pipeline-library pollution from async proxies
- Runtime D3D12 debug warnings showed `ID3D12PipelineLibrary::Load*Pipeline` invalid-desc warnings and `StorePipeline` duplicate-name warnings. This likely means a serialized pipeline library has entries stored under names whose PSO description no longer matches what the game requests.
- Async proxies should not be stored into the game's `ID3D12PipelineLibrary`, because storing fallback/prototype or delayed real variants can poison future library loads and produce `LOADPIPELINE_INVALIDDESC` / `STOREPIPELINE_DUPLICATENAME` warnings.
- Added async proxy lookup helpers for pipeline-library detection.
- Changed `D3D12PipelineLibrary::StorePipeline` to detect async PSO proxies, skip native library storage, log one info message, and return `S_OK`.
- Existing invalid-desc warnings may persist until the already-polluted game/driver pipeline cache is cleared, because the bad entries are in the serialized library blob before ReShade sees a new `StorePipeline` call.
- Validation: per-file `clang-cl` validation succeeded for `source/d3d12/d3d12_async_pipeline.cpp`, `source/d3d12/d3d12_pipeline_library.cpp`, `source/d3d12/d3d12_command_list.cpp`, and `source/d3d12/d3d12_device.cpp` after removing the known bad generated `-fPIC` option. Remaining warnings are pre-existing compiler warnings.


### Phase 3.6 — Test fallback-bound draws without suppression
- Runtime result with draw suppression: the system works mechanically, effects/character draws are skipped while fallback is bound, and stutters are massively reduced.
- Hypothesis: draw suppression should not be fundamentally required now that fallback PSOs are keyed by root signature, topology, RTV/DSV formats, and depth/stencil enabled bits again. The no-op fallback shaders should allow fallback-bound draws to execute safely enough for testing, while still avoiding real PSO compile on the recording thread.
- Disabled fallback-bound suppression in `D3D12GraphicsCommandList::DrawInstanced`, `DrawIndexedInstanced`, `Dispatch`, and `ExecuteIndirect`; these commands now forward normally even when `_async_pipeline_state_is_fallback` is true.
- Kept bind-time fallback tracking and diagnostics intact. Expected runtime check: `resolves_fallback` should still rise, but `fallback_draw_skips` should remain zero. If rendering stays stable and stutters remain reduced, draw suppression is unnecessary and can be removed permanently. If corruption/device removal appears, reintroduce suppression only for incompatible fallback cases or tighten the fallback key/state mirroring.


### Phase 3.7 — Compute-only fallback test
- Changed the prototype to limit async fallback/proxy creation to compute PSOs only.
- Added compile-time toggles in `source/d3d12/d3d12_async_pipeline.cpp`: `async_graphics_fallback_enabled = false` and `async_compute_fallback_enabled = true`.
- Classic `CreateGraphicsPipelineState` now logs that graphics async fallback is disabled for this compute-only test and forwards to native synchronous graphics PSO creation.
- `ID3D12Device2::CreatePipelineState` stream handling now attempts compute stream conversion first. Supported compute streams still use compute fallback proxies; graphics/non-compute streams are forwarded to native synchronous stream creation.
- Expected runtime check: `compute_create` and/or `stream_create` may rise, but `async_proxies` should only rise for compute pipeline requests. Graphics effects/character rendering should no longer be affected by fallback graphics PSOs.


### Phase 3.8 — Restore fallback command suppression
- Restored fallback-bound suppression in `D3D12GraphicsCommandList::DrawInstanced`, `DrawIndexedInstanced`, `Dispatch`, and `ExecuteIndirect`.
- While `_async_pipeline_state_is_fallback` is true, these calls now increment `fallback_draw_skips` and return before forwarding to the native command list or ReShade add-on events.
- This puts the runtime back in the stable mode that skips work recorded under fallback PSOs.


### Phase 4.0 — Optimization and trimming pass
- Kept fallback-bound draw/dispatch/indirect suppression enabled.
- Added `async_pipeline_debug_diagnostics` in `source/d3d12/d3d12_async_pipeline.cpp`. Heavy periodic diagnostic tables, unsupported-desc reason logs, `GetCachedBlob` logs, and fallback bucket creation logs are now debug-gated, while lightweight atomic counters remain active.
- Removed the single-global-fallback behavior and restored keyed fallback cache behavior for graphics and compute fallback PSOs.
- Reworked stream PSO async handling so supported graphics and compute streams are converted to classic descriptors and deep-copied into `CopiedGraphicsPipelineDesc` / `CopiedComputePipelineDesc`. Raw/shallow `CopiedPipelineStateStream` jobs were removed.
- Re-enabled graphics stream async through the deep-copied graphics-desc path. Unsupported/cached stream PSOs still fall back to native synchronous `CreatePipelineState`.
- Removed the 50 ms `wait_until_compile_allowed` timeout. The worker now keeps jobs pending and compiles only jobs whose proxy reached the fallback-bind threshold, so never-bound PSOs do not force delayed compilation or block later eligible jobs.
- Added an async PSO proxy registry and exposed opaque helper functions for faster hot-path proxy lookup. `resolve_async_pipeline_state` no longer uses COM `QueryInterface` on the bind path.
- Added a command-list `SetPipelineState` cache for the last input PSO and cached async proxy. Rebinding the same proxy avoids another registry lookup while still loading the proxy's current native PSO so real publication is observed.
- Added embedded fallback shader bytecode in `source/d3d12/d3d12_async_pipeline_shaders.hpp` and replaced runtime `D3DCompile` fallback shader creation with static bytecode references.
- Simplified pipeline-library store resolution now that `D3D12PipelineLibrary::StorePipeline` skips async proxies before native storage.
- Added async pageable proxy resolution to `D3D12Device::SetResidencyPriority`, matching `MakeResident`, `Evict`, and `EnqueueMakeResident`.
- Validation: per-file `clang-cl` validation succeeded for `source/d3d12/d3d12_async_pipeline.cpp`, `source/d3d12/d3d12_command_list.cpp`, `source/d3d12/d3d12_device.cpp`, and `source/d3d12/d3d12_pipeline_library.cpp` after removing the known generated `-fPIC` option and suppressing the existing HRESULT narrowing diagnostic. Remaining warnings are pre-existing project warnings.


### Phase 4.1 — Restore single fallback PSO mode
- Kept unique async proxy objects per original game PSO request, preserving game-visible PSO identity and per-proxy real PSO state.
- Restored `use_single_global_fallback_pso = true` so fallback creation collapses to the first graphics fallback bucket and first compute fallback bucket for now.
- This means all graphics proxies share one native graphics fallback PSO, and all compute proxies share one native compute fallback PSO. A single native PSO cannot be shared across graphics and compute binds, so these remain separate by D3D12 pipeline type.
- Draw/dispatch/indirect suppression remains enabled while fallback is bound, which is the safety mechanism for global fallback mode.


### Phase 4.2 — Sentinel fallback hardening and trim
- Renamed the active global fallback mode in code to `use_global_sentinel_fallback_pso` to make the current architecture explicit: unique game-visible proxies remain, while native fallback PSOs are shared by pipeline type and protected by fallback-bound command suppression.
- Moved proxy `SetName`, `SetPrivateData`, `SetPrivateDataInterface`, and `GetPrivateData` handling onto `D3D12AsyncPipelineProxy` itself instead of forwarding to the shared fallback PSO. This prevents metadata/name leakage between unrelated proxies using the same sentinel fallback.
- Added metadata replay onto the real native PSO when it is published, so game-set proxy metadata survives async compilation.
- Improved pipeline-library storage: proxied PSOs still never store fallback variants, but if the real native PSO is already published, `StorePipeline` stores that real PSO instead of always skipping.
- Added deferred `StorePipeline` replay: if a pipeline library store is requested before the real PSO exists, the proxy keeps an AddRef'd pipeline-library reference and copied name, then replays the store with the real native PSO after async creation succeeds.
- Removed the now-unused pipeline-library store resolver helper and obsolete per-proxy compile completion condition-variable state.
- Removed the obsolete embedded-shader compile/load diagnostic, since fallback shaders are now static bytecode and do not need a runtime counter.
- Removed the unused async proxy boolean helper from the public async pipeline header; callers now use the AddRef-returning proxy lookup helper directly.


### Phase 4.3 — Just-in-time fallback promotion before skips
- Added command-list fallback promotion before `DrawInstanced`, `DrawIndexedInstanced`, `Dispatch`, and `ExecuteIndirect` skip paths.
- If a command list is marked fallback-bound but still has a cached async proxy, it now checks whether that proxy has published the real native PSO before skipping. If real is available, the command list binds the real PSO immediately and executes the command instead of skipping it.
- `Reset`, `ClearState`, and `CreateCommandList` initial-state paths now preserve the async proxy cache too, so fallback states set outside explicit `SetPipelineState` can still be promoted later.
- Added `fallback_promote` diagnostics to distinguish skipped work from work rescued by late real-PSO publication.


### Phase 4.4 — Eager real PSO compilation/publishing
- Changed real PSO compile gating from first fallback bind to eager compilation by setting `min_fallback_binds_before_compile = 0`.
- Changed real PSO publish gating to eager publish by setting `min_fallback_binds_before_real = 0`, so PSOs compiled during loading screens can be real before first gameplay bind.
- Fallback/suppression remains available only for PSOs whose real native creation is still pending at bind/draw time.


### Phase 4.5 — Worker wakeup trim
- Reduced redundant compile-worker wakeups during PSO creation bursts.
- Queue producers now call `notify_one` only when pushing into an empty compile queue, since condition-variable notifications are not counted and extra notifications while the worker is already active do not add useful work.


### Phase 4.6 — Async residency replay
- Added async proxy residency intent tracking for `MakeResident`, `EnqueueMakeResident`, `Evict`, and `SetResidencyPriority` calls that target a proxy PSO.
- If a proxy is made resident while it still resolves to fallback, the proxy now remembers that intent and replays `MakeResident` onto the real native PSO after async compilation succeeds.
- `SetResidencyPriority` is also remembered and replayed onto the real native PSO, so priority changes made before real PSO publication are not lost.
- Added a separate manager-owned residency worker queue so residency replay does not run on the compile worker and does not block real PSO creation jobs.
- `Evict` currently clears the remembered make-resident intent only; it deliberately does not auto-evict the shared fallback or later real PSO yet, because fallback eviction policy is ambiguous with global sentinel fallback sharing.
- Validation: per-file syntax validation succeeded for `source/d3d12/d3d12_async_pipeline.cpp` and `source/d3d12/d3d12_device.cpp` after removing the known generated `-fPIC` option. Remaining diagnostics are pre-existing warnings.


### Phase 4.7 — Two PSO compile workers
- Changed async real PSO compilation from one worker thread to two worker threads using the same manager-owned compile queue.
- Each worker pops one eligible `CompileJob` under the queue mutex, then releases the lock before calling the native D3D12 PSO creation API, so queue contention should remain tiny compared to driver compile time.
- Queue producers now notify one worker for every queued compile job instead of only on empty-to-non-empty transitions, so the second worker can wake during creation bursts.
- Kept the separate residency worker unchanged; residency replay remains isolated from compile work.
- Validation: per-file syntax validation succeeded for `source/d3d12/d3d12_async_pipeline.cpp` after removing the known generated `-fPIC` option. The only remaining diagnostic was the existing unknown-warning-option warning for `-Wno-changes-meaning`.


### Phase 4.8 — Worker-side add-on pipeline init
- Gave `D3D12AsyncPipelineManager` access to the `D3D12Device` wrapper in addition to the native `ID3D12Device`.
- After a worker successfully creates a real graphics or compute PSO, it now invokes the existing `invoke_create_and_init_pipeline_event(..., with_create_pipeline = false)` helper before publishing the real PSO to the proxy.
- This reuses the existing add-on path for `init_pipeline` and `destroy_pipeline` registration without firing another `create_pipeline` event or creating the PSO a second time.
- Real PSO publication still happens after add-on init handling completes, so later binds/promotions cannot expose the real PSO before `init_pipeline` has run.
- Validation: per-file syntax validation succeeded for `source/d3d12/d3d12_async_pipeline.cpp` and `source/d3d12/d3d12_device.cpp` after removing the known generated `-fPIC` option. Remaining warnings are pre-existing project warnings.


### Phase 4.9 — Avoid sync PSO creation for init-only add-ons
- Fixed a remaining add-on path that could reintroduce synchronous PSO creation: `D3D12Device::CreateGraphicsPipelineState`, `CreateComputePipelineState`, and `CreatePipelineState` now only call the synchronous `invoke_create_and_init_pipeline_event(..., with_create_pipeline = true)` path when an add-on actually registers `create_pipeline`.
- Add-ons that only listen to `init_pipeline` / `destroy_pipeline` now stay on the async path; the worker invokes init/destroy handling after the real PSO is compiled.
- This keeps `create_pipeline` override semantics intact while avoiding the old sync fallback for common lifecycle-only add-ons.
- Validation: per-file syntax validation succeeded for `source/d3d12/d3d12_device.cpp` after removing the known generated `-fPIC` option. Remaining warnings are pre-existing project warnings.


### Phase 4.10 — Keep async for `create_pipeline` add-ons too
- Removed the remaining synchronous add-on gate from `D3D12Device::CreateGraphicsPipelineState`, `CreateComputePipelineState`, and `CreatePipelineState`. Supported graphics/compute PSO requests now still return async proxy PSOs even when an add-on registers `create_pipeline`.
- Moved `create_pipeline` handling to the compile worker by calling `invoke_create_and_init_pipeline_event(..., with_create_pipeline = true)` from `D3D12AsyncPipelineManager::worker_loop` before publishing the real PSO.
- This preserves add-on `create_pipeline` mutation/override semantics, plus `init_pipeline` and `destroy_pipeline` registration, but shifts the expensive real PSO creation off the app thread.
- The worker only falls back to direct native PSO creation when no add-on pipeline events are registered, avoiding unnecessary helper overhead in the no-add-on path.
- Validation: per-file syntax validation succeeded for `source/d3d12/d3d12_device.cpp` and `source/d3d12/d3d12_async_pipeline.cpp` after removing the known generated `-fPIC`/`-Werror` options and suppressing the existing HRESULT narrowing diagnostic. Remaining warnings are pre-existing project warnings.
