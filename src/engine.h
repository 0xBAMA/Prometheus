#pragma once

#include <iostream>
#include <chrono>
#include <thread>

#include <vk_types.h>

struct frameData_t {
	// frame sync primitives
	VkSemaphore swapchainSemaphore;
	VkSemaphore renderSemaphore;
	VkFence renderFence;

	// command buffer + allocator
	VkCommandPool commandPool;
	VkCommandBuffer mainCommandBuffer;
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

	// our frameData struct, which contains command pool/buffer + sync primitive handles
	frameData_t frameData[ FRAME_OVERLAP ];
	frameData_t& getCurrentFrame () { return frameData[ frameNumber % FRAME_OVERLAP ]; }

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

	// swapchain helpers
	void createSwapchain( uint32_t w, uint32_t h );
	void destroySwapchain();
};