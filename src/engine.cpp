#include "engine.h"

#include <SDL.h>
#include <SDL_vulkan.h>

#include <vk_types.h>
#include <vk_initializers.h>
#include <vk_images.h>

#include "VkBootstrap.h"
#include <array>
#include <thread>
#include <chrono>

#define VMA_IMPLEMENTATION
#include "vk_mem_alloc.h"

//============================================================================================================================
//============================================================================================================================
// Initialization
//============================================================================================================================
void PrometheusInstance::Init () {
	// initializing SDL
	SDL_Init( SDL_INIT_VIDEO );
	SDL_WindowFlags windowFlags = ( SDL_WindowFlags ) ( SDL_WINDOW_VULKAN );

	window = SDL_CreateWindow(
		"Prometheus",
		SDL_WINDOWPOS_UNDEFINED,
		SDL_WINDOWPOS_UNDEFINED,
		windowExtent.width,
		windowExtent.height,
		windowFlags );

	initVulkan();
	initSwapchain();
	initCommandStructures();
	initSyncStructures();
	initDescriptors();
	initPipelines();

	// everything went fine
	isInitialized = true;
}

//============================================================================================================================
// Draw
//============================================================================================================================
void PrometheusInstance::Draw () {
	// wait until the gpu has finished rendering the last frame. Timeout of 1 second
	VK_CHECK( vkWaitForFences( device, 1, &getCurrentFrame().renderFence, true, 1000000000 ) );

	// we want to take this opportunity to now reset the deletion queue, since this fence marks the completion
	getCurrentFrame().deletionQueue.flush(); // of all operations which could be using the data...

	// and now reset that fence so we can use it again, to signal this frame's completion
	VK_CHECK( vkResetFences( device, 1, &getCurrentFrame().renderFence ) );

	//request image from the swapchain
	uint32_t swapchainImageIndex;
	VK_CHECK( vkAcquireNextImageKHR( device, swapchain, 1000000000, getCurrentFrame().swapchainSemaphore, nullptr, &swapchainImageIndex ) );

	// Vulkan handles are aliased 64-bit pointers, basically shortens later code
	VkCommandBuffer cmd = getCurrentFrame().mainCommandBuffer;

	// because we've hit the fence, we are safe to reset the image buffer
	VK_CHECK( vkResetCommandBuffer( cmd, 0 ) );

	// begin the command buffer recording. We will use this command buffer exactly once, so we want to let vulkan know that
	VkCommandBufferBeginInfo cmdBeginInfo = vkinit::command_buffer_begin_info( VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT );

	drawExtent.width = drawImage.imageExtent.width;
	drawExtent.height = drawImage.imageExtent.height;

	// start the command buffer recording
	VK_CHECK( vkBeginCommandBuffer( cmd, &cmdBeginInfo ) );

	// put the draw image in a general layout
	vkutil::transition_image( cmd, drawImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL );

	// bind the gradient drawing compute pipeline
	vkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gradientPipeline );

	// bind the descriptor set containing the draw image for the compute pipeline
	vkCmdBindDescriptorSets( cmd, VK_PIPELINE_BIND_POINT_COMPUTE,  gradientPipelineLayout, 0, 1, &drawImageDescriptors, 0, nullptr );

	// execute the compute pipeline dispatch. We are using 16x16 workgroup size so we need to divide by it
	vkCmdDispatch( cmd, std::ceil( drawExtent.width / 16.0f ), std::ceil( drawExtent.height / 16.0f ), 1 );

	// transition the images for the copy
	vkutil::transition_image( cmd, drawImage.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL );
	vkutil::transition_image( cmd, swapchainImages[ swapchainImageIndex ], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL );

	// execute a copy from the draw image into the swapchain
	vkutil::copy_image_to_image( cmd, drawImage.image, swapchainImages[ swapchainImageIndex ], drawExtent, swapchainExtent );

	// transition the image from layout general to ready-for-swapchain-handoff
	vkutil::transition_image( cmd, swapchainImages[ swapchainImageIndex ], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR );

	// Kill recording, and put it in "executable" state
	VK_CHECK( vkEndCommandBuffer( cmd ) );

	// before submitting to the queue, we need to specify the specific dependencies
	// we want to wait on the presentSemaphore, signaled when the swapchain is ready
	// we will signal the renderSemaphore, when rendering has finished
	VkCommandBufferSubmitInfo cmdinfo = vkinit::command_buffer_submit_info( cmd );
	VkSemaphoreSubmitInfo waitInfo = vkinit::semaphore_submit_info( VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT_KHR, getCurrentFrame().swapchainSemaphore );
	VkSemaphoreSubmitInfo signalInfo = vkinit::semaphore_submit_info( VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, getCurrentFrame().renderSemaphore );

	VkSubmitInfo2 submit = vkinit::submit_info( &cmdinfo, &signalInfo, &waitInfo );

	// submit command buffer to the queue and execute it... renderFence will now block until it finishes
	VK_CHECK( vkQueueSubmit2( graphicsQueue, 1, &submit, getCurrentFrame().renderFence ) );

	// swapchain present to visible window...
	VkPresentInfoKHR presentInfo = {};
	presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	presentInfo.pNext = nullptr;
	presentInfo.pSwapchains = &swapchain;
	presentInfo.swapchainCount = 1;
	// wait on renderSemaphore, to tell when we are finished preparing the image
	presentInfo.pWaitSemaphores = &getCurrentFrame().renderSemaphore;
	presentInfo.waitSemaphoreCount = 1;
	presentInfo.pImageIndices = &swapchainImageIndex;
	VK_CHECK( vkQueuePresentKHR( graphicsQueue, &presentInfo ) );

	//increase the number of frames drawn
	frameNumber++;
}


//============================================================================================================================
// Main Loop
//============================================================================================================================
void PrometheusInstance::MainLoop () {
	SDL_Event e;

	bool quit = false;

	while ( !quit ) {
		// event handling loop
		while ( SDL_PollEvent( &e ) != 0 ) {
			if ( e.type == SDL_QUIT ) {
				quit = true;
			}

			if ( e.type == SDL_WINDOWEVENT ) {
				if ( e.window.event == SDL_WINDOWEVENT_MINIMIZED ) {
					stopRendering = true;
				}
				if ( e.window.event == SDL_WINDOWEVENT_RESTORED ) {
					stopRendering = false;
				}
			}
		}

		// handling minimized application
		if ( stopRendering ) {
			// throttle the speed to avoid busy loop
			std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
		} else {
			// we're ready to draw the next frame
			Draw();
		}
	}
}

//============================================================================================================================
// Cleanup
//============================================================================================================================
void PrometheusInstance::ShutDown () {
	// if we successfully made it through init
	if ( isInitialized ) {
		// make sure the gpu has stopped all work
		vkDeviceWaitIdle( device );

		// kill frameData
		for ( int i = 0; i < FRAME_OVERLAP; i++ ) {
			// killing the command pool implicitly kills the command buffers
			vkDestroyCommandPool( device, frameData[ i ].commandPool, nullptr );

			// destroy sync objects
			vkDestroyFence( device, frameData[ i ].renderFence, nullptr );
			vkDestroySemaphore( device, frameData[ i ].renderSemaphore, nullptr );
			vkDestroySemaphore( device, frameData[ i ].swapchainSemaphore, nullptr );

			// delete any remaining per-frame resources...
			frameData[ i ].deletionQueue.flush();
		}

		// destroy any remaining global resources
		mainDeletionQueue.flush();

		// destroy remaining resources
		destroySwapchain();
		vkDestroySurfaceKHR( instance, surface, nullptr );
		vkDestroyDevice( device, nullptr );
		vkb::destroy_debug_utils_messenger( instance, debugMessenger );
		vkDestroyInstance( instance, nullptr );
		SDL_DestroyWindow( window );
	}
}

//===========================================================================================================================
// Helpers
//===========================================================================================================================
void PrometheusInstance::initVulkan () {
	// make the vulkan instance, with basic debug features
	vkb::InstanceBuilder builder;
	auto inst_ret = builder.set_app_name( "Prometheus" )
		.request_validation_layers( useValidationLayers )
		.use_default_debug_messenger()
		.require_api_version( 1, 3, 0 )
		.build();

	vkb::Instance vkb_inst = inst_ret.value();

	//grab the instance
	instance = vkb_inst.instance;
	debugMessenger = vkb_inst.debug_messenger;

	// create a surface to render to
	SDL_Vulkan_CreateSurface( window, instance, &surface );

	//vulkan 1.3 features
	VkPhysicalDeviceVulkan13Features features13{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
	features13.dynamicRendering = true;
	features13.synchronization2 = true;

	//vulkan 1.2 features
	VkPhysicalDeviceVulkan12Features features12{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
	features12.bufferDeviceAddress = true;
	features12.descriptorIndexing = true;

	//use vkbootstrap to select a gpu.
	//We want a gpu that can write to the SDL surface and supports vulkan 1.3 with the correct features
	vkb::PhysicalDeviceSelector selector{ vkb_inst };
	vkb::PhysicalDevice physicalDeviceSelect = selector
		.set_minimum_version( 1, 3 )
		.set_required_features_13( features13 )
		.set_required_features_12( features12 )
		.set_surface( surface )
		.select()
		.value();

	//create the final vulkan device
	vkb::DeviceBuilder deviceBuilder{ physicalDeviceSelect };
	vkb::Device vkbDevice = deviceBuilder.build().value();

	// Get the VkDevice handle used in the rest of a vulkan application
	device = vkbDevice.device;
	physicalDevice = physicalDeviceSelect.physical_device;

	// use vkbootstrap to get a Graphics queue
	graphicsQueue = vkbDevice.get_queue( vkb::QueueType::graphics ).value();
	graphicsQueueFamilyIndex = vkbDevice.get_queue_index( vkb::QueueType::graphics ).value();

	// initialize the memory allocator
	VmaAllocatorCreateInfo allocatorInfo = {};
	allocatorInfo.physicalDevice = physicalDevice;
	allocatorInfo.device = device;
	allocatorInfo.instance = instance;
	allocatorInfo.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
	vmaCreateAllocator( &allocatorInfo, &allocator );

	mainDeletionQueue.push_function( [ & ] () {
		vmaDestroyAllocator( allocator ); // first example of deletion queue...
	});
}

void PrometheusInstance::initSwapchain () {
	createSwapchain( windowExtent.width, windowExtent.height );
}

void PrometheusInstance::initCommandStructures () {
	VkCommandPoolCreateInfo commandPoolInfo = vkinit::command_pool_create_info( graphicsQueueFamilyIndex, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);
	for ( int i = 0; i < FRAME_OVERLAP; i++ ) {
		// create a command pool allocator
		VK_CHECK( vkCreateCommandPool( device, &commandPoolInfo, nullptr, &frameData[ i ].commandPool ) );

		// and a command buffer from that command pool
		VkCommandBufferAllocateInfo cmdAllocInfo = vkinit::command_buffer_allocate_info( frameData[ i ].commandPool, 1 );
		VK_CHECK( vkAllocateCommandBuffers( device, &cmdAllocInfo, &frameData[ i ].mainCommandBuffer ) );
	}
}

void PrometheusInstance::initSyncStructures () {
	VkFenceCreateInfo fenceCreateInfo = vkinit::fence_create_info( VK_FENCE_CREATE_SIGNALED_BIT );
	VkSemaphoreCreateInfo semaphoreCreateInfo = vkinit::semaphore_create_info();
	for ( int i = 0; i < FRAME_OVERLAP; i++ ) {
	// we need to create one fence ( frame end mark )
		VK_CHECK( vkCreateFence( device, &fenceCreateInfo, nullptr, &frameData[ i ].renderFence ) );
	// and two semaphores: swapchain image ready, and render finished
		VK_CHECK( vkCreateSemaphore( device, &semaphoreCreateInfo, nullptr, &frameData[ i ].swapchainSemaphore ) );
		VK_CHECK( vkCreateSemaphore( device, &semaphoreCreateInfo, nullptr, &frameData[ i ].renderSemaphore ) );
	}
}

void PrometheusInstance::initDescriptors  () {
	//create a descriptor pool that will hold 10 sets with 1 image each
	std::vector< DescriptorAllocator::PoolSizeRatio > sizes = {
		{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 }
	};

	globalDescriptorAllocator.init_pool( device, 10, sizes );

	{ //make the descriptor set layout for our compute draw
		DescriptorLayoutBuilder builder;
		builder.add_binding( 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE );
		drawImageDescriptorLayout = builder.build( device, VK_SHADER_STAGE_COMPUTE_BIT );
	}

	drawImageDescriptors = globalDescriptorAllocator.allocate( device, drawImageDescriptorLayout );

	VkDescriptorImageInfo imgInfo{};
	imgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	imgInfo.imageView = drawImage.imageView;

	VkWriteDescriptorSet drawImageWrite = {};
	drawImageWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	drawImageWrite.pNext = nullptr;

	drawImageWrite.dstBinding = 0;
	drawImageWrite.dstSet = drawImageDescriptors;
	drawImageWrite.descriptorCount = 1;
	drawImageWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	drawImageWrite.pImageInfo = &imgInfo;

	vkUpdateDescriptorSets( device, 1, &drawImageWrite, 0, nullptr );

	//make sure both the descriptor allocator and the new layout get cleaned up properly
	mainDeletionQueue.push_function( [ & ] () {
		globalDescriptorAllocator.destroy_pool( device );
		vkDestroyDescriptorSetLayout( device, drawImageDescriptorLayout, nullptr );
	});
}

void PrometheusInstance::initPipelines () {
	initBackgroundPipelines();
}

void PrometheusInstance::initBackgroundPipelines () {
	VkPipelineLayoutCreateInfo computeLayout{};
	computeLayout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	computeLayout.pNext = nullptr;
	computeLayout.pSetLayouts = &drawImageDescriptorLayout;
	computeLayout.setLayoutCount = 1;

	VK_CHECK( vkCreatePipelineLayout( device, &computeLayout, nullptr, &gradientPipelineLayout ) );

	VkShaderModule computeDrawShader;
	if ( !vkutil::load_shader_module("../shaders/gradient.comp.spv", device, &computeDrawShader ) ) {
		fmt::print( "Error when building the compute shader \n" );
	}

	VkPipelineShaderStageCreateInfo stageinfo{};
	stageinfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stageinfo.pNext = nullptr;
	stageinfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	stageinfo.module = computeDrawShader;
	stageinfo.pName = "main";

	VkComputePipelineCreateInfo computePipelineCreateInfo{};
	computePipelineCreateInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	computePipelineCreateInfo.pNext = nullptr;
	computePipelineCreateInfo.layout = gradientPipelineLayout;
	computePipelineCreateInfo.stage = stageinfo;
	VK_CHECK( vkCreateComputePipelines( device, VK_NULL_HANDLE, 1, &computePipelineCreateInfo, nullptr, &gradientPipeline ) );

	vkDestroyShaderModule( device, computeDrawShader, nullptr );

	mainDeletionQueue.push_function( [ & ] () {
		vkDestroyPipelineLayout( device, gradientPipelineLayout, nullptr );
		vkDestroyPipeline( device, gradientPipeline, nullptr );
	});
}
//===========================================================================================================================
// swapchain helpers
//===========================================================================================================================
void PrometheusInstance::createSwapchain ( uint32_t w, uint32_t h ) {
	vkb::SwapchainBuilder swapchainBuilder{ physicalDevice, device, surface };
	swapchainImageFormat = VK_FORMAT_B8G8R8A8_UNORM;
	vkb::Swapchain vkbSwapchain = swapchainBuilder
		//.use_default_format_selection()
		.set_desired_format( VkSurfaceFormatKHR{ .format = swapchainImageFormat, .colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR } )
		//use vsync present mode
		.set_desired_present_mode( VK_PRESENT_MODE_FIFO_KHR )
		.set_desired_extent( w, h )
		.add_image_usage_flags( VK_IMAGE_USAGE_TRANSFER_DST_BIT )
		.build()
		.value();

	//store swapchain and its related images
	swapchain = vkbSwapchain.swapchain;
	swapchainExtent = vkbSwapchain.extent;
	swapchainImages = vkbSwapchain.get_images().value();
	swapchainImageViews = vkbSwapchain.get_image_views().value();

	// draw image size will match the window
	VkExtent3D drawImageExtent = {
		windowExtent.width,
		windowExtent.height,
		1
	};

	// hardcoding the draw format to 16 bit float
	drawImage.imageFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
	drawImage.imageExtent = drawImageExtent;

	VkImageUsageFlags drawImageUsages{};
	drawImageUsages |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	drawImageUsages |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	drawImageUsages |= VK_IMAGE_USAGE_STORAGE_BIT;
	drawImageUsages |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

	VkImageCreateInfo rimg_info = vkinit::image_create_info( drawImage.imageFormat, drawImageUsages, drawImageExtent );

	// for the draw image, we want to allocate it from gpu local memory
	VmaAllocationCreateInfo rimg_allocinfo = {};
	rimg_allocinfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
	rimg_allocinfo.requiredFlags = VkMemoryPropertyFlags( VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT );

	// allocate and create the image
	vmaCreateImage( allocator, &rimg_info, &rimg_allocinfo, &drawImage.image, &drawImage.allocation, nullptr );

	// build a image-view for the draw image to use for rendering
	VkImageViewCreateInfo rview_info = vkinit::imageview_create_info( drawImage.imageFormat, drawImage.image, VK_IMAGE_ASPECT_COLOR_BIT );

	VK_CHECK( vkCreateImageView( device, &rview_info, nullptr, &drawImage.imageView ) );

	// add to deletion queues
	mainDeletionQueue.push_function( [ = ] () {
		vkDestroyImageView( device, drawImage.imageView, nullptr );
		vmaDestroyImage( allocator, drawImage.image, drawImage.allocation );
	});
}

void PrometheusInstance::destroySwapchain () {
	vkDestroySwapchainKHR( device, swapchain, nullptr );
	for (int i = 0; i < swapchainImageViews.size(); i++ ) {
		// we are only destroying the imageViews, the images are owned by the OS
		vkDestroyImageView( device, swapchainImageViews[ i ], nullptr );
	}
}
