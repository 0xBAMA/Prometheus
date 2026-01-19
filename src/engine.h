#pragma once

#include <iostream>
#include <chrono>
#include <thread>

#include <vk_types.h>
#include <vk_descriptors.h>
#include <vk_pipelines.h>

struct DeletionQueue {
	std::deque< std::function< void() > > deletors;

	// called when we add new Vulkan objects
	void push_function( std::function< void() >&& function ) {
		deletors.push_back( function );
	}

	// called during Cleanup()
	void flush() {
		// reverse iterate the deletion queue to execute all the functions
		for ( auto it = deletors.rbegin(); it != deletors.rend(); it++ ) {
			( *it )(); //call functors
		}
		deletors.clear();
	}
};

struct frameData_t {
	// frame sync primitives
	VkSemaphore swapchainSemaphore;
	VkSemaphore renderSemaphore;
	VkFence renderFence;

	// command buffer + allocator
	VkCommandPool commandPool;
	VkCommandBuffer mainCommandBuffer;

	// handling frame-local resources
	DeletionQueue deletionQueue;
};

constexpr unsigned int FRAME_OVERLAP = 2;
constexpr bool useValidationLayers = true;

class PrometheusInstance {
public:

	bool isInitialized { false };
	bool stopRendering { false };
	int frameNumber { 0 };

	// basic Vulkan necessities, environmental handles
	VkInstance instance;						// Vulkan library handle
	VkDebugUtilsMessengerEXT debugMessenger;	// debug output messenger
	VkPhysicalDevice physicalDevice;			// GPU handle for the physical device in use
	VkDevice device;							// the abstract device that we interact with
	VkSurfaceKHR surface;						// the Vulkan window surface

	// an image to draw into and eventually pass to the swapchain
	AllocatedImage drawImage;
	VkExtent2D drawExtent;

	// our frameData struct, which contains command pool/buffer + sync primitive handles
	frameData_t frameData[ FRAME_OVERLAP ];
	frameData_t& getCurrentFrame () { return frameData[ frameNumber % FRAME_OVERLAP ]; }

	DescriptorAllocator globalDescriptorAllocator;

	VkDescriptorSet drawImageDescriptors;
	VkDescriptorSetLayout drawImageDescriptorLayout;

	VkPipeline gradientPipeline;
	VkPipelineLayout gradientPipelineLayout;

	// the queue that we submit work to
	VkQueue graphicsQueue;
	uint32_t graphicsQueueFamilyIndex;

	// window size, swapchain size
	VkExtent2D windowExtent { 1700 , 900 };
	VkExtent2D swapchainExtent;

	// swapchain handles
	VkSwapchainKHR swapchain;
	VkFormat swapchainImageFormat;
	std::vector< VkImage > swapchainImages;
	std::vector< VkImageView > swapchainImageViews;

	// handle for the AMD Vulkan Memory Allocator
	VmaAllocator allocator;

	// deletion queue automatically managing global resources
	DeletionQueue mainDeletionQueue;

	struct SDL_Window* window{ nullptr };
	static PrometheusInstance& Get ();

	void Init ();
	void Draw ();
	void MainLoop ();
	void ShutDown ();

private:
	// init helpers
	void initVulkan ();
	void initSwapchain ();
	void initCommandStructures ();
	void initSyncStructures ();
	void initDescriptors ();
	void initPipelines ();
	void initBackgroundPipelines ();

	// swapchain helpers
	void createSwapchain ( uint32_t w, uint32_t h );
	void destroySwapchain ();
};