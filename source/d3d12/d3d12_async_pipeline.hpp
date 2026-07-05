/*
 * Copyright (C) 2014 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause OR MIT
 */

#pragma once

#include <d3d12.h>

class D3D12AsyncPipelineManager;
class D3D12AsyncPipelineProxy;
class D3D12Device;

void destroy_d3d12_async_pipeline_manager(D3D12AsyncPipelineManager *manager);
D3D12AsyncPipelineManager *create_d3d12_async_pipeline_manager(D3D12Device *device_proxy, ID3D12Device *device);

HRESULT create_async_graphics_pipeline_state(D3D12AsyncPipelineManager *manager, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc, REFIID riid, void **pipeline_state);
HRESULT create_async_compute_pipeline_state(D3D12AsyncPipelineManager *manager, const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc, REFIID riid, void **pipeline_state);
HRESULT create_async_pipeline_state_stream(D3D12AsyncPipelineManager *manager, const D3D12_PIPELINE_STATE_STREAM_DESC *desc, REFIID riid, void **pipeline_state);
void note_async_pipeline_state_stream_create(REFIID riid);
void note_async_pipeline_fallback_draw_skip();
void note_async_pipeline_fallback_promotion();
void note_async_pageable_make_resident(ID3D12Pageable *pageable);
void note_async_pageable_evict(ID3D12Pageable *pageable);
void note_async_pageable_residency_priority(ID3D12Pageable *pageable, D3D12_RESIDENCY_PRIORITY priority);
D3D12AsyncPipelineProxy *get_async_pipeline_state_proxy(ID3D12PipelineState *pipeline_state);
void release_async_pipeline_state_proxy(D3D12AsyncPipelineProxy *proxy);
ID3D12PipelineState *resolve_async_pipeline_state_proxy(D3D12AsyncPipelineProxy *proxy, bool *resolved_to_fallback);
HRESULT store_async_pipeline_state_proxy_or_defer(D3D12AsyncPipelineProxy *proxy, ID3D12PipelineLibrary *library, LPCWSTR name);
ID3D12PipelineState *resolve_async_pipeline_state(ID3D12PipelineState *pipeline_state);
ID3D12PipelineState *resolve_async_pipeline_state(ID3D12PipelineState *pipeline_state, bool *resolved_to_fallback);
ID3D12Pageable *resolve_async_pageable(ID3D12Pageable *pageable);
