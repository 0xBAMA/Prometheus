// vulkan_guide.h : Include file for standard system include files,
// or project specific include files.
//> intro
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <span>
#include <array>
#include <functional>
#include <deque>

#define VK_NO_PROTOTYPES
// #include <vulkan/vulkan.h>
#include "../src/third_party/volk/volk.h"
#include <vulkan/vk_enum_string_helper.h>
#include <vk_mem_alloc.h>

#include <fmt/core.h>
#define GLM_ENABLE_EXPERIMENTAL
#define GLM_FORCE_SWIZZLE
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>
#include <glm/gtx/rotate_vector.hpp>
#include <glm/gtc/packing.hpp>
#include <glm/gtx/string_cast.hpp>

//< intro 

// we will add our main reusable types here
struct AllocatedImage {
	VkImage image;
	VkImageView imageView;
	VmaAllocation allocation;
	VkExtent3D imageExtent;
	VkFormat imageFormat;
};

struct AllocatedBuffer {
	VkBuffer buffer;
	VmaAllocation allocation;
	VmaAllocationInfo info;
	VkDeviceAddress deviceAddress;
};

//> intro
#define VK_CHECK(x)                                                     \
    do {                                                                \
        VkResult err = x;                                               \
        if (err) {                                                      \
             fmt::print("Detected Vulkan error: {}", string_VkResult(err)); \
            abort();                                                    \
        }                                                               \
    } while (0)
//< intro

static VkInstance* globalVkInstancePtr = nullptr;
static VkDevice* globalVkDevicePtr = nullptr;
static VmaAllocator* vmaGlobalAllocatorPtr = nullptr;

static void SetDebugName ( VkObjectType type, uint64_t handle, const char* name ) {
	// Must call extension functions through a function pointer:
	PFN_vkSetDebugUtilsObjectNameEXT pfnSetDebugUtilsObjectNameEXT = ( PFN_vkSetDebugUtilsObjectNameEXT ) vkGetInstanceProcAddr( *globalVkInstancePtr, "vkSetDebugUtilsObjectNameEXT" );

	// // Set a name on the image
	// const VkDebugUtilsObjectNameInfoEXT imageNameInfo =
	// {
	// 	.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
	// 	.pNext = NULL,
	// 	.objectType = VK_OBJECT_TYPE_IMAGE,
	// 	.objectHandle = (uint64_t)image,
	// 	.pObjectName = "Brick Diffuse Texture",
	// };
	//
	// pfnSetDebugUtilsObjectNameEXT(device, &imageNameInfo);

	VkDebugUtilsObjectNameInfoEXT info{};
	info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
	info.pNext = NULL;
	info.objectType = type;
	info.objectHandle = handle;
	info.pObjectName = name;

	pfnSetDebugUtilsObjectNameEXT( *globalVkDevicePtr, &info );
}

static AllocatedBuffer createBuffer ( size_t allocSize, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage, std::string debugName = "" ) {
	// allocate buffer
	VkBufferCreateInfo bufferInfo = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bufferInfo.pNext = nullptr;
	bufferInfo.size = allocSize;
	bufferInfo.usage = usage | VK_BUFFER_USAGE_2_SHADER_DEVICE_ADDRESS_BIT;

	VmaAllocationCreateInfo vmaallocInfo = {};
	vmaallocInfo.usage = memoryUsage;
	vmaallocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
	AllocatedBuffer newBuffer;

	// allocate the buffer
	VK_CHECK( vmaCreateBuffer( *vmaGlobalAllocatorPtr, &bufferInfo, &vmaallocInfo, &newBuffer.buffer, &newBuffer.allocation, &newBuffer.info ) );

	VkBufferDeviceAddressInfo deviceAddressInfo = {};
	deviceAddressInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
	deviceAddressInfo.buffer = newBuffer.buffer;
	newBuffer.deviceAddress = vkGetBufferDeviceAddress( *globalVkDevicePtr, &deviceAddressInfo  );

	if ( debugName != "" ) {
		SetDebugName( VK_OBJECT_TYPE_BUFFER, ( uint64_t ) newBuffer.buffer, debugName.c_str() );
	}

	return newBuffer;
}