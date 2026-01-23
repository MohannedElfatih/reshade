/*
 * Copyright (C) 2021 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <glad/vulkan.h>
#include "reshade_api_pipeline.hpp"
#include <vector>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <limits>
#include <string>

#ifndef AMD_VULKAN_MEMORY_ALLOCATOR_H
using VmaAllocation = void *;
using VmaPool = void *;
#endif

namespace reshade::vulkan
{
	static_assert(sizeof(VkBuffer) == sizeof(api::resource));
	static_assert(sizeof(VkBufferView) == sizeof(api::resource_view));
	static_assert(sizeof(VkViewport) == sizeof(api::viewport));
	static_assert(sizeof(VkDescriptorSet) == sizeof(api::descriptor_table));
	static_assert(sizeof(VkDescriptorBufferInfo) == sizeof(api::buffer_range));
#if VK_KHR_acceleration_structure
	static_assert(sizeof(VkAccelerationStructureKHR) == sizeof(api::resource_view));
	static_assert(sizeof(VkAccelerationStructureInstanceKHR) == sizeof(api::acceleration_structure_instance));
#endif

	template <VkObjectType type>
	struct object_data;

	template <>
	struct object_data<VK_OBJECT_TYPE_IMAGE>
	{
		using Handle = VkImage;

		VmaAllocation allocation = nullptr;
		VmaPool pool = nullptr;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		uint64_t memory_offset = 0;
		VkImageCreateInfo create_info;
		VkImageView default_view = VK_NULL_HANDLE;
	};

	template <>
	struct object_data<VK_OBJECT_TYPE_BUFFER>
	{
		using Handle = VkBuffer;

		VmaAllocation allocation = nullptr;
		VmaPool pool = nullptr;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		uint64_t memory_offset = 0;
		VkBufferCreateInfo create_info;
	};

	template <>
	struct object_data<VK_OBJECT_TYPE_IMAGE_VIEW>
	{
		using Handle = VkImageView;

		VkImageViewCreateInfo create_info;
		// Keep track of image extent to avoid common extra lookup of view
		VkExtent3D image_extent;
	};

	template <>
	struct object_data<VK_OBJECT_TYPE_BUFFER_VIEW>
	{
		using Handle = VkBufferView;

		VkBufferViewCreateInfo create_info;
	};

	template <>
	struct object_data<VK_OBJECT_TYPE_SAMPLER>
	{
		using Handle = VkSampler;

		VkSamplerCreateInfo create_info;
	};

	template <>
	struct object_data<VK_OBJECT_TYPE_SHADER_MODULE>
	{
		using Handle = VkShaderModule;

		std::vector<uint8_t> spirv;
	};

	template <>
	struct object_data<VK_OBJECT_TYPE_RENDER_PASS>
	{
		using Handle = VkRenderPass;

		struct subpass
		{
			uint32_t color_attachments[8];
			uint32_t resolve_attachments[8];
			uint32_t num_color_attachments;
			uint32_t input_attachments[8];
			uint32_t num_input_attachments;
			uint32_t depth_stencil_attachment;
		};

		std::vector<subpass> subpasses;
		std::vector<VkAttachmentDescription> attachments;
	};

	template <>
	struct object_data<VK_OBJECT_TYPE_FRAMEBUFFER>
	{
		using Handle = VkFramebuffer;

		std::vector<VkImageView> attachments;
	};

	template <>
	struct object_data<VK_OBJECT_TYPE_PIPELINE_LAYOUT>
	{
		using Handle = VkPipelineLayout;

		std::vector<VkDescriptorSetLayout> set_layouts;
		std::vector<VkDescriptorSetLayout> owned_set_layouts;
		bool owns_set_layouts = false;
		std::vector<VkSampler> embedded_samplers;
	};

	template <>
	struct object_data<VK_OBJECT_TYPE_PIPELINE>
	{
		using Handle = VkPipeline;

		// Rendering compatibility signature (extracted from render pass or dynamic info)
		struct rendering_signature
		{
			uint32_t color_count = 0;
			VkFormat color_formats[8] = {};
			VkFormat depth_format = VK_FORMAT_UNDEFINED;
			VkFormat stencil_format = VK_FORMAT_UNDEFINED;
			VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
		};

		// Filled at vkCreateGraphicsPipelines time
		rendering_signature signature = {};

		// Optional: cached dynamic-rendering clone
		std::vector<rendering_signature> dynamic_rendering_signatures;
		std::vector<VkPipeline> dynamic_rendering_pipelines;
		std::unique_ptr<std::mutex> dynamic_rendering_mutex = std::make_unique<std::mutex>();

		// --- NEW: capture enough to recreate the pipeline ---
		bool is_graphics = false;

		VkGraphicsPipelineCreateInfo captured_ci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };

		// Owned backing storage for pointer fields
		std::vector<VkPipelineShaderStageCreateInfo> stages;

		VkPipelineVertexInputStateCreateInfo vertex_input = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
		std::vector<VkVertexInputBindingDescription> vi_bindings;
		std::vector<VkVertexInputAttributeDescription> vi_attribs;

		VkPipelineInputAssemblyStateCreateInfo input_assembly = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
		VkPipelineTessellationStateCreateInfo tessellation = { VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO };

		VkPipelineViewportStateCreateInfo viewport = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };

		VkPipelineRasterizationStateCreateInfo raster = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
		VkPipelineMultisampleStateCreateInfo msaa = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
		VkPipelineDepthStencilStateCreateInfo depth_stencil = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };

		VkPipelineColorBlendStateCreateInfo blend = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
		std::vector<VkPipelineColorBlendAttachmentState> blend_attachments;

		VkPipelineDynamicStateCreateInfo dynamic = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
		std::vector<VkDynamicState> dynamic_states;

		struct owned_stage
		{
			VkPipelineShaderStageCreateInfo ci = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };

			// Owned storage for pointer fields
			std::string entry_name;
			std::vector<VkSpecializationMapEntry> spec_entries;
			std::vector<uint8_t> spec_data;
			VkSpecializationInfo spec_info = {};
		};
		std::vector<owned_stage> owned_stages;

		// NOTE:
		// We intentionally do NOT capture pNext chains for the state structs.
		// If you need specific extensions (e.g. conservative raster, line raster, vertex divisor, etc.),
		// we can selectively deep-copy those later.
	};

	template <>
	struct object_data<VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT>
	{
		using Handle = VkDescriptorSetLayout;

		VkDescriptorSetLayoutCreateFlags create_flags = 0;
		uint32_t num_descriptors;
		std::vector<api::descriptor_range> ranges;
		std::vector<api::descriptor_range_with_static_samplers> ranges_with_static_samplers;
		std::vector<std::vector<VkSampler>> immutable_sampler_handles;
		std::vector<std::vector<api::sampler_desc>> static_samplers;
		std::vector<uint32_t> binding_to_offset;
		std::vector<VkDescriptorBindingFlags> binding_flags;
		bool push_descriptors;
	};

	template <>
	struct object_data<VK_OBJECT_TYPE_DESCRIPTOR_SET>
	{
		using Handle = VkDescriptorSet;

		VkDescriptorPool pool;
		uint32_t offset;
		VkDescriptorSetLayout layout;
	};

	template <>
	struct object_data<VK_OBJECT_TYPE_DESCRIPTOR_POOL>
	{
		using Handle = VkDescriptorPool;

		uint32_t max_sets;
		uint32_t max_descriptors;
		uint32_t next_set;
		uint32_t next_offset;
		std::vector<object_data<VK_OBJECT_TYPE_DESCRIPTOR_SET>> sets;
	};

	template <>
	struct object_data<VK_OBJECT_TYPE_DESCRIPTOR_UPDATE_TEMPLATE>
	{
		using Handle = VkDescriptorUpdateTemplate;

		VkPipelineBindPoint bind_point;
		std::vector<VkDescriptorUpdateTemplateEntry> entries;
	};

	template <>
	struct object_data<VK_OBJECT_TYPE_QUERY_POOL>
	{
		using Handle = VkQueryPool;

		VkQueryType type;
	};

#if VK_KHR_acceleration_structure
	template <>
	struct object_data<VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR>
	{
		using Handle = VkAccelerationStructureKHR;

		VkAccelerationStructureCreateInfoKHR create_info;
	};
#endif

	auto convert_format(api::format format, VkComponentMapping *components = nullptr) -> VkFormat;
	auto convert_format(VkFormat vk_format, const VkComponentMapping *components = nullptr) -> api::format;

	auto convert_color_space(api::color_space color_space) -> VkColorSpaceKHR;
	auto convert_color_space(VkColorSpaceKHR color_space) -> api::color_space;

	inline VkImageAspectFlags aspect_flags_from_format(VkFormat format)
	{
		if (format >= VK_FORMAT_D16_UNORM && format <= VK_FORMAT_D32_SFLOAT)
			return VK_IMAGE_ASPECT_DEPTH_BIT;
		if (format == VK_FORMAT_S8_UINT)
			return VK_IMAGE_ASPECT_STENCIL_BIT;
		if (format >= VK_FORMAT_D16_UNORM_S8_UINT && format <= VK_FORMAT_D32_SFLOAT_S8_UINT)
			return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
		return VK_IMAGE_ASPECT_COLOR_BIT;
	}

	inline bool is_integer_format(VkFormat format)
	{
		switch (format)
		{
		case VK_FORMAT_R8_UINT:
		case VK_FORMAT_R8_SINT:
		case VK_FORMAT_R8G8_UINT:
		case VK_FORMAT_R8G8_SINT:
		case VK_FORMAT_R8G8B8_UINT:
		case VK_FORMAT_R8G8B8_SINT:
		case VK_FORMAT_B8G8R8_UINT:
		case VK_FORMAT_B8G8R8_SINT:
		case VK_FORMAT_R8G8B8A8_UINT:
		case VK_FORMAT_R8G8B8A8_SINT:
		case VK_FORMAT_B8G8R8A8_UINT:
		case VK_FORMAT_B8G8R8A8_SINT:
		case VK_FORMAT_A8B8G8R8_UINT_PACK32:
		case VK_FORMAT_A8B8G8R8_SINT_PACK32:
		case VK_FORMAT_A2R10G10B10_UINT_PACK32:
		case VK_FORMAT_A2B10G10R10_UINT_PACK32:
		case VK_FORMAT_R16_UINT:
		case VK_FORMAT_R16_SINT:
		case VK_FORMAT_R16G16_UINT:
		case VK_FORMAT_R16G16_SINT:
		case VK_FORMAT_R16G16B16A16_UINT:
		case VK_FORMAT_R16G16B16A16_SINT:
		case VK_FORMAT_R32_UINT:
		case VK_FORMAT_R32_SINT:
		case VK_FORMAT_R32G32_UINT:
		case VK_FORMAT_R32G32_SINT:
		case VK_FORMAT_R32G32B32_UINT:
		case VK_FORMAT_R32G32B32_SINT:
		case VK_FORMAT_R32G32B32A32_UINT:
		case VK_FORMAT_R32G32B32A32_SINT:
		case VK_FORMAT_R64_UINT:
		case VK_FORMAT_R64_SINT:
		case VK_FORMAT_R64G64_UINT:
		case VK_FORMAT_R64G64_SINT:
		case VK_FORMAT_R64G64B64_UINT:
		case VK_FORMAT_R64G64B64_SINT:
		case VK_FORMAT_R64G64B64A64_UINT:
		case VK_FORMAT_R64G64B64A64_SINT:
			return true;
		default:
			return false;
		}
	}

	inline void convert_subresource(uint32_t subresource, const VkImageCreateInfo &create_info, VkImageSubresourceLayers &subresource_info)
	{
		subresource_info.aspectMask = aspect_flags_from_format(create_info.format);
		subresource_info.mipLevel = subresource % create_info.mipLevels;
		subresource_info.baseArrayLayer = subresource / create_info.mipLevels;
		subresource_info.layerCount = 1;
	}

	auto convert_access_to_usage(VkAccessFlags2 flags) -> api::resource_usage;
	auto convert_image_layout_to_usage(VkImageLayout layout) -> api::resource_usage;
	void convert_image_usage_flags_to_usage(const VkImageUsageFlags image_flags, api::resource_usage &usage);
	void convert_buffer_usage_flags_to_usage(const VkBufferUsageFlags2 buffer_flags, api::resource_usage &usage);

	auto convert_usage_to_access(api::resource_usage state) -> VkAccessFlags;
	auto convert_usage_to_image_layout(api::resource_usage state) -> VkImageLayout;
	auto convert_usage_to_pipeline_stage(api::resource_usage state, bool src_stage, const VkPhysicalDeviceFeatures &enabled_features, const GladVulkanContext &context) -> VkPipelineStageFlags;

	void convert_usage_to_image_usage_flags(api::resource_usage usage, VkImageUsageFlags &image_flags);
	void convert_usage_to_buffer_usage_flags(api::resource_usage usage, VkBufferUsageFlags &buffer_flags);

	void convert_sampler_desc(const api::sampler_desc &desc, VkSamplerCreateInfo &create_info);
	api::sampler_desc convert_sampler_desc(const VkSamplerCreateInfo &create_info);

	void convert_resource_desc(const api::resource_desc &desc, VkImageCreateInfo &create_info);
	void convert_resource_desc(const api::resource_desc &desc, VkBufferCreateInfo &create_info);
	api::resource_desc convert_resource_desc(const VkImageCreateInfo &create_info);
	api::resource_desc convert_resource_desc(const VkBufferCreateInfo &create_info);

	void convert_resource_view_desc(const api::resource_view_desc &desc, VkImageViewCreateInfo &create_info);
	void convert_resource_view_desc(const api::resource_view_desc &desc, VkBufferViewCreateInfo &create_info);
#if VK_KHR_acceleration_structure
	void convert_resource_view_desc(const api::resource_view_desc &desc, VkAccelerationStructureCreateInfoKHR &create_info);
#endif
	api::resource_view_desc convert_resource_view_desc(const VkImageViewCreateInfo &create_info);
	api::resource_view_desc convert_resource_view_desc(const VkBufferViewCreateInfo &create_info);
#if VK_KHR_acceleration_structure
	api::resource_view_desc convert_resource_view_desc(const VkAccelerationStructureCreateInfoKHR &create_info);
#endif

	void convert_dynamic_states(uint32_t count, const api::dynamic_state *states, std::vector<VkDynamicState> &internal_states);
	std::vector<api::dynamic_state> convert_dynamic_states(const VkPipelineDynamicStateCreateInfo *create_info);

	void convert_input_layout_desc(uint32_t count, const api::input_element *elements, std::vector<VkVertexInputBindingDescription> &vertex_bindings, std::vector<VkVertexInputAttributeDescription> &vertex_attributes, std::vector<VkVertexInputBindingDivisorDescription> &vertex_binding_divisors);
	std::vector<api::input_element> convert_input_layout_desc(const VkPipelineVertexInputStateCreateInfo *create_info);

#if VK_EXT_transform_feedback
	void convert_stream_output_desc(const api::stream_output_desc &desc, VkPipelineRasterizationStateCreateInfo &create_info);
	api::stream_output_desc convert_stream_output_desc(const VkPipelineRasterizationStateCreateInfo *create_info);
#endif
	void convert_blend_desc(const api::blend_desc &desc, VkPipelineColorBlendStateCreateInfo &create_info, VkPipelineMultisampleStateCreateInfo &multisample_create_info);
	api::blend_desc convert_blend_desc(const VkPipelineColorBlendStateCreateInfo *create_info, const VkPipelineMultisampleStateCreateInfo *multisample_create_info);
	void convert_rasterizer_desc(const api::rasterizer_desc &desc, VkPipelineRasterizationStateCreateInfo &create_info);
	api::rasterizer_desc convert_rasterizer_desc(const VkPipelineRasterizationStateCreateInfo *create_info, const VkPipelineMultisampleStateCreateInfo *multisample_create_info = nullptr);
	void convert_depth_stencil_desc(const api::depth_stencil_desc &desc, VkPipelineDepthStencilStateCreateInfo &create_info);
	api::depth_stencil_desc convert_depth_stencil_desc(const VkPipelineDepthStencilStateCreateInfo *create_info);

	auto convert_logic_op(api::logic_op value) -> VkLogicOp;
	auto convert_logic_op(VkLogicOp value) -> api::logic_op;
	auto convert_blend_op(api::blend_op value) -> VkBlendOp;
	auto convert_blend_op(VkBlendOp value) -> api::blend_op;
	auto convert_blend_factor(api::blend_factor value) -> VkBlendFactor;
	auto convert_blend_factor(VkBlendFactor value) -> api::blend_factor;
	auto convert_fill_mode(api::fill_mode value) -> VkPolygonMode;
	auto convert_fill_mode(VkPolygonMode value) -> api::fill_mode;
	auto convert_cull_mode(api::cull_mode value) -> VkCullModeFlags;
	auto convert_cull_mode(VkCullModeFlags value) -> api::cull_mode;
	auto convert_compare_op(api::compare_op value) -> VkCompareOp;
	auto convert_compare_op(VkCompareOp value) -> api::compare_op;
	auto convert_stencil_op(api::stencil_op value) -> VkStencilOp;
	auto convert_stencil_op(VkStencilOp value) -> api::stencil_op;
	auto convert_primitive_topology(api::primitive_topology value) -> VkPrimitiveTopology;
	auto convert_primitive_topology(VkPrimitiveTopology value) -> api::primitive_topology;

	auto convert_query_type(api::query_type value) -> VkQueryType;
	auto convert_query_type(VkQueryType value, uint32_t index = 0) -> api::query_type;

	auto convert_descriptor_type(api::descriptor_type value) -> VkDescriptorType;
	auto convert_descriptor_type(VkDescriptorType value) -> api::descriptor_type;

	auto convert_render_pass_load_op(api::render_pass_load_op value) -> VkAttachmentLoadOp;
	auto convert_render_pass_load_op(VkAttachmentLoadOp value) -> api::render_pass_load_op;
	auto convert_render_pass_store_op(api::render_pass_store_op value) -> VkAttachmentStoreOp;
	auto convert_render_pass_store_op(VkAttachmentStoreOp value) -> api::render_pass_store_op;

	auto convert_pipeline_flags(api::pipeline_flags value) -> VkPipelineCreateFlags;
	auto convert_pipeline_flags(VkPipelineCreateFlags2 value) -> api::pipeline_flags;
#if VK_KHR_ray_tracing_pipeline
	auto convert_shader_group_type(api::shader_group_type value) -> VkRayTracingShaderGroupTypeKHR;
	auto convert_shader_group_type(VkRayTracingShaderGroupTypeKHR value) -> api::shader_group_type;
#endif
#if VK_KHR_acceleration_structure
	auto convert_acceleration_structure_type(api::acceleration_structure_type value) -> VkAccelerationStructureTypeKHR;
	auto convert_acceleration_structure_type(VkAccelerationStructureTypeKHR value) -> api::acceleration_structure_type;
	auto convert_acceleration_structure_copy_mode(api::acceleration_structure_copy_mode value) -> VkCopyAccelerationStructureModeKHR;
	auto convert_acceleration_structure_copy_mode(VkCopyAccelerationStructureModeKHR value) -> api::acceleration_structure_copy_mode;
	auto convert_acceleration_structure_build_flags(api::acceleration_structure_build_flags value) -> VkBuildAccelerationStructureFlagsKHR;
	auto convert_acceleration_structure_build_flags(VkBuildAccelerationStructureFlagsKHR value) -> api::acceleration_structure_build_flags;

	void convert_acceleration_structure_build_input(const api::acceleration_structure_build_input &build_input, VkAccelerationStructureGeometryKHR &geometry, VkAccelerationStructureBuildRangeInfoKHR &range_info);
	api::acceleration_structure_build_input convert_acceleration_structure_build_input(const VkAccelerationStructureGeometryKHR &geometry, const VkAccelerationStructureBuildRangeInfoKHR &range_info);
#endif

	auto convert_shader_stages(VkPipelineBindPoint value) -> api::shader_stage;
	auto convert_pipeline_stages(api::pipeline_stage value) -> VkPipelineBindPoint;
	auto convert_pipeline_stages(VkPipelineBindPoint value) -> api::pipeline_stage;
}
