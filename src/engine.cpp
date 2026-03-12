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

#include "third_party/imgui/imgui.h"
#include "third_party/imgui/imgui_impl_sdl2.h"
#include "third_party/imgui/imgui_impl_vulkan.h"

#include <glm/gtx/transform.hpp>

//============================================================================================================================
//============================================================================================================================
// Initialization
//============================================================================================================================
void PrometheusInstance::Init () {
	// initializing SDL
	SDL_Init( SDL_INIT_VIDEO );
	SDL_WindowFlags windowFlags = ( SDL_WindowFlags ) ( SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE );

	SDL_Rect viewRect;
	SDL_GetDisplayBounds( 0, &viewRect );
	windowExtent.width = viewRect.w;
	windowExtent.height = viewRect.h;

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
	initImgui();
	initDefaultData();

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
	getCurrentFrame().frameDescriptors.clear_pools( device ); // mark the allocated descriptors as available

	// and now reset that fence so we can use it again, to signal this frame's completion
	VK_CHECK( vkResetFences( device, 1, &getCurrentFrame().renderFence ) );

	//request image from the swapchain
	uint32_t swapchainImageIndex;
	VkResult e = vkAcquireNextImageKHR( device, swapchain, 1000000000, getCurrentFrame().swapchainSemaphore, nullptr, &swapchainImageIndex );
	if ( e == VK_ERROR_OUT_OF_DATE_KHR ) {
		resizeRequest = true;
		return; // we will skip trying to draw the rest of the frame, because we have detected a swapchain mismatch
	}

	// Vulkan handles are aliased 64-bit pointers, basically shortens later code
	VkCommandBuffer cmd = getCurrentFrame().mainCommandBuffer;

	// because we've hit the fence, we are safe to reset the image buffer
	VK_CHECK( vkResetCommandBuffer( cmd, 0 ) );

	// begin the command buffer recording. We will use this command buffer exactly once, so we want to let vulkan know that
	VkCommandBufferBeginInfo cmdBeginInfo = vkinit::command_buffer_begin_info( VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT );

	// this is for render scaling
	drawExtent.height = std::min( swapchainExtent.height, drawImage.imageExtent.height ) * renderScale;
	drawExtent.width = std::min( swapchainExtent.width, drawImage.imageExtent.width ) * renderScale;

	// start the command buffer recording
	VK_CHECK( vkBeginCommandBuffer( cmd, &cmdBeginInfo ) );

	// put the draw image in a general layout
	vkutil::transition_image( cmd, drawImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL );

	drawBackground( cmd );

	vkutil::transition_image( cmd, drawImage.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL );
	vkutil::transition_image( cmd, depthImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL );

	drawGeometry( cmd );

	// transition the images for the copy
	vkutil::transition_image( cmd, drawImage.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL );
	vkutil::transition_image( cmd, swapchainImages[ swapchainImageIndex ], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL );

	// execute a copy from the draw image into the swapchain
	vkutil::copy_image_to_image( cmd, drawImage.image, swapchainImages[ swapchainImageIndex ], drawExtent, swapchainExtent );

	// set swapchain image layout to Attachment Optimal so we can draw it
	vkutil::transition_image( cmd, swapchainImages[ swapchainImageIndex ], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL );

	//draw imgui into the swapchain image
	drawImgui( cmd, swapchainImageViews[ swapchainImageIndex ] );

	// transition the image from layout general to ready-for-swapchain-handoff
	vkutil::transition_image( cmd, swapchainImages[ swapchainImageIndex ], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR );

	// Kill recording, and put it in "executable" state
	VK_CHECK( vkEndCommandBuffer( cmd ) );

	// before submitting to the queue, we need to specify the specific dependencies
	// we want to wait on the presentSemaphore, signaled when the swapchain is ready
	// we will signal the renderSemaphore, when rendering has finished
	VkCommandBufferSubmitInfo cmdinfo = vkinit::command_buffer_submit_info( cmd );
	VkSemaphoreSubmitInfo waitInfo = vkinit::semaphore_submit_info( VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT_KHR, getCurrentFrame().swapchainSemaphore );
	VkSemaphoreSubmitInfo signalInfo = vkinit::semaphore_submit_info( VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, swapchainPresentSemaphores[ swapchainImageIndex ] );

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
	presentInfo.pWaitSemaphores = &swapchainPresentSemaphores[ swapchainImageIndex ];
	presentInfo.waitSemaphoreCount = 1;
	presentInfo.pImageIndices = &swapchainImageIndex;

	VkResult presentResult = vkQueuePresentKHR( graphicsQueue, &presentInfo );
	if ( presentResult == VK_ERROR_OUT_OF_DATE_KHR ) {
		resizeRequest = true; // swapchain mismatch
	}

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

			if ( e.type == SDL_KEYUP && e.key.keysym.scancode == SDL_SCANCODE_ESCAPE ) {
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

			//send SDL event to imgui for handling
			ImGui_ImplSDL2_ProcessEvent( &e );
		}

		// handling minimized application
		if ( stopRendering ) {
			// throttle the speed to avoid busy loop
			std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
		} else {
			// imgui new frame
			ImGui_ImplVulkan_NewFrame();
			ImGui_ImplSDL2_NewFrame();
			ImGui::NewFrame();

			//some imgui UI to test
			// ImGui::ShowDemoWindow();

			if ( ImGui::Begin( "Edit" ) ) {
				ImGui::SliderFloat( "Render Scale", &renderScale, 0.3f, 1.0f );
				ImGui::ColorPicker3( "Color 1", ( float * ) &computeEffects[ 0 ].data.data1[ 0 ] );
				ImGui::ColorPicker3( "Color 2", ( float * ) &computeEffects[ 0 ].data.data2[ 0 ] );
			}
			ImGui::End();

			//make imgui calculate internal draw structures
			ImGui::Render();

			// we're ready to draw the next frame
			Draw();
		}
	}

	// checking to see if we have flagged a window resize
	if ( resizeRequest ) {
		resizeSwapchain();
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
			vkDestroySemaphore( device, frameData[ i ].swapchainSemaphore, nullptr );

			// delete any remaining per-frame resources...
			frameData[ i ].deletionQueue.flush();
		}

		for ( auto& s : swapchainPresentSemaphores ) {
			vkDestroySemaphore( device, s, nullptr );
		}

		for ( auto& mesh : testMeshes ) {
			destroyBuffer( mesh->meshBuffers.indexBuffer );
			destroyBuffer( mesh->meshBuffers.vertexBuffer );
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

	{
		// reporting some platform info
		VkPhysicalDeviceProperties temp;
		vkGetPhysicalDeviceProperties( vkbDevice.physical_device, &temp );

		std::string GPUType;
		switch ( temp.deviceType ) {
			case VK_PHYSICAL_DEVICE_TYPE_OTHER: GPUType = "Other GPU"; break;
			case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: GPUType = "Integrated GPU"; break;
			case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: GPUType = "Discrete GPU"; break;
			case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: GPUType = "Virtual GPU"; break;
			case VK_PHYSICAL_DEVICE_TYPE_CPU: GPUType = "CPU as GPU"; break;
			default: GPUType = "Unknown"; break;
		}
		fmt::print( "Running on {} ({})", temp.deviceName, GPUType );
		fmt::print( "\n\nDevice Limits:\n" );
		// fmt::print( "{}\n" );
		// fmt::print( "{}\n" );
		fmt::print( "Max Push Constant Size: {}\n", temp.limits.maxPushConstantsSize );
		fmt::print( "Max Compute Workgroup Size: {}x {}y {}z\n", temp.limits.maxComputeWorkGroupSize[ 0 ], temp.limits.maxComputeWorkGroupSize[ 1 ], temp.limits.maxComputeWorkGroupSize[ 2 ] );
		fmt::print( "Max Compute Workgroup Invocations (single workgroup): {}\n", temp.limits.maxComputeWorkGroupInvocations );
		fmt::print( "Max Compute Workgroup Count: {}x {}y {}z\n", temp.limits.maxComputeWorkGroupCount[ 0 ], temp.limits.maxComputeWorkGroupCount[ 1 ], temp.limits.maxComputeWorkGroupCount[ 2 ] );
		fmt::print( "Max Compute Shared Memory Size: {}\n\n", temp.limits.maxComputeSharedMemorySize );
		fmt::print( "Max Storage Buffer Range: {}\n", temp.limits.maxStorageBufferRange );
		fmt::print( "Max Framebuffer Width: {}\n", temp.limits.maxFramebufferWidth );
		fmt::print( "Max Framebuffer Height: {}\n", temp.limits.maxFramebufferHeight );
		fmt::print( "Max Image Dimension(1D): {}\n", temp.limits.maxImageDimension1D );
		fmt::print( "Max Image Dimension(2D): {}\n", temp.limits.maxImageDimension2D );
		fmt::print( "Max Image Dimension(3D): {}\n", temp.limits.maxImageDimension3D );
		fmt::print( "\n\n" );
	}

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
	VK_CHECK( vkCreateCommandPool( device, &commandPoolInfo, nullptr, &immediateCommandPool ) );

	// allocating the command buffer for immediate submits
	VkCommandBufferAllocateInfo cmdAllocInfo = vkinit::command_buffer_allocate_info( immediateCommandPool, 1 );
	VK_CHECK( vkAllocateCommandBuffers( device, &cmdAllocInfo, &immediateCommandBuffer ) );

	mainDeletionQueue.push_function( [ = ] ()  {
		vkDestroyCommandPool( device, immediateCommandPool, nullptr );
	});
}

void PrometheusInstance::initSyncStructures () {
	VkFenceCreateInfo fenceCreateInfo = vkinit::fence_create_info( VK_FENCE_CREATE_SIGNALED_BIT );
	VkSemaphoreCreateInfo semaphoreCreateInfo = vkinit::semaphore_create_info();
	for ( int i = 0; i < FRAME_OVERLAP; i++ ) {
	// we need to create one fence ( frame end mark )
		VK_CHECK( vkCreateFence( device, &fenceCreateInfo, nullptr, &frameData[ i ].renderFence ) );
	// and two semaphores: swapchain image ready, and render finished
		VK_CHECK( vkCreateSemaphore( device, &semaphoreCreateInfo, nullptr, &frameData[ i ].swapchainSemaphore ) );
	}

	swapchainPresentSemaphores.resize( swapchainImages.size() );
	for ( int i = 0; i < swapchainImages.size(); i++ ) {
		VK_CHECK( vkCreateSemaphore( device, &semaphoreCreateInfo, nullptr, &swapchainPresentSemaphores[ i ] ) );
	}

	VK_CHECK( vkCreateFence( device, &fenceCreateInfo, nullptr, &immediateFence ) );
	mainDeletionQueue.push_function( [ = ] ()  { vkDestroyFence( device, immediateFence, nullptr ); });
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

	{
		DescriptorLayoutBuilder builder;
		builder.add_binding( 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER );
		gpuSceneDataDescriptorLayout = builder.build( device, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT );
	}

	{
		DescriptorLayoutBuilder builder;
		builder.add_binding( 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER );
		singleImageDescriptorLayout = builder.build( device, VK_SHADER_STAGE_FRAGMENT_BIT );
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

	for ( int i = 0; i < FRAME_OVERLAP; i++ ) {
		// create a descriptor pool
		std::vector< DescriptorAllocatorGrowable::PoolSizeRatio > frameSizes = {
			{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3 },
			{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 },
			{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3 },
			{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4 },
		};

		frameData[ i ].frameDescriptors = DescriptorAllocatorGrowable{};
		frameData[ i ].frameDescriptors.init( device, 1000, frameSizes );

		mainDeletionQueue.push_function([ &, i ]() {
			frameData[ i ].frameDescriptors.destroy_pools( device );
		});
	}
}

void PrometheusInstance::initPipelines () {
	// compute pipelines
	initBackgroundPipelines();

	// graphics pipelines
	initTrianglePipeline();
	initMeshPipeline();
}

void PrometheusInstance::initBackgroundPipelines () {
	VkPipelineLayoutCreateInfo computeLayout{};
	computeLayout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	computeLayout.pNext = nullptr;
	computeLayout.pSetLayouts = &drawImageDescriptorLayout;
	computeLayout.setLayoutCount = 1;

	VkPushConstantRange pushConstant{};
	pushConstant.offset = 0;
	pushConstant.size = sizeof( ComputePushConstants ) ;
	pushConstant.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

	computeLayout.pPushConstantRanges = &pushConstant;
	computeLayout.pushConstantRangeCount = 1;

	ComputeEffect gradient;
	VK_CHECK( vkCreatePipelineLayout( device, &computeLayout, nullptr, &gradient.layout ) );

	VkShaderModule computeDrawShader;
	if ( !vkutil::load_shader_module("../shaders/gradient.comp.spv", device, &computeDrawShader ) ) {
		fmt::print( "Error when building the compute shader\n" );
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
	computePipelineCreateInfo.layout = gradient.layout;
	computePipelineCreateInfo.stage = stageinfo;

	gradient.layout = gradient.layout;
	gradient.name = "gradient";
	gradient.data = {};

	// default colors
	gradient.data.data1 = glm::vec4( 1.0f, 0.0f, 0.0f, 1.0f );
	gradient.data.data2 = glm::vec4( 0.0f, 0.0f, 1.0f, 1.0f );

	// create the pipeline
	VK_CHECK( vkCreateComputePipelines( device, VK_NULL_HANDLE, 1, &computePipelineCreateInfo, nullptr, &gradient.pipeline ) );
	vkDestroyShaderModule( device, computeDrawShader, nullptr );

	// add it to the list:
	computeEffects.push_back( gradient );

	// deletors for the pipeline layout + pipeline
	mainDeletionQueue.push_function( [ & ] () {
		vkDestroyPipelineLayout( device, gradient.layout, nullptr );
		vkDestroyPipeline( device, gradient.pipeline, nullptr );
	});
}

void PrometheusInstance::initTrianglePipeline () {
	VkShaderModule triangleFragShader;
	if ( !vkutil::load_shader_module( "../shaders/colored_triangle.frag.spv", device, &triangleFragShader ) ) {
		fmt::print( "Error when building the triangle fragment shader module\n" );
	} else {
		fmt::print( "Triangle fragment shader successfully loaded\n" );
	}

	VkShaderModule triangleVertexShader;
	if ( !vkutil::load_shader_module( "../shaders/colored_triangle.vert.spv", device, &triangleVertexShader ) ) {
		fmt::print( "Error when building the triangle vertex shader module\n" );
	} else {
		fmt::print( "Triangle vertex shader successfully loaded\n" );
	}

	//build the pipeline layout that controls the inputs/outputs of the shader
	//we are not using descriptor sets or other systems yet, so no need to use anything other than empty default
	VkPipelineLayoutCreateInfo pipeline_layout_info = vkinit::pipeline_layout_create_info();
	VK_CHECK( vkCreatePipelineLayout( device, &pipeline_layout_info, nullptr, &trianglePipelineLayout ) );

	PipelineBuilder pipelineBuilder;

	//use the triangle layout we created
	pipelineBuilder._pipelineLayout = trianglePipelineLayout;
	//connecting the vertex and pixel shaders to the pipeline
	pipelineBuilder.set_shaders( triangleVertexShader, triangleFragShader );
	//it will draw triangles
	pipelineBuilder.set_input_topology( VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST );
	//filled triangles
	pipelineBuilder.set_polygon_mode( VK_POLYGON_MODE_FILL );
	//no backface culling
	pipelineBuilder.set_cull_mode( VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE );
	//no multisampling
	pipelineBuilder.set_multisampling_none();
	//no blending
	pipelineBuilder.disable_blending();
	//no depth testing
	pipelineBuilder.disable_depthtest();

	//connect the image format we will draw into, from draw image
	pipelineBuilder.set_color_attachment_format( drawImage.imageFormat );
	pipelineBuilder.set_depth_format( depthImage.imageFormat );

	//finally build the pipeline
	trianglePipeline = pipelineBuilder.build_pipeline( device );

	//clean structures
	vkDestroyShaderModule( device, triangleFragShader, nullptr );
	vkDestroyShaderModule( device, triangleVertexShader, nullptr );

	mainDeletionQueue.push_function( [ & ] ()  {
		vkDestroyPipelineLayout( device, trianglePipelineLayout, nullptr );
		vkDestroyPipeline( device, trianglePipeline, nullptr );
	});
}

void PrometheusInstance::initMeshPipeline () {
	VkShaderModule meshFragShader;
	if ( !vkutil::load_shader_module( "../shaders/tex_img.frag.spv", device, &meshFragShader ) ) {
		fmt::print( "Error when building the triangle fragment shader module\n" );
	} else {
		fmt::print( "Triangle fragment shader successfully loaded\n" );
	}

	VkShaderModule meshVertexShader;
	if ( !vkutil::load_shader_module( "../shaders/colored_triangle_mesh.vert.spv", device, &meshVertexShader ) ) {
		fmt::print( "Error when building the triangle vertex shader module\n" );
	} else {
		fmt::print( "Triangle vertex shader successfully loaded\n" );
	}

	//build the pipeline layout that controls the inputs/outputs of the shader
	//we are not using descriptor sets or other systems yet, so no need to use anything other than empty default

	VkPushConstantRange bufferRange{};
	bufferRange.offset = 0;
	bufferRange.size = sizeof( GPUDrawPushConstants );
	bufferRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

	VkPipelineLayoutCreateInfo pipeline_layout_info = vkinit::pipeline_layout_create_info();
	pipeline_layout_info.pPushConstantRanges = &bufferRange;
	pipeline_layout_info.pushConstantRangeCount = 1;

	// descriptor for the single image sampler
	pipeline_layout_info.pSetLayouts = &singleImageDescriptorLayout;
	pipeline_layout_info.setLayoutCount = 1;

	VK_CHECK( vkCreatePipelineLayout( device, &pipeline_layout_info, nullptr, &meshPipelineLayout ) );

	PipelineBuilder pipelineBuilder;

	//use the triangle layout we created
	pipelineBuilder._pipelineLayout = meshPipelineLayout;
	//connecting the vertex and pixel shaders to the pipeline
	pipelineBuilder.set_shaders( meshVertexShader, meshFragShader );
	//it will draw triangles
	pipelineBuilder.set_input_topology( VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST );
	//filled triangles
	pipelineBuilder.set_polygon_mode( VK_POLYGON_MODE_FILL );
	//no backface culling
	pipelineBuilder.set_cull_mode( VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE );
	//no multisampling
	pipelineBuilder.set_multisampling_none();
	// alpha blending
	pipelineBuilder.enable_blending_alphablend();
	// depth testing
	pipelineBuilder.enable_depthtest( true, VK_COMPARE_OP_GREATER_OR_EQUAL );

	//connect the image format we will draw into, from draw image
	pipelineBuilder.set_color_attachment_format( drawImage.imageFormat );
	pipelineBuilder.set_depth_format( depthImage.imageFormat );

	//finally build the pipeline
	meshPipeline = pipelineBuilder.build_pipeline( device );

	//clean structures
	vkDestroyShaderModule( device, meshFragShader, nullptr );
	vkDestroyShaderModule( device, meshVertexShader, nullptr );

	mainDeletionQueue.push_function( [ & ] ()  {
		vkDestroyPipelineLayout( device, meshPipelineLayout, nullptr );
		vkDestroyPipeline( device, meshPipeline, nullptr );
	});
}

AllocatedBuffer PrometheusInstance::createBuffer ( size_t allocSize, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage ) {
	// allocate buffer
	VkBufferCreateInfo bufferInfo = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bufferInfo.pNext = nullptr;
	bufferInfo.size = allocSize;
	bufferInfo.usage = usage;

	VmaAllocationCreateInfo vmaallocInfo = {};
	vmaallocInfo.usage = memoryUsage;
	vmaallocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
	AllocatedBuffer newBuffer;

	// allocate the buffer
	VK_CHECK( vmaCreateBuffer( allocator, &bufferInfo, &vmaallocInfo, &newBuffer.buffer, &newBuffer.allocation, &newBuffer.info ) );

	return newBuffer;
}

void PrometheusInstance::destroyBuffer ( const AllocatedBuffer& buffer ) {
	vmaDestroyBuffer( allocator, buffer.buffer, buffer.allocation );
}

AllocatedImage PrometheusInstance::createImage ( VkExtent3D size, VkFormat format, VkImageUsageFlags usage, bool mipmapped ) {
	AllocatedImage newImage;
	newImage.imageFormat = format;
	newImage.imageExtent = size;

	VkImageCreateInfo img_info = vkinit::image_create_info( format, usage, size );
	if ( mipmapped ) {
		img_info.mipLevels = static_cast<uint32_t>( std::floor( std::log2( std::max( size.width, size.height ) ) ) ) + 1;
	}

	// always allocate images on dedicated GPU memory
	VmaAllocationCreateInfo allocinfo = {};
	allocinfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
	allocinfo.requiredFlags = VkMemoryPropertyFlags( VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT );

	// allocate and create the image
	VK_CHECK( vmaCreateImage( allocator, &img_info, &allocinfo, &newImage.image, &newImage.allocation, nullptr ) );

	// if the format is a depth format, we will need to have it use the correct aspect flag
	VkImageAspectFlags aspectFlag = VK_IMAGE_ASPECT_COLOR_BIT;
	if ( format == VK_FORMAT_D32_SFLOAT ) {
		aspectFlag = VK_IMAGE_ASPECT_DEPTH_BIT;
	}

	// build a image-view for the image
	VkImageViewCreateInfo view_info = vkinit::imageview_create_info( format, newImage.image, aspectFlag );
	view_info.subresourceRange.levelCount = img_info.mipLevels;

	VK_CHECK( vkCreateImageView( device, &view_info, nullptr, &newImage.imageView ) );

	return newImage;
}

AllocatedImage PrometheusInstance::createImage ( void* data, VkExtent3D size, VkFormat format, VkImageUsageFlags usage, bool mipmapped ) {
	size_t dataSize = size.depth * size.width * size.height * 4;
	AllocatedBuffer uploadbuffer = createBuffer( dataSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU );

	// data from the void pointer, copied to the upload buffer
	memcpy( uploadbuffer.info.pMappedData, data, dataSize );

	// call to the read/write styled image creation function
	AllocatedImage new_image = createImage( size, format, usage | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, mipmapped );

	// immediate mode submission, to copy the upload buffer to the allocated image
	immediateSubmit( [ & ] ( VkCommandBuffer cmd ) {
		vkutil::transition_image( cmd, new_image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL );

		VkBufferImageCopy copyRegion = {};
		copyRegion.bufferOffset = 0;
		copyRegion.bufferRowLength = 0;
		copyRegion.bufferImageHeight = 0;

		copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copyRegion.imageSubresource.mipLevel = 0;
		copyRegion.imageSubresource.baseArrayLayer = 0;
		copyRegion.imageSubresource.layerCount = 1;
		copyRegion.imageExtent = size;

		// copy the buffer into the image
		vkCmdCopyBufferToImage( cmd, uploadbuffer.buffer, new_image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion );

		// flagging the data as read-only, for shader reading... could just as easily do
		vkutil::transition_image( cmd, new_image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL );
	});

	// finished uploading, that data is now available
	destroyBuffer( uploadbuffer );

	return new_image;
}

void PrometheusInstance::destroyImage ( const AllocatedImage& img ) {
	vkDestroyImageView( device, img.imageView, nullptr );
	vmaDestroyImage( allocator, img.image, img.allocation );
}

GPUMeshBuffers PrometheusInstance::uploadMesh ( std::span<uint32_t> indices, std::span<Vertex> vertices ) {
	const size_t vertexBufferSize = vertices.size() * sizeof( Vertex );
	const size_t indexBufferSize = indices.size() * sizeof( uint32_t );

	GPUMeshBuffers newSurface;

	//create vertex buffer
	newSurface.vertexBuffer = createBuffer( vertexBufferSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
		VMA_MEMORY_USAGE_GPU_ONLY );

	//find the adress of the vertex buffer
	VkBufferDeviceAddressInfo deviceAdressInfo{
		.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
		.buffer = newSurface.vertexBuffer.buffer };
	newSurface.vertexBufferAddress = vkGetBufferDeviceAddress( device, &deviceAdressInfo );

	//create index buffer
	newSurface.indexBuffer = createBuffer( indexBufferSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		VMA_MEMORY_USAGE_GPU_ONLY );

	AllocatedBuffer staging = createBuffer( vertexBufferSize + indexBufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY );

	void* data = staging.allocation->GetMappedData();

	// copy vertex buffer
	memcpy( data, vertices.data(), vertexBufferSize );
	// copy index buffer
	memcpy( ( char* )data + vertexBufferSize, indices.data(), indexBufferSize );

	immediateSubmit( [&] ( VkCommandBuffer cmd ) {
		VkBufferCopy vertexCopy{ 0 };
		vertexCopy.dstOffset = 0;
		vertexCopy.srcOffset = 0;
		vertexCopy.size = vertexBufferSize;

		vkCmdCopyBuffer( cmd, staging.buffer, newSurface.vertexBuffer.buffer, 1, &vertexCopy );

		VkBufferCopy indexCopy{ 0 };
		indexCopy.dstOffset = 0;
		indexCopy.srcOffset = vertexBufferSize;
		indexCopy.size = indexBufferSize;

		vkCmdCopyBuffer( cmd, staging.buffer, newSurface.indexBuffer.buffer, 1, &indexCopy );
	});

	// immediate submit completes
	destroyBuffer( staging );

	// buffer contents are populated
	return newSurface;
}

void PrometheusInstance::initDefaultData () {

// BASIC TEST MESH
	testMeshes = loadGLTFMeshes( this,"..\\assets\\basicmesh.glb" ).value();

	std::array<Vertex, 4> rect_vertices;

	rect_vertices[ 0 ].position = {  0.5f,-0.5f, 0.0f };
	rect_vertices[ 1 ].position = {  0.5f, 0.5f, 0.0f };
	rect_vertices[ 2 ].position = { -0.5f,-0.5f, 0.0f };
	rect_vertices[ 3 ].position = { -0.5f, 0.5f, 0.0f };

	rect_vertices[ 0 ].color = { 0.0f, 0.0f, 0.0f, 1.0f };
	rect_vertices[ 1 ].color = { 0.5f, 0.5f, 0.5f, 1.0f };
	rect_vertices[ 2 ].color = { 1.0f, 0.0f, 0.0f, 1.0f };
	rect_vertices[ 3 ].color = { 0.0f, 1.0f, 0.0f, 1.0f };

	std::array<uint32_t, 6> rect_indices;

	rect_indices[ 0 ] = 0;
	rect_indices[ 1 ] = 1;
	rect_indices[ 2 ] = 2;

	rect_indices[ 3 ] = 2;
	rect_indices[ 4 ] = 1;
	rect_indices[ 5 ] = 3;

	rectangle = uploadMesh( rect_indices, rect_vertices );

	//delete the rectangle data on engine shutdown
	mainDeletionQueue.push_function([&](){
		destroyBuffer( rectangle.indexBuffer );
		destroyBuffer( rectangle.vertexBuffer );
	});

// TEXTURES
	// 3 default textures, white, grey, black. 1 pixel each
	uint32_t white = glm::packUnorm4x8( glm::vec4( 1.0f, 1.0f, 1.0f, 1.0f ) );
	whiteImage = createImage( ( void * ) &white, VkExtent3D{ 1, 1, 1 }, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT );

	uint32_t grey = glm::packUnorm4x8(glm::vec4( 0.66f, 0.66f, 0.66f, 1 ) );
	greyImage = createImage( ( void * ) &grey, VkExtent3D{ 1, 1, 1 }, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT );

	uint32_t black = glm::packUnorm4x8(glm::vec4(0, 0, 0, 0 ) );
	blackImage = createImage( ( void * ) &black, VkExtent3D{ 1, 1, 1 }, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT );

	//checkerboard image
	uint32_t magenta = glm::packUnorm4x8( glm::vec4( 1, 0, 1, 1 ) );
	std::array< uint32_t, 16 * 16 > pixels; //for 16x16 checkerboard texture
	for ( int x = 0; x < 16; x++ ) {
		for ( int y = 0; y < 16; y++ ) {
			pixels[ y * 16 + x ] = ( ( x % 2) ^ ( y % 2 ) ) ? magenta : black;
		}
	}
	errorCheckerboardImage = createImage( pixels.data(), VkExtent3D{ 16, 16, 1 }, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT );

// SAMPLER OBJECTS
	VkSamplerCreateInfo sampl = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };

	sampl.magFilter = VK_FILTER_NEAREST;
	sampl.minFilter = VK_FILTER_NEAREST;

	vkCreateSampler( device, &sampl, nullptr, &defaultSamplerNearest );

	sampl.magFilter = VK_FILTER_LINEAR;
	sampl.minFilter = VK_FILTER_LINEAR;
	vkCreateSampler( device, &sampl, nullptr, &defaultSamplerLinear );

	mainDeletionQueue.push_function([&](){
		vkDestroySampler( device, defaultSamplerNearest,nullptr );
		vkDestroySampler( device, defaultSamplerLinear,nullptr );

		destroyImage( whiteImage );
		destroyImage( greyImage );
		destroyImage( blackImage );
		destroyImage( errorCheckerboardImage );
	});
}

void PrometheusInstance::drawBackground ( VkCommandBuffer cmd ) const {
	// the gradient draw compute effect
	const ComputeEffect& effect = computeEffects[ 0 ];

	// bind the gradient drawing compute pipeline
	vkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, effect.pipeline );

	// bind the descriptor set containing the draw image for the compute pipeline
	vkCmdBindDescriptorSets( cmd, VK_PIPELINE_BIND_POINT_COMPUTE,  effect.layout, 0, 1, &drawImageDescriptors, 0, nullptr );

	// pushing the new values of the push constants (mirrors uniform usage)
	vkCmdPushConstants( cmd, effect.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof( ComputePushConstants ), &effect.data );

	// execute the compute pipeline dispatch. We are using 16x16 workgroup size so we need to divide by it
	vkCmdDispatch( cmd, std::ceil( drawExtent.width / 16.0f ), std::ceil( drawExtent.height / 16.0f ), 1 );
}

void PrometheusInstance::drawGeometry ( VkCommandBuffer cmd ) {
	//begin a render pass  connected to our draw image
	VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info( drawImage.imageView, nullptr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL );
	VkRenderingAttachmentInfo depthAttachment = vkinit::depth_attachment_info( depthImage.imageView, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL );
	VkRenderingInfo renderInfo = vkinit::rendering_info( drawExtent, &colorAttachment, &depthAttachment );

	//allocate a new uniform buffer for the scene data
	AllocatedBuffer gpuSceneDataBuffer = createBuffer( sizeof( BasicGPUSceneData ), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU );

	//add it to the deletion queue of this frame so it gets deleted once its been used
	getCurrentFrame().deletionQueue.push_function( [ =, this ] () {
		destroyBuffer( gpuSceneDataBuffer );
	});

	//write the buffer
	BasicGPUSceneData* sceneUniformData = ( BasicGPUSceneData * ) gpuSceneDataBuffer.allocation->GetMappedData();
	*sceneUniformData = sceneData;

	//create a descriptor set that binds that buffer and update it
	VkDescriptorSet globalDescriptor = getCurrentFrame().frameDescriptors.allocate( device, gpuSceneDataDescriptorLayout);

	DescriptorWriter writer;
	writer.write_buffer( 0, gpuSceneDataBuffer.buffer, sizeof( BasicGPUSceneData ), 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER );
	writer.update_set( device, globalDescriptor );

	vkCmdBeginRendering( cmd, &renderInfo);
	vkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, trianglePipeline );

	//set dynamic viewport and scissor
	VkViewport viewport = {};
	viewport.x = 0;
	viewport.y = 0;
	viewport.width = drawExtent.width;
	viewport.height = drawExtent.height;
	viewport.minDepth = 0.0f;
	viewport.maxDepth = 1.0f;
	vkCmdSetViewport( cmd, 0, 1, &viewport );

	VkRect2D scissor = {};
	scissor.offset.x = 0;
	scissor.offset.y = 0;
	scissor.extent.width = drawExtent.width;
	scissor.extent.height = drawExtent.height;
	vkCmdSetScissor( cmd, 0, 1, &scissor );

	//launch a draw command to draw 3 vertices
	vkCmdDraw( cmd, 3, 1, 0, 0 );

	// now draw the "mesh"
	vkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshPipeline );

	// dynamic descriptor allocation, to bind a texture
	VkDescriptorSet imageSet = getCurrentFrame().frameDescriptors.allocate( device, singleImageDescriptorLayout );
	{
		DescriptorWriter writer;
		writer.write_image( 0, errorCheckerboardImage.imageView, defaultSamplerNearest, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER );
		writer.update_set( device, imageSet );
	}

	vkCmdBindDescriptorSets( cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshPipelineLayout, 0, 1, &imageSet, 0, nullptr );

	GPUDrawPushConstants push_constants;
	push_constants.worldMatrix = glm::mat4{ 1.0f };
	push_constants.vertexBuffer = rectangle.vertexBufferAddress;

	vkCmdPushConstants( cmd, meshPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof( GPUDrawPushConstants ), &push_constants );
	vkCmdBindIndexBuffer( cmd, rectangle.indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32 );

	vkCmdDrawIndexed( cmd, 6, 1, 0, 0, 0 );

	push_constants.vertexBuffer = testMeshes[ 2 ]->meshBuffers.vertexBufferAddress;

	// dynamic rotation via push constant
	static float rot = 0.0f;
	rot += 0.01f;

	// view and camera projection
	glm::mat4 view = glm::rotate( glm::translate( glm::vec3{ 0.0f,0.0f,-5.0f } ), rot, glm::vec3( 0.0f, 1.0f, 0.0f ) );
	glm::mat4 projection = glm::perspective( glm::radians( 70.0f ), ( float ) drawExtent.width / ( float ) drawExtent.height, 10000.f, 0.1f );

	// invert the Y direction on projection matrix so that we are more similar to opengl and gltf axis
	projection[ 1 ][ 1 ] *= -1.0f;

	push_constants.worldMatrix = projection * view;
	vkCmdPushConstants( cmd, meshPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof( GPUDrawPushConstants ), &push_constants );
	vkCmdBindIndexBuffer( cmd, testMeshes[ 2 ]->meshBuffers.indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32 );

	vkCmdDrawIndexed( cmd, testMeshes[ 2 ]->surfaces[ 0 ].count, 1, testMeshes[ 2 ]->surfaces[ 0 ].startIndex, 0, 0 );

	vkCmdEndRendering( cmd );
}

void PrometheusInstance::initImgui () {
	// 1: create descriptor pool for IMGUI
	//  the size of the pool is very oversize, but it's copied from imgui demo
	//  itself.
	VkDescriptorPoolSize pool_sizes[] = { { VK_DESCRIPTOR_TYPE_SAMPLER, 1000 },
		{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000 },
		{ VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000 },
		{ VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000 },
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000 },
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000 },
		{ VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000 } };

	VkDescriptorPoolCreateInfo pool_info = {};
	pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
	pool_info.maxSets = 1000;
	pool_info.poolSizeCount = ( uint32_t ) std::size(pool_sizes);
	pool_info.pPoolSizes = pool_sizes;

	VkDescriptorPool imguiPool;
	VK_CHECK( vkCreateDescriptorPool( device, &pool_info, nullptr, &imguiPool ) );

	// 2: initialize imgui library
	// this initializes the core structures of imgui
	ImGui::CreateContext();

	// this initializes imgui for SDL
	ImGui_ImplSDL2_InitForVulkan( window );

	// this initializes imgui for Vulkan
	ImGui_ImplVulkan_InitInfo init_info = {};
	init_info.Instance = instance;
	init_info.PhysicalDevice = physicalDevice;
	init_info.Device = device;
	init_info.Queue = graphicsQueue;
	init_info.DescriptorPool = imguiPool;
	init_info.MinImageCount = 3;
	init_info.ImageCount = 3;
	init_info.UseDynamicRendering = true;

	//dynamic rendering parameters for imgui to use
	init_info.PipelineRenderingCreateInfo = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
	init_info.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
	init_info.PipelineRenderingCreateInfo.pColorAttachmentFormats = &swapchainImageFormat;


	init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;

	ImGui_ImplVulkan_Init( &init_info );
	ImGui_ImplVulkan_CreateFontsTexture();

	// add the destroy the imgui created structures
	mainDeletionQueue.push_function( [ = ] ()  {
		ImGui_ImplVulkan_Shutdown();
		vkDestroyDescriptorPool( device, imguiPool, nullptr );
	});
}

//===========================================================================================================================
// swapchain helpers
//===========================================================================================================================
void PrometheusInstance::resizeSwapchain () {
	// wait till the device shows as idle
	vkDeviceWaitIdle( device );

	// kill the existing swapchain
	destroySwapchain();

	// use SDL to find the new window size
	int w, h;
	SDL_GetWindowSize( window, &w, &h );
	windowExtent.width = w;
	windowExtent.height = h;

	// create the new swapchain and rearm trigger
	createSwapchain( w, h );
	resizeRequest = false;
}

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
		// 64, // custom hacked in resolution
		// 64,
		1
	};

	// draw image config
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
	// allocate and create the color image
	vmaCreateImage( allocator, &rimg_info, &rimg_allocinfo, &drawImage.image, &drawImage.allocation, nullptr );
	// build a image-view for the draw image to use for rendering
	VkImageViewCreateInfo rview_info = vkinit::imageview_create_info( drawImage.imageFormat, drawImage.image, VK_IMAGE_ASPECT_COLOR_BIT );
	VK_CHECK( vkCreateImageView( device, &rview_info, nullptr, &drawImage.imageView ) );

	// depth image config
	depthImage.imageFormat = VK_FORMAT_D32_SFLOAT;
	depthImage.imageExtent = drawImageExtent;
	VkImageUsageFlags depthImageUsages{};
	depthImageUsages |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	VkImageCreateInfo dimg_info = vkinit::image_create_info( depthImage.imageFormat, depthImageUsages, drawImageExtent );
	//allocate and create the depth image
	vmaCreateImage( allocator, &dimg_info, &rimg_allocinfo, &depthImage.image, &depthImage.allocation, nullptr );
	// build a image-view for the draw image to use for rendering
	VkImageViewCreateInfo dview_info = vkinit::imageview_create_info( depthImage.imageFormat, depthImage.image, VK_IMAGE_ASPECT_DEPTH_BIT );
	VK_CHECK( vkCreateImageView( device, &dview_info, nullptr, &depthImage.imageView ) );


	// add to deletion queues
	mainDeletionQueue.push_function( [ = ] () {
		vkDestroyImageView( device, drawImage.imageView, nullptr );
		vmaDestroyImage( allocator, drawImage.image, drawImage.allocation );

		vkDestroyImageView( device, depthImage.imageView, nullptr );
		vmaDestroyImage( allocator, depthImage.image, depthImage.allocation );
	});
}

void PrometheusInstance::destroySwapchain () {
	vkDestroySwapchainKHR( device, swapchain, nullptr );
	for (int i = 0; i < swapchainImageViews.size(); i++ ) {
		// we are only destroying the imageViews, the images are owned by the OS
		vkDestroyImageView( device, swapchainImageViews[ i ], nullptr );
	}
}

void PrometheusInstance::immediateSubmit( std::function< void( VkCommandBuffer cmd ) > && function ) {
	VK_CHECK( vkResetFences( device, 1, &immediateFence ) );
	VK_CHECK( vkResetCommandBuffer( immediateCommandBuffer, 0 ) );

	VkCommandBuffer cmd = immediateCommandBuffer;
	VkCommandBufferBeginInfo cmdBeginInfo = vkinit::command_buffer_begin_info( VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT );

	VK_CHECK( vkBeginCommandBuffer( cmd, &cmdBeginInfo ) );
	function( cmd );
	VK_CHECK( vkEndCommandBuffer( cmd ) );

	VkCommandBufferSubmitInfo cmdinfo = vkinit::command_buffer_submit_info( cmd );
	VkSubmitInfo2 submit = vkinit::submit_info( &cmdinfo, nullptr, nullptr );

	// submit command buffer to the queue and execute it.
	//  _renderFence will now block until the graphic commands finish execution
	VK_CHECK( vkQueueSubmit2( graphicsQueue, 1, &submit, immediateFence ) );
	VK_CHECK( vkWaitForFences( device, 1, &immediateFence, true, 9999999999 ) );
}

void PrometheusInstance::drawImgui ( VkCommandBuffer cmd, VkImageView targetImageView ) {
	VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info( targetImageView, nullptr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL );
	VkRenderingInfo renderInfo = vkinit::rendering_info( swapchainExtent, &colorAttachment, nullptr );

	vkCmdBeginRendering( cmd, &renderInfo );
	ImGui_ImplVulkan_RenderDrawData( ImGui::GetDrawData(), cmd );
	vkCmdEndRendering( cmd );
}
