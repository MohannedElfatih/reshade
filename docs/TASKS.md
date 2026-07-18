# Tasks

## Now

- Validate `[ASYNC] WaitForCachedBlob=1` in target games and inspect cached-blob wait diagnostics.

## Next

- Decide whether executable keyed mesh fallback PSOs are worth implementing after sentinel command suppression is enforced.

## Later

- Complete the remaining D3D12 asynchronous pipeline release gates in [plan.md](../source/d3d12/plan.md).

## Done

- 2026-07-18 — Added default real-PSO waiting for `GetCachedBlob`, terminal wakeups, INI control, and wait-duration diagnostics.
- 2026-07-18 — Made detailed async diagnostics atomic writes debug-only and added a dedicated no-output mesh sentinel PSO.
