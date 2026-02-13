/*
 * Copyright (C) 2014 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause OR MIT
 */

#include "vulkan_hooks.hpp"
#include "vulkan_impl_device.hpp"
#include "vulkan_impl_command_queue.hpp"
#include "vulkan_impl_swapchain.hpp"
#include "vulkan_impl_type_convert.hpp"
#include "dll_log.hpp"
#include "ini_file.hpp"
#include "addon_manager.hpp"
#include "runtime_manager.hpp"
#include "runtime.hpp"
#include "lockfree_linear_map.hpp"
#include <atomic>
#include <algorithm> // std::fill_n, std::sort, std::unique
#include <mutex>
#include <unordered_map>
#include <unordered_set>

extern thread_local bool g_in_dxgi_runtime;

extern lockfree_linear_map<VkSurfaceKHR, HWND, 16> g_vulkan_surfaces;
extern lockfree_linear_map<void *, reshade::vulkan::device_impl *, 8> g_vulkan_devices;

#if RESHADE_ADDON
extern void create_default_view(reshade::vulkan::device_impl *device_impl, VkImage image);
extern void destroy_default_view(reshade::vulkan::device_impl *device_impl, VkImage image);
#endif

static bool is_streamline_dlssg_active()
{
	if (GetModuleHandleW(L"sl.interposer.dll") == nullptr)
		return false;

	return
		GetModuleHandleW(L"sl.dlss_g.dll") != nullptr ||
		GetModuleHandleW(L"nvngx_dlssg.dll") != nullptr;
}

static bool is_vulkan_swapchain_image_logging_enabled()
{
	static const bool enabled = []
	{
		bool value = false;
		reshade::global_config().get("DEBUG", "LogVulkanSwapchainImages", value);
		if (!value)
			reshade::global_config().get("DEBUG", "LogVulkanImageOrigins", value);
		return value;
	}();

	return enabled;
}

static std::mutex s_streamline_fg_primary_swapchain_mutex;
static std::unordered_map<HWND, VkSwapchainKHR> s_streamline_fg_primary_swapchain_per_hwnd;
static std::unordered_set<VkSwapchainKHR> s_streamline_fg_auxiliary_swapchains;
static std::recursive_mutex s_streamline_fg_swapchain_lifecycle_mutex;
static std::unordered_map<HWND, reshade::vulkan::object_data<VK_OBJECT_TYPE_SWAPCHAIN_KHR> *> s_streamline_fg_runtime_handoff_per_hwnd;

static bool should_enable_runtime_for_streamline_fg_swapchain(HWND hwnd, VkSwapchainKHR new_swapchain, VkSwapchainKHR old_swapchain)
{
	const std::unique_lock<std::mutex> lock(s_streamline_fg_primary_swapchain_mutex);

	const auto primary_it = s_streamline_fg_primary_swapchain_per_hwnd.find(hwnd);
	const bool old_is_primary = old_swapchain != VK_NULL_HANDLE &&
		primary_it != s_streamline_fg_primary_swapchain_per_hwnd.end() &&
		primary_it->second == old_swapchain;
	const bool old_is_auxiliary = old_swapchain != VK_NULL_HANDLE &&
		s_streamline_fg_auxiliary_swapchains.erase(old_swapchain) != 0;

	if (primary_it == s_streamline_fg_primary_swapchain_per_hwnd.end())
	{
		s_streamline_fg_primary_swapchain_per_hwnd.emplace(hwnd, new_swapchain);
		return true;
	}

	if (old_is_primary)
	{
		primary_it->second = new_swapchain;
		return true;
	}

	if (old_is_auxiliary)
	{
		s_streamline_fg_auxiliary_swapchains.insert(new_swapchain);
		return false;
	}

	if (primary_it->second == new_swapchain)
		return true;

	s_streamline_fg_auxiliary_swapchains.insert(new_swapchain);
	return false;
}

static bool is_streamline_fg_auxiliary_swapchain(VkSwapchainKHR swapchain)
{
	const std::unique_lock<std::mutex> lock(s_streamline_fg_primary_swapchain_mutex);
	return s_streamline_fg_auxiliary_swapchains.find(swapchain) != s_streamline_fg_auxiliary_swapchains.end();
}

static void unregister_streamline_fg_swapchain(VkSwapchainKHR swapchain)
{
	if (swapchain == VK_NULL_HANDLE)
		return;

	const std::unique_lock<std::mutex> lock(s_streamline_fg_primary_swapchain_mutex);
	s_streamline_fg_auxiliary_swapchains.erase(swapchain);

	for (auto it = s_streamline_fg_primary_swapchain_per_hwnd.begin(); it != s_streamline_fg_primary_swapchain_per_hwnd.end();)
	{
		if (it->second == swapchain)
			it = s_streamline_fg_primary_swapchain_per_hwnd.erase(it);
		else
			++it;
	}
}

static reshade::vulkan::object_data<VK_OBJECT_TYPE_SWAPCHAIN_KHR> *take_streamline_fg_runtime_handoff(HWND hwnd)
{
	const std::unique_lock<std::recursive_mutex> lock(s_streamline_fg_swapchain_lifecycle_mutex);
	const auto it = s_streamline_fg_runtime_handoff_per_hwnd.find(hwnd);
	if (it == s_streamline_fg_runtime_handoff_per_hwnd.end())
		return nullptr;

	reshade::vulkan::object_data<VK_OBJECT_TYPE_SWAPCHAIN_KHR> *const swapchain_impl = it->second;
	s_streamline_fg_runtime_handoff_per_hwnd.erase(it);
	return swapchain_impl;
}

static void remap_swapchain_color_space_for_format(VkSwapchainCreateInfoKHR &create_info)
{
	switch (create_info.imageFormat)
	{
	case VK_FORMAT_R16G16B16A16_SFLOAT:
		create_info.imageColorSpace = VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT;
		break;
	case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
	case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
		create_info.imageColorSpace = VK_COLOR_SPACE_HDR10_ST2084_EXT;
		break;
	default:
		break;
	}
}

static const char *swapchain_format_to_string(VkFormat format)
{
	switch (format)
	{
	case VK_FORMAT_UNDEFINED:
		return "VK_FORMAT_UNDEFINED";
	case VK_FORMAT_R8G8B8A8_UNORM:
		return "VK_FORMAT_R8G8B8A8_UNORM";
	case VK_FORMAT_R8G8B8A8_SRGB:
		return "VK_FORMAT_R8G8B8A8_SRGB";
	case VK_FORMAT_B8G8R8A8_UNORM:
		return "VK_FORMAT_B8G8R8A8_UNORM";
	case VK_FORMAT_B8G8R8A8_SRGB:
		return "VK_FORMAT_B8G8R8A8_SRGB";
	case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
		return "VK_FORMAT_A2B10G10R10_UNORM_PACK32";
	case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
		return "VK_FORMAT_A2R10G10B10_UNORM_PACK32";
	case VK_FORMAT_R16G16B16A16_UNORM:
		return "VK_FORMAT_R16G16B16A16_UNORM";
	case VK_FORMAT_R16G16B16A16_SFLOAT:
		return "VK_FORMAT_R16G16B16A16_SFLOAT";
	default:
		return nullptr;
	}
}

static const char *swapchain_color_space_to_string(VkColorSpaceKHR color_space)
{
	switch (color_space)
	{
	case VK_COLOR_SPACE_SRGB_NONLINEAR_KHR:
		return "VK_COLOR_SPACE_SRGB_NONLINEAR_KHR";
#if VK_EXT_swapchain_colorspace
	case VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT:
		return "VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT";
	case VK_COLOR_SPACE_BT2020_LINEAR_EXT:
		return "VK_COLOR_SPACE_BT2020_LINEAR_EXT";
	case VK_COLOR_SPACE_HDR10_ST2084_EXT:
		return "VK_COLOR_SPACE_HDR10_ST2084_EXT";
	case VK_COLOR_SPACE_HDR10_HLG_EXT:
		return "VK_COLOR_SPACE_HDR10_HLG_EXT";
#endif
	default:
		return nullptr;
	}
}

static void log_vulkan_swapchain_image_mapping(
	const char *phase,
	VkSwapchainKHR swapchain,
	uint32_t image_index,
	VkImage image,
	const VkSwapchainCreateInfoKHR &create_info,
	HWND hwnd,
	bool effect_runtime_enabled)
{
	if (!is_vulkan_swapchain_image_logging_enabled())
		return;

	const char *const format_string = swapchain_format_to_string(create_info.imageFormat);
	const char *const color_space_string = swapchain_color_space_to_string(create_info.imageColorSpace);

	reshade::log::message(reshade::log::level::info,
		"Vulkan swapchain image %s: swapchain=%p image[%u]=%p format=%d(%s) colorspace=%d(%s) extent=%ux%u layers=%u usage=%#x hwnd=%p runtime_enabled=%s auxiliary=%s.",
		phase,
		swapchain,
		image_index,
		image,
		static_cast<int>(create_info.imageFormat),
		format_string != nullptr ? format_string : "VK_FORMAT_UNKNOWN",
		static_cast<int>(create_info.imageColorSpace),
		color_space_string != nullptr ? color_space_string : "VK_COLOR_SPACE_UNKNOWN",
		create_info.imageExtent.width,
		create_info.imageExtent.height,
		create_info.imageArrayLayers,
		static_cast<unsigned int>(create_info.imageUsage),
		hwnd,
		effect_runtime_enabled ? "true" : "false",
		is_streamline_fg_auxiliary_swapchain(swapchain) ? "true" : "false");
}


template <typename T>
static void modify_image_format_list_create_info(T& create_info, VkFormat image_format)
{
	thread_local std::vector<VkFormat> format_list;
	// Use thread_local for the struct so the memory remains valid after function return
	thread_local VkImageFormatListCreateInfoKHR format_list_info_storage;

	format_list.clear();
	format_list.push_back(reshade::vulkan::convert_format(
		reshade::api::format_to_default_typed(reshade::vulkan::convert_format(image_format), 0)));

	if (const auto existing_info = find_in_structure_chain<VkImageFormatListCreateInfoKHR>(
		create_info.pNext, VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO_KHR))
	{
		auto* mutable_info = const_cast<VkImageFormatListCreateInfoKHR*>(existing_info);
		mutable_info->viewFormatCount = static_cast<uint32_t>(format_list.size());
		mutable_info->pViewFormats = format_list.data();
	}
	else
	{
		format_list_info_storage = { VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO_KHR };
		format_list_info_storage.pNext = create_info.pNext;
		format_list_info_storage.viewFormatCount = static_cast<uint32_t>(format_list.size());
		format_list_info_storage.pViewFormats = format_list.data();

		create_info.pNext = &format_list_info_storage;
	}
}

#if VK_KHR_swapchain
VkResult VKAPI_CALL vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkSwapchainKHR *pSwapchain)
{
	reshade::log::message(reshade::log::level::info, "Redirecting vkCreateSwapchainKHR(device = %p, pCreateInfo = %p, pAllocator = %p, pSwapchain = %p) ...", device, pCreateInfo, pAllocator, pSwapchain);
	const bool streamline_dlssg_active = is_streamline_dlssg_active();
	reshade::log::message(reshade::log::level::debug,
		"vkCreateSwapchainKHR context: fg_active=%s old_swapchain=%p surface=%p min_images=%u extent=%ux%u usage=%#x present_mode=%u.",
		streamline_dlssg_active ? "true" : "false",
		pCreateInfo != nullptr ? pCreateInfo->oldSwapchain : VK_NULL_HANDLE,
		pCreateInfo != nullptr ? pCreateInfo->surface : VK_NULL_HANDLE,
		pCreateInfo != nullptr ? pCreateInfo->minImageCount : 0,
		pCreateInfo != nullptr ? pCreateInfo->imageExtent.width : 0,
		pCreateInfo != nullptr ? pCreateInfo->imageExtent.height : 0,
		pCreateInfo != nullptr ? pCreateInfo->imageUsage : 0,
		pCreateInfo != nullptr ? pCreateInfo->presentMode : 0);
	std::unique_lock<std::recursive_mutex> streamline_fg_lifecycle_lock;
	if (streamline_dlssg_active)
	{
		streamline_fg_lifecycle_lock = std::unique_lock<std::recursive_mutex>(s_streamline_fg_swapchain_lifecycle_mutex);
		reshade::log::message(reshade::log::level::debug, "vkCreateSwapchainKHR acquired FG lifecycle lock.");
	}

	reshade::vulkan::device_impl *const device_impl = g_vulkan_devices.at(dispatch_key_from_handle(device));
	RESHADE_VULKAN_GET_DEVICE_DISPATCH_PTR(CreateSwapchainKHR, device_impl);

	assert(pCreateInfo != nullptr && pSwapchain != nullptr);

	std::vector<VkFormat> format_list;
	std::vector<uint32_t> queue_family_list;
	VkSwapchainCreateInfoKHR create_info = *pCreateInfo;
	VkImageFormatListCreateInfo format_list_info;

	// Only have to enable additional features if there is a graphics queue, since ReShade will not run otherwise
	if (device_impl->_primary_graphics_queue != nullptr)
	{
		// Add required usage flags to create info
		create_info.imageUsage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

		// Add required format variants, so e.g. both linear and sRGB views can be created for the swap chain images
		format_list.push_back(reshade::vulkan::convert_format(
			reshade::api::format_to_default_typed(reshade::vulkan::convert_format(create_info.imageFormat), 0)));
		format_list.push_back(reshade::vulkan::convert_format(
			reshade::api::format_to_default_typed(reshade::vulkan::convert_format(create_info.imageFormat), 1)));

#if VK_KHR_swapchain_mutable_format
		// Only have to make format mutable if they are actually different
		if (format_list[0] != format_list[1])
			create_info.flags |= VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR;

		// Patch the format list in the create info of the application
		if (const auto format_list_info2 = find_in_structure_chain<VkImageFormatListCreateInfo>(
				pCreateInfo->pNext, VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO))
		{
			format_list.insert(format_list.end(),
				format_list_info2->pViewFormats, format_list_info2->pViewFormats + format_list_info2->viewFormatCount);

			// Remove duplicates from the list (since the new formats may have already been added by the application)
			std::sort(format_list.begin(), format_list.end());
			format_list.erase(std::unique(format_list.begin(), format_list.end()), format_list.end());

			// This is evil, because writing into application memory, but eh =)
			const_cast<VkImageFormatListCreateInfo *>(format_list_info2)->viewFormatCount = static_cast<uint32_t>(format_list.size());
			const_cast<VkImageFormatListCreateInfo *>(format_list_info2)->pViewFormats = format_list.data();
		}
		else if (format_list[0] != format_list[1])
		{
			format_list_info = { VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO };
			format_list_info.pNext = create_info.pNext;
			format_list_info.viewFormatCount = static_cast<uint32_t>(format_list.size());
			format_list_info.pViewFormats = format_list.data();

			create_info.pNext = &format_list_info;
		}
#endif

		// Add required queue family indices, so images can be used on the graphics queue
		if (create_info.imageSharingMode == VK_SHARING_MODE_CONCURRENT)
		{
			queue_family_list.reserve(create_info.queueFamilyIndexCount + 1);
			queue_family_list.push_back(device_impl->_primary_graphics_queue_family_index);

			for (uint32_t i = 0; i < create_info.queueFamilyIndexCount; ++i)
				if (create_info.pQueueFamilyIndices[i] != device_impl->_primary_graphics_queue_family_index)
					queue_family_list.push_back(create_info.pQueueFamilyIndices[i]);

			create_info.queueFamilyIndexCount = static_cast<uint32_t>(queue_family_list.size());
			create_info.pQueueFamilyIndices = queue_family_list.data();
		}
	}

	// Dump swap chain description
	{
		const char *const format_string = swapchain_format_to_string(create_info.imageFormat);
		const char *const color_space_string = swapchain_color_space_to_string(create_info.imageColorSpace);

		reshade::log::message(reshade::log::level::info, "> Dumping swap chain description:");
		reshade::log::message(reshade::log::level::info, "  +-----------------------------------------+-----------------------------------------+");
		reshade::log::message(reshade::log::level::info, "  | Parameter                               | Value                                   |");
		reshade::log::message(reshade::log::level::info, "  +-----------------------------------------+-----------------------------------------+");
		reshade::log::message(reshade::log::level::info, "  | flags                                   |"                               " %-#39x |", static_cast<unsigned int>(create_info.flags));
		reshade::log::message(reshade::log::level::info, "  | surface                                 |"                                " %-39p |", create_info.surface);
		reshade::log::message(reshade::log::level::info, "  | minImageCount                           |"                                " %-39u |", create_info.minImageCount);
		if (format_string != nullptr)
		reshade::log::message(reshade::log::level::info, "  | imageFormat                             |"                                " %-39s |", format_string);
		else
		reshade::log::message(reshade::log::level::info, "  | imageFormat                             |"                                " %-39d |", static_cast<int>(create_info.imageFormat));
		if (color_space_string != nullptr)
		reshade::log::message(reshade::log::level::info, "  | imageColorSpace                         |"                                " %-39s |", color_space_string);
		else
		reshade::log::message(reshade::log::level::info, "  | imageColorSpace                         |"                                " %-39d |", static_cast<int>(create_info.imageColorSpace));
		reshade::log::message(reshade::log::level::info, "  | imageExtent                             |"            " %-19u"            " %-19u |", create_info.imageExtent.width, create_info.imageExtent.height);
		reshade::log::message(reshade::log::level::info, "  | imageArrayLayers                        |"                                " %-39u |", create_info.imageArrayLayers);
		reshade::log::message(reshade::log::level::info, "  | imageUsage                              |"                               " %-#39x |", static_cast<unsigned int>(create_info.imageUsage));
		reshade::log::message(reshade::log::level::info, "  | imageSharingMode                        |"                                " %-39d |", static_cast<int>(create_info.imageSharingMode));
		reshade::log::message(reshade::log::level::info, "  | queueFamilyIndexCount                   |"                                " %-39u |", create_info.queueFamilyIndexCount);
		reshade::log::message(reshade::log::level::info, "  | preTransform                            |"                               " %-#39x |", static_cast<unsigned int>(create_info.preTransform));
		reshade::log::message(reshade::log::level::info, "  | compositeAlpha                          |"                               " %-#39x |", static_cast<unsigned int>(create_info.compositeAlpha));
		reshade::log::message(reshade::log::level::info, "  | presentMode                             |"                                " %-39d |", static_cast<int>(create_info.presentMode));
		reshade::log::message(reshade::log::level::info, "  | clipped                                 |"                                " %-39s |", create_info.clipped ? "true" : "false");
		reshade::log::message(reshade::log::level::info, "  | oldSwapchain                            |"                                " %-39p |", create_info.oldSwapchain);
		reshade::log::message(reshade::log::level::info, "  +-----------------------------------------+-----------------------------------------+");
	}

	// Look up window handle from surface
	HWND const hwnd = g_vulkan_surfaces.at(create_info.surface);

#if RESHADE_ADDON
	reshade::api::swapchain_desc desc = {};
	desc.back_buffer.type = reshade::api::resource_type::texture_2d;
	desc.back_buffer.texture.width = create_info.imageExtent.width;
	desc.back_buffer.texture.height = create_info.imageExtent.height;
	assert(create_info.imageArrayLayers <= std::numeric_limits<uint16_t>::max());
	desc.back_buffer.texture.depth_or_layers = static_cast<uint16_t>(create_info.imageArrayLayers);
	desc.back_buffer.texture.levels = 1;
	desc.back_buffer.texture.format = reshade::vulkan::convert_format(create_info.imageFormat);
	desc.back_buffer.texture.samples = 1;
	desc.back_buffer.heap = reshade::api::memory_heap::gpu_only;
	reshade::vulkan::convert_image_usage_flags_to_usage(create_info.imageUsage, desc.back_buffer.usage);

	// modify_image_format_list_create_info(create_info, create_info.imageFormat);

	// TODO(Ritsu): find another way to do this from addon
	remap_swapchain_color_space_for_format(create_info);

	desc.back_buffer_count = create_info.minImageCount;
	desc.present_mode = static_cast<uint32_t>(create_info.presentMode);
	desc.present_flags = create_info.flags;
	desc.sync_interval = create_info.presentMode == VK_PRESENT_MODE_IMMEDIATE_KHR ? 0 : UINT32_MAX;

#if VK_EXT_full_screen_exclusive
	// Optionally change fullscreen state
	VkSurfaceFullScreenExclusiveInfoEXT fullscreen_info;
	if (const auto existing_fullscreen_info = find_in_structure_chain<VkSurfaceFullScreenExclusiveInfoEXT>(
			pCreateInfo->pNext, VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT))
	{
		fullscreen_info = *existing_fullscreen_info;

		desc.fullscreen_state = existing_fullscreen_info->fullScreenExclusive == VK_FULL_SCREEN_EXCLUSIVE_ALLOWED_EXT;
	}
	else
	{
		fullscreen_info = { VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT, const_cast<void *>(create_info.pNext) };
		fullscreen_info.fullScreenExclusive = VK_FULL_SCREEN_EXCLUSIVE_DEFAULT_EXT;
	}
#endif

	if (reshade::invoke_addon_event<reshade::addon_event::create_swapchain>(reshade::api::device_api::vulkan, desc, hwnd))
	{
		create_info.imageFormat = reshade::vulkan::convert_format(desc.back_buffer.texture.format);
		create_info.imageExtent.width = desc.back_buffer.texture.width;
		create_info.imageExtent.height = desc.back_buffer.texture.height;
		create_info.imageArrayLayers = desc.back_buffer.texture.depth_or_layers;
		reshade::vulkan::convert_usage_to_image_usage_flags(desc.back_buffer.usage, create_info.imageUsage);
		modify_image_format_list_create_info(create_info, create_info.imageFormat);
		// Add-ons may change the format, so re-derive a matching color space afterwards.
		remap_swapchain_color_space_for_format(create_info);
		create_info.minImageCount = desc.back_buffer_count;
		create_info.presentMode = static_cast<VkPresentModeKHR>(desc.present_mode);
		create_info.flags = static_cast<uint32_t>(desc.present_flags);

#if VK_EXT_full_screen_exclusive
		if (desc.fullscreen_state)
		{
			if (fullscreen_info.fullScreenExclusive != VK_FULL_SCREEN_EXCLUSIVE_APPLICATION_CONTROLLED_EXT)
			{
				fullscreen_info.fullScreenExclusive = VK_FULL_SCREEN_EXCLUSIVE_ALLOWED_EXT;

				create_info.pNext = &fullscreen_info;
			}
		}
		else
		{
			if (fullscreen_info.fullScreenExclusive == VK_FULL_SCREEN_EXCLUSIVE_ALLOWED_EXT)
			{
				fullscreen_info.fullScreenExclusive = VK_FULL_SCREEN_EXCLUSIVE_APPLICATION_CONTROLLED_EXT;

				create_info.pNext = &fullscreen_info;
			}
		}
#endif

		if (desc.sync_interval == 0)
			create_info.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
	}
#endif

	const char *const final_format_string = swapchain_format_to_string(create_info.imageFormat);
	const char *const final_color_space_string = swapchain_color_space_to_string(create_info.imageColorSpace);
	reshade::log::message(reshade::log::level::info,
		"vkCreateSwapchainKHR final create info: format=%d(%s) colorspace=%d(%s) extent=%ux%u usage=%#x present_mode=%u flags=%#x.",
		static_cast<int>(create_info.imageFormat),
		final_format_string != nullptr ? final_format_string : "VK_FORMAT_UNKNOWN",
		static_cast<int>(create_info.imageColorSpace),
		final_color_space_string != nullptr ? final_color_space_string : "VK_COLOR_SPACE_UNKNOWN",
		create_info.imageExtent.width,
		create_info.imageExtent.height,
		static_cast<unsigned int>(create_info.imageUsage),
		static_cast<unsigned int>(create_info.presentMode),
		static_cast<unsigned int>(create_info.flags));

	// Unregister object from old swap chain so that a call to 'vkDestroySwapchainKHR' won't reset the effect runtime again
	reshade::vulkan::object_data<VK_OBJECT_TYPE_SWAPCHAIN_KHR> *swapchain_impl = nullptr;
	bool reused_from_handoff = false;
	if (create_info.oldSwapchain != VK_NULL_HANDLE)
		swapchain_impl = device_impl->get_private_data_for_object<VK_OBJECT_TYPE_SWAPCHAIN_KHR>(create_info.oldSwapchain);
	else if (streamline_dlssg_active)
	{
		swapchain_impl = take_streamline_fg_runtime_handoff(hwnd);
		reused_from_handoff = swapchain_impl != nullptr;
		if (swapchain_impl != nullptr)
			reshade::log::message(reshade::log::level::debug,
				"Vulkan Streamline FG: reusing preserved runtime handoff for hwnd=%p old_runtime_swapchain=%p.",
				hwnd, swapchain_impl->_orig);
	}

	if (nullptr != swapchain_impl)
	{
		// Reuse the existing effect runtime if this swap chain was not created from scratch, but reset it before initializing again below
		if (swapchain_impl->_effect_runtime_enabled)
		{
			const std::unique_lock<std::recursive_mutex> runtime_lock(swapchain_impl->get_runtime_mutex());
			reshade::reset_effect_runtime(swapchain_impl);
		}

		// The handoff path already destroyed old swap chain resources during 'vkDestroySwapchainKHR',
		// so only do old-swapchain cleanup when replacing via 'oldSwapchain'.
		if (!reused_from_handoff)
		{
			// Get back buffer images of old swap chain
			uint32_t num_images = 0;
			device_impl->_dispatch_table.GetSwapchainImagesKHR(device, swapchain_impl->_orig, &num_images, nullptr);
			temp_mem<VkImage, 3> swapchain_images(num_images);
			device_impl->_dispatch_table.GetSwapchainImagesKHR(device, swapchain_impl->_orig, &num_images, swapchain_images.p);

#if RESHADE_ADDON
			reshade::invoke_addon_event<reshade::addon_event::destroy_swapchain>(swapchain_impl, false);
#endif

			for (uint32_t i = 0; i < num_images; ++i)
			{
#if RESHADE_ADDON
				destroy_default_view(device_impl, swapchain_images[i]);
#endif

				device_impl->unregister_object<VK_OBJECT_TYPE_IMAGE>(swapchain_images[i]);
			}

			device_impl->unregister_object<VK_OBJECT_TYPE_SWAPCHAIN_KHR, false>(swapchain_impl->_orig);
		}
	}

	assert(!g_in_dxgi_runtime);
	g_in_dxgi_runtime = true;
	const VkResult result = trampoline(device, &create_info, pAllocator, pSwapchain);
	g_in_dxgi_runtime = false;
	if (result < VK_SUCCESS)
	{
		reshade::log::message(reshade::log::level::warning, "vkCreateSwapchainKHR failed with error code %d.", static_cast<int>(result));
		return result;
	}

	const bool effect_runtime_enabled = !streamline_dlssg_active || should_enable_runtime_for_streamline_fg_swapchain(hwnd, *pSwapchain, create_info.oldSwapchain);
	reshade::log::message(reshade::log::level::debug,
		"vkCreateSwapchainKHR runtime ownership: new_swapchain=%p hwnd=%p effect_runtime_enabled=%s reused_runtime=%s.",
		*pSwapchain,
		hwnd,
		effect_runtime_enabled ? "true" : "false",
		swapchain_impl != nullptr ? "true" : "false");
	if (streamline_dlssg_active && !effect_runtime_enabled)
		reshade::log::message(reshade::log::level::debug, "Vulkan Streamline FG: creating auxiliary swap chain %p without effect runtime (hwnd=%p).", *pSwapchain, hwnd);

	if (nullptr == swapchain_impl)
	{
		swapchain_impl = new reshade::vulkan::object_data<VK_OBJECT_TYPE_SWAPCHAIN_KHR>(device_impl, *pSwapchain, create_info, hwnd);
		swapchain_impl->_effect_runtime_enabled = effect_runtime_enabled;

		if (swapchain_impl->_effect_runtime_enabled)
			reshade::create_effect_runtime(swapchain_impl, device_impl->_primary_graphics_queue);
	}
	else
	{
		swapchain_impl->_orig = *pSwapchain;
		swapchain_impl->_create_info = create_info;
		swapchain_impl->_create_info.pNext = nullptr; // Clear out structure chain pointer, since it becomes invalid once leaving the current scope
		swapchain_impl->_hwnd = hwnd;
		swapchain_impl->_effect_runtime_enabled = effect_runtime_enabled;
	}

	device_impl->register_object<VK_OBJECT_TYPE_SWAPCHAIN_KHR>(swapchain_impl->_orig, swapchain_impl);

	// Get back buffer images of new swap chain
	uint32_t num_images = 0;
	device_impl->_dispatch_table.GetSwapchainImagesKHR(device, swapchain_impl->_orig, &num_images, nullptr);
	temp_mem<VkImage, 3> swapchain_images(num_images);
	device_impl->_dispatch_table.GetSwapchainImagesKHR(device, swapchain_impl->_orig, &num_images, swapchain_images.p);

	// Add swap chain images to the image list
	for (uint32_t i = 0; i < num_images; ++i)
	{
		reshade::vulkan::object_data<VK_OBJECT_TYPE_IMAGE> &image_data = *device_impl->register_object<VK_OBJECT_TYPE_IMAGE>(swapchain_images[i]);
		image_data.create_info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
		image_data.create_info.imageType = VK_IMAGE_TYPE_2D;
		image_data.create_info.format = create_info.imageFormat;
		image_data.create_info.extent = { create_info.imageExtent.width, create_info.imageExtent.height, 1 };
		image_data.create_info.mipLevels = 1;
		image_data.create_info.arrayLayers = create_info.imageArrayLayers;
		image_data.create_info.samples = VK_SAMPLE_COUNT_1_BIT;
		image_data.create_info.usage = create_info.imageUsage;
		image_data.create_info.sharingMode = create_info.imageSharingMode;
		image_data.create_info.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

		// See https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateSwapchainKHR.html#_description
		if ((create_info.flags & VK_SWAPCHAIN_CREATE_SPLIT_INSTANCE_BIND_REGIONS_BIT_KHR) != 0)
			image_data.create_info.flags |= VK_IMAGE_CREATE_SPLIT_INSTANCE_BIND_REGIONS_BIT;
		if ((create_info.flags & VK_SWAPCHAIN_CREATE_PROTECTED_BIT_KHR) != 0)
			image_data.create_info.flags |= VK_IMAGE_CREATE_PROTECTED_BIT;
#if VK_KHR_swapchain_mutable_format
		if ((create_info.flags & VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR) != 0)
			image_data.create_info.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
#endif

		log_vulkan_swapchain_image_mapping(
			"register",
			swapchain_impl->_orig,
			i,
			swapchain_images[i],
			create_info,
			hwnd,
			swapchain_impl->_effect_runtime_enabled);
	}

#if RESHADE_ADDON
	reshade::invoke_addon_event<reshade::addon_event::init_swapchain>(swapchain_impl, false);

	// Create default views for swap chain images (do this after the 'init_swapchain' event, so that the images are known to add-ons)
	for (uint32_t i = 0; i < num_images; ++i)
		create_default_view(device_impl, swapchain_images[i]);

#if VK_EXT_full_screen_exclusive
	if (fullscreen_info.fullScreenExclusive != VK_FULL_SCREEN_EXCLUSIVE_DEFAULT_EXT)
	{
		if (const auto fullscreen_win32_info = find_in_structure_chain<VkSurfaceFullScreenExclusiveWin32InfoEXT>(
				create_info.pNext, VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_WIN32_INFO_EXT))
			swapchain_impl->hmonitor = fullscreen_win32_info->hmonitor;

		reshade::invoke_addon_event<reshade::addon_event::set_fullscreen_state>(swapchain_impl, fullscreen_info.fullScreenExclusive == VK_FULL_SCREEN_EXCLUSIVE_ALLOWED_EXT, swapchain_impl->hmonitor);
	}
#endif
#endif

	if (swapchain_impl->_effect_runtime_enabled)
	{
		const std::unique_lock<std::recursive_mutex> runtime_lock(swapchain_impl->get_runtime_mutex());
		reshade::init_effect_runtime(swapchain_impl);
	}

#if RESHADE_VERBOSE_LOG
	reshade::log::message(reshade::log::level::debug, "Returning Vulkan swap chain %p.", *pSwapchain);
#endif
	return result;
}
void     VKAPI_CALL vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks *pAllocator)
{
	reshade::log::message(reshade::log::level::info, "Redirecting vkDestroySwapchainKHR(device = %p, swapchain = %p, pAllocator = %p) ...", device, swapchain, pAllocator);
	const bool streamline_dlssg_active = is_streamline_dlssg_active();
	reshade::log::message(reshade::log::level::debug,
		"vkDestroySwapchainKHR context: fg_active=%s swapchain=%p.",
		streamline_dlssg_active ? "true" : "false",
		swapchain);
	std::unique_lock<std::recursive_mutex> streamline_fg_lifecycle_lock;
	if (streamline_dlssg_active)
	{
		streamline_fg_lifecycle_lock = std::unique_lock<std::recursive_mutex>(s_streamline_fg_swapchain_lifecycle_mutex);
		reshade::log::message(reshade::log::level::debug, "vkDestroySwapchainKHR acquired FG lifecycle lock.");
	}

	if (swapchain == VK_NULL_HANDLE)
		return;

	reshade::vulkan::device_impl *const device_impl = g_vulkan_devices.at(dispatch_key_from_handle(device));
	RESHADE_VULKAN_GET_DEVICE_DISPATCH_PTR(DestroySwapchainKHR, device_impl);

	// Remove swap chain from global list
	reshade::vulkan::object_data<VK_OBJECT_TYPE_SWAPCHAIN_KHR> *const swapchain_impl = device_impl->get_private_data_for_object<VK_OBJECT_TYPE_SWAPCHAIN_KHR, true>(swapchain);
	const bool preserve_runtime_handoff = streamline_dlssg_active && swapchain_impl != nullptr && swapchain_impl->_effect_runtime_enabled;
	reshade::log::message(reshade::log::level::debug,
		"vkDestroySwapchainKHR runtime ownership: swapchain=%p runtime=%p enabled=%s preserve_handoff=%s hwnd=%p.",
		swapchain,
		swapchain_impl,
		swapchain_impl != nullptr && swapchain_impl->_effect_runtime_enabled ? "true" : "false",
		preserve_runtime_handoff ? "true" : "false",
		swapchain_impl != nullptr ? swapchain_impl->_hwnd : nullptr);
	if (swapchain_impl != nullptr)
	{
		if (swapchain_impl->_effect_runtime_enabled && !preserve_runtime_handoff)
		{
			const std::unique_lock<std::recursive_mutex> runtime_lock(swapchain_impl->get_runtime_mutex());
			reshade::reset_effect_runtime(swapchain_impl);
		}

		// Get back buffer images of old swap chain
		uint32_t num_images = 0;
		device_impl->_dispatch_table.GetSwapchainImagesKHR(device, swapchain, &num_images, nullptr);
		temp_mem<VkImage, 3> swapchain_images(num_images);
		device_impl->_dispatch_table.GetSwapchainImagesKHR(device, swapchain, &num_images, swapchain_images.p);

#if RESHADE_ADDON
		reshade::invoke_addon_event<reshade::addon_event::destroy_swapchain>(swapchain_impl, false);
#endif

		for (uint32_t i = 0; i < num_images; ++i)
		{
			log_vulkan_swapchain_image_mapping(
				"unregister",
				swapchain,
				i,
				swapchain_images[i],
				swapchain_impl->_create_info,
				swapchain_impl->_hwnd,
				swapchain_impl->_effect_runtime_enabled);

#if RESHADE_ADDON
			destroy_default_view(device_impl, swapchain_images[i]);
#endif

			device_impl->unregister_object<VK_OBJECT_TYPE_IMAGE>(swapchain_images[i]);
		}

		if (swapchain_impl->_effect_runtime_enabled && !preserve_runtime_handoff)
		{
			const std::unique_lock<std::recursive_mutex> runtime_lock(swapchain_impl->get_runtime_mutex());
			reshade::destroy_effect_runtime(swapchain_impl);
		}
	}

	device_impl->unregister_object<VK_OBJECT_TYPE_SWAPCHAIN_KHR, false>(swapchain);
	unregister_streamline_fg_swapchain(swapchain);

	if (preserve_runtime_handoff)
	{
		s_streamline_fg_runtime_handoff_per_hwnd[swapchain_impl->_hwnd] = swapchain_impl;
		reshade::log::message(reshade::log::level::debug,
			"Vulkan Streamline FG: preserving runtime handoff for hwnd=%p across swap chain recreation.", swapchain_impl->_hwnd);
	}
	else
	{
		delete swapchain_impl;
	}

	trampoline(device, swapchain, pAllocator);
}

#if VK_EXT_hdr_metadata
void VKAPI_CALL vkSetHdrMetadataEXT(VkDevice device, uint32_t swapchainCount, const VkSwapchainKHR *pSwapchains, const VkHdrMetadataEXT *pMetadata)
{
	reshade::vulkan::device_impl *const device_impl = g_vulkan_devices.at(dispatch_key_from_handle(device));
	RESHADE_VULKAN_GET_DEVICE_DISPATCH_PTR(SetHdrMetadataEXT, device_impl);

	reshade::log::message(reshade::log::level::info,
		"Redirecting vkSetHdrMetadataEXT(device = %p, swapchainCount = %u, pSwapchains = %p, pMetadata = %p) ...",
		device, swapchainCount, pSwapchains, pMetadata);

	if (pSwapchains != nullptr && pMetadata != nullptr)
	{
		for (uint32_t i = 0; i < swapchainCount; ++i)
		{
			const VkSwapchainKHR swapchain = pSwapchains[i];
			const VkHdrMetadataEXT &metadata = pMetadata[i];

			const reshade::vulkan::object_data<VK_OBJECT_TYPE_SWAPCHAIN_KHR> *const swapchain_impl =
				device_impl->get_private_data_for_object<VK_OBJECT_TYPE_SWAPCHAIN_KHR, true>(swapchain);
			const VkFormat swapchain_format = swapchain_impl != nullptr ? swapchain_impl->_create_info.imageFormat : VK_FORMAT_UNDEFINED;
			const VkColorSpaceKHR swapchain_color_space = swapchain_impl != nullptr ? swapchain_impl->_create_info.imageColorSpace : VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
			const char *const swapchain_format_string = swapchain_format_to_string(swapchain_format);
			const char *const swapchain_color_space_string = swapchain_color_space_to_string(swapchain_color_space);

			reshade::log::message(reshade::log::level::info,
				"Vulkan HDR metadata: swapchain=%p format=%d(%s) colorspace=%d(%s) max_lum=%.3f min_lum=%.6f max_cll=%.3f max_fall=%.3f primaries_r=(%.6f,%.6f) primaries_g=(%.6f,%.6f) primaries_b=(%.6f,%.6f) white=(%.6f,%.6f).",
				swapchain,
				static_cast<int>(swapchain_format),
				swapchain_format_string != nullptr ? swapchain_format_string : "VK_FORMAT_UNKNOWN",
				static_cast<int>(swapchain_color_space),
				swapchain_color_space_string != nullptr ? swapchain_color_space_string : "VK_COLOR_SPACE_UNKNOWN",
				metadata.maxLuminance,
				metadata.minLuminance,
				metadata.maxContentLightLevel,
				metadata.maxFrameAverageLightLevel,
				metadata.displayPrimaryRed.x,
				metadata.displayPrimaryRed.y,
				metadata.displayPrimaryGreen.x,
				metadata.displayPrimaryGreen.y,
				metadata.displayPrimaryBlue.x,
				metadata.displayPrimaryBlue.y,
				metadata.whitePoint.x,
				metadata.whitePoint.y);
		}
	}

	trampoline(device, swapchainCount, pSwapchains, pMetadata);
}
#endif

VkResult VKAPI_CALL vkAcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t *pImageIndex)
{
	assert(pImageIndex != nullptr);

	reshade::vulkan::device_impl *const device_impl = g_vulkan_devices.at(dispatch_key_from_handle(device));
	RESHADE_VULKAN_GET_DEVICE_DISPATCH_PTR(AcquireNextImageKHR, device_impl);

	const VkResult result = trampoline(device, swapchain, timeout, semaphore, fence, pImageIndex);
	if (result == VK_SUCCESS)
	{
		if (reshade::vulkan::object_data<VK_OBJECT_TYPE_SWAPCHAIN_KHR> *const swapchain_impl = device_impl->get_private_data_for_object<VK_OBJECT_TYPE_SWAPCHAIN_KHR, true>(swapchain))
			swapchain_impl->_swap_index = *pImageIndex;
	}
#if RESHADE_VERBOSE_LOG
	else if (result < VK_SUCCESS)
	{
		reshade::log::message(reshade::log::level::warning, "vkAcquireNextImageKHR failed with error code %d.", static_cast<int>(result));
	}
#endif

	return result;
}
VkResult VKAPI_CALL vkAcquireNextImage2KHR(VkDevice device, const VkAcquireNextImageInfoKHR *pAcquireInfo, uint32_t *pImageIndex)
{
	assert(pAcquireInfo != nullptr && pImageIndex != nullptr);

	reshade::vulkan::device_impl *const device_impl = g_vulkan_devices.at(dispatch_key_from_handle(device));
	RESHADE_VULKAN_GET_DEVICE_DISPATCH_PTR(AcquireNextImage2KHR, device_impl);

	const VkResult result = trampoline(device, pAcquireInfo, pImageIndex);
	if (result == VK_SUCCESS)
	{
		if (reshade::vulkan::object_data<VK_OBJECT_TYPE_SWAPCHAIN_KHR> *const swapchain_impl = device_impl->get_private_data_for_object<VK_OBJECT_TYPE_SWAPCHAIN_KHR, true>(pAcquireInfo->swapchain))
			swapchain_impl->_swap_index = *pImageIndex;
	}
#if RESHADE_VERBOSE_LOG
	else if (result < VK_SUCCESS)
	{
		reshade::log::message(reshade::log::level::warning, "vkAcquireNextImage2KHR failed with error code %d.", static_cast<int>(result));
	}
#endif

	return result;
}

VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo)
{
	assert(pPresentInfo != nullptr);

	VkPresentInfoKHR present_info = *pPresentInfo;
	std::vector<VkSemaphore> merged_wait_semaphores;
	const bool streamline_dlssg_active = is_streamline_dlssg_active();
	if (streamline_dlssg_active)
	{
		static bool s_logged_streamline_fg_safe_present_mode = false;
		if (!s_logged_streamline_fg_safe_present_mode)
		{
			reshade::log::message(reshade::log::level::info,
				"Enabling Vulkan Streamline FG present compatibility: preserving app present semaphores.");
			s_logged_streamline_fg_safe_present_mode = true;
		}
	}
	reshade::vulkan::device_impl *const device_impl = g_vulkan_devices.at(dispatch_key_from_handle(queue));
	reshade::vulkan::command_queue_impl *const queue_impl = device_impl->get_private_data_for_object<VK_OBJECT_TYPE_QUEUE>(queue);

	const bool present_from_secondary_queue = device_impl->_primary_graphics_queue != nullptr && device_impl->_primary_graphics_queue != queue_impl;
	const bool use_cross_queue_locking = present_from_secondary_queue && !streamline_dlssg_active;
	bool queue_lock_acquired = false;
	bool primary_queue_lock_acquired = false;
	if (use_cross_queue_locking)
	{
		std::lock(queue_impl->_mutex, device_impl->_primary_graphics_queue->_mutex);
		queue_lock_acquired = true;
		primary_queue_lock_acquired = true;
	}
	else if (streamline_dlssg_active)
	{
		if (!queue_impl->_mutex.try_lock())
		{
			static bool s_logged_streamline_fg_present_lock_contention = false;
			if (!s_logged_streamline_fg_present_lock_contention)
			{
				reshade::log::message(reshade::log::level::warning,
					"Vulkan Streamline FG: queue mutex contention in vkQueuePresentKHR, bypassing ReShade work for this frame.");
				s_logged_streamline_fg_present_lock_contention = true;
			}

			RESHADE_VULKAN_GET_DEVICE_DISPATCH_PTR(QueuePresentKHR, device_impl);
			assert(!g_in_dxgi_runtime);
			g_in_dxgi_runtime = true;
			const VkResult passthrough_result = trampoline(queue, pPresentInfo);
			g_in_dxgi_runtime = false;
			if (passthrough_result < VK_SUCCESS)
			{
				static std::atomic<bool> s_logged_queue_present_passthrough_failure = false;
				if (!s_logged_queue_present_passthrough_failure.exchange(true, std::memory_order_relaxed))
				{
					reshade::log::message(
						reshade::log::level::error,
						"vkQueuePresentKHR passthrough failed (first hit): result=%d queue=%p swapchain_count=%u wait_count=%u fg_active=%u reason=queue_mutex_contention.",
						static_cast<int>(passthrough_result),
						queue,
						pPresentInfo->swapchainCount,
						pPresentInfo->waitSemaphoreCount,
						streamline_dlssg_active ? 1u : 0u);
				}
			}
			return passthrough_result;
		}

		queue_lock_acquired = true;
	}
	else
	{
		queue_impl->_mutex.lock();
		queue_lock_acquired = true;
	}

	uint32_t skipped_auxiliary_swapchains = 0;
	for (uint32_t i = 0; i < pPresentInfo->swapchainCount; ++i)
	{
		reshade::vulkan::swapchain_impl *const swapchain_impl = device_impl->get_private_data_for_object<VK_OBJECT_TYPE_SWAPCHAIN_KHR>(pPresentInfo->pSwapchains[i]);
		if (streamline_dlssg_active && is_streamline_fg_auxiliary_swapchain(pPresentInfo->pSwapchains[i]))
		{
			++skipped_auxiliary_swapchains;
			continue;
		}

#if RESHADE_ADDON
#if VK_KHR_incremental_present
		uint32_t dirty_rect_count = 0;
		temp_mem<reshade::api::rect, 16> dirty_rects;

		const auto present_regions = find_in_structure_chain<VkPresentRegionsKHR>(pPresentInfo->pNext, VK_STRUCTURE_TYPE_PRESENT_REGIONS_KHR);
		if (present_regions != nullptr)
		{
			assert(present_regions->swapchainCount == pPresentInfo->swapchainCount);

			dirty_rect_count = present_regions->pRegions[i].rectangleCount;
			if (dirty_rect_count > 16)
				dirty_rects.p = new reshade::api::rect[dirty_rect_count];

			const VkRectLayerKHR *const rects = present_regions->pRegions[i].pRectangles;

			for (uint32_t k = 0; k < dirty_rect_count; ++k)
			{
				dirty_rects[k] = {
					rects[k].offset.x,
					rects[k].offset.y,
					rects[k].offset.x + static_cast<int32_t>(rects[k].extent.width),
					rects[k].offset.y + static_cast<int32_t>(rects[k].extent.height)
				};
			}
		}
#endif

#if VK_KHR_display_swapchain
		reshade::api::rect source_rect, dest_rect;

		const auto display_present_info = find_in_structure_chain<VkDisplayPresentInfoKHR>(pPresentInfo->pNext, VK_STRUCTURE_TYPE_DISPLAY_PRESENT_INFO_KHR);
		if (display_present_info != nullptr)
		{
			source_rect = {
				display_present_info->srcRect.offset.x,
				display_present_info->srcRect.offset.y,
				display_present_info->srcRect.offset.x + static_cast<int32_t>(display_present_info->srcRect.extent.width),
				display_present_info->srcRect.offset.y + static_cast<int32_t>(display_present_info->srcRect.extent.height)
			};
			dest_rect = {
				display_present_info->dstRect.offset.x,
				display_present_info->dstRect.offset.y,
				display_present_info->dstRect.offset.x + static_cast<int32_t>(display_present_info->dstRect.extent.width),
				display_present_info->dstRect.offset.y + static_cast<int32_t>(display_present_info->dstRect.extent.height)
			};
		}

		reshade::invoke_addon_event<reshade::addon_event::present>(
			queue_impl,
			swapchain_impl,
			display_present_info != nullptr ? &source_rect : nullptr,
			display_present_info != nullptr ? &dest_rect : nullptr,
#else
		reshade::invoke_addon_event<reshade::addon_event::present>(
			queue_impl,
			swapchain_impl,
			nullptr,
			nullptr,
#endif
#if VK_KHR_incremental_present
				dirty_rect_count,
				dirty_rect_count != 0 ? dirty_rects.p : nullptr);
#else
				0, nullptr);
#endif
#endif

		const std::unique_lock<std::recursive_mutex> runtime_lock(swapchain_impl->get_runtime_mutex(), std::try_to_lock);
		if (runtime_lock.owns_lock())
		{
			if (streamline_dlssg_active)
			{
				reshade::runtime *const runtime = static_cast<reshade::api::swapchain *>(swapchain_impl)->get_private_data<reshade::runtime>();
				if (runtime != nullptr && runtime->get_command_queue() != queue_impl)
				{
					runtime->set_command_queue(queue_impl);

					static bool s_logged_streamline_fg_runtime_queue_switch = false;
					if (!s_logged_streamline_fg_runtime_queue_switch)
					{
						reshade::log::message(reshade::log::level::info,
							"Vulkan Streamline FG: switching runtime command queue to active present queue for overlay stability.");
						s_logged_streamline_fg_runtime_queue_switch = true;
					}
				}
			}

			reshade::present_effect_runtime(swapchain_impl);
		}
	}

	// Synchronize immediate command list flush
	VkSubmitInfo submit_info { VK_STRUCTURE_TYPE_SUBMIT_INFO };
	temp_mem<VkPipelineStageFlags> wait_stages(present_info.waitSemaphoreCount);
	std::fill_n(wait_stages.p, present_info.waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
	submit_info.waitSemaphoreCount = present_info.waitSemaphoreCount;
	submit_info.pWaitSemaphores = present_info.pWaitSemaphores;
	submit_info.pWaitDstStageMask = wait_stages.p;
	const uint32_t original_wait_count = present_info.waitSemaphoreCount;
	const VkSemaphore *const original_wait_semaphores = present_info.pWaitSemaphores;

	if (!streamline_dlssg_active)
	{
		queue_impl->flush_immediate_command_list(submit_info);

		// If the application is presenting with a different queue than rendering, synchronize these two queues
		if (present_from_secondary_queue)
		{
			queue_impl->wait_and_signal(submit_info);

			device_impl->_primary_graphics_queue->flush_immediate_command_list(submit_info);
		}

		// Override wait semaphores based on the last queue submit
		present_info.waitSemaphoreCount = submit_info.waitSemaphoreCount;
		present_info.pWaitSemaphores = submit_info.pWaitSemaphores;
	}
	else
	{
		queue_impl->flush_immediate_command_list(submit_info);
		// In Streamline FG mode, avoid cross-queue synchronization from present to prevent
		// lock inversion with the app/framegen present queue threading model.
		if (present_from_secondary_queue)
		{
			static bool s_logged_streamline_fg_skip_cross_queue_sync = false;
			if (!s_logged_streamline_fg_skip_cross_queue_sync)
			{
				reshade::log::message(reshade::log::level::info,
					"Vulkan Streamline FG: skipping cross-queue present synchronization to avoid deadlock.");
				s_logged_streamline_fg_skip_cross_queue_sync = true;
			}
		}

		// Preserve app-provided semaphores for Streamline while still synchronizing ReShade runtime work.
		merged_wait_semaphores.clear();
		merged_wait_semaphores.reserve(original_wait_count + submit_info.waitSemaphoreCount);
		merged_wait_semaphores.insert(
			merged_wait_semaphores.end(),
			original_wait_semaphores,
			original_wait_semaphores + original_wait_count);

		for (uint32_t i = 0; i < submit_info.waitSemaphoreCount; ++i)
		{
			const VkSemaphore semaphore = submit_info.pWaitSemaphores[i];
			if (std::find(merged_wait_semaphores.begin(), merged_wait_semaphores.end(), semaphore) == merged_wait_semaphores.end())
				merged_wait_semaphores.push_back(semaphore);
		}

		present_info.waitSemaphoreCount = static_cast<uint32_t>(merged_wait_semaphores.size());
		present_info.pWaitSemaphores = merged_wait_semaphores.data();

		static bool s_logged_streamline_fg_present_merge = false;
		if (!s_logged_streamline_fg_present_merge)
		{
			reshade::log::message(reshade::log::level::info,
				"Vulkan Streamline FG present sync: app waits=%u, reshade waits=%u, merged waits=%u.",
				original_wait_count,
				submit_info.waitSemaphoreCount,
				present_info.waitSemaphoreCount);
			s_logged_streamline_fg_present_merge = true;
		}
	}

	device_impl->advance_transient_descriptor_pool();

	RESHADE_VULKAN_GET_DEVICE_DISPATCH_PTR(QueuePresentKHR, device_impl);
	assert(!g_in_dxgi_runtime);
	g_in_dxgi_runtime = true;
	const VkResult result = trampoline(queue, &present_info);
	g_in_dxgi_runtime = false;
	if (result < VK_SUCCESS)
	{
		static std::atomic<bool> s_logged_queue_present_failure = false;
		if (!s_logged_queue_present_failure.exchange(true, std::memory_order_relaxed))
		{
			reshade::log::message(
				reshade::log::level::error,
				"vkQueuePresentKHR failed (first hit): result=%d queue=%p swapchain_count=%u wait_count=%u fg_active=%u skipped_auxiliary=%u.",
				static_cast<int>(result),
				queue,
				present_info.swapchainCount,
				present_info.waitSemaphoreCount,
				streamline_dlssg_active ? 1u : 0u,
				skipped_auxiliary_swapchains);
		}
	}
	if (streamline_dlssg_active && result < VK_SUCCESS)
	{
		reshade::log::message(reshade::log::level::warning,
			"Vulkan Streamline FG present failed: result=%d skipped_auxiliary=%u final_waits=%u.",
			static_cast<int>(result),
			skipped_auxiliary_swapchains,
			present_info.waitSemaphoreCount);
	}

#if RESHADE_ADDON
	if (result >= VK_SUCCESS && reshade::has_addon_event<reshade::addon_event::finish_present>())
	{
		for (uint32_t i = 0; i < pPresentInfo->swapchainCount; ++i)
		{
			if (streamline_dlssg_active && is_streamline_fg_auxiliary_swapchain(pPresentInfo->pSwapchains[i]))
				continue;

			reshade::vulkan::swapchain_impl *const swapchain_impl = device_impl->get_private_data_for_object<VK_OBJECT_TYPE_SWAPCHAIN_KHR>(pPresentInfo->pSwapchains[i]);

			reshade::invoke_addon_event<reshade::addon_event::finish_present>(queue_impl, swapchain_impl);
		}
	}
#endif

	if (primary_queue_lock_acquired)
		device_impl->_primary_graphics_queue->_mutex.unlock();
	if (queue_lock_acquired)
		queue_impl->_mutex.unlock();

	return result;
}
#endif

#if VK_EXT_full_screen_exclusive
VkResult VKAPI_CALL vkAcquireFullScreenExclusiveModeEXT(VkDevice device, VkSwapchainKHR swapchain)
{
	reshade::vulkan::device_impl *const device_impl = g_vulkan_devices.at(dispatch_key_from_handle(device));
	RESHADE_VULKAN_GET_DEVICE_DISPATCH_PTR(AcquireFullScreenExclusiveModeEXT, device_impl);

#if RESHADE_ADDON
	if (reshade::vulkan::object_data<VK_OBJECT_TYPE_SWAPCHAIN_KHR> *const swapchain_impl = device_impl->get_private_data_for_object<VK_OBJECT_TYPE_SWAPCHAIN_KHR, true>(swapchain))
		if (reshade::invoke_addon_event<reshade::addon_event::set_fullscreen_state>(swapchain_impl, true, swapchain_impl->hmonitor))
			return VK_SUCCESS;
#endif

	return trampoline(device, swapchain);
}
VkResult VKAPI_CALL vkReleaseFullScreenExclusiveModeEXT(VkDevice device, VkSwapchainKHR swapchain)
{
	reshade::vulkan::device_impl *const device_impl = g_vulkan_devices.at(dispatch_key_from_handle(device));
	RESHADE_VULKAN_GET_DEVICE_DISPATCH_PTR(ReleaseFullScreenExclusiveModeEXT, device_impl);

#if RESHADE_ADDON
	if (reshade::vulkan::object_data<VK_OBJECT_TYPE_SWAPCHAIN_KHR> *const swapchain_impl = device_impl->get_private_data_for_object<VK_OBJECT_TYPE_SWAPCHAIN_KHR, true>(swapchain))
		if (reshade::invoke_addon_event<reshade::addon_event::set_fullscreen_state>(swapchain_impl, false, swapchain_impl->hmonitor))
			return VK_SUCCESS;
#endif

	return trampoline(device, swapchain);
}
#endif
