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
	std::atomic<uint64_t> fallback_draw_skips = 0;
	std::atomic<uint64_t> fallback_promotions = 0;
	std::atomic<uint64_t> deferred_store_queued = 0;
	std::atomic<uint64_t> deferred_store_replayed = 0;
};

static AsyncPipelineDiagnostics g_async_pipeline_diagnostics;

static constexpr bool async_pipeline_debug_diagnostics = false;
static constexpr bool use_global_sentinel_fallback_pso = true;
static constexpr bool publish_fallback_pipelines_to_addons = false;
static constexpr bool allow_addons_to_modify_fallback_pipelines = false;
static constexpr bool async_graphics_fallback_enabled = true;
static constexpr bool async_compute_fallback_enabled = true;
static constexpr uint64_t min_fallback_binds_before_real = 0;
static constexpr uint64_t min_fallback_binds_before_compile = 0;
static constexpr unsigned int async_pipeline_compile_worker_thread_percentage = 75;

static size_t get_async_pipeline_compile_worker_count()
{
	const unsigned int hardware_threads = std::thread::hardware_concurrency();
	const size_t hardware_thread_count = hardware_threads != 0 ? static_cast<size_t>(hardware_threads) : 1;
	const unsigned int percentage = async_pipeline_compile_worker_thread_percentage != 0 ? async_pipeline_compile_worker_thread_percentage : 100;
	return std::max<size_t>(1, (hardware_thread_count * percentage) / 100);
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
	uint64_t current = target.load(std::memory_order_relaxed);
	while (current < value && !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {}
}

static bool should_log_periodic(uint64_t count)
{
	if constexpr (!async_pipeline_debug_diagnostics)
		return false;
	return count <= 16 || (count % 512) == 0;
}

static void log_async_pipeline_diagnostics_row(const char *name_a, uint64_t value_a, const char *name_b, uint64_t value_b)
{
	reshade::log::message(reshade::log::level::info, "  | %-24s | %-16llu | %-24s | %-16llu |",
		name_a,
		static_cast<unsigned long long>(value_a),
		name_b,
		static_cast<unsigned long long>(value_b));
}

static void log_async_pipeline_diagnostics(const char *reason)
{
	const uint64_t compile_successes = g_async_pipeline_diagnostics.async_compile_successes.load(std::memory_order_relaxed);
	const uint64_t compile_total_us = g_async_pipeline_diagnostics.async_compile_total_us.load(std::memory_order_relaxed);
	reshade::log::message(reshade::log::level::info, "Async D3D12 PSO %s diagnostics:", reason);
	reshade::log::message(reshade::log::level::info, "  +--------------------------+------------------+--------------------------+------------------+");
	reshade::log::message(reshade::log::level::info, "  | Counter                  | Value            | Counter                  | Value            |");
	reshade::log::message(reshade::log::level::info, "  +--------------------------+------------------+--------------------------+------------------+");
	log_async_pipeline_diagnostics_row("graphics_create", g_async_pipeline_diagnostics.graphics_create_calls.load(std::memory_order_relaxed), "graphics_iid1", g_async_pipeline_diagnostics.graphics_pipeline_state1_requests.load(std::memory_order_relaxed));
	log_async_pipeline_diagnostics_row("compute_create", g_async_pipeline_diagnostics.compute_create_calls.load(std::memory_order_relaxed), "compute_iid1", g_async_pipeline_diagnostics.compute_pipeline_state1_requests.load(std::memory_order_relaxed));
	log_async_pipeline_diagnostics_row("stream_create", g_async_pipeline_diagnostics.stream_create_calls.load(std::memory_order_relaxed), "stream_iid1", g_async_pipeline_diagnostics.stream_pipeline_state1_requests.load(std::memory_order_relaxed));
	log_async_pipeline_diagnostics_row("async_proxies", g_async_pipeline_diagnostics.async_proxy_creates.load(std::memory_order_relaxed), "sync_interface", g_async_pipeline_diagnostics.sync_unsupported_interface.load(std::memory_order_relaxed));
	log_async_pipeline_diagnostics_row("sync_desc", g_async_pipeline_diagnostics.sync_unsupported_desc.load(std::memory_order_relaxed), "sync_fallback_fail", g_async_pipeline_diagnostics.sync_fallback_failure.load(std::memory_order_relaxed));
	log_async_pipeline_diagnostics_row("fallback_hits", g_async_pipeline_diagnostics.fallback_cache_hits.load(std::memory_order_relaxed), "fallback_misses", g_async_pipeline_diagnostics.fallback_cache_misses.load(std::memory_order_relaxed));
	log_async_pipeline_diagnostics_row(use_global_sentinel_fallback_pso ? "sentinel_fallbacks" : "fallback_buckets", g_async_pipeline_diagnostics.fallback_buckets_created.load(std::memory_order_relaxed), "fallback_failures", g_async_pipeline_diagnostics.fallback_creation_failures.load(std::memory_order_relaxed));
	log_async_pipeline_diagnostics_row("queued", g_async_pipeline_diagnostics.queued_jobs.load(std::memory_order_relaxed), "queue_hwm", g_async_pipeline_diagnostics.queue_depth_high_watermark.load(std::memory_order_relaxed));
	log_async_pipeline_diagnostics_row("compile_ok", compile_successes, "compile_fail", g_async_pipeline_diagnostics.async_compile_failures.load(std::memory_order_relaxed));
	log_async_pipeline_diagnostics_row("compile_avg_us", compile_successes != 0 ? compile_total_us / compile_successes : 0, "compile_max_us", g_async_pipeline_diagnostics.async_compile_max_us.load(std::memory_order_relaxed));
	log_async_pipeline_diagnostics_row("proxy_resolves", g_async_pipeline_diagnostics.proxy_resolves.load(std::memory_order_relaxed), "resolves_fallback", g_async_pipeline_diagnostics.proxy_resolves_to_fallback.load(std::memory_order_relaxed));
	log_async_pipeline_diagnostics_row("resolves_real", g_async_pipeline_diagnostics.proxy_resolves_to_real.load(std::memory_order_relaxed), "pageable_resolves", g_async_pipeline_diagnostics.pageable_proxy_resolves.load(std::memory_order_relaxed));
	log_async_pipeline_diagnostics_row("get_cached_blob", g_async_pipeline_diagnostics.get_cached_blob_calls.load(std::memory_order_relaxed), "fallback_draw_skips", g_async_pipeline_diagnostics.fallback_draw_skips.load(std::memory_order_relaxed));
	log_async_pipeline_diagnostics_row("fallback_promote", g_async_pipeline_diagnostics.fallback_promotions.load(std::memory_order_relaxed), "store_deferred", g_async_pipeline_diagnostics.deferred_store_queued.load(std::memory_order_relaxed));
	log_async_pipeline_diagnostics_row("store_replayed", g_async_pipeline_diagnostics.deferred_store_replayed.load(std::memory_order_relaxed), "unused", 0);
	reshade::log::message(reshade::log::level::info, "  +--------------------------+------------------+--------------------------+------------------+");
}

static void note_async_pipeline_unsupported_desc(const char *reason)
{
	const uint64_t count = g_async_pipeline_diagnostics.sync_unsupported_desc.fetch_add(1, std::memory_order_relaxed) + 1;
	if constexpr (async_pipeline_debug_diagnostics)
		if (should_log_periodic(count))
		reshade::log::message(reshade::log::level::info, "Async D3D12 PSO: synchronous creation because desc is unsupported: %s (sync_desc=%llu).", reason, static_cast<unsigned long long>(count));
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
		ID3D12PipelineState *const published_real = _published_real.load(std::memory_order_acquire);
		const bool is_real = published_real != nullptr && current_native() == published_real;
		g_async_pipeline_diagnostics.get_cached_blob_calls.fetch_add(1, std::memory_order_relaxed);
		if constexpr (async_pipeline_debug_diagnostics)
			reshade::log::message(reshade::log::level::info, "Async D3D12 PSO proxy %llu: GetCachedBlob called while bound to %s PSO.", static_cast<unsigned long long>(_id), is_real ? "real" : "fallback");
		return current_native()->GetCachedBlob(ppBlob);
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

		g_async_pipeline_diagnostics.deferred_store_queued.fetch_add(1, std::memory_order_relaxed);
		return S_OK;
	}
	ID3D12PipelineState *current_native_for_bind(bool *resolved_to_fallback = nullptr)
	{
		if (resolved_to_fallback != nullptr)
			*resolved_to_fallback = false;

		ID3D12PipelineState *const native = current_native();
		g_async_pipeline_diagnostics.proxy_resolves.fetch_add(1, std::memory_order_relaxed);
		if (ID3D12PipelineState *const real = _published_real.load(std::memory_order_acquire); real != nullptr && native == real)
		{
			_real_bind_count.fetch_add(1, std::memory_order_relaxed);
			g_async_pipeline_diagnostics.proxy_resolves_to_real.fetch_add(1, std::memory_order_relaxed);
		}
		else
		{
			if (resolved_to_fallback != nullptr)
				*resolved_to_fallback = true;
			const uint64_t fallback_bind_count = _fallback_bind_count.fetch_add(1, std::memory_order_relaxed) + 1;
			g_async_pipeline_diagnostics.proxy_resolves_to_fallback.fetch_add(1, std::memory_order_relaxed);
			if (fallback_bind_count >= min_fallback_binds_before_compile)
				g_async_pipeline_compile_gate_cv.notify_all();
			if (fallback_bind_count >= min_fallback_binds_before_real)
				publish_pending_real();
		}
		return native;
	}
	bool is_compile_allowed() const
	{
		return _fallback_bind_count.load(std::memory_order_relaxed) >= min_fallback_binds_before_compile;
	}
	void set_real(ID3D12PipelineState *real)
	{
		assert(real != nullptr);
		{
			std::lock_guard<std::mutex> lock(_real_mutex);
			_pending_real = real;
		}
		replay_deferred_stores(real);
		replay_residency_to_real(real);
		if (_fallback_bind_count.load(std::memory_order_relaxed) >= min_fallback_binds_before_real)
			publish_pending_real();
	}
	void set_compile_failed()
	{
		std::lock_guard<std::mutex> lock(_deferred_store_mutex);
		_deferred_stores.clear();
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
				g_async_pipeline_diagnostics.deferred_store_replayed.fetch_add(1, std::memory_order_relaxed);
			}
			else if constexpr (async_pipeline_debug_diagnostics)
			{
				reshade::log::message(reshade::log::level::warning, "Async D3D12 PSO proxy %llu: deferred StorePipeline failed with error code %s.", static_cast<unsigned long long>(_id), reshade::log::hr_to_string(hr).c_str());
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
	std::vector<uint8_t> vs, ps, ds, hs, gs, cached_pso;
	std::vector<D3D12_INPUT_ELEMENT_DESC> input_elements;
	std::vector<std::string> semantic_names;

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
	}
};

struct CopiedComputePipelineDesc
{
	D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
	std::vector<uint8_t> cs, cached_pso;

	explicit CopiedComputePipelineDesc(const D3D12_COMPUTE_PIPELINE_STATE_DESC &source)
	{
		desc = source;
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
	if (source.DepthBoundsTestEnable != FALSE)
		return false;
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

static bool convert_pipeline_state_stream_to_graphics_desc(const D3D12_PIPELINE_STATE_STREAM_DESC &stream_desc, D3D12_GRAPHICS_PIPELINE_STATE_DESC &desc, const char *&unsupported_reason, bool &has_cached_pso)
{
	unsupported_reason = "none";
	has_cached_pso = false;
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
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
			unsupported_reason = "stream is compute (CS)";
			return false;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
			unsupported_reason = "stream uses amplification shader (AS)";
			return false;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS:
			unsupported_reason = "stream uses mesh shader (MS)";
			return false;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING:
			unsupported_reason = "stream uses view instancing";
			return false;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2:
			unsupported_reason = "stream uses DEPTH_STENCIL2";
			return false;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER1:
			unsupported_reason = "stream uses RASTERIZER1";
			return false;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER2:
			unsupported_reason = "stream uses RASTERIZER2";
			return false;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SERIALIZED_ROOT_SIGNATURE:
			unsupported_reason = "stream uses serialized root signature";
			return false;
		default:
			unsupported_reason = "stream uses unknown subobject";
			return false;
		}
	}

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
		for (size_t i = 0; i < _compile_worker_count; ++i)
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

		if constexpr (async_pipeline_debug_diagnostics)
		{
			log_async_pipeline_diagnostics("summary");
		}
		else
		{
			reshade::log::message(reshade::log::level::info,
				"Async D3D12 PSO summary: graphics=%llu, compute=%llu, stream=%llu, proxies=%llu, fallback_resolves=%llu, real_resolves=%llu, skipped=%llu, promoted=%llu, compile_ok=%llu, compile_fail=%llu.",
				static_cast<unsigned long long>(g_async_pipeline_diagnostics.graphics_create_calls.load(std::memory_order_relaxed)),
				static_cast<unsigned long long>(g_async_pipeline_diagnostics.compute_create_calls.load(std::memory_order_relaxed)),
				static_cast<unsigned long long>(g_async_pipeline_diagnostics.stream_create_calls.load(std::memory_order_relaxed)),
				static_cast<unsigned long long>(g_async_pipeline_diagnostics.async_proxy_creates.load(std::memory_order_relaxed)),
				static_cast<unsigned long long>(g_async_pipeline_diagnostics.proxy_resolves_to_fallback.load(std::memory_order_relaxed)),
				static_cast<unsigned long long>(g_async_pipeline_diagnostics.proxy_resolves_to_real.load(std::memory_order_relaxed)),
				static_cast<unsigned long long>(g_async_pipeline_diagnostics.fallback_draw_skips.load(std::memory_order_relaxed)),
				static_cast<unsigned long long>(g_async_pipeline_diagnostics.fallback_promotions.load(std::memory_order_relaxed)),
				static_cast<unsigned long long>(g_async_pipeline_diagnostics.async_compile_successes.load(std::memory_order_relaxed)),
				static_cast<unsigned long long>(g_async_pipeline_diagnostics.async_compile_failures.load(std::memory_order_relaxed)));
		}

		_device->Release();
	}

	HRESULT create_graphics_pipeline_state(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc, REFIID riid, void **pipeline_state)
	{
		const uint64_t graphics_create_count = g_async_pipeline_diagnostics.graphics_create_calls.fetch_add(1, std::memory_order_relaxed) + 1;
		if (should_log_periodic(graphics_create_count))
			log_async_pipeline_diagnostics("graphics-create");

		if (pipeline_state == nullptr)
			return E_POINTER;
		*pipeline_state = nullptr;

		if (riid != __uuidof(ID3D12PipelineState))
		{
			if (riid == __uuidof(ID3D12PipelineState1))
				g_async_pipeline_diagnostics.graphics_pipeline_state1_requests.fetch_add(1, std::memory_order_relaxed);
			g_async_pipeline_diagnostics.sync_unsupported_interface.fetch_add(1, std::memory_order_relaxed);
			return _device->CreateGraphicsPipelineState(desc, riid, pipeline_state);
		}

		if constexpr (!async_graphics_fallback_enabled)
		{
			note_async_pipeline_unsupported_desc("graphics async fallback disabled for compute-only test");
			return _device->CreateGraphicsPipelineState(desc, riid, pipeline_state);
		}

		const char *unsupported_reason = "none";
		D3D12_GRAPHICS_PIPELINE_STATE_DESC working_desc = *desc;
		if (!is_supported(working_desc, unsupported_reason))
		{
			note_async_pipeline_unsupported_desc(unsupported_reason);
			return _device->CreateGraphicsPipelineState(desc, riid, pipeline_state);
		}

		com_ptr<ID3D12PipelineState> fallback;
		const HRESULT fallback_hr = get_or_create_fallback(working_desc, fallback);
		if (FAILED(fallback_hr))
		{
			g_async_pipeline_diagnostics.sync_fallback_failure.fetch_add(1, std::memory_order_relaxed);
			reshade::log::message(reshade::log::level::warning, "Async D3D12 PSO: fallback creation failed with error code %s; falling back to synchronous real PSO creation.", reshade::log::hr_to_string(fallback_hr).c_str());
			return _device->CreateGraphicsPipelineState(desc, riid, pipeline_state);
		}

		const uint64_t id = _next_proxy_id.fetch_add(1, std::memory_order_relaxed);
		D3D12AsyncPipelineProxy *const proxy = new D3D12AsyncPipelineProxy(this, _device, fallback.get(), id);

		bool notify_worker = false;
		{
			std::lock_guard<std::mutex> lock(_queue_mutex);
			CompileJob job = {};
			job.proxy = proxy;
			job.graphics_desc = std::make_unique<CopiedGraphicsPipelineDesc>(working_desc);
			job.id = id;
			_compile_queue.push_back(std::move(job));
			notify_worker = _compile_queue.size() <= _compile_worker_count;
			g_async_pipeline_diagnostics.queued_jobs.fetch_add(1, std::memory_order_relaxed);
			update_atomic_max(g_async_pipeline_diagnostics.queue_depth_high_watermark, _compile_queue.size());
			proxy->AddRef(); // Worker owns a reference until compile finishes.
		}
		if (notify_worker)
			g_async_pipeline_compile_gate_cv.notify_one();

		g_async_pipeline_diagnostics.async_proxy_creates.fetch_add(1, std::memory_order_relaxed);
		*pipeline_state = proxy;
		return S_OK;
	}

	HRESULT create_compute_pipeline_state(const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc, REFIID riid, void **pipeline_state)
	{
		const uint64_t compute_create_count = g_async_pipeline_diagnostics.compute_create_calls.fetch_add(1, std::memory_order_relaxed) + 1;
		if (should_log_periodic(compute_create_count))
			log_async_pipeline_diagnostics("compute-create");

		if (pipeline_state == nullptr)
			return E_POINTER;
		*pipeline_state = nullptr;

		if (riid != __uuidof(ID3D12PipelineState))
		{
			if (riid == __uuidof(ID3D12PipelineState1))
				g_async_pipeline_diagnostics.compute_pipeline_state1_requests.fetch_add(1, std::memory_order_relaxed);
			g_async_pipeline_diagnostics.sync_unsupported_interface.fetch_add(1, std::memory_order_relaxed);
			return _device->CreateComputePipelineState(desc, riid, pipeline_state);
		}

		if constexpr (!async_compute_fallback_enabled)
		{
			note_async_pipeline_unsupported_desc("compute async fallback disabled");
			return _device->CreateComputePipelineState(desc, riid, pipeline_state);
		}

		const char *unsupported_reason = "none";
		D3D12_COMPUTE_PIPELINE_STATE_DESC working_desc = *desc;
		if (!is_supported(working_desc, unsupported_reason))
		{
			note_async_pipeline_unsupported_desc(unsupported_reason);
			return _device->CreateComputePipelineState(desc, riid, pipeline_state);
		}

		com_ptr<ID3D12PipelineState> fallback;
		const HRESULT fallback_hr = get_or_create_compute_fallback(working_desc, fallback);
		if (FAILED(fallback_hr))
		{
			g_async_pipeline_diagnostics.sync_fallback_failure.fetch_add(1, std::memory_order_relaxed);
			reshade::log::message(reshade::log::level::warning, "Async D3D12 PSO: compute fallback creation failed with error code %s; falling back to synchronous real PSO creation.", reshade::log::hr_to_string(fallback_hr).c_str());
			return _device->CreateComputePipelineState(desc, riid, pipeline_state);
		}

		const uint64_t id = _next_proxy_id.fetch_add(1, std::memory_order_relaxed);
		D3D12AsyncPipelineProxy *const proxy = new D3D12AsyncPipelineProxy(this, _device, fallback.get(), id);

		bool notify_worker = false;
		{
			std::lock_guard<std::mutex> lock(_queue_mutex);
			CompileJob job = {};
			job.proxy = proxy;
			job.compute_desc = std::make_unique<CopiedComputePipelineDesc>(working_desc);
			job.id = id;
			_compile_queue.push_back(std::move(job));
			notify_worker = _compile_queue.size() <= _compile_worker_count;
			g_async_pipeline_diagnostics.queued_jobs.fetch_add(1, std::memory_order_relaxed);
			update_atomic_max(g_async_pipeline_diagnostics.queue_depth_high_watermark, _compile_queue.size());
			proxy->AddRef(); // Worker owns a reference until compile finishes.
		}
		if (notify_worker)
			g_async_pipeline_compile_gate_cv.notify_one();

		g_async_pipeline_diagnostics.async_proxy_creates.fetch_add(1, std::memory_order_relaxed);
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
			g_async_pipeline_diagnostics.sync_unsupported_interface.fetch_add(1, std::memory_order_relaxed);
			return device2->CreatePipelineState(stream_desc, riid, pipeline_state);
		}

		D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics_desc = {};
		bool graphics_has_cached_pso = false;
		const char *graphics_unsupported_reason = "none";
		D3D12_COMPUTE_PIPELINE_STATE_DESC compute_desc = {};
		bool compute_has_cached_pso = false;
		const char *compute_unsupported_reason = "none";
		if (convert_pipeline_state_stream_to_graphics_desc(*stream_desc, graphics_desc, graphics_unsupported_reason, graphics_has_cached_pso))
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
			if (!is_supported(graphics_desc, graphics_unsupported_reason))
			{
				note_async_pipeline_unsupported_desc(graphics_unsupported_reason);
				goto sync_with_events;
			}

			com_ptr<ID3D12PipelineState> fallback;
			const HRESULT fallback_hr = get_or_create_fallback(graphics_desc, fallback);
			if (FAILED(fallback_hr))
			{
				g_async_pipeline_diagnostics.sync_fallback_failure.fetch_add(1, std::memory_order_relaxed);
				reshade::log::message(reshade::log::level::warning, "Async D3D12 PSO: graphics fallback creation failed with error code %s; falling back to synchronous real PSO creation.", reshade::log::hr_to_string(fallback_hr).c_str());
				goto sync_with_events;
			}

			const uint64_t id = _next_proxy_id.fetch_add(1, std::memory_order_relaxed);
			D3D12AsyncPipelineProxy *const proxy = new D3D12AsyncPipelineProxy(this, _device, fallback.get(), id);

			bool notify_worker = false;
			{
				std::lock_guard<std::mutex> lock(_queue_mutex);
				CompileJob job = {};
				job.proxy = proxy;
				job.graphics_desc = std::make_unique<CopiedGraphicsPipelineDesc>(graphics_desc);
				job.id = id;
				_compile_queue.push_back(std::move(job));
				notify_worker = _compile_queue.size() <= _compile_worker_count;
				g_async_pipeline_diagnostics.queued_jobs.fetch_add(1, std::memory_order_relaxed);
				update_atomic_max(g_async_pipeline_diagnostics.queue_depth_high_watermark, _compile_queue.size());
				proxy->AddRef(); // Worker owns a reference until compile finishes.
			}
			if (notify_worker)
				g_async_pipeline_compile_gate_cv.notify_one();

			g_async_pipeline_diagnostics.async_proxy_creates.fetch_add(1, std::memory_order_relaxed);
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
			if (!is_supported(compute_desc, compute_unsupported_reason))
			{
				note_async_pipeline_unsupported_desc(compute_unsupported_reason);
				goto sync_with_events;
			}

			com_ptr<ID3D12PipelineState> fallback;
			const HRESULT fallback_hr = get_or_create_compute_fallback(compute_desc, fallback);
			if (FAILED(fallback_hr))
			{
				g_async_pipeline_diagnostics.sync_fallback_failure.fetch_add(1, std::memory_order_relaxed);
				reshade::log::message(reshade::log::level::warning, "Async D3D12 PSO: compute fallback creation failed with error code %s; falling back to synchronous real PSO creation.", reshade::log::hr_to_string(fallback_hr).c_str());
				goto sync_with_events;
			}

			const uint64_t id = _next_proxy_id.fetch_add(1, std::memory_order_relaxed);
			D3D12AsyncPipelineProxy *const proxy = new D3D12AsyncPipelineProxy(this, _device, fallback.get(), id);

			bool notify_worker = false;
			{
				std::lock_guard<std::mutex> lock(_queue_mutex);
				CompileJob job = {};
				job.proxy = proxy;
				job.compute_desc = std::make_unique<CopiedComputePipelineDesc>(compute_desc);
				job.id = id;
				_compile_queue.push_back(std::move(job));
				notify_worker = _compile_queue.size() <= _compile_worker_count;
				g_async_pipeline_diagnostics.queued_jobs.fetch_add(1, std::memory_order_relaxed);
				update_atomic_max(g_async_pipeline_diagnostics.queue_depth_high_watermark, _compile_queue.size());
				proxy->AddRef(); // Worker owns a reference until compile finishes.
			}
			if (notify_worker)
				g_async_pipeline_compile_gate_cv.notify_one();

			g_async_pipeline_diagnostics.async_proxy_creates.fetch_add(1, std::memory_order_relaxed);
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
		uint64_t id = 0;
	};

	static bool is_supported(const D3D12_GRAPHICS_PIPELINE_STATE_DESC &desc, const char *&unsupported_reason)
	{
		unsupported_reason = "none";
		if (desc.pRootSignature == nullptr)
		{
			unsupported_reason = "graphics desc has null root signature";
			return false;
		}
		if (desc.SampleDesc.Count != 1 || desc.SampleDesc.Quality != 0)
		{
			unsupported_reason = "graphics desc uses MSAA/sample quality";
			return false;
		}
		if (desc.HS.pShaderBytecode != nullptr)
		{
			unsupported_reason = "graphics desc uses hull shader";
			return false;
		}
		if (desc.DS.pShaderBytecode != nullptr)
		{
			unsupported_reason = "graphics desc uses domain shader";
			return false;
		}
		if (desc.GS.pShaderBytecode != nullptr)
		{
			unsupported_reason = "graphics desc uses geometry shader";
			return false;
		}
		if (desc.StreamOutput.NumEntries != 0 || desc.StreamOutput.NumStrides != 0)
		{
			unsupported_reason = "graphics desc uses stream output";
			return false;
		}
		if (desc.PrimitiveTopologyType == D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH)
		{
			unsupported_reason = "graphics desc uses patch topology";
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

	static bool is_supported(const D3D12_COMPUTE_PIPELINE_STATE_DESC &desc, const char *&unsupported_reason)
	{
		unsupported_reason = "none";
		if (desc.pRootSignature == nullptr)
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

	HRESULT get_or_create_fallback(const D3D12_GRAPHICS_PIPELINE_STATE_DESC &source_desc, com_ptr<ID3D12PipelineState> &fallback)
	{
		GraphicsFallbackKey key = make_key(source_desc);
		{
			std::lock_guard<std::mutex> lock(_fallback_mutex);
			if constexpr (use_global_sentinel_fallback_pso)
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
				g_async_pipeline_diagnostics.fallback_cache_hits.fetch_add(1, std::memory_order_relaxed);
				fallback = it->second;
				return S_OK;
			}
		}
		g_async_pipeline_diagnostics.fallback_cache_misses.fetch_add(1, std::memory_order_relaxed);

		D3D12_SHADER_BYTECODE vs = {}, ps = {};
		ensure_fallback_shaders(vs, ps);

		D3D12_GRAPHICS_PIPELINE_STATE_DESC fallback_desc = {};
		fallback_desc.pRootSignature = source_desc.pRootSignature;
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
		fallback_desc.PrimitiveTopologyType = key.primitive_topology_type;
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
			g_async_pipeline_diagnostics.fallback_creation_failures.fetch_add(1, std::memory_order_relaxed);
		if (FAILED(hr))
		{
			if constexpr (use_global_sentinel_fallback_pso)
			{
				std::lock_guard<std::mutex> lock(_fallback_mutex);
				if (_fallback_cache.empty())
					_has_global_fallback_key = false;
			}
			return hr;
		}

		std::lock_guard<std::mutex> lock(_fallback_mutex);
		if constexpr (use_global_sentinel_fallback_pso)
			key = _global_fallback_key;
		const auto [it, inserted] = _fallback_cache.emplace(key, created);
		fallback = it->second;
		if (inserted)
			g_async_pipeline_diagnostics.fallback_buckets_created.fetch_add(1, std::memory_order_relaxed);
		if constexpr (async_pipeline_debug_diagnostics)
			reshade::log::message(reshade::log::level::info, "Async D3D12 PSO: created fallback PSO bucket, total buckets = %zu.", _fallback_cache.size());
		return S_OK;
	}

	HRESULT get_or_create_compute_fallback(const D3D12_COMPUTE_PIPELINE_STATE_DESC &source_desc, com_ptr<ID3D12PipelineState> &fallback)
	{
		ComputeFallbackKey key = make_key(source_desc);
		{
			std::lock_guard<std::mutex> lock(_compute_fallback_mutex);
			if constexpr (use_global_sentinel_fallback_pso)
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
				g_async_pipeline_diagnostics.fallback_cache_hits.fetch_add(1, std::memory_order_relaxed);
				fallback = it->second;
				return S_OK;
			}
		}
		g_async_pipeline_diagnostics.fallback_cache_misses.fetch_add(1, std::memory_order_relaxed);

		D3D12_SHADER_BYTECODE cs = {};
		ensure_compute_fallback_shader(cs);

		D3D12_COMPUTE_PIPELINE_STATE_DESC fallback_desc = {};
		fallback_desc.pRootSignature = source_desc.pRootSignature;
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
			g_async_pipeline_diagnostics.fallback_creation_failures.fetch_add(1, std::memory_order_relaxed);
		if (FAILED(hr))
		{
			if constexpr (use_global_sentinel_fallback_pso)
			{
				std::lock_guard<std::mutex> lock(_compute_fallback_mutex);
				if (_compute_fallback_cache.empty())
					_has_global_compute_fallback_key = false;
			}
			return hr;
		}

		std::lock_guard<std::mutex> lock(_compute_fallback_mutex);
		if constexpr (use_global_sentinel_fallback_pso)
			key = _global_compute_fallback_key;
		const auto [it, inserted] = _compute_fallback_cache.emplace(key, created);
		fallback = it->second;
		if (inserted)
			g_async_pipeline_diagnostics.fallback_buckets_created.fetch_add(1, std::memory_order_relaxed);
		if constexpr (async_pipeline_debug_diagnostics)
			reshade::log::message(reshade::log::level::info, "Async D3D12 PSO: created compute fallback PSO bucket, total buckets = %zu.", _compute_fallback_cache.size());
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
					for (CompileJob &queued_job : _compile_queue)
						queued_job.proxy->Release();
					_compile_queue.clear();
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

			com_ptr<ID3D12PipelineState> real;
			const auto compile_start = std::chrono::steady_clock::now();
			HRESULT hr = E_FAIL;
			bool handled_by_addon_events = false;
#if RESHADE_ADDON >= 2
			ID3D12PipelineState *event_pipeline = nullptr;
			if (job.graphics_desc != nullptr)
				handled_by_addon_events = _device_proxy->invoke_create_and_init_pipeline_event(job.graphics_desc->desc, event_pipeline, hr, true);
			else if (job.compute_desc != nullptr)
				handled_by_addon_events = _device_proxy->invoke_create_and_init_pipeline_event(job.compute_desc->desc, event_pipeline, hr, true);

			if (handled_by_addon_events && SUCCEEDED(hr))
				real = com_ptr<ID3D12PipelineState>(event_pipeline, true);
#endif
			if (!handled_by_addon_events && job.graphics_desc != nullptr)
			{
				hr = _device->CreateGraphicsPipelineState(&job.graphics_desc->desc, IID_PPV_ARGS(&real));
			}
			else if (!handled_by_addon_events && job.compute_desc != nullptr)
			{
				hr = _device->CreateComputePipelineState(&job.compute_desc->desc, IID_PPV_ARGS(&real));
			}
			const auto compile_end = std::chrono::steady_clock::now();
			const uint64_t compile_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(compile_end - compile_start).count());
			update_atomic_max(g_async_pipeline_diagnostics.async_compile_max_us, compile_us);
			if (SUCCEEDED(hr))
			{
				g_async_pipeline_diagnostics.async_compile_successes.fetch_add(1, std::memory_order_relaxed);
				g_async_pipeline_diagnostics.async_compile_total_us.fetch_add(compile_us, std::memory_order_relaxed);
				job.proxy->set_real(real.get());
			}
			else
			{
				g_async_pipeline_diagnostics.async_compile_failures.fetch_add(1, std::memory_order_relaxed);
				reshade::log::message(reshade::log::level::warning, "Async D3D12 PSO proxy %llu: real PSO creation failed with error code %s; keeping fallback bound.", static_cast<unsigned long long>(job.id), reshade::log::hr_to_string(hr).c_str());
				job.proxy->set_compile_failed();
			}
			job.proxy->Release();
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
	std::mutex _fallback_mutex;
	GraphicsFallbackKey _global_fallback_key = {};
	bool _has_global_fallback_key = false;
	std::unordered_map<GraphicsFallbackKey, com_ptr<ID3D12PipelineState>, GraphicsFallbackKeyHash> _fallback_cache;
	std::mutex _compute_fallback_mutex;
	ComputeFallbackKey _global_compute_fallback_key = {};
	bool _has_global_compute_fallback_key = false;
	std::unordered_map<ComputeFallbackKey, com_ptr<ID3D12PipelineState>, ComputeFallbackKeyHash> _compute_fallback_cache;
	std::mutex _queue_mutex;
	std::deque<CompileJob> _compile_queue;
	std::atomic_bool _stop = false;
	const size_t _compile_worker_count = get_async_pipeline_compile_worker_count();
	std::vector<std::thread> _workers;
	std::mutex _residency_queue_mutex;
	std::condition_variable _residency_cv;
	std::deque<ResidencyJob> _residency_queue;
	bool _residency_stop = false;
	std::thread _residency_worker;
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
	return new D3D12AsyncPipelineManager(device_proxy, device);
}

HRESULT create_async_graphics_pipeline_state(D3D12AsyncPipelineManager *manager, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc, REFIID riid, void **pipeline_state)
{
	return manager != nullptr ? manager->create_graphics_pipeline_state(desc, riid, pipeline_state) : E_FAIL;
}

HRESULT create_async_compute_pipeline_state(D3D12AsyncPipelineManager *manager, const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc, REFIID riid, void **pipeline_state)
{
	return manager != nullptr ? manager->create_compute_pipeline_state(desc, riid, pipeline_state) : E_FAIL;
}

HRESULT create_async_pipeline_state_stream(D3D12AsyncPipelineManager *manager, const D3D12_PIPELINE_STATE_STREAM_DESC *desc, REFIID riid, void **pipeline_state)
{
	return manager != nullptr ? manager->create_pipeline_state_stream(desc, riid, pipeline_state) : E_FAIL;
}

void note_async_pipeline_state_stream_create(REFIID riid)
{
	const uint64_t stream_create_count = g_async_pipeline_diagnostics.stream_create_calls.fetch_add(1, std::memory_order_relaxed) + 1;
	if (riid == __uuidof(ID3D12PipelineState1))
		g_async_pipeline_diagnostics.stream_pipeline_state1_requests.fetch_add(1, std::memory_order_relaxed);
	if (should_log_periodic(stream_create_count))
		log_async_pipeline_diagnostics("stream-create");
}

void note_async_pipeline_fallback_draw_skip()
{
	g_async_pipeline_diagnostics.fallback_draw_skips.fetch_add(1, std::memory_order_relaxed);
}

void note_async_pipeline_fallback_promotion()
{
	g_async_pipeline_diagnostics.fallback_promotions.fetch_add(1, std::memory_order_relaxed);
}

void note_async_pageable_make_resident(ID3D12Pageable *pageable)
{
	D3D12AsyncPipelineProxy *proxy = nullptr;
	if (pageable != nullptr && SUCCEEDED(pageable->QueryInterface(__uuidof(D3D12AsyncPipelineProxy), reinterpret_cast<void **>(&proxy))))
	{
		proxy->note_make_resident_requested();
		release_async_pipeline_state_proxy(proxy);
	}
}

void note_async_pageable_evict(ID3D12Pageable *pageable)
{
	D3D12AsyncPipelineProxy *proxy = nullptr;
	if (pageable != nullptr && SUCCEEDED(pageable->QueryInterface(__uuidof(D3D12AsyncPipelineProxy), reinterpret_cast<void **>(&proxy))))
	{
		proxy->note_evicted();
		release_async_pipeline_state_proxy(proxy);
	}
}

void note_async_pageable_residency_priority(ID3D12Pageable *pageable, D3D12_RESIDENCY_PRIORITY priority)
{
	D3D12AsyncPipelineProxy *proxy = nullptr;
	if (pageable != nullptr && SUCCEEDED(pageable->QueryInterface(__uuidof(D3D12AsyncPipelineProxy), reinterpret_cast<void **>(&proxy))))
	{
		proxy->note_residency_priority(priority);
		release_async_pipeline_state_proxy(proxy);
	}
}

D3D12AsyncPipelineProxy *get_async_pipeline_state_proxy(ID3D12PipelineState *pipeline_state)
{
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
	return proxy != nullptr ? proxy->current_native_for_bind(resolved_to_fallback) : nullptr;
}

HRESULT store_async_pipeline_state_proxy_or_defer(D3D12AsyncPipelineProxy *proxy, ID3D12PipelineLibrary *library, LPCWSTR name)
{
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

	D3D12AsyncPipelineProxy *proxy = nullptr;
	if (SUCCEEDED(pageable->QueryInterface(__uuidof(D3D12AsyncPipelineProxy), reinterpret_cast<void **>(&proxy))))
	{
		g_async_pipeline_diagnostics.pageable_proxy_resolves.fetch_add(1, std::memory_order_relaxed);
		ID3D12Pageable *const native = proxy->current_native();
		release_async_pipeline_state_proxy(proxy);
		return native;
	}

	return pageable;
}
