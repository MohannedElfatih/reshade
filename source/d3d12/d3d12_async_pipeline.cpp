/*
 * Copyright (C) 2014 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause OR MIT
 */

#include "d3d12_async_pipeline.hpp"
#include "d3d12_async_pipeline_shaders.hpp"
#include "d3d12_device.hpp"
#include "d3d12_impl_type_convert.hpp"
#include "com_ptr.hpp"
#include "com_utils.hpp"
#include "dll_log.hpp"
#include "ini_file.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// ReShade.ini [ASYNC] settings:
//   EnableMiniDump=0  Write an exception minidump beside ReShade.ini (handled in 'dll_main.cpp').
//   Debug=0           Enable detailed async-PSO diagnostic logging.
//   Sentinel=1        Use a shared sentinel fallback PSO; set to 0 for keyed fallback PSOs (Unique fallbacks based on root signature, slower but could be more stable).
//   FallbackMode=0    Skip pending draw/dispatch/indirect commands; set to 1 to execute fallback shaders (Slower but might be more stable).
//   CompilePacing=1   Limit PSO compilation workers when the rolling frame time is high.
//   WaitForCachedBlob=1 Wait for the real PSO before returning cached pipeline data (Should always be on, but can be disabled for testing).


/* e.g:
[ASYNC]
EnableMiniDump=0
Debug=0
Sentinel=1
FallbackMode=0
CompilePacing=1
WaitForCachedBlob=1
*/

struct AsyncPipelineDiagnostics
{
	std::atomic<uint64_t> graphics_create_calls = 0;
	std::atomic<uint64_t> graphics_pipeline_state1_requests = 0;
	std::atomic<uint64_t> compute_create_calls = 0;
	std::atomic<uint64_t> compute_pipeline_state1_requests = 0;
	std::atomic<uint64_t> stream_create_calls = 0;
	std::atomic<uint64_t> stream_pipeline_state1_requests = 0;
	std::atomic<uint64_t> async_proxy_creates = 0;
	std::atomic<uint64_t> sync_unsupported_interface = 0;
	std::atomic<uint64_t> sync_unsupported_desc = 0;
	std::atomic<uint64_t> sync_fallback_failure = 0;
	std::atomic<uint64_t> fallback_cache_hits = 0;
	std::atomic<uint64_t> fallback_cache_misses = 0;
	std::atomic<uint64_t> fallback_buckets_created = 0;
	std::atomic<uint64_t> fallback_creation_failures = 0;
	std::atomic<uint64_t> queued_jobs = 0;
	std::atomic<uint64_t> queue_depth_high_watermark = 0;
	std::atomic<uint64_t> async_compile_successes = 0;
	std::atomic<uint64_t> async_compile_failures = 0;
	std::atomic<uint64_t> async_compile_total_us = 0;
	std::atomic<uint64_t> async_compile_max_us = 0;
	std::atomic<uint64_t> proxy_resolves = 0;
	std::atomic<uint64_t> proxy_resolves_to_fallback = 0;
	std::atomic<uint64_t> proxy_resolves_to_real = 0;
	std::atomic<uint64_t> pageable_proxy_resolves = 0;
	std::atomic<uint64_t> get_cached_blob_calls = 0;
	std::atomic<uint64_t> get_cached_blob_waits = 0;
	std::atomic<uint64_t> get_cached_blob_wait_total_us = 0;
	std::atomic<uint64_t> get_cached_blob_wait_max_us = 0;
	std::atomic<uint64_t> fallback_draw_skips = 0;
	std::atomic<uint64_t> fallback_promotions = 0;
	std::atomic<uint64_t> deferred_store_queued = 0;
	std::atomic<uint64_t> deferred_store_replayed = 0;
};

static AsyncPipelineDiagnostics g_async_pipeline_diagnostics;

enum class AsyncPipelineFallbackMode : unsigned int
{
	skip_commands = 0,
	execute_fallback = 1,
};

static AsyncPipelineFallbackMode async_pipeline_fallback_mode = AsyncPipelineFallbackMode::skip_commands;
static bool use_global_sentinel_fallback_pso = true;
static bool async_pipeline_debug_diagnostics = false;
static bool async_pipeline_compile_pacing = true;
static bool async_pipeline_wait_for_cached_blob = true;
static std::once_flag async_pipeline_config_once;
static constexpr bool publish_fallback_pipelines_to_addons = false;
static constexpr bool allow_addons_to_modify_fallback_pipelines = false;
static constexpr bool async_graphics_fallback_enabled = true;
static constexpr bool async_compute_fallback_enabled = true;
static constexpr uint64_t min_fallback_binds_before_real = 0;
static constexpr uint64_t min_fallback_binds_before_compile = 0;
static constexpr unsigned int async_pipeline_compile_worker_thread_percentage = 75;
static constexpr unsigned int async_pipeline_compile_middle_thread_percentage = 50;
static constexpr unsigned int async_pipeline_compile_lower_thread_percentage = 25;
static constexpr uint64_t async_pipeline_pacing_control_interval_us = 250'000;
static constexpr uint32_t async_pipeline_pacing_confirmation_samples = 2;

static constexpr bool async_pipeline_enabled = async_graphics_fallback_enabled || async_compute_fallback_enabled;

static const char *get_async_pipeline_fallback_mode_name()
{
	return async_pipeline_fallback_mode == AsyncPipelineFallbackMode::skip_commands ? "skip" : "execute";
}

static void load_async_pipeline_config()
{
	std::call_once(async_pipeline_config_once, []() {
		unsigned int fallback_mode = static_cast<unsigned int>(AsyncPipelineFallbackMode::skip_commands);
		reshade::global_config().get("ASYNC", "FallbackMode", fallback_mode);
		async_pipeline_fallback_mode = fallback_mode == static_cast<unsigned int>(AsyncPipelineFallbackMode::execute_fallback) ? AsyncPipelineFallbackMode::execute_fallback : AsyncPipelineFallbackMode::skip_commands;

		reshade::global_config().get("ASYNC", "Sentinel", use_global_sentinel_fallback_pso);
		async_pipeline_debug_diagnostics = reshade::global_config().get("ASYNC", "Debug");
		reshade::global_config().get("ASYNC", "CompilePacing", async_pipeline_compile_pacing);
		reshade::global_config().get("ASYNC", "WaitForCachedBlob", async_pipeline_wait_for_cached_blob);
	});
}

static uint32_t get_async_pipeline_hardware_thread_count()
{
	const unsigned int hardware_threads = std::thread::hardware_concurrency();
	return hardware_threads != 0 ? hardware_threads : 1;
}

static uint32_t get_async_pipeline_compile_worker_count(uint32_t hardware_thread_count, unsigned int percentage)
{
	return std::max<uint32_t>(1, (hardware_thread_count * percentage) / 100);
}

class D3D12AsyncPipelineProxy;
class D3D12AsyncPipelineManager;
static void register_async_pipeline_proxy(ID3D12PipelineState *pipeline_state, D3D12AsyncPipelineProxy *proxy);
static void unregister_async_pipeline_proxy(ID3D12PipelineState *pipeline_state);
static D3D12AsyncPipelineProxy *try_addref_async_pipeline_proxy(ID3D12PipelineState *pipeline_state);
static void enqueue_async_residency_make_resident(D3D12AsyncPipelineManager *manager, ID3D12Pageable *pageable);
static void enqueue_async_residency_priority(D3D12AsyncPipelineManager *manager, ID3D12Pageable *pageable, D3D12_RESIDENCY_PRIORITY priority);
static std::mutex g_async_pipeline_proxy_registry_mutex;
static std::unordered_map<ID3D12PipelineState *, D3D12AsyncPipelineProxy *> g_async_pipeline_proxy_registry;
static std::condition_variable g_async_pipeline_compile_gate_cv;

static void update_atomic_max(std::atomic<uint64_t> &target, uint64_t value)
{
	if (!async_pipeline_debug_diagnostics)
		return;

	uint64_t current = target.load(std::memory_order_relaxed);
	while (current < value && !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {}
}

static uint64_t increment_async_pipeline_diagnostic(std::atomic<uint64_t> &target, uint64_t value = 1)
{
	if (!async_pipeline_debug_diagnostics)
		return 0;
	return target.fetch_add(value, std::memory_order_relaxed) + value;
}

static bool should_log_periodic(uint64_t count)
{
	if (!async_pipeline_debug_diagnostics)
		return false;
	return count <= 16 || (count % 512) == 0;
}

static void log_async_pipeline_diagnostics_row(const char *name_a, uint64_t value_a, const char *name_b, uint64_t value_b)
{
	reshade::log::message(reshade::log::level::info, "[ASYNC]   | %-24s | %-16llu | %-24s | %-16llu |",
		name_a,
		static_cast<unsigned long long>(value_a),
		name_b,
		static_cast<unsigned long long>(value_b));
}

static void log_async_pipeline_diagnostics_timing_row(const char *name_a, double value_a, const char *name_b, double value_b)
{
	reshade::log::message(reshade::log::level::info, "[ASYNC]   | %-24s | %-16.3f | %-24s | %-16.3f |", name_a, value_a, name_b, value_b);
}

static void log_async_pipeline_diagnostics(const char *reason)
{
	const uint64_t compile_successes = g_async_pipeline_diagnostics.async_compile_successes.load(std::memory_order_relaxed);
	const uint64_t compile_total_us = g_async_pipeline_diagnostics.async_compile_total_us.load(std::memory_order_relaxed);
	reshade::log::message(reshade::log::level::info, "[ASYNC] Async D3D12 PSO %s diagnostics (sentinel=%u, fallback_mode=%s):", reason, use_global_sentinel_fallback_pso, get_async_pipeline_fallback_mode_name());
	reshade::log::message(reshade::log::level::info, "[ASYNC]   +--------------------------+------------------+--------------------------+------------------+");
	reshade::log::message(reshade::log::level::info, "[ASYNC]   | Counter                  | Value            | Counter                  | Value            |");
	reshade::log::message(reshade::log::level::info, "[ASYNC]   +--------------------------+------------------+--------------------------+------------------+");
	log_async_pipeline_diagnostics_row("graphics_create", g_async_pipeline_diagnostics.graphics_create_calls.load(std::memory_order_relaxed), "graphics_iid1", g_async_pipeline_diagnostics.graphics_pipeline_state1_requests.load(std::memory_order_relaxed));
	log_async_pipeline_diagnostics_row("compute_create", g_async_pipeline_diagnostics.compute_create_calls.load(std::memory_order_relaxed), "compute_iid1", g_async_pipeline_diagnostics.compute_pipeline_state1_requests.load(std::memory_order_relaxed));
	log_async_pipeline_diagnostics_row("stream_create", g_async_pipeline_diagnostics.stream_create_calls.load(std::memory_order_relaxed), "stream_iid1", g_async_pipeline_diagnostics.stream_pipeline_state1_requests.load(std::memory_order_relaxed));
	log_async_pipeline_diagnostics_row("async_proxies", g_async_pipeline_diagnostics.async_proxy_creates.load(std::memory_order_relaxed), "sync_interface", g_async_pipeline_diagnostics.sync_unsupported_interface.load(std::memory_order_relaxed));
	log_async_pipeline_diagnostics_row("sync_desc", g_async_pipeline_diagnostics.sync_unsupported_desc.load(std::memory_order_relaxed), "sync_fallback_fail", g_async_pipeline_diagnostics.sync_fallback_failure.load(std::memory_order_relaxed));
	log_async_pipeline_diagnostics_row("fallback_hits", g_async_pipeline_diagnostics.fallback_cache_hits.load(std::memory_order_relaxed), "fallback_misses", g_async_pipeline_diagnostics.fallback_cache_misses.load(std::memory_order_relaxed));
	log_async_pipeline_diagnostics_row(use_global_sentinel_fallback_pso ? "sentinel_fallbacks" : "fallback_buckets", g_async_pipeline_diagnostics.fallback_buckets_created.load(std::memory_order_relaxed), "fallback_failures", g_async_pipeline_diagnostics.fallback_creation_failures.load(std::memory_order_relaxed));
	log_async_pipeline_diagnostics_row("queued", g_async_pipeline_diagnostics.queued_jobs.load(std::memory_order_relaxed), "queue_hwm", g_async_pipeline_diagnostics.queue_depth_high_watermark.load(std::memory_order_relaxed));
	log_async_pipeline_diagnostics_row("compile_ok", compile_successes, "compile_fail", g_async_pipeline_diagnostics.async_compile_failures.load(std::memory_order_relaxed));
	log_async_pipeline_diagnostics_timing_row("compile_avg_ms", compile_successes != 0 ? static_cast<double>(compile_total_us) / static_cast<double>(compile_successes) / 1000.0 : 0.0, "compile_max_ms", static_cast<double>(g_async_pipeline_diagnostics.async_compile_max_us.load(std::memory_order_relaxed)) / 1000.0);
	log_async_pipeline_diagnostics_row("proxy_resolves", g_async_pipeline_diagnostics.proxy_resolves.load(std::memory_order_relaxed), "resolves_fallback", g_async_pipeline_diagnostics.proxy_resolves_to_fallback.load(std::memory_order_relaxed));
	log_async_pipeline_diagnostics_row("resolves_real", g_async_pipeline_diagnostics.proxy_resolves_to_real.load(std::memory_order_relaxed), "pageable_resolves", g_async_pipeline_diagnostics.pageable_proxy_resolves.load(std::memory_order_relaxed));
	const uint64_t cached_blob_waits = g_async_pipeline_diagnostics.get_cached_blob_waits.load(std::memory_order_relaxed);
	const uint64_t cached_blob_wait_total_us = g_async_pipeline_diagnostics.get_cached_blob_wait_total_us.load(std::memory_order_relaxed);
	log_async_pipeline_diagnostics_row("get_cached_blob", g_async_pipeline_diagnostics.get_cached_blob_calls.load(std::memory_order_relaxed), "cached_blob_waits", cached_blob_waits);
	log_async_pipeline_diagnostics_timing_row("cached_wait_avg_ms", cached_blob_waits != 0 ? static_cast<double>(cached_blob_wait_total_us) / static_cast<double>(cached_blob_waits) / 1000.0 : 0.0, "cached_wait_max_ms", static_cast<double>(g_async_pipeline_diagnostics.get_cached_blob_wait_max_us.load(std::memory_order_relaxed)) / 1000.0);
	log_async_pipeline_diagnostics_row("fallback_draw_skips", g_async_pipeline_diagnostics.fallback_draw_skips.load(std::memory_order_relaxed), "fallback_promote", g_async_pipeline_diagnostics.fallback_promotions.load(std::memory_order_relaxed));
	log_async_pipeline_diagnostics_row("store_deferred", g_async_pipeline_diagnostics.deferred_store_queued.load(std::memory_order_relaxed), "store_replayed", g_async_pipeline_diagnostics.deferred_store_replayed.load(std::memory_order_relaxed));
	reshade::log::message(reshade::log::level::info, "[ASYNC]   +--------------------------+------------------+--------------------------+------------------+");
}

static void note_async_pipeline_unsupported_desc(const char *reason)
{
	const uint64_t count = increment_async_pipeline_diagnostic(g_async_pipeline_diagnostics.sync_unsupported_desc, 1);
	if (async_pipeline_debug_diagnostics)
		if (should_log_periodic(count))
		reshade::log::message(reshade::log::level::info, "[ASYNC] Async D3D12 PSO: synchronous creation because desc is unsupported: %s (sync_desc=%llu).", reason, static_cast<unsigned long long>(count));
}

struct AsyncPrivateDataGuidHash
{
	size_t operator()(REFGUID guid) const
	{
		const uint64_t *const words = reinterpret_cast<const uint64_t *>(&guid);
		return static_cast<size_t>(words[0] ^ (words[1] + 0x9e3779b97f4a7c15ull + (words[0] << 6) + (words[0] >> 2)));
	}
};

struct AsyncPrivateDataGuidEqual
{
	bool operator()(REFGUID lhs, REFGUID rhs) const
	{
		return std::memcmp(&lhs, &rhs, sizeof(GUID)) == 0;
	}
};

struct AsyncPrivateDataValue
{
	std::vector<uint8_t> data;
	com_ptr<IUnknown> object;
	bool is_object = false;
};

struct AsyncDeferredPipelineStore
{
	com_ptr<ID3D12PipelineLibrary> library;
	std::wstring name;
	bool has_name = false;
};

class DECLSPEC_UUID("8F4A52A3-B932-4FE2-9F6F-18D62D98D6D4") D3D12AsyncPipelineProxy final : public ID3D12PipelineState
{
public:
	D3D12AsyncPipelineProxy(D3D12AsyncPipelineManager *manager, ID3D12Device *device, ID3D12PipelineState *fallback, uint64_t id) :
		_manager(manager), _device(device), _fallback(fallback), _current(fallback), _id(id)
	{
		assert(_manager != nullptr && _device != nullptr && _fallback != nullptr);
		_device->AddRef();
		register_async_pipeline_proxy(static_cast<ID3D12PipelineState *>(this), this);
	}
	~D3D12AsyncPipelineProxy()
	{
		unregister_async_pipeline_proxy(static_cast<ID3D12PipelineState *>(this));
		_device->Release();
	}

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObj) override
	{
		if (ppvObj == nullptr)
			return E_POINTER;

		if (riid == __uuidof(D3D12AsyncPipelineProxy) ||
			riid == __uuidof(IUnknown) ||
			riid == __uuidof(ID3D12Object) ||
			riid == __uuidof(ID3D12DeviceChild) ||
			riid == __uuidof(ID3D12Pageable) ||
			riid == __uuidof(ID3D12PipelineState))
		{
			*ppvObj = static_cast<ID3D12PipelineState *>(this);
			AddRef();
			return S_OK;
		}
		if (riid == IID_UnwrappedObject)
		{
			ID3D12PipelineState *const current = current_native();
			current->AddRef();
			*ppvObj = current;
			return S_OK;
		}

		*ppvObj = nullptr;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&_ref); }
	ULONG STDMETHODCALLTYPE Release() override
	{
		const ULONG ref = InterlockedDecrement(&_ref);
		if (ref == 0)
			delete this;
		return ref;
	}

	HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT *pDataSize, void *pData) override
	{
		if (pDataSize == nullptr)
			return E_INVALIDARG;

		std::lock_guard<std::mutex> lock(_metadata_mutex);
		const auto it = _private_data.find(guid);
		if (it == _private_data.end())
		{
			*pDataSize = 0;
			return DXGI_ERROR_NOT_FOUND;
		}

		const AsyncPrivateDataValue &value = it->second;
		const UINT required_size = value.is_object ? static_cast<UINT>(sizeof(IUnknown *)) : static_cast<UINT>(value.data.size());
		if (pData == nullptr || *pDataSize < required_size)
		{
			*pDataSize = required_size;
			return DXGI_ERROR_MORE_DATA;
		}

		*pDataSize = required_size;
		if (value.is_object)
		{
			IUnknown *object = value.object.get();
			if (object != nullptr)
				object->AddRef();
			std::memcpy(pData, &object, sizeof(IUnknown *));
		}
		else if (required_size != 0)
		{
			std::memcpy(pData, value.data.data(), required_size);
		}
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void *pData) override
	{
		if (DataSize != 0 && pData == nullptr)
			return E_INVALIDARG;

		std::lock_guard<std::mutex> lock(_metadata_mutex);
		if (DataSize == 0)
		{
			_private_data.erase(guid);
			return S_OK;
		}

		AsyncPrivateDataValue &value = _private_data[guid];
		value.is_object = false;
		value.object.reset();
		value.data.resize(DataSize);
		std::memcpy(value.data.data(), pData, DataSize);
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown *pData) override
	{
		std::lock_guard<std::mutex> lock(_metadata_mutex);
		if (pData == nullptr)
		{
			_private_data.erase(guid);
			return S_OK;
		}

		AsyncPrivateDataValue &value = _private_data[guid];
		value.is_object = true;
		value.data.clear();
		value.object.reset(const_cast<IUnknown *>(pData));
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE SetName(LPCWSTR Name) override
	{
		std::lock_guard<std::mutex> lock(_metadata_mutex);
		_name = Name != nullptr ? Name : L"";
		_has_name = Name != nullptr;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **ppvDevice) override { return _device->QueryInterface(riid, ppvDevice); }
	HRESULT STDMETHODCALLTYPE GetCachedBlob(ID3DBlob **ppBlob) override
	{
		increment_async_pipeline_diagnostic(g_async_pipeline_diagnostics.get_cached_blob_calls, 1);

		if (!async_pipeline_wait_for_cached_blob)
		{
			ID3D12PipelineState *const native = current_native();
			if (async_pipeline_debug_diagnostics)
				reshade::log::message(reshade::log::level::info, "[ASYNC] Async D3D12 PSO proxy %llu: GetCachedBlob wait disabled; forwarding to current %s PSO.", static_cast<unsigned long long>(_id), native == _published_real.load(std::memory_order_acquire) ? "real" : "fallback");
			return native->GetCachedBlob(ppBlob);
		}

		com_ptr<ID3D12PipelineState> real;
		HRESULT compile_result = E_FAIL;
		const auto wait_start = std::chrono::steady_clock::now();
		bool waited = false;
		{
			std::unique_lock<std::mutex> lock(_real_mutex);
			waited = !_compile_finished;
			_real_cv.wait(lock, [this]() { return _compile_finished; });
			compile_result = _compile_result;
			if (SUCCEEDED(compile_result))
			{
				ID3D12PipelineState *const available_real = _published_real.load(std::memory_order_relaxed) != nullptr ? _published_real.load(std::memory_order_relaxed) : _pending_real.get();
				if (available_real != nullptr)
					real = available_real;
				else
					compile_result = E_FAIL;
			}
		}

		if (waited)
		{
			const uint64_t wait_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - wait_start).count());
			increment_async_pipeline_diagnostic(g_async_pipeline_diagnostics.get_cached_blob_waits, 1);
			increment_async_pipeline_diagnostic(g_async_pipeline_diagnostics.get_cached_blob_wait_total_us, wait_us);
			update_atomic_max(g_async_pipeline_diagnostics.get_cached_blob_wait_max_us, wait_us);
			if (async_pipeline_debug_diagnostics)
				reshade::log::message(reshade::log::level::info, "[ASYNC] Async D3D12 PSO proxy %llu: GetCachedBlob waited %.3f ms for real PSO creation.", static_cast<unsigned long long>(_id), static_cast<double>(wait_us) / 1000.0);
		}

		if (FAILED(compile_result))
		{
			if (ppBlob != nullptr)
				*ppBlob = nullptr;
			return compile_result;
		}
		return real->GetCachedBlob(ppBlob);
	}

	ID3D12PipelineState *current_native() const { return _current.load(std::memory_order_acquire); }
	HRESULT store_or_defer(ID3D12PipelineLibrary *library, LPCWSTR name)
	{
		if (library == nullptr)
			return E_POINTER;

		if (ID3D12PipelineState *const real = real_for_store_addref())
		{
			const HRESULT hr = library->StorePipeline(name, real);
			real->Release();
			return hr;
		}

		{
			std::lock_guard<std::mutex> lock(_deferred_store_mutex);
			if (ID3D12PipelineState *const real = real_for_store_addref())
			{
				const HRESULT hr = library->StorePipeline(name, real);
				real->Release();
				return hr;
			}

			AsyncDeferredPipelineStore store;
			store.library = library;
			store.has_name = name != nullptr;
			if (store.has_name)
				store.name = name;
			_deferred_stores.push_back(std::move(store));
		}

		increment_async_pipeline_diagnostic(g_async_pipeline_diagnostics.deferred_store_queued, 1);
		return S_OK;
	}
	ID3D12PipelineState *current_native_for_bind(bool *resolved_to_fallback = nullptr)
	{
		if (resolved_to_fallback != nullptr)
			*resolved_to_fallback = false;

		ID3D12PipelineState *const native = current_native();
		increment_async_pipeline_diagnostic(g_async_pipeline_diagnostics.proxy_resolves, 1);
		if (ID3D12PipelineState *const real = _published_real.load(std::memory_order_acquire); real != nullptr && native == real)
		{
			if (async_pipeline_debug_diagnostics)
				_real_bind_count.fetch_add(1, std::memory_order_relaxed);
			increment_async_pipeline_diagnostic(g_async_pipeline_diagnostics.proxy_resolves_to_real, 1);
		}
		else
		{
			if (resolved_to_fallback != nullptr)
				*resolved_to_fallback = true;
			uint64_t fallback_bind_count = 0;
			if (async_pipeline_debug_diagnostics || min_fallback_binds_before_compile != 0 || min_fallback_binds_before_real != 0)
				fallback_bind_count = _fallback_bind_count.fetch_add(1, std::memory_order_relaxed) + 1;
			increment_async_pipeline_diagnostic(g_async_pipeline_diagnostics.proxy_resolves_to_fallback, 1);
			if constexpr (min_fallback_binds_before_compile != 0)
				if (fallback_bind_count >= min_fallback_binds_before_compile)
					g_async_pipeline_compile_gate_cv.notify_all();
			if constexpr (min_fallback_binds_before_real != 0)
				if (fallback_bind_count >= min_fallback_binds_before_real)
					publish_pending_real();
		}
		return native;
	}
	bool is_compile_allowed() const
	{
		if constexpr (min_fallback_binds_before_compile == 0)
			return true;
		else
			return _fallback_bind_count.load(std::memory_order_relaxed) >= min_fallback_binds_before_compile;
	}
	void set_real(ID3D12PipelineState *real)
	{
		assert(real != nullptr);
		{
			std::lock_guard<std::mutex> lock(_real_mutex);
			_pending_real = real;
			_compile_result = S_OK;
			_compile_finished = true;
		}
		replay_deferred_stores(real);
		replay_residency_to_real(real);
		if constexpr (min_fallback_binds_before_real == 0)
			publish_pending_real();
		else if (_fallback_bind_count.load(std::memory_order_relaxed) >= min_fallback_binds_before_real)
			publish_pending_real();
		_real_cv.notify_all();
	}
	void set_compile_failed(HRESULT result = E_FAIL)
	{
		{
			std::lock_guard<std::mutex> lock(_real_mutex);
			_compile_result = FAILED(result) ? result : E_FAIL;
			_compile_finished = true;
		}
		{
			std::lock_guard<std::mutex> lock(_deferred_store_mutex);
			_deferred_stores.clear();
		}
		_real_cv.notify_all();
	}
	void note_make_resident_requested()
	{
		{
			std::lock_guard<std::mutex> lock(_residency_mutex);
			_make_resident_requested = true;
		}

		if (ID3D12PipelineState *const real = real_for_residency_addref())
		{
			enqueue_async_residency_make_resident(_manager, real);
			real->Release();
		}
	}
	void note_evicted()
	{
		std::lock_guard<std::mutex> lock(_residency_mutex);
		_make_resident_requested = false;
	}
	void note_residency_priority(D3D12_RESIDENCY_PRIORITY priority)
	{
		{
			std::lock_guard<std::mutex> lock(_residency_mutex);
			_residency_priority = priority;
			_has_residency_priority = true;
		}

		if (ID3D12PipelineState *const real = real_for_residency_addref())
		{
			enqueue_async_residency_priority(_manager, real, priority);
			real->Release();
		}
	}
private:
	ID3D12PipelineState *real_for_residency_addref() const
	{
		std::lock_guard<std::mutex> lock(_real_mutex);
		ID3D12PipelineState *real = _published_real.load(std::memory_order_relaxed);
		if (real == nullptr)
			real = _pending_real.get();
		if (real != nullptr)
			real->AddRef();
		return real;
	}

	ID3D12PipelineState *real_for_store_addref() const
	{
		std::lock_guard<std::mutex> lock(_real_mutex);
		ID3D12PipelineState *real = _published_real.load(std::memory_order_relaxed);
		if (real == nullptr)
			real = _pending_real.get();
		if (real != nullptr)
			real->AddRef();
		return real;
	}

	void publish_pending_real()
	{
		if (_published_real.load(std::memory_order_acquire) != nullptr)
			return;
		if constexpr (min_fallback_binds_before_real != 0)
			if (_fallback_bind_count.load(std::memory_order_relaxed) < min_fallback_binds_before_real)
				return;

		std::lock_guard<std::mutex> lock(_real_mutex);
		if (_published_real.load(std::memory_order_relaxed) != nullptr || _pending_real.get() == nullptr)
			return;

		_real = _pending_real;
		_pending_real.reset();
		replay_metadata(_real.get());
		_published_real.store(_real.get(), std::memory_order_release);
		_current.store(_real.get(), std::memory_order_release);
	}
	void replay_deferred_stores(ID3D12PipelineState *real)
	{
		assert(real != nullptr);

		std::vector<AsyncDeferredPipelineStore> stores;
		{
			std::lock_guard<std::mutex> lock(_deferred_store_mutex);
			stores.swap(_deferred_stores);
		}

		for (const AsyncDeferredPipelineStore &store : stores)
		{
			const HRESULT hr = store.library->StorePipeline(store.has_name ? store.name.c_str() : nullptr, real);
			if (SUCCEEDED(hr))
			{
				increment_async_pipeline_diagnostic(g_async_pipeline_diagnostics.deferred_store_replayed, 1);
			}
			else if (async_pipeline_debug_diagnostics)
			{
				reshade::log::message(reshade::log::level::warning, "[ASYNC] Async D3D12 PSO proxy %llu: deferred StorePipeline failed with error code %s.", static_cast<unsigned long long>(_id), reshade::log::hr_to_string(hr).c_str());
			}
		}
	}
	void replay_residency_to_real(ID3D12PipelineState *real)
	{
		assert(real != nullptr);

		bool make_resident_requested = false;
		bool has_residency_priority = false;
		D3D12_RESIDENCY_PRIORITY residency_priority = D3D12_RESIDENCY_PRIORITY_NORMAL;
		{
			std::lock_guard<std::mutex> lock(_residency_mutex);
			make_resident_requested = _make_resident_requested;
			has_residency_priority = _has_residency_priority;
			residency_priority = _residency_priority;
		}

		if (make_resident_requested)
			enqueue_async_residency_make_resident(_manager, real);
		if (has_residency_priority)
			enqueue_async_residency_priority(_manager, real, residency_priority);
	}
	void replay_metadata(ID3D12PipelineState *target)
	{
		assert(target != nullptr);

		std::lock_guard<std::mutex> lock(_metadata_mutex);
		if (_has_name)
			target->SetName(_name.c_str());

		for (const auto &entry : _private_data)
		{
			const AsyncPrivateDataValue &value = entry.second;
			if (value.is_object)
				target->SetPrivateDataInterface(entry.first, value.object.get());
			else
				target->SetPrivateData(entry.first, static_cast<UINT>(value.data.size()), value.data.data());
		}
	}

	D3D12AsyncPipelineManager *_manager = nullptr;
	LONG _ref = 1;
	ID3D12Device *_device = nullptr;
	com_ptr<ID3D12PipelineState> _fallback;
	com_ptr<ID3D12PipelineState> _real;
	com_ptr<ID3D12PipelineState> _pending_real;
	std::atomic<ID3D12PipelineState *> _current;
	std::atomic<ID3D12PipelineState *> _published_real = nullptr;
	std::atomic<uint64_t> _fallback_bind_count = 0;
	std::atomic<uint64_t> _real_bind_count = 0;
	uint64_t _id = 0;
	mutable std::mutex _real_mutex;
	std::condition_variable _real_cv;
	HRESULT _compile_result = E_PENDING;
	bool _compile_finished = false;
	mutable std::mutex _metadata_mutex;
	std::wstring _name;
	bool _has_name = false;
	std::unordered_map<GUID, AsyncPrivateDataValue, AsyncPrivateDataGuidHash, AsyncPrivateDataGuidEqual> _private_data;
	std::mutex _deferred_store_mutex;
	std::vector<AsyncDeferredPipelineStore> _deferred_stores;
	std::mutex _residency_mutex;
	bool _make_resident_requested = false;
	bool _has_residency_priority = false;
	D3D12_RESIDENCY_PRIORITY _residency_priority = D3D12_RESIDENCY_PRIORITY_NORMAL;
};

static void register_async_pipeline_proxy(ID3D12PipelineState *pipeline_state, D3D12AsyncPipelineProxy *proxy)
{
	std::lock_guard<std::mutex> lock(g_async_pipeline_proxy_registry_mutex);
	g_async_pipeline_proxy_registry.emplace(pipeline_state, proxy);
}

static void unregister_async_pipeline_proxy(ID3D12PipelineState *pipeline_state)
{
	std::lock_guard<std::mutex> lock(g_async_pipeline_proxy_registry_mutex);
	g_async_pipeline_proxy_registry.erase(pipeline_state);
}

static D3D12AsyncPipelineProxy *try_addref_async_pipeline_proxy(ID3D12PipelineState *pipeline_state)
{
	std::lock_guard<std::mutex> lock(g_async_pipeline_proxy_registry_mutex);
	const auto it = g_async_pipeline_proxy_registry.find(pipeline_state);
	if (it == g_async_pipeline_proxy_registry.end())
		return nullptr;

	it->second->AddRef();
	return it->second;
}

struct GraphicsFallbackKey
{
	ID3D12RootSignature *root_signature = nullptr;
	D3D12_PRIMITIVE_TOPOLOGY_TYPE primitive_topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED;
	UINT num_render_targets = 0;
	DXGI_FORMAT rtv_formats[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
	DXGI_FORMAT dsv_format = DXGI_FORMAT_UNKNOWN;
	bool depth_enabled = false;
	bool stencil_enabled = false;

	bool operator==(const GraphicsFallbackKey &other) const
	{
		return root_signature == other.root_signature &&
			primitive_topology_type == other.primitive_topology_type &&
			num_render_targets == other.num_render_targets &&
			dsv_format == other.dsv_format &&
			depth_enabled == other.depth_enabled &&
			stencil_enabled == other.stencil_enabled &&
			std::memcmp(rtv_formats, other.rtv_formats, sizeof(rtv_formats)) == 0;
	}
};

struct GraphicsFallbackKeyHash
{
	size_t operator()(const GraphicsFallbackKey &key) const
	{
		size_t hash = reinterpret_cast<size_t>(key.root_signature);
		hash ^= static_cast<size_t>(key.primitive_topology_type) + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
		hash ^= static_cast<size_t>(key.num_render_targets) + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
		hash ^= static_cast<size_t>(key.dsv_format) + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
		hash ^= static_cast<size_t>(key.depth_enabled) + (static_cast<size_t>(key.stencil_enabled) << 1);
		for (DXGI_FORMAT format : key.rtv_formats)
			hash ^= static_cast<size_t>(format) + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
		return hash;
	}
};

struct ComputeFallbackKey
{
	ID3D12RootSignature *root_signature = nullptr;

	bool operator==(const ComputeFallbackKey &other) const
	{
		return root_signature == other.root_signature;
	}
};

struct ComputeFallbackKeyHash
{
	size_t operator()(const ComputeFallbackKey &key) const
	{
		return reinterpret_cast<size_t>(key.root_signature);
	}
};

struct CopiedGraphicsPipelineDesc
{
	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
	com_ptr<ID3D12RootSignature> root_signature;
	std::vector<uint8_t> vs, ps, ds, hs, gs, cached_pso;
	std::vector<D3D12_INPUT_ELEMENT_DESC> input_elements;
	std::vector<std::string> semantic_names;
	std::vector<D3D12_SO_DECLARATION_ENTRY> stream_output_entries;
	std::vector<std::string> stream_output_semantic_names;
	std::vector<UINT> stream_output_strides;

	static void copy_bytecode(const D3D12_SHADER_BYTECODE &src, std::vector<uint8_t> &storage, D3D12_SHADER_BYTECODE &dst)
	{
		if (src.pShaderBytecode == nullptr || src.BytecodeLength == 0)
		{
			dst = {};
			return;
		}
		storage.resize(src.BytecodeLength);
		std::memcpy(storage.data(), src.pShaderBytecode, src.BytecodeLength);
		dst = { storage.data(), storage.size() };
	}

	explicit CopiedGraphicsPipelineDesc(const D3D12_GRAPHICS_PIPELINE_STATE_DESC &source)
	{
		desc = source;
		root_signature = source.pRootSignature;
		desc.pRootSignature = root_signature.get();
		copy_bytecode(source.VS, vs, desc.VS);
		copy_bytecode(source.PS, ps, desc.PS);
		copy_bytecode(source.DS, ds, desc.DS);
		copy_bytecode(source.HS, hs, desc.HS);
		copy_bytecode(source.GS, gs, desc.GS);

		if (source.CachedPSO.pCachedBlob != nullptr && source.CachedPSO.CachedBlobSizeInBytes != 0)
		{
			cached_pso.resize(source.CachedPSO.CachedBlobSizeInBytes);
			std::memcpy(cached_pso.data(), source.CachedPSO.pCachedBlob, source.CachedPSO.CachedBlobSizeInBytes);
			desc.CachedPSO = { cached_pso.data(), cached_pso.size() };
		}
		// Classic CreateGraphicsPipelineState cannot reliably consume stream/library cached blobs.
		desc.CachedPSO = {};

		if (source.InputLayout.pInputElementDescs != nullptr && source.InputLayout.NumElements != 0)
			input_elements.assign(source.InputLayout.pInputElementDescs, source.InputLayout.pInputElementDescs + source.InputLayout.NumElements);
		semantic_names.reserve(input_elements.size());
		for (D3D12_INPUT_ELEMENT_DESC &element : input_elements)
		{
			semantic_names.emplace_back(element.SemanticName != nullptr ? element.SemanticName : "");
			element.SemanticName = semantic_names.back().c_str();
		}
		desc.InputLayout = { input_elements.data(), static_cast<UINT>(input_elements.size()) };

		if (source.StreamOutput.pSODeclaration != nullptr && source.StreamOutput.NumEntries != 0)
			stream_output_entries.assign(source.StreamOutput.pSODeclaration, source.StreamOutput.pSODeclaration + source.StreamOutput.NumEntries);
		stream_output_semantic_names.reserve(stream_output_entries.size());
		for (D3D12_SO_DECLARATION_ENTRY &entry : stream_output_entries)
		{
			stream_output_semantic_names.emplace_back(entry.SemanticName != nullptr ? entry.SemanticName : "");
			entry.SemanticName = stream_output_semantic_names.back().c_str();
		}
		if (source.StreamOutput.pBufferStrides != nullptr && source.StreamOutput.NumStrides != 0)
			stream_output_strides.assign(source.StreamOutput.pBufferStrides, source.StreamOutput.pBufferStrides + source.StreamOutput.NumStrides);
		desc.StreamOutput = {
			stream_output_entries.empty() ? nullptr : stream_output_entries.data(),
			static_cast<UINT>(stream_output_entries.size()),
			stream_output_strides.empty() ? nullptr : stream_output_strides.data(),
			static_cast<UINT>(stream_output_strides.size()),
			source.StreamOutput.RasterizedStream
		};
	}
};

struct CopiedComputePipelineDesc
{
	D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
	com_ptr<ID3D12RootSignature> root_signature;
	std::vector<uint8_t> cs, cached_pso;

	explicit CopiedComputePipelineDesc(const D3D12_COMPUTE_PIPELINE_STATE_DESC &source)
	{
		desc = source;
		root_signature = source.pRootSignature;
		desc.pRootSignature = root_signature.get();
		CopiedGraphicsPipelineDesc::copy_bytecode(source.CS, cs, desc.CS);

		if (source.CachedPSO.pCachedBlob != nullptr && source.CachedPSO.CachedBlobSizeInBytes != 0)
		{
			cached_pso.resize(source.CachedPSO.CachedBlobSizeInBytes);
			std::memcpy(cached_pso.data(), source.CachedPSO.pCachedBlob, source.CachedPSO.CachedBlobSizeInBytes);
			desc.CachedPSO = { cached_pso.data(), cached_pso.size() };
		}
		// Classic CreateComputePipelineState cannot reliably consume stream/library cached blobs.
		desc.CachedPSO = {};
	}
};

struct CopiedPipelineStateStream
{
	D3D12_PIPELINE_STATE_STREAM_DESC desc = {};
	std::vector<uint8_t> stream;
	std::vector<std::vector<uint8_t>> byte_storage;
	std::vector<std::vector<char>> string_storage;
	std::vector<std::vector<D3D12_INPUT_ELEMENT_DESC>> input_layout_storage;
	std::vector<std::vector<D3D12_SO_DECLARATION_ENTRY>> stream_output_storage;
	std::vector<std::vector<UINT>> stream_output_stride_storage;
	std::vector<std::vector<D3D12_VIEW_INSTANCE_LOCATION>> view_instance_storage;
	std::vector<com_ptr<ID3D12RootSignature>> root_signature_storage;
	bool valid = false;

	static size_t subobject_size(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type)
	{
		switch (type)
		{
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: return sizeof(D3D12_PIPELINE_STATE_STREAM_ROOT_SIGNATURE);
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS: return sizeof(D3D12_PIPELINE_STATE_STREAM_VS);
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS: return sizeof(D3D12_PIPELINE_STATE_STREAM_PS);
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS: return sizeof(D3D12_PIPELINE_STATE_STREAM_DS);
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS: return sizeof(D3D12_PIPELINE_STATE_STREAM_HS);
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS: return sizeof(D3D12_PIPELINE_STATE_STREAM_GS);
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS: return sizeof(D3D12_PIPELINE_STATE_STREAM_CS);
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT: return sizeof(D3D12_PIPELINE_STATE_STREAM_STREAM_OUTPUT);
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND: return sizeof(D3D12_PIPELINE_STATE_STREAM_BLEND_DESC);
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK: return sizeof(D3D12_PIPELINE_STATE_STREAM_SAMPLE_MASK);
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER: return sizeof(D3D12_PIPELINE_STATE_STREAM_RASTERIZER);
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL: return sizeof(D3D12_PIPELINE_STATE_STREAM_DEPTH_STENCIL);
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT: return sizeof(D3D12_PIPELINE_STATE_STREAM_INPUT_LAYOUT);
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE: return sizeof(D3D12_PIPELINE_STATE_STREAM_IB_STRIP_CUT_VALUE);
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY: return sizeof(D3D12_PIPELINE_STATE_STREAM_PRIMITIVE_TOPOLOGY);
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: return sizeof(D3D12_PIPELINE_STATE_STREAM_RENDER_TARGET_FORMATS);
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT: return sizeof(D3D12_PIPELINE_STATE_STREAM_DEPTH_STENCIL_FORMAT);
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC: return sizeof(D3D12_PIPELINE_STATE_STREAM_SAMPLE_DESC);
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK: return sizeof(D3D12_PIPELINE_STATE_STREAM_NODE_MASK);
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO: return sizeof(D3D12_PIPELINE_STATE_STREAM_CACHED_PSO);
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS: return sizeof(D3D12_PIPELINE_STATE_STREAM_FLAGS);
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1: return sizeof(D3D12_PIPELINE_STATE_STREAM_DEPTH_STENCIL1);
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: return sizeof(D3D12_PIPELINE_STATE_STREAM_VIEW_INSTANCING);
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS: return sizeof(D3D12_PIPELINE_STATE_STREAM_AS);
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS: return sizeof(D3D12_PIPELINE_STATE_STREAM_MS);
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2: return sizeof(D3D12_PIPELINE_STATE_STREAM_DEPTH_STENCIL2);
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER1: return sizeof(D3D12_PIPELINE_STATE_STREAM_RASTERIZER1);
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER2: return sizeof(D3D12_PIPELINE_STATE_STREAM_RASTERIZER2);
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SERIALIZED_ROOT_SIGNATURE: return sizeof(D3D12_PIPELINE_STATE_STREAM_SERIALIZED_ROOT_SIGNATURE);
		default: return 0;
		}
	}

	static void copy_byte_span(const void *source, size_t size, const void *&dest, std::vector<std::vector<uint8_t>> &storage)
	{
		if (source == nullptr || size == 0)
		{
			dest = nullptr;
			return;
		}

		std::vector<uint8_t> copy(size);
		std::memcpy(copy.data(), source, size);
		storage.push_back(std::move(copy));
		dest = storage.back().data();
	}

	static void copy_shader_bytecode(D3D12_SHADER_BYTECODE &bytecode, std::vector<std::vector<uint8_t>> &storage)
	{
		copy_byte_span(bytecode.pShaderBytecode, bytecode.BytecodeLength, bytecode.pShaderBytecode, storage);
	}

	static void copy_cached_pso(D3D12_CACHED_PIPELINE_STATE &cached_pso, std::vector<std::vector<uint8_t>> &storage)
	{
		copy_byte_span(cached_pso.pCachedBlob, cached_pso.CachedBlobSizeInBytes, cached_pso.pCachedBlob, storage);
	}

	static void copy_serialized_root_signature(D3D12_SERIALIZED_ROOT_SIGNATURE_DESC &root_signature, std::vector<std::vector<uint8_t>> &storage)
	{
		copy_byte_span(root_signature.pSerializedBlob, root_signature.SerializedBlobSizeInBytes, root_signature.pSerializedBlob, storage);
	}

	const char *copy_string(const char *source)
	{
		if (source == nullptr)
			return nullptr;

		const size_t size = std::strlen(source) + 1;
		std::vector<char> copy(size);
		std::memcpy(copy.data(), source, size);
		string_storage.push_back(std::move(copy));
		return string_storage.back().data();
	}

	explicit CopiedPipelineStateStream(const D3D12_PIPELINE_STATE_STREAM_DESC &source)
	{
		if (source.pPipelineStateSubobjectStream == nullptr || source.SizeInBytes == 0)
			return;

		stream.resize(source.SizeInBytes);
		std::memcpy(stream.data(), source.pPipelineStateSubobjectStream, source.SizeInBytes);
		desc = { stream.size(), stream.data() };

		const uintptr_t original_base = reinterpret_cast<uintptr_t>(source.pPipelineStateSubobjectStream);
		const uintptr_t original_end = original_base + source.SizeInBytes;
		for (uintptr_t original_p = original_base, copied_p = reinterpret_cast<uintptr_t>(stream.data()); original_p < original_end;)
		{
			if (original_p + sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE) > original_end)
				return;

			const D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = *reinterpret_cast<const D3D12_PIPELINE_STATE_SUBOBJECT_TYPE *>(original_p);
			const size_t size = subobject_size(type);
			if (size == 0 || original_p + size > original_end)
				return;

			switch (type)
			{
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE:
			{
				ID3D12RootSignature *&root_signature = reinterpret_cast<D3D12_PIPELINE_STATE_STREAM_ROOT_SIGNATURE *>(copied_p)->data;
				if (root_signature != nullptr)
				{
					root_signature_storage.emplace_back(root_signature);
					root_signature = root_signature_storage.back().get();
				}
				break;
			}
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
				copy_shader_bytecode(reinterpret_cast<D3D12_PIPELINE_STATE_STREAM_VS *>(copied_p)->data, byte_storage);
				break;
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
				copy_shader_bytecode(reinterpret_cast<D3D12_PIPELINE_STATE_STREAM_PS *>(copied_p)->data, byte_storage);
				break;
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
				copy_shader_bytecode(reinterpret_cast<D3D12_PIPELINE_STATE_STREAM_DS *>(copied_p)->data, byte_storage);
				break;
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
				copy_shader_bytecode(reinterpret_cast<D3D12_PIPELINE_STATE_STREAM_HS *>(copied_p)->data, byte_storage);
				break;
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
				copy_shader_bytecode(reinterpret_cast<D3D12_PIPELINE_STATE_STREAM_GS *>(copied_p)->data, byte_storage);
				break;
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
				copy_shader_bytecode(reinterpret_cast<D3D12_PIPELINE_STATE_STREAM_CS *>(copied_p)->data, byte_storage);
				break;
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
				copy_shader_bytecode(reinterpret_cast<D3D12_PIPELINE_STATE_STREAM_AS *>(copied_p)->data, byte_storage);
				break;
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS:
				copy_shader_bytecode(reinterpret_cast<D3D12_PIPELINE_STATE_STREAM_MS *>(copied_p)->data, byte_storage);
				break;
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SERIALIZED_ROOT_SIGNATURE:
				copy_serialized_root_signature(reinterpret_cast<D3D12_PIPELINE_STATE_STREAM_SERIALIZED_ROOT_SIGNATURE *>(copied_p)->data, byte_storage);
				break;
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO:
				copy_cached_pso(reinterpret_cast<D3D12_PIPELINE_STATE_STREAM_CACHED_PSO *>(copied_p)->data, byte_storage);
				break;
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT:
			{
				D3D12_INPUT_LAYOUT_DESC &layout = reinterpret_cast<D3D12_PIPELINE_STATE_STREAM_INPUT_LAYOUT *>(copied_p)->data;
				if (layout.pInputElementDescs != nullptr && layout.NumElements != 0)
				{
					input_layout_storage.emplace_back(layout.pInputElementDescs, layout.pInputElementDescs + layout.NumElements);
					for (D3D12_INPUT_ELEMENT_DESC &element : input_layout_storage.back())
						element.SemanticName = copy_string(element.SemanticName);
					layout.pInputElementDescs = input_layout_storage.back().data();
				}
				break;
			}
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT:
			{
				D3D12_STREAM_OUTPUT_DESC &stream_output = reinterpret_cast<D3D12_PIPELINE_STATE_STREAM_STREAM_OUTPUT *>(copied_p)->data;
				if (stream_output.pSODeclaration != nullptr && stream_output.NumEntries != 0)
				{
					stream_output_storage.emplace_back(stream_output.pSODeclaration, stream_output.pSODeclaration + stream_output.NumEntries);
					for (D3D12_SO_DECLARATION_ENTRY &entry : stream_output_storage.back())
						entry.SemanticName = copy_string(entry.SemanticName);
					stream_output.pSODeclaration = stream_output_storage.back().data();
				}
				if (stream_output.pBufferStrides != nullptr && stream_output.NumStrides != 0)
				{
					stream_output_stride_storage.emplace_back(stream_output.pBufferStrides, stream_output.pBufferStrides + stream_output.NumStrides);
					stream_output.pBufferStrides = stream_output_stride_storage.back().data();
				}
				break;
			}
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING:
			{
				D3D12_VIEW_INSTANCING_DESC &view_instancing = reinterpret_cast<D3D12_PIPELINE_STATE_STREAM_VIEW_INSTANCING *>(copied_p)->data;
				if (view_instancing.pViewInstanceLocations != nullptr && view_instancing.ViewInstanceCount != 0)
				{
					view_instance_storage.emplace_back(view_instancing.pViewInstanceLocations, view_instancing.pViewInstanceLocations + view_instancing.ViewInstanceCount);
					view_instancing.pViewInstanceLocations = view_instance_storage.back().data();
				}
				break;
			}
			default:
				break;
			}

			original_p += size;
			copied_p += size;
		}

		valid = true;
	}
};

static D3D12_BLEND_DESC make_default_blend_desc()
{
	D3D12_BLEND_DESC desc = {};
	desc.AlphaToCoverageEnable = FALSE;
	desc.IndependentBlendEnable = FALSE;
	for (D3D12_RENDER_TARGET_BLEND_DESC &render_target : desc.RenderTarget)
	{
		render_target.BlendEnable = FALSE;
		render_target.LogicOpEnable = FALSE;
		render_target.SrcBlend = D3D12_BLEND_ONE;
		render_target.DestBlend = D3D12_BLEND_ZERO;
		render_target.BlendOp = D3D12_BLEND_OP_ADD;
		render_target.SrcBlendAlpha = D3D12_BLEND_ONE;
		render_target.DestBlendAlpha = D3D12_BLEND_ZERO;
		render_target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
		render_target.LogicOp = D3D12_LOGIC_OP_NOOP;
		render_target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	}
	return desc;
}

static D3D12_RASTERIZER_DESC make_default_rasterizer_desc()
{
	D3D12_RASTERIZER_DESC desc = {};
	desc.FillMode = D3D12_FILL_MODE_SOLID;
	desc.CullMode = D3D12_CULL_MODE_BACK;
	desc.FrontCounterClockwise = FALSE;
	desc.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
	desc.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
	desc.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
	desc.DepthClipEnable = TRUE;
	desc.MultisampleEnable = FALSE;
	desc.AntialiasedLineEnable = FALSE;
	desc.ForcedSampleCount = 0;
	desc.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
	return desc;
}

static D3D12_DEPTH_STENCIL_DESC make_default_depth_stencil_desc()
{
	D3D12_DEPTH_STENCIL_DESC desc = {};
	desc.DepthEnable = FALSE;
	desc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
	desc.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
	desc.StencilEnable = FALSE;
	desc.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
	desc.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
	desc.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
	desc.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
	desc.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
	desc.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
	desc.BackFace = desc.FrontFace;
	return desc;
}

static bool convert_depth_stencil_desc1(const D3D12_DEPTH_STENCIL_DESC1 &source, D3D12_DEPTH_STENCIL_DESC &dest)
{
	dest.DepthEnable = source.DepthEnable;
	dest.DepthWriteMask = source.DepthWriteMask;
	dest.DepthFunc = source.DepthFunc;
	dest.StencilEnable = source.StencilEnable;
	dest.StencilReadMask = source.StencilReadMask;
	dest.StencilWriteMask = source.StencilWriteMask;
	dest.FrontFace = source.FrontFace;
	dest.BackFace = source.BackFace;
	return true;
}

static bool convert_depth_stencil_desc2(const D3D12_DEPTH_STENCIL_DESC2 &source, D3D12_DEPTH_STENCIL_DESC &dest)
{
	dest.DepthEnable = source.DepthEnable;
	dest.DepthWriteMask = source.DepthWriteMask;
	dest.DepthFunc = source.DepthFunc;
	dest.StencilEnable = source.StencilEnable;
	dest.StencilReadMask = source.FrontFace.StencilReadMask;
	dest.StencilWriteMask = source.FrontFace.StencilWriteMask;
	dest.FrontFace.StencilFailOp = source.FrontFace.StencilFailOp;
	dest.FrontFace.StencilDepthFailOp = source.FrontFace.StencilDepthFailOp;
	dest.FrontFace.StencilPassOp = source.FrontFace.StencilPassOp;
	dest.FrontFace.StencilFunc = source.FrontFace.StencilFunc;
	dest.BackFace.StencilFailOp = source.BackFace.StencilFailOp;
	dest.BackFace.StencilDepthFailOp = source.BackFace.StencilDepthFailOp;
	dest.BackFace.StencilPassOp = source.BackFace.StencilPassOp;
	dest.BackFace.StencilFunc = source.BackFace.StencilFunc;
	return true;
}

static D3D12_RASTERIZER_DESC convert_rasterizer_desc1(const D3D12_RASTERIZER_DESC1 &source)
{
	D3D12_RASTERIZER_DESC dest = {};
	dest.FillMode = source.FillMode;
	dest.CullMode = source.CullMode;
	dest.FrontCounterClockwise = source.FrontCounterClockwise;
	dest.DepthBias = static_cast<INT>(source.DepthBias);
	dest.DepthBiasClamp = source.DepthBiasClamp;
	dest.SlopeScaledDepthBias = source.SlopeScaledDepthBias;
	dest.DepthClipEnable = source.DepthClipEnable;
	dest.MultisampleEnable = source.MultisampleEnable;
	dest.AntialiasedLineEnable = source.AntialiasedLineEnable;
	dest.ForcedSampleCount = source.ForcedSampleCount;
	dest.ConservativeRaster = source.ConservativeRaster;
	return dest;
}

static D3D12_RASTERIZER_DESC convert_rasterizer_desc2(const D3D12_RASTERIZER_DESC2 &source)
{
	D3D12_RASTERIZER_DESC dest = {};
	dest.FillMode = source.FillMode;
	dest.CullMode = source.CullMode;
	dest.FrontCounterClockwise = source.FrontCounterClockwise;
	dest.DepthBias = static_cast<INT>(source.DepthBias);
	dest.DepthBiasClamp = source.DepthBiasClamp;
	dest.SlopeScaledDepthBias = source.SlopeScaledDepthBias;
	dest.DepthClipEnable = source.DepthClipEnable;
	dest.MultisampleEnable = FALSE;
	dest.AntialiasedLineEnable = source.LineRasterizationMode != D3D12_LINE_RASTERIZATION_MODE_ALIASED;
	dest.ForcedSampleCount = source.ForcedSampleCount;
	dest.ConservativeRaster = source.ConservativeRaster;
	return dest;
}

static bool convert_pipeline_state_stream_to_graphics_desc(const D3D12_PIPELINE_STATE_STREAM_DESC &stream_desc, D3D12_GRAPHICS_PIPELINE_STATE_DESC &desc, const char *&unsupported_reason, bool &has_cached_pso, bool &has_mesh_shader)
{
	unsupported_reason = "none";
	has_cached_pso = false;
	has_mesh_shader = false;
	if (stream_desc.pPipelineStateSubobjectStream == nullptr)
	{
		unsupported_reason = "stream has null subobject pointer";
		return false;
	}

	desc = {};
	desc.BlendState = make_default_blend_desc();
	desc.SampleMask = UINT_MAX;
	desc.RasterizerState = make_default_rasterizer_desc();
	desc.DepthStencilState = make_default_depth_stencil_desc();
	desc.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
	desc.SampleDesc = { 1, 0 };
	desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

	bool has_graphics_shader = false;
	const uintptr_t end = reinterpret_cast<uintptr_t>(stream_desc.pPipelineStateSubobjectStream) + stream_desc.SizeInBytes;
	for (uintptr_t p = reinterpret_cast<uintptr_t>(stream_desc.pPipelineStateSubobjectStream); p < end;)
	{
		if (p + sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE) > end)
		{
			unsupported_reason = "stream truncated before subobject type";
			return false;
		}

		switch (*reinterpret_cast<const D3D12_PIPELINE_STATE_SUBOBJECT_TYPE *>(p))
		{
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE:
			if (p + sizeof(D3D12_PIPELINE_STATE_STREAM_ROOT_SIGNATURE) > end) { unsupported_reason = "stream truncated in ROOT_SIGNATURE"; return false; }
			desc.pRootSignature = reinterpret_cast<const D3D12_PIPELINE_STATE_STREAM_ROOT_SIGNATURE *>(p)->data;
			p += sizeof(D3D12_PIPELINE_STATE_STREAM_ROOT_SIGNATURE);
			continue;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
			if (p + sizeof(D3D12_PIPELINE_STATE_STREAM_VS) > end) { unsupported_reason = "stream truncated in VS"; return false; }
			desc.VS = reinterpret_cast<const D3D12_PIPELINE_STATE_STREAM_VS *>(p)->data;
			has_graphics_shader = has_graphics_shader || desc.VS.pShaderBytecode != nullptr;
			p += sizeof(D3D12_PIPELINE_STATE_STREAM_VS);
			continue;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
			if (p + sizeof(D3D12_PIPELINE_STATE_STREAM_PS) > end) { unsupported_reason = "stream truncated in PS"; return false; }
			desc.PS = reinterpret_cast<const D3D12_PIPELINE_STATE_STREAM_PS *>(p)->data;
			p += sizeof(D3D12_PIPELINE_STATE_STREAM_PS);
			continue;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
			if (p + sizeof(D3D12_PIPELINE_STATE_STREAM_DS) > end) { unsupported_reason = "stream truncated in DS"; return false; }
			desc.DS = reinterpret_cast<const D3D12_PIPELINE_STATE_STREAM_DS *>(p)->data;
			p += sizeof(D3D12_PIPELINE_STATE_STREAM_DS);
			continue;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
			if (p + sizeof(D3D12_PIPELINE_STATE_STREAM_HS) > end) { unsupported_reason = "stream truncated in HS"; return false; }
			desc.HS = reinterpret_cast<const D3D12_PIPELINE_STATE_STREAM_HS *>(p)->data;
			p += sizeof(D3D12_PIPELINE_STATE_STREAM_HS);
			continue;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
			if (p + sizeof(D3D12_PIPELINE_STATE_STREAM_GS) > end) { unsupported_reason = "stream truncated in GS"; return false; }
			desc.GS = reinterpret_cast<const D3D12_PIPELINE_STATE_STREAM_GS *>(p)->data;
			p += sizeof(D3D12_PIPELINE_STATE_STREAM_GS);
			continue;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT:
			if (p + sizeof(D3D12_PIPELINE_STATE_STREAM_STREAM_OUTPUT) > end) { unsupported_reason = "stream truncated in STREAM_OUTPUT"; return false; }
			desc.StreamOutput = reinterpret_cast<const D3D12_PIPELINE_STATE_STREAM_STREAM_OUTPUT *>(p)->data;
			p += sizeof(D3D12_PIPELINE_STATE_STREAM_STREAM_OUTPUT);
			continue;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND:
			if (p + sizeof(D3D12_PIPELINE_STATE_STREAM_BLEND_DESC) > end) { unsupported_reason = "stream truncated in BLEND"; return false; }
			desc.BlendState = reinterpret_cast<const D3D12_PIPELINE_STATE_STREAM_BLEND_DESC *>(p)->data;
			p += sizeof(D3D12_PIPELINE_STATE_STREAM_BLEND_DESC);
			continue;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK:
			if (p + sizeof(D3D12_PIPELINE_STATE_STREAM_SAMPLE_MASK) > end) { unsupported_reason = "stream truncated in SAMPLE_MASK"; return false; }
			desc.SampleMask = reinterpret_cast<const D3D12_PIPELINE_STATE_STREAM_SAMPLE_MASK *>(p)->data;
			p += sizeof(D3D12_PIPELINE_STATE_STREAM_SAMPLE_MASK);
			continue;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER:
			if (p + sizeof(D3D12_PIPELINE_STATE_STREAM_RASTERIZER) > end) { unsupported_reason = "stream truncated in RASTERIZER"; return false; }
			desc.RasterizerState = reinterpret_cast<const D3D12_PIPELINE_STATE_STREAM_RASTERIZER *>(p)->data;
			p += sizeof(D3D12_PIPELINE_STATE_STREAM_RASTERIZER);
			continue;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL:
			if (p + sizeof(D3D12_PIPELINE_STATE_STREAM_DEPTH_STENCIL) > end) { unsupported_reason = "stream truncated in DEPTH_STENCIL"; return false; }
			desc.DepthStencilState = reinterpret_cast<const D3D12_PIPELINE_STATE_STREAM_DEPTH_STENCIL *>(p)->data;
			p += sizeof(D3D12_PIPELINE_STATE_STREAM_DEPTH_STENCIL);
			continue;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT:
			if (p + sizeof(D3D12_PIPELINE_STATE_STREAM_INPUT_LAYOUT) > end) { unsupported_reason = "stream truncated in INPUT_LAYOUT"; return false; }
			desc.InputLayout = reinterpret_cast<const D3D12_PIPELINE_STATE_STREAM_INPUT_LAYOUT *>(p)->data;
			p += sizeof(D3D12_PIPELINE_STATE_STREAM_INPUT_LAYOUT);
			continue;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE:
			if (p + sizeof(D3D12_PIPELINE_STATE_STREAM_IB_STRIP_CUT_VALUE) > end) { unsupported_reason = "stream truncated in IB_STRIP_CUT_VALUE"; return false; }
			desc.IBStripCutValue = reinterpret_cast<const D3D12_PIPELINE_STATE_STREAM_IB_STRIP_CUT_VALUE *>(p)->data;
			p += sizeof(D3D12_PIPELINE_STATE_STREAM_IB_STRIP_CUT_VALUE);
			continue;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY:
			if (p + sizeof(D3D12_PIPELINE_STATE_STREAM_PRIMITIVE_TOPOLOGY) > end) { unsupported_reason = "stream truncated in PRIMITIVE_TOPOLOGY"; return false; }
			desc.PrimitiveTopologyType = reinterpret_cast<const D3D12_PIPELINE_STATE_STREAM_PRIMITIVE_TOPOLOGY *>(p)->data;
			p += sizeof(D3D12_PIPELINE_STATE_STREAM_PRIMITIVE_TOPOLOGY);
			continue;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS:
			if (p + sizeof(D3D12_PIPELINE_STATE_STREAM_RENDER_TARGET_FORMATS) > end) { unsupported_reason = "stream truncated in RENDER_TARGET_FORMATS"; return false; }
			desc.NumRenderTargets = reinterpret_cast<const D3D12_PIPELINE_STATE_STREAM_RENDER_TARGET_FORMATS *>(p)->data.NumRenderTargets;
			if (desc.NumRenderTargets > D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT) { unsupported_reason = "stream has too many render targets"; return false; }
			std::memcpy(desc.RTVFormats, reinterpret_cast<const D3D12_PIPELINE_STATE_STREAM_RENDER_TARGET_FORMATS *>(p)->data.RTFormats, sizeof(desc.RTVFormats));
			p += sizeof(D3D12_PIPELINE_STATE_STREAM_RENDER_TARGET_FORMATS);
			continue;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT:
			if (p + sizeof(D3D12_PIPELINE_STATE_STREAM_DEPTH_STENCIL_FORMAT) > end) { unsupported_reason = "stream truncated in DEPTH_STENCIL_FORMAT"; return false; }
			desc.DSVFormat = reinterpret_cast<const D3D12_PIPELINE_STATE_STREAM_DEPTH_STENCIL_FORMAT *>(p)->data;
			p += sizeof(D3D12_PIPELINE_STATE_STREAM_DEPTH_STENCIL_FORMAT);
			continue;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC:
			if (p + sizeof(D3D12_PIPELINE_STATE_STREAM_SAMPLE_DESC) > end) { unsupported_reason = "stream truncated in SAMPLE_DESC"; return false; }
			desc.SampleDesc = reinterpret_cast<const D3D12_PIPELINE_STATE_STREAM_SAMPLE_DESC *>(p)->data;
			p += sizeof(D3D12_PIPELINE_STATE_STREAM_SAMPLE_DESC);
			continue;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK:
			if (p + sizeof(D3D12_PIPELINE_STATE_STREAM_NODE_MASK) > end) { unsupported_reason = "stream truncated in NODE_MASK"; return false; }
			desc.NodeMask = reinterpret_cast<const D3D12_PIPELINE_STATE_STREAM_NODE_MASK *>(p)->data;
			p += sizeof(D3D12_PIPELINE_STATE_STREAM_NODE_MASK);
			continue;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO:
			if (p + sizeof(D3D12_PIPELINE_STATE_STREAM_CACHED_PSO) > end) { unsupported_reason = "stream truncated in CACHED_PSO"; return false; }
			desc.CachedPSO = reinterpret_cast<const D3D12_PIPELINE_STATE_STREAM_CACHED_PSO *>(p)->data;
			has_cached_pso = desc.CachedPSO.pCachedBlob != nullptr && desc.CachedPSO.CachedBlobSizeInBytes != 0;
			p += sizeof(D3D12_PIPELINE_STATE_STREAM_CACHED_PSO);
			continue;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS:
			if (p + sizeof(D3D12_PIPELINE_STATE_STREAM_FLAGS) > end) { unsupported_reason = "stream truncated in FLAGS"; return false; }
			desc.Flags = reinterpret_cast<const D3D12_PIPELINE_STATE_STREAM_FLAGS *>(p)->data;
			p += sizeof(D3D12_PIPELINE_STATE_STREAM_FLAGS);
			continue;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1:
			if (p + sizeof(D3D12_PIPELINE_STATE_STREAM_DEPTH_STENCIL1) > end) { unsupported_reason = "stream truncated in DEPTH_STENCIL1"; return false; }
			if (!convert_depth_stencil_desc1(reinterpret_cast<const D3D12_PIPELINE_STATE_STREAM_DEPTH_STENCIL1 *>(p)->data, desc.DepthStencilState)) { unsupported_reason = "DEPTH_STENCIL1 uses depth bounds"; return false; }
			p += sizeof(D3D12_PIPELINE_STATE_STREAM_DEPTH_STENCIL1);
			continue;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING:
			if (p + sizeof(D3D12_PIPELINE_STATE_STREAM_VIEW_INSTANCING) > end) { unsupported_reason = "stream truncated in VIEW_INSTANCING"; return false; }
			p += sizeof(D3D12_PIPELINE_STATE_STREAM_VIEW_INSTANCING);
			continue;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2:
			if (p + sizeof(D3D12_PIPELINE_STATE_STREAM_DEPTH_STENCIL2) > end) { unsupported_reason = "stream truncated in DEPTH_STENCIL2"; return false; }
			if (!convert_depth_stencil_desc2(reinterpret_cast<const D3D12_PIPELINE_STATE_STREAM_DEPTH_STENCIL2 *>(p)->data, desc.DepthStencilState)) { unsupported_reason = "DEPTH_STENCIL2 conversion failed"; return false; }
			p += sizeof(D3D12_PIPELINE_STATE_STREAM_DEPTH_STENCIL2);
			continue;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER1:
			if (p + sizeof(D3D12_PIPELINE_STATE_STREAM_RASTERIZER1) > end) { unsupported_reason = "stream truncated in RASTERIZER1"; return false; }
			desc.RasterizerState = convert_rasterizer_desc1(reinterpret_cast<const D3D12_PIPELINE_STATE_STREAM_RASTERIZER1 *>(p)->data);
			p += sizeof(D3D12_PIPELINE_STATE_STREAM_RASTERIZER1);
			continue;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER2:
			if (p + sizeof(D3D12_PIPELINE_STATE_STREAM_RASTERIZER2) > end) { unsupported_reason = "stream truncated in RASTERIZER2"; return false; }
			desc.RasterizerState = convert_rasterizer_desc2(reinterpret_cast<const D3D12_PIPELINE_STATE_STREAM_RASTERIZER2 *>(p)->data);
			p += sizeof(D3D12_PIPELINE_STATE_STREAM_RASTERIZER2);
			continue;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SERIALIZED_ROOT_SIGNATURE:
			if (p + sizeof(D3D12_PIPELINE_STATE_STREAM_SERIALIZED_ROOT_SIGNATURE) > end) { unsupported_reason = "stream truncated in SERIALIZED_ROOT_SIGNATURE"; return false; }
			if (!use_global_sentinel_fallback_pso) { unsupported_reason = "stream uses serialized root signature without sentinel fallback"; return false; }
			desc.pRootSignature = nullptr;
			p += sizeof(D3D12_PIPELINE_STATE_STREAM_SERIALIZED_ROOT_SIGNATURE);
			continue;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
			unsupported_reason = "stream is compute (CS)";
			return false;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
			if (p + sizeof(D3D12_PIPELINE_STATE_STREAM_AS) > end) { unsupported_reason = "stream truncated in AS"; return false; }
			p += sizeof(D3D12_PIPELINE_STATE_STREAM_AS);
			continue;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS:
			if (p + sizeof(D3D12_PIPELINE_STATE_STREAM_MS) > end) { unsupported_reason = "stream truncated in MS"; return false; }
			has_mesh_shader = reinterpret_cast<const D3D12_PIPELINE_STATE_STREAM_MS *>(p)->data.pShaderBytecode != nullptr;
			has_graphics_shader = has_graphics_shader || has_mesh_shader;
			p += sizeof(D3D12_PIPELINE_STATE_STREAM_MS);
			continue;
		default:
			unsupported_reason = "stream uses unknown subobject";
			return false;
		}
	}

	if (has_mesh_shader && desc.PrimitiveTopologyType == D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED)
		desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

	if (!has_graphics_shader)
	{
		unsupported_reason = "stream has no graphics shader";
		return false;
	}

	return true;
}

static bool convert_pipeline_state_stream_to_compute_desc(const D3D12_PIPELINE_STATE_STREAM_DESC &stream_desc, D3D12_COMPUTE_PIPELINE_STATE_DESC &desc, const char *&unsupported_reason, bool &has_cached_pso)
{
	unsupported_reason = "none";
	has_cached_pso = false;
	if (stream_desc.pPipelineStateSubobjectStream == nullptr)
	{
		unsupported_reason = "stream has null subobject pointer";
		return false;
	}

	desc = {};
	bool has_compute_shader = false;
	const uintptr_t end = reinterpret_cast<uintptr_t>(stream_desc.pPipelineStateSubobjectStream) + stream_desc.SizeInBytes;
	for (uintptr_t p = reinterpret_cast<uintptr_t>(stream_desc.pPipelineStateSubobjectStream); p < end;)
	{
		if (p + sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE) > end)
		{
			unsupported_reason = "stream truncated before subobject type";
			return false;
		}

		switch (*reinterpret_cast<const D3D12_PIPELINE_STATE_SUBOBJECT_TYPE *>(p))
		{
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE:
			if (p + sizeof(D3D12_PIPELINE_STATE_STREAM_ROOT_SIGNATURE) > end) { unsupported_reason = "stream truncated in ROOT_SIGNATURE"; return false; }
			desc.pRootSignature = reinterpret_cast<const D3D12_PIPELINE_STATE_STREAM_ROOT_SIGNATURE *>(p)->data;
			p += sizeof(D3D12_PIPELINE_STATE_STREAM_ROOT_SIGNATURE);
			continue;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
			if (p + sizeof(D3D12_PIPELINE_STATE_STREAM_CS) > end) { unsupported_reason = "stream truncated in CS"; return false; }
			desc.CS = reinterpret_cast<const D3D12_PIPELINE_STATE_STREAM_CS *>(p)->data;
			has_compute_shader = desc.CS.pShaderBytecode != nullptr;
			p += sizeof(D3D12_PIPELINE_STATE_STREAM_CS);
			continue;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK:
			if (p + sizeof(D3D12_PIPELINE_STATE_STREAM_NODE_MASK) > end) { unsupported_reason = "stream truncated in NODE_MASK"; return false; }
			desc.NodeMask = reinterpret_cast<const D3D12_PIPELINE_STATE_STREAM_NODE_MASK *>(p)->data;
			p += sizeof(D3D12_PIPELINE_STATE_STREAM_NODE_MASK);
			continue;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO:
			if (p + sizeof(D3D12_PIPELINE_STATE_STREAM_CACHED_PSO) > end) { unsupported_reason = "stream truncated in CACHED_PSO"; return false; }
			desc.CachedPSO = reinterpret_cast<const D3D12_PIPELINE_STATE_STREAM_CACHED_PSO *>(p)->data;
			has_cached_pso = desc.CachedPSO.pCachedBlob != nullptr && desc.CachedPSO.CachedBlobSizeInBytes != 0;
			p += sizeof(D3D12_PIPELINE_STATE_STREAM_CACHED_PSO);
			continue;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS:
			if (p + sizeof(D3D12_PIPELINE_STATE_STREAM_FLAGS) > end) { unsupported_reason = "stream truncated in FLAGS"; return false; }
			desc.Flags = reinterpret_cast<const D3D12_PIPELINE_STATE_STREAM_FLAGS *>(p)->data;
			p += sizeof(D3D12_PIPELINE_STATE_STREAM_FLAGS);
			continue;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SERIALIZED_ROOT_SIGNATURE:
			if (p + sizeof(D3D12_PIPELINE_STATE_STREAM_SERIALIZED_ROOT_SIGNATURE) > end) { unsupported_reason = "stream truncated in SERIALIZED_ROOT_SIGNATURE"; return false; }
			if (!use_global_sentinel_fallback_pso) { unsupported_reason = "stream uses serialized root signature without sentinel fallback"; return false; }
			desc.pRootSignature = nullptr;
			p += sizeof(D3D12_PIPELINE_STATE_STREAM_SERIALIZED_ROOT_SIGNATURE);
			continue;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS:
			unsupported_reason = "stream is graphics/mesh, not compute";
			return false;
		default:
			unsupported_reason = "stream uses non-compute subobject";
			return false;
		}
	}

	if (!has_compute_shader)
	{
		unsupported_reason = "stream has no compute shader";
		return false;
	}

	return true;
}

class D3D12AsyncPipelineManager final
{
public:
	struct ResidencyJob
	{
		enum class Type
		{
			MakeResident,
			SetPriority
		};

		Type type = Type::MakeResident;
		com_ptr<ID3D12Pageable> object;
		D3D12_RESIDENCY_PRIORITY priority = D3D12_RESIDENCY_PRIORITY_NORMAL;
	};

	D3D12AsyncPipelineManager(D3D12Device *device_proxy, ID3D12Device *device) : _device_proxy(device_proxy), _device(device)
	{
		assert(_device_proxy != nullptr);
		assert(_device != nullptr);
		_device->AddRef();
		reshade::log::message(reshade::log::level::info, "[ASYNC] Async D3D12 PSO enabled: sentinel=%u, fallback_mode=%s, compile_workers=%u/%u/%u (75/50/25%% of %u hardware threads), compile_pacing=%u, wait_for_cached_blob=%u, debug=%u.", use_global_sentinel_fallback_pso, get_async_pipeline_fallback_mode_name(), _compile_worker_count, _compile_worker_middle_count, _compile_worker_lower_count, _hardware_thread_count, async_pipeline_compile_pacing, async_pipeline_wait_for_cached_blob, async_pipeline_debug_diagnostics);
		for (uint32_t i = 0; i < _compile_worker_count; ++i)
			_workers.emplace_back([this]() { worker_loop(); });
		_residency_worker = std::thread([this]() { residency_worker_loop(); });
	}
	~D3D12AsyncPipelineManager()
	{
		{
			std::lock_guard<std::mutex> lock(_queue_mutex);
			_stop = true;
		}
		g_async_pipeline_compile_gate_cv.notify_all();
		_compile_slot_cv.notify_all();
		for (std::thread &worker : _workers)
			if (worker.joinable())
				worker.join();

		{
			std::lock_guard<std::mutex> lock(_residency_queue_mutex);
			_residency_stop = true;
		}
		_residency_cv.notify_all();
		if (_residency_worker.joinable())
			_residency_worker.join();

		if (async_pipeline_debug_diagnostics)
			log_async_pipeline_diagnostics("summary");

		_device->Release();
	}

	HRESULT create_graphics_pipeline_state(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc, REFIID riid, void **pipeline_state)
	{
		const uint64_t graphics_create_count = increment_async_pipeline_diagnostic(g_async_pipeline_diagnostics.graphics_create_calls, 1);
		if (should_log_periodic(graphics_create_count))
			log_async_pipeline_diagnostics("graphics-create");

		if (pipeline_state == nullptr)
			return E_POINTER;
		*pipeline_state = nullptr;

		if (riid != __uuidof(ID3D12PipelineState))
		{
			if (riid == __uuidof(ID3D12PipelineState1))
				increment_async_pipeline_diagnostic(g_async_pipeline_diagnostics.graphics_pipeline_state1_requests, 1);
			increment_async_pipeline_diagnostic(g_async_pipeline_diagnostics.sync_unsupported_interface, 1);
			return _device->CreateGraphicsPipelineState(desc, riid, pipeline_state);
		}

		if constexpr (!async_graphics_fallback_enabled)
		{
			note_async_pipeline_unsupported_desc("graphics async fallback disabled for compute-only test");
			return _device->CreateGraphicsPipelineState(desc, riid, pipeline_state);
		}

		const char *unsupported_reason = "none";
		D3D12_GRAPHICS_PIPELINE_STATE_DESC working_desc = *desc;
		if (!is_supported(working_desc, unsupported_reason, true, use_global_sentinel_fallback_pso))
		{
			note_async_pipeline_unsupported_desc(unsupported_reason);
			return _device->CreateGraphicsPipelineState(desc, riid, pipeline_state);
		}

		com_ptr<ID3D12PipelineState> fallback;
		const HRESULT fallback_hr = get_or_create_fallback(working_desc, fallback);
		if (FAILED(fallback_hr))
		{
			increment_async_pipeline_diagnostic(g_async_pipeline_diagnostics.sync_fallback_failure, 1);
			reshade::log::message(reshade::log::level::warning, "[ASYNC] Async D3D12 PSO: fallback creation failed with error code %s; falling back to synchronous real PSO creation.", reshade::log::hr_to_string(fallback_hr).c_str());
			return _device->CreateGraphicsPipelineState(desc, riid, pipeline_state);
		}

		const uint64_t id = _next_proxy_id.fetch_add(1, std::memory_order_relaxed);
		D3D12AsyncPipelineProxy *const proxy = new D3D12AsyncPipelineProxy(this, _device, fallback.get(), id);

		CompileJob job = {};
		job.proxy = proxy;
		job.graphics_desc = std::make_unique<CopiedGraphicsPipelineDesc>(working_desc);
		job.id = id;
		enqueue_compile_job(std::move(job));

		increment_async_pipeline_diagnostic(g_async_pipeline_diagnostics.async_proxy_creates, 1);
		*pipeline_state = proxy;
		return S_OK;
	}

	HRESULT create_compute_pipeline_state(const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc, REFIID riid, void **pipeline_state)
	{
		const uint64_t compute_create_count = increment_async_pipeline_diagnostic(g_async_pipeline_diagnostics.compute_create_calls, 1);
		if (should_log_periodic(compute_create_count))
			log_async_pipeline_diagnostics("compute-create");

		if (pipeline_state == nullptr)
			return E_POINTER;
		*pipeline_state = nullptr;

		if (riid != __uuidof(ID3D12PipelineState))
		{
			if (riid == __uuidof(ID3D12PipelineState1))
				increment_async_pipeline_diagnostic(g_async_pipeline_diagnostics.compute_pipeline_state1_requests, 1);
			increment_async_pipeline_diagnostic(g_async_pipeline_diagnostics.sync_unsupported_interface, 1);
			return _device->CreateComputePipelineState(desc, riid, pipeline_state);
		}

		if constexpr (!async_compute_fallback_enabled)
		{
			note_async_pipeline_unsupported_desc("compute async fallback disabled");
			return _device->CreateComputePipelineState(desc, riid, pipeline_state);
		}

		const char *unsupported_reason = "none";
		D3D12_COMPUTE_PIPELINE_STATE_DESC working_desc = *desc;
		if (!is_supported(working_desc, unsupported_reason, use_global_sentinel_fallback_pso))
		{
			note_async_pipeline_unsupported_desc(unsupported_reason);
			return _device->CreateComputePipelineState(desc, riid, pipeline_state);
		}

		com_ptr<ID3D12PipelineState> fallback;
		const HRESULT fallback_hr = get_or_create_compute_fallback(working_desc, fallback);
		if (FAILED(fallback_hr))
		{
			increment_async_pipeline_diagnostic(g_async_pipeline_diagnostics.sync_fallback_failure, 1);
			reshade::log::message(reshade::log::level::warning, "[ASYNC] Async D3D12 PSO: compute fallback creation failed with error code %s; falling back to synchronous real PSO creation.", reshade::log::hr_to_string(fallback_hr).c_str());
			return _device->CreateComputePipelineState(desc, riid, pipeline_state);
		}

		const uint64_t id = _next_proxy_id.fetch_add(1, std::memory_order_relaxed);
		D3D12AsyncPipelineProxy *const proxy = new D3D12AsyncPipelineProxy(this, _device, fallback.get(), id);

		CompileJob job = {};
		job.proxy = proxy;
		job.compute_desc = std::make_unique<CopiedComputePipelineDesc>(working_desc);
		job.id = id;
		enqueue_compile_job(std::move(job));

		increment_async_pipeline_diagnostic(g_async_pipeline_diagnostics.async_proxy_creates, 1);
		*pipeline_state = proxy;
		return S_OK;
	}

	HRESULT create_pipeline_state_stream(const D3D12_PIPELINE_STATE_STREAM_DESC *stream_desc, REFIID riid, void **pipeline_state)
	{
		if (pipeline_state == nullptr)
			return E_POINTER;
		*pipeline_state = nullptr;

		if (stream_desc == nullptr)
			return E_INVALIDARG;

		com_ptr<ID3D12Device2> device2;
		const HRESULT device2_hr = _device->QueryInterface(&device2);
		if (FAILED(device2_hr))
			return device2_hr;

		if (riid != __uuidof(ID3D12PipelineState))
		{
			increment_async_pipeline_diagnostic(g_async_pipeline_diagnostics.sync_unsupported_interface, 1);
			return device2->CreatePipelineState(stream_desc, riid, pipeline_state);
		}

		D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics_desc = {};
		bool graphics_has_cached_pso = false;
		bool graphics_has_mesh_shader = false;
		const char *graphics_unsupported_reason = "none";
		D3D12_COMPUTE_PIPELINE_STATE_DESC compute_desc = {};
		bool compute_has_cached_pso = false;
		const char *compute_unsupported_reason = "none";
		if (convert_pipeline_state_stream_to_graphics_desc(*stream_desc, graphics_desc, graphics_unsupported_reason, graphics_has_cached_pso, graphics_has_mesh_shader))
		{
			if constexpr (!async_graphics_fallback_enabled)
			{
				note_async_pipeline_unsupported_desc("graphics stream async fallback disabled");
				goto sync_with_events;
			}
			if (graphics_has_cached_pso)
			{
				note_async_pipeline_unsupported_desc("graphics stream uses cached PSO / pipeline library");
				goto sync_with_events;
			}
			if (!is_supported(graphics_desc, graphics_unsupported_reason, true, use_global_sentinel_fallback_pso))
			{
				note_async_pipeline_unsupported_desc(graphics_unsupported_reason);
				goto sync_with_events;
			}

			com_ptr<ID3D12PipelineState> fallback;
			const HRESULT fallback_hr = graphics_has_mesh_shader && use_global_sentinel_fallback_pso ? get_or_create_mesh_fallback(graphics_desc, fallback) : get_or_create_fallback(graphics_desc, fallback);
			if (FAILED(fallback_hr))
			{
				increment_async_pipeline_diagnostic(g_async_pipeline_diagnostics.sync_fallback_failure, 1);
				reshade::log::message(reshade::log::level::warning, "[ASYNC] Async D3D12 PSO: graphics fallback creation failed with error code %s; falling back to synchronous real PSO creation.", reshade::log::hr_to_string(fallback_hr).c_str());
				goto sync_with_events;
			}

			const uint64_t id = _next_proxy_id.fetch_add(1, std::memory_order_relaxed);
			D3D12AsyncPipelineProxy *const proxy = new D3D12AsyncPipelineProxy(this, _device, fallback.get(), id);
			std::unique_ptr<CopiedPipelineStateStream> copied_stream_desc = std::make_unique<CopiedPipelineStateStream>(*stream_desc);
			if (!copied_stream_desc->valid)
			{
				proxy->Release();
				note_async_pipeline_unsupported_desc("graphics stream copy failed");
				goto sync_with_events;
			}

			CompileJob job = {};
			job.proxy = proxy;
			job.stream_desc = std::move(copied_stream_desc);
			job.id = id;
			enqueue_compile_job(std::move(job));

			increment_async_pipeline_diagnostic(g_async_pipeline_diagnostics.async_proxy_creates, 1);
			*pipeline_state = proxy;
			return S_OK;
		}

		if (convert_pipeline_state_stream_to_compute_desc(*stream_desc, compute_desc, compute_unsupported_reason, compute_has_cached_pso))
		{
			if constexpr (!async_compute_fallback_enabled)
			{
				note_async_pipeline_unsupported_desc("compute stream async fallback disabled");
				goto sync_with_events;
			}
			if (compute_has_cached_pso)
			{
				note_async_pipeline_unsupported_desc("compute stream uses cached PSO / pipeline library");
				goto sync_with_events;
			}
			if (!is_supported(compute_desc, compute_unsupported_reason, use_global_sentinel_fallback_pso))
			{
				note_async_pipeline_unsupported_desc(compute_unsupported_reason);
				goto sync_with_events;
			}

			com_ptr<ID3D12PipelineState> fallback;
			const HRESULT fallback_hr = get_or_create_compute_fallback(compute_desc, fallback);
			if (FAILED(fallback_hr))
			{
				increment_async_pipeline_diagnostic(g_async_pipeline_diagnostics.sync_fallback_failure, 1);
				reshade::log::message(reshade::log::level::warning, "[ASYNC] Async D3D12 PSO: compute fallback creation failed with error code %s; falling back to synchronous real PSO creation.", reshade::log::hr_to_string(fallback_hr).c_str());
				goto sync_with_events;
			}

			const uint64_t id = _next_proxy_id.fetch_add(1, std::memory_order_relaxed);
			D3D12AsyncPipelineProxy *const proxy = new D3D12AsyncPipelineProxy(this, _device, fallback.get(), id);
			std::unique_ptr<CopiedPipelineStateStream> copied_stream_desc = std::make_unique<CopiedPipelineStateStream>(*stream_desc);
			if (!copied_stream_desc->valid)
			{
				proxy->Release();
				note_async_pipeline_unsupported_desc("compute stream copy failed");
				goto sync_with_events;
			}

			CompileJob job = {};
			job.proxy = proxy;
			job.stream_desc = std::move(copied_stream_desc);
			job.id = id;
			enqueue_compile_job(std::move(job));

			increment_async_pipeline_diagnostic(g_async_pipeline_diagnostics.async_proxy_creates, 1);
			*pipeline_state = proxy;
			return S_OK;
		}

		note_async_pipeline_unsupported_desc(graphics_unsupported_reason);

sync_with_events:
		return create_pipeline_state_stream_with_events(*stream_desc, device2.get(), pipeline_state);
	}

private:
	HRESULT create_pipeline_state_stream_with_events(const D3D12_PIPELINE_STATE_STREAM_DESC &stream_desc, ID3D12Device2 *device2, void **pipeline_state)
	{
		HRESULT hr = S_OK;
#if RESHADE_ADDON >= 2
		ID3D12PipelineState *event_pipeline = nullptr;
		if (_device_proxy->invoke_create_and_init_pipeline_event(stream_desc, event_pipeline, hr, true))
		{
			if (SUCCEEDED(hr))
				*pipeline_state = event_pipeline;
			return hr;
		}
#endif
		return device2->CreatePipelineState(&stream_desc, IID_PPV_ARGS(reinterpret_cast<ID3D12PipelineState **>(pipeline_state)));
	}

	struct CompileJob
	{
		D3D12AsyncPipelineProxy *proxy = nullptr;
		std::unique_ptr<CopiedGraphicsPipelineDesc> graphics_desc;
		std::unique_ptr<CopiedComputePipelineDesc> compute_desc;
		std::unique_ptr<CopiedPipelineStateStream> stream_desc;
		uint64_t id = 0;
	};
	struct FramePacingSourceState
	{
		uint32_t requested_worker_limit = 0;
		uint32_t candidate_worker_limit = 0;
		uint32_t candidate_samples = 0;
	};

	static bool is_supported(const D3D12_GRAPHICS_PIPELINE_STATE_DESC &desc, const char *&unsupported_reason, bool allow_stream_output = false, bool allow_null_root_signature = false)
	{
		unsupported_reason = "none";
		if (!allow_null_root_signature && desc.pRootSignature == nullptr)
		{
			unsupported_reason = "graphics desc has null root signature";
			return false;
		}
		if (desc.SampleDesc.Count != 1 || desc.SampleDesc.Quality != 0)
		{
			unsupported_reason = "graphics desc uses MSAA/sample quality";
			return false;
		}
		if (!allow_stream_output && (desc.StreamOutput.NumEntries != 0 || desc.StreamOutput.NumStrides != 0))
		{
			unsupported_reason = "graphics desc uses stream output";
			return false;
		}
		if (desc.PrimitiveTopologyType == D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED)
		{
			unsupported_reason = "graphics desc has undefined topology";
			return false;
		}
		if (desc.CachedPSO.pCachedBlob != nullptr && desc.CachedPSO.CachedBlobSizeInBytes != 0)
		{
			unsupported_reason = "graphics desc uses cached PSO";
			return false;
		}
		if (desc.NodeMask != 0 && desc.NodeMask != 1)
		{
			unsupported_reason = "graphics desc uses multi-adapter node mask";
			return false;
		}
		return true;
	}

	static bool is_supported(const D3D12_COMPUTE_PIPELINE_STATE_DESC &desc, const char *&unsupported_reason, bool allow_null_root_signature = false)
	{
		unsupported_reason = "none";
		if (!allow_null_root_signature && desc.pRootSignature == nullptr)
		{
			unsupported_reason = "compute desc has null root signature";
			return false;
		}
		if (desc.CS.pShaderBytecode == nullptr || desc.CS.BytecodeLength == 0)
		{
			unsupported_reason = "compute desc has no compute shader";
			return false;
		}
		if (desc.CachedPSO.pCachedBlob != nullptr && desc.CachedPSO.CachedBlobSizeInBytes != 0)
		{
			unsupported_reason = "compute desc uses cached PSO";
			return false;
		}
		if (desc.NodeMask != 0 && desc.NodeMask != 1)
		{
			unsupported_reason = "compute desc uses multi-adapter node mask";
			return false;
		}
		return true;
	}

	static GraphicsFallbackKey make_key(const D3D12_GRAPHICS_PIPELINE_STATE_DESC &desc)
	{
		GraphicsFallbackKey key = {};
		key.root_signature = desc.pRootSignature;
		key.primitive_topology_type = desc.PrimitiveTopologyType;
		key.num_render_targets = desc.NumRenderTargets;
		std::memcpy(key.rtv_formats, desc.RTVFormats, sizeof(key.rtv_formats));
		key.dsv_format = desc.DSVFormat;
		key.depth_enabled = desc.DepthStencilState.DepthEnable != FALSE;
		key.stencil_enabled = desc.DepthStencilState.StencilEnable != FALSE;
		return key;
	}

	static ComputeFallbackKey make_key(const D3D12_COMPUTE_PIPELINE_STATE_DESC &desc)
	{
		ComputeFallbackKey key = {};
		key.root_signature = desc.pRootSignature;
		return key;
	}

	HRESULT get_or_create_dummy_root_signature(com_ptr<ID3D12RootSignature> &root_signature)
	{
		{
			std::lock_guard<std::mutex> lock(_dummy_root_signature_mutex);
			if (_dummy_root_signature != nullptr)
			{
				root_signature = _dummy_root_signature;
				return S_OK;
			}
		}

		D3D12_ROOT_SIGNATURE_DESC desc = {};
		desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

		com_ptr<ID3DBlob> blob, error_blob;
		HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error_blob);
		if (FAILED(hr))
			return hr;

		com_ptr<ID3D12RootSignature> created;
		hr = _device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&created));
		if (FAILED(hr))
			return hr;

		std::lock_guard<std::mutex> lock(_dummy_root_signature_mutex);
		if (_dummy_root_signature == nullptr)
			_dummy_root_signature = created;
		root_signature = _dummy_root_signature;
		return S_OK;
	}

	HRESULT get_or_create_fallback(const D3D12_GRAPHICS_PIPELINE_STATE_DESC &source_desc, com_ptr<ID3D12PipelineState> &fallback)
	{
		GraphicsFallbackKey key = make_key(source_desc);
		{
			std::lock_guard<std::mutex> lock(_fallback_mutex);
			if (use_global_sentinel_fallback_pso)
			{
				if (!_has_global_fallback_key)
				{
					_global_fallback_key = key;
					_has_global_fallback_key = true;
				}
				else
				{
					key = _global_fallback_key;
				}
			}

			const auto it = _fallback_cache.find(key);
			if (it != _fallback_cache.end())
			{
				increment_async_pipeline_diagnostic(g_async_pipeline_diagnostics.fallback_cache_hits, 1);
				fallback = it->second;
				return S_OK;
			}
		}
		increment_async_pipeline_diagnostic(g_async_pipeline_diagnostics.fallback_cache_misses, 1);

		D3D12_SHADER_BYTECODE vs = {}, ps = {};
		ensure_fallback_shaders(vs, ps);
		com_ptr<ID3D12RootSignature> dummy_root_signature;
		if (use_global_sentinel_fallback_pso)
		{
			const HRESULT root_signature_hr = get_or_create_dummy_root_signature(dummy_root_signature);
			if (FAILED(root_signature_hr))
				return root_signature_hr;
		}

		D3D12_GRAPHICS_PIPELINE_STATE_DESC fallback_desc = {};
		fallback_desc.pRootSignature = use_global_sentinel_fallback_pso ? dummy_root_signature.get() : source_desc.pRootSignature;
		fallback_desc.VS = vs;
		fallback_desc.PS = ps;
		fallback_desc.BlendState.AlphaToCoverageEnable = FALSE;
		fallback_desc.BlendState.IndependentBlendEnable = FALSE;
		for (D3D12_RENDER_TARGET_BLEND_DESC &render_target : fallback_desc.BlendState.RenderTarget)
		{
			render_target.BlendEnable = FALSE;
			render_target.LogicOpEnable = FALSE;
			render_target.SrcBlend = D3D12_BLEND_ONE;
			render_target.DestBlend = D3D12_BLEND_ZERO;
			render_target.BlendOp = D3D12_BLEND_OP_ADD;
			render_target.SrcBlendAlpha = D3D12_BLEND_ONE;
			render_target.DestBlendAlpha = D3D12_BLEND_ZERO;
			render_target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
			render_target.LogicOp = D3D12_LOGIC_OP_NOOP;
			render_target.RenderTargetWriteMask = 0;
		}
		fallback_desc.SampleMask = UINT_MAX;
		fallback_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		fallback_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		fallback_desc.RasterizerState.FrontCounterClockwise = FALSE;
		fallback_desc.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
		fallback_desc.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
		fallback_desc.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
		fallback_desc.RasterizerState.DepthClipEnable = TRUE;
		fallback_desc.RasterizerState.MultisampleEnable = FALSE;
		fallback_desc.RasterizerState.AntialiasedLineEnable = FALSE;
		fallback_desc.RasterizerState.ForcedSampleCount = 0;
		fallback_desc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
		fallback_desc.DepthStencilState.DepthEnable = key.depth_enabled;
		fallback_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		fallback_desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		fallback_desc.DepthStencilState.StencilEnable = FALSE;
		fallback_desc.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
		fallback_desc.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
		fallback_desc.InputLayout = {};
		fallback_desc.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
		fallback_desc.PrimitiveTopologyType = key.primitive_topology_type != D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH ? key.primitive_topology_type : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		fallback_desc.NumRenderTargets = key.num_render_targets;
		std::memcpy(fallback_desc.RTVFormats, key.rtv_formats, sizeof(fallback_desc.RTVFormats));
		fallback_desc.DSVFormat = key.dsv_format;
		fallback_desc.SampleDesc = { 1, 0 };
		fallback_desc.NodeMask = source_desc.NodeMask;
		fallback_desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

		com_ptr<ID3D12PipelineState> created;
		HRESULT hr = E_FAIL;
		bool handled_by_addon_events = false;
#if RESHADE_ADDON >= 2
		if constexpr (publish_fallback_pipelines_to_addons)
		{
			ID3D12PipelineState *event_pipeline = nullptr;
			handled_by_addon_events = _device_proxy->invoke_create_and_init_pipeline_event(fallback_desc, event_pipeline, hr, allow_addons_to_modify_fallback_pipelines);
			if (handled_by_addon_events && SUCCEEDED(hr))
				created = com_ptr<ID3D12PipelineState>(event_pipeline, true);
		}
#endif
		if (!handled_by_addon_events)
			hr = _device->CreateGraphicsPipelineState(&fallback_desc, IID_PPV_ARGS(&created));
		if (FAILED(hr))
			increment_async_pipeline_diagnostic(g_async_pipeline_diagnostics.fallback_creation_failures, 1);
		if (FAILED(hr))
		{
			if (use_global_sentinel_fallback_pso)
			{
				std::lock_guard<std::mutex> lock(_fallback_mutex);
				if (_fallback_cache.empty())
					_has_global_fallback_key = false;
			}
			return hr;
		}

		std::lock_guard<std::mutex> lock(_fallback_mutex);
		if (use_global_sentinel_fallback_pso)
			key = _global_fallback_key;
		const auto [it, inserted] = _fallback_cache.emplace(key, created);
		fallback = it->second;
		if (inserted)
			increment_async_pipeline_diagnostic(g_async_pipeline_diagnostics.fallback_buckets_created, 1);
		if (async_pipeline_debug_diagnostics)
			reshade::log::message(reshade::log::level::info, "[ASYNC] Async D3D12 PSO: created fallback PSO bucket, total buckets = %zu.", _fallback_cache.size());
		return S_OK;
	}

	HRESULT get_or_create_mesh_fallback(const D3D12_GRAPHICS_PIPELINE_STATE_DESC &source_desc, com_ptr<ID3D12PipelineState> &fallback)
	{
		assert(use_global_sentinel_fallback_pso);
		std::lock_guard<std::mutex> lock(_mesh_fallback_mutex);
		if (_mesh_fallback != nullptr)
		{
			increment_async_pipeline_diagnostic(g_async_pipeline_diagnostics.fallback_cache_hits);
			fallback = _mesh_fallback;
			return S_OK;
		}
		increment_async_pipeline_diagnostic(g_async_pipeline_diagnostics.fallback_cache_misses);

		com_ptr<ID3D12RootSignature> dummy_root_signature;
		HRESULT hr = get_or_create_dummy_root_signature(dummy_root_signature);
		if (FAILED(hr))
			return hr;

		D3D12_SHADER_BYTECODE ms = {}, ps = {};
		ensure_mesh_fallback_shaders(ms, ps);

		D3D12_BLEND_DESC blend = {};
		for (D3D12_RENDER_TARGET_BLEND_DESC &render_target : blend.RenderTarget)
		{
			render_target.SrcBlend = D3D12_BLEND_ONE;
			render_target.DestBlend = D3D12_BLEND_ZERO;
			render_target.BlendOp = D3D12_BLEND_OP_ADD;
			render_target.SrcBlendAlpha = D3D12_BLEND_ONE;
			render_target.DestBlendAlpha = D3D12_BLEND_ZERO;
			render_target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
			render_target.LogicOp = D3D12_LOGIC_OP_NOOP;
			render_target.RenderTargetWriteMask = 0;
		}

		D3D12_RASTERIZER_DESC rasterizer = {};
		rasterizer.FillMode = D3D12_FILL_MODE_SOLID;
		rasterizer.CullMode = D3D12_CULL_MODE_NONE;
		rasterizer.DepthClipEnable = TRUE;

		D3D12_DEPTH_STENCIL_DESC depth_stencil = {};
		depth_stencil.DepthEnable = source_desc.DepthStencilState.DepthEnable;
		depth_stencil.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		depth_stencil.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		depth_stencil.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
		depth_stencil.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;

		D3D12_RT_FORMAT_ARRAY render_target_formats = {};
		render_target_formats.NumRenderTargets = source_desc.NumRenderTargets;
		std::memcpy(render_target_formats.RTFormats, source_desc.RTVFormats, sizeof(render_target_formats.RTFormats));

		struct MeshFallbackPipelineStream
		{
			D3D12_PIPELINE_STATE_STREAM_ROOT_SIGNATURE root_signature;
			D3D12_PIPELINE_STATE_STREAM_MS mesh_shader;
			D3D12_PIPELINE_STATE_STREAM_PS pixel_shader;
			D3D12_PIPELINE_STATE_STREAM_BLEND_DESC blend;
			D3D12_PIPELINE_STATE_STREAM_SAMPLE_MASK sample_mask;
			D3D12_PIPELINE_STATE_STREAM_RASTERIZER rasterizer;
			D3D12_PIPELINE_STATE_STREAM_DEPTH_STENCIL depth_stencil;
			D3D12_PIPELINE_STATE_STREAM_RENDER_TARGET_FORMATS render_target_formats;
			D3D12_PIPELINE_STATE_STREAM_DEPTH_STENCIL_FORMAT depth_stencil_format;
			D3D12_PIPELINE_STATE_STREAM_SAMPLE_DESC sample_desc;
			D3D12_PIPELINE_STATE_STREAM_NODE_MASK node_mask;
			D3D12_PIPELINE_STATE_STREAM_FLAGS flags;
		} stream;
		stream.root_signature = dummy_root_signature.get();
		stream.mesh_shader = ms;
		stream.pixel_shader = ps;
		stream.blend = blend;
		stream.sample_mask = UINT_MAX;
		stream.rasterizer = rasterizer;
		stream.depth_stencil = depth_stencil;
		stream.render_target_formats = render_target_formats;
		stream.depth_stencil_format = source_desc.DSVFormat;
		stream.sample_desc = DXGI_SAMPLE_DESC { 1, 0 };
		stream.node_mask = source_desc.NodeMask;
		stream.flags = D3D12_PIPELINE_STATE_FLAG_NONE;

		com_ptr<ID3D12Device2> device2;
		hr = _device->QueryInterface(&device2);
		if (FAILED(hr))
			return hr;

		const D3D12_PIPELINE_STATE_STREAM_DESC stream_desc = { sizeof(stream), &stream };
		com_ptr<ID3D12PipelineState> created;
		hr = device2->CreatePipelineState(&stream_desc, IID_PPV_ARGS(&created));
		if (FAILED(hr))
		{
			increment_async_pipeline_diagnostic(g_async_pipeline_diagnostics.fallback_creation_failures);
			return hr;
		}

		_mesh_fallback = created;
		fallback = _mesh_fallback;
		increment_async_pipeline_diagnostic(g_async_pipeline_diagnostics.fallback_buckets_created);
		if (async_pipeline_debug_diagnostics)
			reshade::log::message(reshade::log::level::info, "[ASYNC] Async D3D12 PSO: created global mesh sentinel fallback PSO.");
		return S_OK;
	}

	HRESULT get_or_create_compute_fallback(const D3D12_COMPUTE_PIPELINE_STATE_DESC &source_desc, com_ptr<ID3D12PipelineState> &fallback)
	{
		ComputeFallbackKey key = make_key(source_desc);
		{
			std::lock_guard<std::mutex> lock(_compute_fallback_mutex);
			if (use_global_sentinel_fallback_pso)
			{
				if (!_has_global_compute_fallback_key)
				{
					_global_compute_fallback_key = key;
					_has_global_compute_fallback_key = true;
				}
				else
				{
					key = _global_compute_fallback_key;
				}
			}

			const auto it = _compute_fallback_cache.find(key);
			if (it != _compute_fallback_cache.end())
			{
				increment_async_pipeline_diagnostic(g_async_pipeline_diagnostics.fallback_cache_hits, 1);
				fallback = it->second;
				return S_OK;
			}
		}
		increment_async_pipeline_diagnostic(g_async_pipeline_diagnostics.fallback_cache_misses, 1);

		D3D12_SHADER_BYTECODE cs = {};
		ensure_compute_fallback_shader(cs);
		com_ptr<ID3D12RootSignature> dummy_root_signature;
		if (use_global_sentinel_fallback_pso)
		{
			const HRESULT root_signature_hr = get_or_create_dummy_root_signature(dummy_root_signature);
			if (FAILED(root_signature_hr))
				return root_signature_hr;
		}

		D3D12_COMPUTE_PIPELINE_STATE_DESC fallback_desc = {};
		fallback_desc.pRootSignature = use_global_sentinel_fallback_pso ? dummy_root_signature.get() : source_desc.pRootSignature;
		fallback_desc.CS = cs;
		fallback_desc.NodeMask = source_desc.NodeMask;
		fallback_desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

		com_ptr<ID3D12PipelineState> created;
		HRESULT hr = E_FAIL;
		bool handled_by_addon_events = false;
#if RESHADE_ADDON >= 2
		if constexpr (publish_fallback_pipelines_to_addons)
		{
			ID3D12PipelineState *event_pipeline = nullptr;
			handled_by_addon_events = _device_proxy->invoke_create_and_init_pipeline_event(fallback_desc, event_pipeline, hr, allow_addons_to_modify_fallback_pipelines);
			if (handled_by_addon_events && SUCCEEDED(hr))
				created = com_ptr<ID3D12PipelineState>(event_pipeline, true);
		}
#endif
		if (!handled_by_addon_events)
			hr = _device->CreateComputePipelineState(&fallback_desc, IID_PPV_ARGS(&created));
		if (FAILED(hr))
			increment_async_pipeline_diagnostic(g_async_pipeline_diagnostics.fallback_creation_failures, 1);
		if (FAILED(hr))
		{
			if (use_global_sentinel_fallback_pso)
			{
				std::lock_guard<std::mutex> lock(_compute_fallback_mutex);
				if (_compute_fallback_cache.empty())
					_has_global_compute_fallback_key = false;
			}
			return hr;
		}

		std::lock_guard<std::mutex> lock(_compute_fallback_mutex);
		if (use_global_sentinel_fallback_pso)
			key = _global_compute_fallback_key;
		const auto [it, inserted] = _compute_fallback_cache.emplace(key, created);
		fallback = it->second;
		if (inserted)
			increment_async_pipeline_diagnostic(g_async_pipeline_diagnostics.fallback_buckets_created, 1);
		if (async_pipeline_debug_diagnostics)
			reshade::log::message(reshade::log::level::info, "[ASYNC] Async D3D12 PSO: created compute fallback PSO bucket, total buckets = %zu.", _compute_fallback_cache.size());
		return S_OK;
	}

	static void ensure_fallback_shaders(D3D12_SHADER_BYTECODE &vs, D3D12_SHADER_BYTECODE &ps)
	{
		vs = { reshade::d3d12::async_pipeline_shaders::fallback_vs, sizeof(reshade::d3d12::async_pipeline_shaders::fallback_vs) };
		ps = { reshade::d3d12::async_pipeline_shaders::fallback_ps, sizeof(reshade::d3d12::async_pipeline_shaders::fallback_ps) };
	}

	static void ensure_compute_fallback_shader(D3D12_SHADER_BYTECODE &cs)
	{
		cs = { reshade::d3d12::async_pipeline_shaders::fallback_cs, sizeof(reshade::d3d12::async_pipeline_shaders::fallback_cs) };
	}

	static void ensure_mesh_fallback_shaders(D3D12_SHADER_BYTECODE &ms, D3D12_SHADER_BYTECODE &ps)
	{
		ms = { reshade::d3d12::async_pipeline_shaders::fallback_ms, sizeof(reshade::d3d12::async_pipeline_shaders::fallback_ms) };
		ps = { reshade::d3d12::async_pipeline_shaders::fallback_ps_dxil, sizeof(reshade::d3d12::async_pipeline_shaders::fallback_ps_dxil) };
	}

	static uint64_t hash_bytes(const void *data, size_t size)
	{
		if (data == nullptr || size == 0)
			return 0;

		uint64_t hash = 14695981039346656037ull;
		const uint8_t *const bytes = static_cast<const uint8_t *>(data);
		for (size_t i = 0; i < size; ++i)
		{
			hash ^= bytes[i];
			hash *= 1099511628211ull;
		}
		return hash;
	}

	static void log_shader_bytecode(uint64_t id, const char *name, const D3D12_SHADER_BYTECODE &bytecode)
	{
		if (async_pipeline_debug_diagnostics)
			reshade::log::message(reshade::log::level::info, "[ASYNC] Async D3D12 PSO proxy %llu:   %-3s ptr=%p size=%zu hash=0x%016llx.",
				static_cast<unsigned long long>(id), name, bytecode.pShaderBytecode, bytecode.BytecodeLength,
				static_cast<unsigned long long>(hash_bytes(bytecode.pShaderBytecode, bytecode.BytecodeLength)));
	}

	static void log_graphics_pipeline_desc(uint64_t id, const D3D12_GRAPHICS_PIPELINE_STATE_DESC &desc)
	{
		if (async_pipeline_debug_diagnostics)
		{
			reshade::log::message(reshade::log::level::info,
				"[ASYNC] Async D3D12 PSO proxy %llu: graphics real desc root=%p node=%u flags=0x%x topology=%u sample=%u/%u rts=%u dsv=%u depth=%u stencil=%u cached=%p/%zu input=%u so_entries=%u so_strides=%u.",
				static_cast<unsigned long long>(id), desc.pRootSignature, desc.NodeMask, desc.Flags, desc.PrimitiveTopologyType,
				desc.SampleDesc.Count, desc.SampleDesc.Quality, desc.NumRenderTargets, desc.DSVFormat,
				desc.DepthStencilState.DepthEnable != FALSE, desc.DepthStencilState.StencilEnable != FALSE,
				desc.CachedPSO.pCachedBlob, desc.CachedPSO.CachedBlobSizeInBytes,
				desc.InputLayout.NumElements, desc.StreamOutput.NumEntries, desc.StreamOutput.NumStrides);
			reshade::log::message(reshade::log::level::info,
				"[ASYNC] Async D3D12 PSO proxy %llu:   RTV formats = [%u, %u, %u, %u, %u, %u, %u, %u].",
				static_cast<unsigned long long>(id), desc.RTVFormats[0], desc.RTVFormats[1], desc.RTVFormats[2], desc.RTVFormats[3], desc.RTVFormats[4], desc.RTVFormats[5], desc.RTVFormats[6], desc.RTVFormats[7]);
			log_shader_bytecode(id, "VS", desc.VS);
			log_shader_bytecode(id, "PS", desc.PS);
			log_shader_bytecode(id, "DS", desc.DS);
			log_shader_bytecode(id, "HS", desc.HS);
			log_shader_bytecode(id, "GS", desc.GS);
		}
	}

	static void log_compute_pipeline_desc(uint64_t id, const D3D12_COMPUTE_PIPELINE_STATE_DESC &desc)
	{
		if (async_pipeline_debug_diagnostics)
		{
			reshade::log::message(reshade::log::level::info,
				"[ASYNC] Async D3D12 PSO proxy %llu: compute real desc root=%p node=%u flags=0x%x cached=%p/%zu.",
				static_cast<unsigned long long>(id), desc.pRootSignature, desc.NodeMask, desc.Flags,
				desc.CachedPSO.pCachedBlob, desc.CachedPSO.CachedBlobSizeInBytes);
			log_shader_bytecode(id, "CS", desc.CS);
		}
	}

	static const char *subobject_type_name(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type)
	{
		switch (type)
		{
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: return "ROOT_SIGNATURE";
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS: return "VS";
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS: return "PS";
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS: return "DS";
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS: return "HS";
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS: return "GS";
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS: return "CS";
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT: return "STREAM_OUTPUT";
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND: return "BLEND";
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK: return "SAMPLE_MASK";
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER: return "RASTERIZER";
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL: return "DEPTH_STENCIL";
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT: return "INPUT_LAYOUT";
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE: return "IB_STRIP_CUT_VALUE";
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY: return "PRIMITIVE_TOPOLOGY";
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: return "RENDER_TARGET_FORMATS";
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT: return "DEPTH_STENCIL_FORMAT";
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC: return "SAMPLE_DESC";
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK: return "NODE_MASK";
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO: return "CACHED_PSO";
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS: return "FLAGS";
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1: return "DEPTH_STENCIL1";
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: return "VIEW_INSTANCING";
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS: return "AS";
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS: return "MS";
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2: return "DEPTH_STENCIL2";
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER1: return "RASTERIZER1";
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER2: return "RASTERIZER2";
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SERIALIZED_ROOT_SIGNATURE: return "SERIALIZED_ROOT_SIGNATURE";
		default: return "UNKNOWN";
		}
	}

	static void log_pipeline_state_stream_desc(uint64_t id, const D3D12_PIPELINE_STATE_STREAM_DESC &desc)
	{
		if (async_pipeline_debug_diagnostics)
		{
			reshade::log::message(reshade::log::level::info, "[ASYNC] Async D3D12 PSO proxy %llu: stream real desc ptr=%p size=%zu.",
				static_cast<unsigned long long>(id), desc.pPipelineStateSubobjectStream, desc.SizeInBytes);

			if (desc.pPipelineStateSubobjectStream == nullptr || desc.SizeInBytes == 0)
				return;

			const uintptr_t end = reinterpret_cast<uintptr_t>(desc.pPipelineStateSubobjectStream) + desc.SizeInBytes;
			unsigned int index = 0;
			for (uintptr_t p = reinterpret_cast<uintptr_t>(desc.pPipelineStateSubobjectStream); p < end; ++index)
			{
				if (p + sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE) > end)
				{
					reshade::log::message(reshade::log::level::warning, "[ASYNC] Async D3D12 PSO proxy %llu:   stream[%u] truncated before type at offset %zu.", static_cast<unsigned long long>(id), index, static_cast<size_t>(p - reinterpret_cast<uintptr_t>(desc.pPipelineStateSubobjectStream)));
					return;
				}

				const D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = *reinterpret_cast<const D3D12_PIPELINE_STATE_SUBOBJECT_TYPE *>(p);
				const size_t size = CopiedPipelineStateStream::subobject_size(type);
				reshade::log::message(reshade::log::level::info, "[ASYNC] Async D3D12 PSO proxy %llu:   stream[%u] type=%s(%u) offset=%zu size=%zu.",
					static_cast<unsigned long long>(id), index, subobject_type_name(type), type, static_cast<size_t>(p - reinterpret_cast<uintptr_t>(desc.pPipelineStateSubobjectStream)), size);
				if (size == 0 || p + size > end)
				{
					reshade::log::message(reshade::log::level::warning, "[ASYNC] Async D3D12 PSO proxy %llu:   stream[%u] has invalid size; stream is malformed.", static_cast<unsigned long long>(id), index);
					return;
				}

				switch (type)
				{
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS: log_shader_bytecode(id, "VS", reinterpret_cast<const D3D12_PIPELINE_STATE_STREAM_VS *>(p)->data); break;
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS: log_shader_bytecode(id, "PS", reinterpret_cast<const D3D12_PIPELINE_STATE_STREAM_PS *>(p)->data); break;
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS: log_shader_bytecode(id, "DS", reinterpret_cast<const D3D12_PIPELINE_STATE_STREAM_DS *>(p)->data); break;
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS: log_shader_bytecode(id, "HS", reinterpret_cast<const D3D12_PIPELINE_STATE_STREAM_HS *>(p)->data); break;
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS: log_shader_bytecode(id, "GS", reinterpret_cast<const D3D12_PIPELINE_STATE_STREAM_GS *>(p)->data); break;
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS: log_shader_bytecode(id, "CS", reinterpret_cast<const D3D12_PIPELINE_STATE_STREAM_CS *>(p)->data); break;
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS: log_shader_bytecode(id, "AS", reinterpret_cast<const D3D12_PIPELINE_STATE_STREAM_AS *>(p)->data); break;
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS: log_shader_bytecode(id, "MS", reinterpret_cast<const D3D12_PIPELINE_STATE_STREAM_MS *>(p)->data); break;
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO:
				{
					const D3D12_CACHED_PIPELINE_STATE &cached_pso = reinterpret_cast<const D3D12_PIPELINE_STATE_STREAM_CACHED_PSO *>(p)->data;
					reshade::log::message(reshade::log::level::info, "[ASYNC] Async D3D12 PSO proxy %llu:     cached_pso ptr=%p size=%zu hash=0x%016llx.", static_cast<unsigned long long>(id), cached_pso.pCachedBlob, cached_pso.CachedBlobSizeInBytes, static_cast<unsigned long long>(hash_bytes(cached_pso.pCachedBlob, cached_pso.CachedBlobSizeInBytes)));
					break;
				}
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SERIALIZED_ROOT_SIGNATURE:
				{
					const D3D12_SERIALIZED_ROOT_SIGNATURE_DESC &root_signature = reinterpret_cast<const D3D12_PIPELINE_STATE_STREAM_SERIALIZED_ROOT_SIGNATURE *>(p)->data;
					reshade::log::message(reshade::log::level::info, "[ASYNC] Async D3D12 PSO proxy %llu:     serialized_root_signature ptr=%p size=%zu hash=0x%016llx.", static_cast<unsigned long long>(id), root_signature.pSerializedBlob, root_signature.SerializedBlobSizeInBytes, static_cast<unsigned long long>(hash_bytes(root_signature.pSerializedBlob, root_signature.SerializedBlobSizeInBytes)));
					break;
				}
				default:
					break;
				}

				p += size;
			}
		}
	}

	void worker_loop()
	{
		for (;;)
		{
			CompileJob job;
			{
				std::unique_lock<std::mutex> lock(_queue_mutex);
				g_async_pipeline_compile_gate_cv.wait(lock, [this]() { return _stop.load(std::memory_order_acquire) || has_compile_eligible_job_unlocked(); });
				if (_stop.load(std::memory_order_acquire) && !has_compile_eligible_job_unlocked())
				{
					const size_t discarded_job_count = _compile_queue.size();
					for (CompileJob &queued_job : _compile_queue)
					{
						queued_job.proxy->set_compile_failed(E_ABORT);
						queued_job.proxy->Release();
					}
					_compile_queue.clear();
					if (discarded_job_count != 0)
						finish_pending_compile_jobs(static_cast<uint32_t>(discarded_job_count));
					return;
				}

				auto job_it = _compile_queue.end();
				for (auto it = _compile_queue.begin(); it != _compile_queue.end(); ++it)
				{
					if (it->proxy->is_compile_allowed())
					{
						job_it = it;
						break;
					}
				}
				if (job_it == _compile_queue.end())
					continue;

				job = std::move(*job_it);
				_compile_queue.erase(job_it);
			}

			if (!acquire_compile_worker_slot())
			{
				job.proxy->set_compile_failed(E_ABORT);
				job.proxy->Release();
				finish_pending_compile_jobs(1);
				continue;
			}
			com_ptr<ID3D12PipelineState> real;
			const auto compile_start = std::chrono::steady_clock::now();
			HRESULT hr = E_FAIL;
			bool handled_by_addon_events = false;
#if RESHADE_ADDON >= 2
			ID3D12PipelineState *event_pipeline = nullptr;
			if (job.stream_desc != nullptr)
				handled_by_addon_events = _device_proxy->invoke_create_and_init_pipeline_event(job.stream_desc->desc, event_pipeline, hr, true);
			else if (job.graphics_desc != nullptr)
				handled_by_addon_events = _device_proxy->invoke_create_and_init_pipeline_event(job.graphics_desc->desc, event_pipeline, hr, true);
			else if (job.compute_desc != nullptr)
				handled_by_addon_events = _device_proxy->invoke_create_and_init_pipeline_event(job.compute_desc->desc, event_pipeline, hr, true);

			if (handled_by_addon_events && SUCCEEDED(hr))
				real = com_ptr<ID3D12PipelineState>(event_pipeline, true);
#endif
			if (!handled_by_addon_events && job.stream_desc != nullptr)
			{
				com_ptr<ID3D12Device2> device2;
				hr = _device->QueryInterface(&device2);
				if (SUCCEEDED(hr))
					hr = device2->CreatePipelineState(&job.stream_desc->desc, IID_PPV_ARGS(&real));
			}
			else if (!handled_by_addon_events && job.graphics_desc != nullptr)
			{
				hr = _device->CreateGraphicsPipelineState(&job.graphics_desc->desc, IID_PPV_ARGS(&real));
			}
			else if (!handled_by_addon_events && job.compute_desc != nullptr)
			{
				hr = _device->CreateComputePipelineState(&job.compute_desc->desc, IID_PPV_ARGS(&real));
			}
			const auto compile_end = std::chrono::steady_clock::now();
			release_compile_worker_slot();
			const uint64_t compile_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(compile_end - compile_start).count());
			update_atomic_max(g_async_pipeline_diagnostics.async_compile_max_us, compile_us);
			if (SUCCEEDED(hr))
			{
				increment_async_pipeline_diagnostic(g_async_pipeline_diagnostics.async_compile_successes, 1);
				increment_async_pipeline_diagnostic(g_async_pipeline_diagnostics.async_compile_total_us, compile_us);
				job.proxy->set_real(real.get());
				if (async_pipeline_debug_diagnostics)
					reshade::log::message(reshade::log::level::info, "[ASYNC] Async D3D12 PSO proxy %llu: real PSO published (compile_ms=%.3f, addon_events=%u).", static_cast<unsigned long long>(job.id), static_cast<double>(compile_us) / 1000.0, handled_by_addon_events);
			}
			else
			{
				increment_async_pipeline_diagnostic(g_async_pipeline_diagnostics.async_compile_failures, 1);
				reshade::log::message(reshade::log::level::warning, "[ASYNC] Async D3D12 PSO proxy %llu: real PSO creation failed with error code %s; keeping fallback bound (handled_by_addon_events=%u, compile_ms=%.3f).", static_cast<unsigned long long>(job.id), reshade::log::hr_to_string(hr).c_str(), handled_by_addon_events, static_cast<double>(compile_us) / 1000.0);
				if (async_pipeline_debug_diagnostics)
				{
					if (job.stream_desc != nullptr)
						log_pipeline_state_stream_desc(job.id, job.stream_desc->desc);
					if (job.graphics_desc != nullptr)
						log_graphics_pipeline_desc(job.id, job.graphics_desc->desc);
					if (job.compute_desc != nullptr)
						log_compute_pipeline_desc(job.id, job.compute_desc->desc);
				}
				job.proxy->set_compile_failed(hr);
			}
			job.proxy->Release();
			finish_pending_compile_jobs(1);
		}
	}

public:
	void enqueue_residency_make_resident(ID3D12Pageable *pageable)
	{
		if (pageable == nullptr)
			return;

		ResidencyJob job;
		job.type = ResidencyJob::Type::MakeResident;
		job.object = pageable;
		{
			std::lock_guard<std::mutex> lock(_residency_queue_mutex);
			_residency_queue.push_back(std::move(job));
		}
		_residency_cv.notify_one();
	}

	void enqueue_residency_priority(ID3D12Pageable *pageable, D3D12_RESIDENCY_PRIORITY priority)
	{
		if (pageable == nullptr)
			return;

		ResidencyJob job;
		job.type = ResidencyJob::Type::SetPriority;
		job.object = pageable;
		job.priority = priority;
		{
			std::lock_guard<std::mutex> lock(_residency_queue_mutex);
			_residency_queue.push_back(std::move(job));
		}
		_residency_cv.notify_one();
	}

	void residency_worker_loop()
	{
		for (;;)
		{
			ResidencyJob job;
			{
				std::unique_lock<std::mutex> lock(_residency_queue_mutex);
				_residency_cv.wait(lock, [this]() { return _residency_stop || !_residency_queue.empty(); });
				if (_residency_stop && _residency_queue.empty())
					return;

				job = std::move(_residency_queue.front());
				_residency_queue.pop_front();
			}

			ID3D12Pageable *const object = job.object.get();
			if (object == nullptr)
				continue;

			if (job.type == ResidencyJob::Type::MakeResident)
			{
				_device->MakeResident(1, &object);
			}
			else
			{
				const D3D12_RESIDENCY_PRIORITY priority = job.priority;
				com_ptr<ID3D12Device1> device1;
				if (SUCCEEDED(_device->QueryInterface(&device1)))
					device1->SetResidencyPriority(1, &object, &priority);
			}
		}
	}

private:
	void enqueue_compile_job(CompileJob &&job)
	{
		bool notify_worker = false;
		{
			std::lock_guard<std::mutex> lock(_queue_mutex);
			_compile_queue.push_back(std::move(job));
			notify_worker = _compile_queue.size() <= _compile_worker_count;
			increment_async_pipeline_diagnostic(g_async_pipeline_diagnostics.queued_jobs, 1);
			update_atomic_max(g_async_pipeline_diagnostics.queue_depth_high_watermark, _compile_queue.size());
			_compile_queue.back().proxy->AddRef(); // Worker owns a reference until compilation finishes.
			_pending_compile_jobs.fetch_add(1, std::memory_order_relaxed);
		}
		if (notify_worker)
			g_async_pipeline_compile_gate_cv.notify_one();
	}

	bool acquire_compile_worker_slot()
	{
		std::unique_lock<std::mutex> lock(_compile_slot_mutex);
		_compile_slot_cv.wait(lock, [this]() {
			const uint32_t worker_limit = _compile_worker_limit.load(std::memory_order_acquire);
			return _stop.load(std::memory_order_acquire) || _active_compile_workers < worker_limit;
		});
		if (_stop.load(std::memory_order_acquire))
			return false;
		++_active_compile_workers;
		const uint32_t worker_limit = _compile_worker_limit.load(std::memory_order_acquire);
		const bool wake_next_worker = _active_compile_workers < worker_limit;
		lock.unlock();
		if (wake_next_worker)
			_compile_slot_cv.notify_one();
		return true;
	}

public:
	void register_frame_pacing_source(const void *source)
	{
		if (!async_pipeline_compile_pacing || source == nullptr)
			return;
		std::lock_guard<std::mutex> lock(_frame_pacing_mutex);
		_frame_pacing_sources.emplace(source, FramePacingSourceState { _compile_worker_count, _compile_worker_count, 0 });
	}

	void unregister_frame_pacing_source(const void *source)
	{
		if (!async_pipeline_compile_pacing || source == nullptr)
			return;

		std::lock_guard<std::mutex> lock(_frame_pacing_mutex);
		_frame_pacing_sources.erase(source);
		const uint32_t aggregate_limit = get_aggregate_frame_pacing_limit_unlocked();
		apply_compile_worker_limit_unlocked(aggregate_limit, _last_recent_frame_us.load(std::memory_order_relaxed), _last_baseline_frame_us.load(std::memory_order_relaxed), _pending_compile_jobs.load(std::memory_order_relaxed), "pacing source removed");
	}

	void reset_frame_pacing_source(const void *source)
	{
		if (!async_pipeline_compile_pacing || source == nullptr)
			return;

		std::lock_guard<std::mutex> lock(_frame_pacing_mutex);
		if (auto it = _frame_pacing_sources.find(source); it != _frame_pacing_sources.end())
		{
			it->second = FramePacingSourceState { _compile_worker_count, _compile_worker_count, 0 };
			apply_compile_worker_limit_unlocked(get_aggregate_frame_pacing_limit_unlocked(), _last_recent_frame_us.load(std::memory_order_relaxed), _last_baseline_frame_us.load(std::memory_order_relaxed), _pending_compile_jobs.load(std::memory_order_relaxed), "pacing source reset");
		}
	}

	void update_frame_pacing_source(const void *source, uint64_t recent_frame_us, uint64_t baseline_frame_us)
	{
		if (!async_pipeline_compile_pacing || source == nullptr || recent_frame_us == 0 || baseline_frame_us == 0)
			return;

		_last_recent_frame_us.store(recent_frame_us, std::memory_order_relaxed);
		_last_baseline_frame_us.store(baseline_frame_us, std::memory_order_relaxed);
		const uint32_t pending_compile_jobs = _pending_compile_jobs.load(std::memory_order_relaxed);
		std::lock_guard<std::mutex> lock(_frame_pacing_mutex);
		if (pending_compile_jobs <= _compile_worker_count)
		{
			restore_full_compile_worker_limit_unlocked(pending_compile_jobs, "pending PSOs fit available workers");
			return;
		}

		auto source_it = _frame_pacing_sources.find(source);
		if (source_it == _frame_pacing_sources.end())
			return;

		FramePacingSourceState &state = source_it->second;
		const uint32_t desired_limit = calculate_frame_pacing_limit(recent_frame_us, baseline_frame_us);
		if (desired_limit == state.requested_worker_limit)
		{
			state.candidate_worker_limit = desired_limit;
			state.candidate_samples = 0;
			return;
		}

		if (state.candidate_worker_limit != desired_limit)
		{
			state.candidate_worker_limit = desired_limit;
			state.candidate_samples = 1;
			return;
		}
		if (++state.candidate_samples < async_pipeline_pacing_confirmation_samples)
			return;

		state.requested_worker_limit = next_compile_worker_tier(state.requested_worker_limit, desired_limit);
		state.candidate_worker_limit = state.requested_worker_limit;
		state.candidate_samples = 0;
		apply_compile_worker_limit_unlocked(get_aggregate_frame_pacing_limit_unlocked(), recent_frame_us, baseline_frame_us, pending_compile_jobs, "frame pacing");
	}

private:
	uint32_t calculate_frame_pacing_limit(uint64_t recent_frame_us, uint64_t baseline_frame_us) const
	{
		const uint64_t proportional_limit = std::clamp<uint64_t>((static_cast<uint64_t>(_compile_worker_count) * baseline_frame_us + recent_frame_us / 2) / recent_frame_us, _compile_worker_lower_count, _compile_worker_count);
		uint32_t desired_limit = _compile_worker_count;
		uint64_t best_distance = proportional_limit > desired_limit ? proportional_limit - desired_limit : desired_limit - proportional_limit;
		const uint64_t middle_distance = proportional_limit > _compile_worker_middle_count ? proportional_limit - _compile_worker_middle_count : _compile_worker_middle_count - proportional_limit;
		if (middle_distance < best_distance)
		{
			desired_limit = _compile_worker_middle_count;
			best_distance = middle_distance;
		}
		const uint64_t lower_distance = proportional_limit > _compile_worker_lower_count ? proportional_limit - _compile_worker_lower_count : _compile_worker_lower_count - proportional_limit;
		if (lower_distance < best_distance)
			desired_limit = _compile_worker_lower_count;
		return desired_limit;
	}

	uint32_t next_compile_worker_tier(uint32_t current_limit, uint32_t desired_limit) const
	{
		if (desired_limit < current_limit)
			return current_limit > _compile_worker_middle_count ? _compile_worker_middle_count : _compile_worker_lower_count;
		if (desired_limit > current_limit)
			return current_limit < _compile_worker_middle_count ? _compile_worker_middle_count : _compile_worker_count;
		return current_limit;
	}

	uint32_t get_aggregate_frame_pacing_limit_unlocked() const
	{
		uint32_t limit = _compile_worker_count;
		for (const auto &[source, state] : _frame_pacing_sources)
			limit = std::min(limit, state.requested_worker_limit);
		return limit;
	}

	void restore_full_compile_worker_limit_unlocked(uint32_t pending_compile_jobs, const char *reason)
	{
		for (auto &[source, state] : _frame_pacing_sources)
			state = FramePacingSourceState { _compile_worker_count, _compile_worker_count, 0 };
		apply_compile_worker_limit_unlocked(_compile_worker_count, _last_recent_frame_us.load(std::memory_order_relaxed), _last_baseline_frame_us.load(std::memory_order_relaxed), pending_compile_jobs, reason);
	}

	void apply_compile_worker_limit_unlocked(uint32_t new_limit, uint64_t recent_frame_us, uint64_t baseline_frame_us, uint32_t pending_compile_jobs, const char *reason)
	{
		const uint32_t old_limit = _compile_worker_limit.exchange(new_limit, std::memory_order_acq_rel);
		if (new_limit == old_limit)
			return;

		const bool reducing = new_limit < old_limit;
		const reshade::log::level log_level = reducing ? reshade::log::level::warning : reshade::log::level::info;
		const char *const transition = reducing ? (old_limit == _compile_worker_count ? "detected" : "intensified") : (new_limit == _compile_worker_count ? "cleared" : "eased");
		reshade::log::message(log_level,
			"[ASYNC] Async D3D12 PSO frame congestion %s (%s): recent=%.3f ms, baseline=%.3f ms, pending_psos=%u, worker_limit=%u->%u (tiers=%u/%u/%u).",
			transition, reason, static_cast<double>(recent_frame_us) / 1000.0, static_cast<double>(baseline_frame_us) / 1000.0, pending_compile_jobs, old_limit, new_limit, _compile_worker_count, _compile_worker_middle_count, _compile_worker_lower_count);
		if (new_limit > old_limit)
			_compile_slot_cv.notify_one();
	}

	void finish_pending_compile_jobs(uint32_t job_count)
	{
		const uint32_t previous_count = _pending_compile_jobs.fetch_sub(job_count, std::memory_order_relaxed);
		assert(previous_count >= job_count);
	}

	void release_compile_worker_slot()
	{
		{
			std::lock_guard<std::mutex> lock(_compile_slot_mutex);
			--_active_compile_workers;
		}
		_compile_slot_cv.notify_one();
	}

	bool has_compile_eligible_job_unlocked() const
	{
		for (const CompileJob &job : _compile_queue)
			if (job.proxy->is_compile_allowed())
				return true;
		return false;
	}

	D3D12Device *_device_proxy = nullptr;
	ID3D12Device *_device = nullptr;
	std::atomic<uint64_t> _next_proxy_id = 1;
	std::mutex _dummy_root_signature_mutex;
	com_ptr<ID3D12RootSignature> _dummy_root_signature;
	std::mutex _fallback_mutex;
	GraphicsFallbackKey _global_fallback_key = {};
	bool _has_global_fallback_key = false;
	std::unordered_map<GraphicsFallbackKey, com_ptr<ID3D12PipelineState>, GraphicsFallbackKeyHash> _fallback_cache;
	std::mutex _mesh_fallback_mutex;
	com_ptr<ID3D12PipelineState> _mesh_fallback;
	std::mutex _compute_fallback_mutex;
	ComputeFallbackKey _global_compute_fallback_key = {};
	bool _has_global_compute_fallback_key = false;
	std::unordered_map<ComputeFallbackKey, com_ptr<ID3D12PipelineState>, ComputeFallbackKeyHash> _compute_fallback_cache;
	std::mutex _queue_mutex;
	std::deque<CompileJob> _compile_queue;
	std::atomic_bool _stop = false;
	std::atomic<uint32_t> _pending_compile_jobs = 0;
	std::mutex _compile_slot_mutex;
	std::condition_variable _compile_slot_cv;
	uint32_t _active_compile_workers = 0;
	const uint32_t _hardware_thread_count = get_async_pipeline_hardware_thread_count();
	const uint32_t _compile_worker_count = get_async_pipeline_compile_worker_count(_hardware_thread_count, async_pipeline_compile_worker_thread_percentage);
	const uint32_t _compile_worker_middle_count = std::min(_compile_worker_count, get_async_pipeline_compile_worker_count(_hardware_thread_count, async_pipeline_compile_middle_thread_percentage));
	const uint32_t _compile_worker_lower_count = std::min(_compile_worker_middle_count, get_async_pipeline_compile_worker_count(_hardware_thread_count, async_pipeline_compile_lower_thread_percentage));
	std::atomic<uint32_t> _compile_worker_limit { _compile_worker_count };
	std::atomic<uint64_t> _last_recent_frame_us = 0;
	std::atomic<uint64_t> _last_baseline_frame_us = 0;
	std::mutex _frame_pacing_mutex;
	std::unordered_map<const void *, FramePacingSourceState> _frame_pacing_sources;
	std::vector<std::thread> _workers;
	std::mutex _residency_queue_mutex;
	std::condition_variable _residency_cv;
	std::deque<ResidencyJob> _residency_queue;
	bool _residency_stop = false;
	std::thread _residency_worker;
};

class D3D12AsyncPipelineFramePacing
{
public:
	explicit D3D12AsyncPipelineFramePacing(D3D12AsyncPipelineManager *manager) :
		_manager(manager)
	{
		assert(_manager != nullptr);
		_manager->register_frame_pacing_source(this);
	}
	~D3D12AsyncPipelineFramePacing()
	{
		_manager->unregister_frame_pacing_source(this);
	}

	void note_present()
	{
		const uint64_t now_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
		const uint64_t previous_us = _last_present_us;
		_last_present_us = now_us;
		if (previous_us == 0 || now_us <= previous_us)
			return;

		const uint64_t frame_us = now_us - previous_us;
		if (frame_us > 1'000'000) // Ignore debugger pauses, loading stalls, and long periods without presentation.
		{
			_recent_frame_us = 0;
			_baseline_frame_us = 0;
			_last_control_update_us = now_us;
			_manager->reset_frame_pacing_source(this);
			return;
		}

		_recent_frame_us = _recent_frame_us == 0 ? frame_us : (_recent_frame_us * 7 + frame_us) / 8;
		// Learn improvements quickly, but adapt upward slowly so a hitch does not become the new normal.
		_baseline_frame_us = _baseline_frame_us == 0 ? frame_us : frame_us < _baseline_frame_us ? (_baseline_frame_us * 7 + frame_us) / 8 : (_baseline_frame_us * 63 + frame_us) / 64;
		if (_last_control_update_us != 0 && now_us - _last_control_update_us < async_pipeline_pacing_control_interval_us)
			return;

		_last_control_update_us = now_us;
		_manager->update_frame_pacing_source(this, _recent_frame_us, _baseline_frame_us);
	}

private:
	D3D12AsyncPipelineManager *const _manager;
	uint64_t _last_present_us = 0;
	uint64_t _last_control_update_us = 0;
	uint64_t _recent_frame_us = 0;
	uint64_t _baseline_frame_us = 0;
};

static void enqueue_async_residency_make_resident(D3D12AsyncPipelineManager *manager, ID3D12Pageable *pageable)
{
	if (manager != nullptr)
		manager->enqueue_residency_make_resident(pageable);
}

static void enqueue_async_residency_priority(D3D12AsyncPipelineManager *manager, ID3D12Pageable *pageable, D3D12_RESIDENCY_PRIORITY priority)
{
	if (manager != nullptr)
		manager->enqueue_residency_priority(pageable, priority);
}

void destroy_d3d12_async_pipeline_manager(D3D12AsyncPipelineManager *manager)
{
	delete manager;
}

D3D12AsyncPipelineManager *create_d3d12_async_pipeline_manager(D3D12Device *device_proxy, ID3D12Device *device)
{
	if constexpr (!async_pipeline_enabled)
		return nullptr;

	load_async_pipeline_config();
	return new D3D12AsyncPipelineManager(device_proxy, device);
}

HRESULT create_async_graphics_pipeline_state(D3D12AsyncPipelineManager *manager, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc, REFIID riid, void **pipeline_state)
{
	if constexpr (!async_pipeline_enabled)
		return E_FAIL;

	return manager != nullptr ? manager->create_graphics_pipeline_state(desc, riid, pipeline_state) : E_FAIL;
}

HRESULT create_async_compute_pipeline_state(D3D12AsyncPipelineManager *manager, const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc, REFIID riid, void **pipeline_state)
{
	if constexpr (!async_pipeline_enabled)
		return E_FAIL;

	return manager != nullptr ? manager->create_compute_pipeline_state(desc, riid, pipeline_state) : E_FAIL;
}

HRESULT create_async_pipeline_state_stream(D3D12AsyncPipelineManager *manager, const D3D12_PIPELINE_STATE_STREAM_DESC *desc, REFIID riid, void **pipeline_state)
{
	if constexpr (!async_pipeline_enabled)
		return E_FAIL;

	return manager != nullptr ? manager->create_pipeline_state_stream(desc, riid, pipeline_state) : E_FAIL;
}

void note_async_pipeline_state_stream_create(REFIID riid)
{
	if constexpr (!async_pipeline_enabled)
		return;

	const uint64_t stream_create_count = increment_async_pipeline_diagnostic(g_async_pipeline_diagnostics.stream_create_calls, 1);
	if (riid == __uuidof(ID3D12PipelineState1))
		increment_async_pipeline_diagnostic(g_async_pipeline_diagnostics.stream_pipeline_state1_requests, 1);
	if (should_log_periodic(stream_create_count))
		log_async_pipeline_diagnostics("stream-create");
}

void note_async_pipeline_fallback_draw_skip()
{
	if constexpr (!async_pipeline_enabled)
		return;

	increment_async_pipeline_diagnostic(g_async_pipeline_diagnostics.fallback_draw_skips, 1);
}

void note_async_pipeline_fallback_promotion()
{
	if constexpr (!async_pipeline_enabled)
		return;

	increment_async_pipeline_diagnostic(g_async_pipeline_diagnostics.fallback_promotions, 1);
}

D3D12AsyncPipelineFramePacing *create_async_pipeline_frame_pacing(D3D12AsyncPipelineManager *manager)
{
	if constexpr (!async_pipeline_enabled)
		return nullptr;
	return manager != nullptr && async_pipeline_compile_pacing ? new D3D12AsyncPipelineFramePacing(manager) : nullptr;
}

void destroy_async_pipeline_frame_pacing(D3D12AsyncPipelineFramePacing *frame_pacing)
{
	delete frame_pacing;
}

void note_async_pipeline_frame_present(D3D12AsyncPipelineFramePacing *frame_pacing)
{
	if (frame_pacing != nullptr)
		frame_pacing->note_present();
}

bool should_skip_async_pipeline_fallback_commands()
{
	return async_pipeline_fallback_mode == AsyncPipelineFallbackMode::skip_commands;
}

void note_async_pageable_make_resident(ID3D12Pageable *pageable)
{
	if constexpr (!async_pipeline_enabled)
		return;

	D3D12AsyncPipelineProxy *proxy = nullptr;
	if (pageable != nullptr && SUCCEEDED(pageable->QueryInterface(__uuidof(D3D12AsyncPipelineProxy), reinterpret_cast<void **>(&proxy))))
	{
		proxy->note_make_resident_requested();
		release_async_pipeline_state_proxy(proxy);
	}
}

void note_async_pageable_evict(ID3D12Pageable *pageable)
{
	if constexpr (!async_pipeline_enabled)
		return;

	D3D12AsyncPipelineProxy *proxy = nullptr;
	if (pageable != nullptr && SUCCEEDED(pageable->QueryInterface(__uuidof(D3D12AsyncPipelineProxy), reinterpret_cast<void **>(&proxy))))
	{
		proxy->note_evicted();
		release_async_pipeline_state_proxy(proxy);
	}
}

void note_async_pageable_residency_priority(ID3D12Pageable *pageable, D3D12_RESIDENCY_PRIORITY priority)
{
	if constexpr (!async_pipeline_enabled)
		return;

	D3D12AsyncPipelineProxy *proxy = nullptr;
	if (pageable != nullptr && SUCCEEDED(pageable->QueryInterface(__uuidof(D3D12AsyncPipelineProxy), reinterpret_cast<void **>(&proxy))))
	{
		proxy->note_residency_priority(priority);
		release_async_pipeline_state_proxy(proxy);
	}
}

D3D12AsyncPipelineProxy *get_async_pipeline_state_proxy(ID3D12PipelineState *pipeline_state)
{
	if constexpr (!async_pipeline_enabled)
		return nullptr;

	return pipeline_state != nullptr ? try_addref_async_pipeline_proxy(pipeline_state) : nullptr;
}

void release_async_pipeline_state_proxy(D3D12AsyncPipelineProxy *proxy)
{
	if (proxy != nullptr)
		proxy->Release();
}

ID3D12PipelineState *resolve_async_pipeline_state_proxy(D3D12AsyncPipelineProxy *proxy, bool *resolved_to_fallback)
{
	if (resolved_to_fallback != nullptr)
		*resolved_to_fallback = false;
	if constexpr (!async_pipeline_enabled)
		return nullptr;

	return proxy != nullptr ? proxy->current_native_for_bind(resolved_to_fallback) : nullptr;
}

HRESULT store_async_pipeline_state_proxy_or_defer(D3D12AsyncPipelineProxy *proxy, ID3D12PipelineLibrary *library, LPCWSTR name)
{
	if constexpr (!async_pipeline_enabled)
		return E_POINTER;

	return proxy != nullptr ? proxy->store_or_defer(library, name) : E_POINTER;
}

ID3D12PipelineState *resolve_async_pipeline_state(ID3D12PipelineState *pipeline_state)
{
	return resolve_async_pipeline_state(pipeline_state, nullptr);
}

ID3D12PipelineState *resolve_async_pipeline_state(ID3D12PipelineState *pipeline_state, bool *resolved_to_fallback)
{
	if (resolved_to_fallback != nullptr)
		*resolved_to_fallback = false;

	if (pipeline_state == nullptr)
		return nullptr;
	if constexpr (!async_pipeline_enabled)
		return pipeline_state;

	D3D12AsyncPipelineProxy *const proxy = get_async_pipeline_state_proxy(pipeline_state);
	if (proxy != nullptr)
	{
		ID3D12PipelineState *const native = proxy->current_native_for_bind(resolved_to_fallback);
		release_async_pipeline_state_proxy(proxy);
		return native;
	}

	return pipeline_state;
}

ID3D12Pageable *resolve_async_pageable(ID3D12Pageable *pageable)
{
	if (pageable == nullptr)
		return nullptr;
	if constexpr (!async_pipeline_enabled)
		return pageable;

	D3D12AsyncPipelineProxy *proxy = nullptr;
	if (SUCCEEDED(pageable->QueryInterface(__uuidof(D3D12AsyncPipelineProxy), reinterpret_cast<void **>(&proxy))))
	{
		increment_async_pipeline_diagnostic(g_async_pipeline_diagnostics.pageable_proxy_resolves, 1);
		ID3D12Pageable *const native = proxy->current_native();
		release_async_pipeline_state_proxy(proxy);
		return native;
	}

	return pageable;
}
