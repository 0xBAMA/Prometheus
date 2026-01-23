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
	initImgui();

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

	drawBackground( cmd );

	vkutil::transition_image( cmd, drawImage.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL );


	// transition the images for the copy
	vkutil::transition_image( cmd, drawImage.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL );
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
		VK_CHECK( vkCreateSemaphore( device, &semaphoreCreateInfo, nullptr, &frameData[ i ].renderSemaphore ) );
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
	initTrianglePipeline();
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
	computePipelineCreateInfo.layout = gradient.layout;
	computePipelineCreateInfo.stage = stageinfo;

	gradient.layout = gradient.layout;
	gradient.name = "gradient";
	gradient.data = {};

	//default colors
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
		fmt::print( "Error when building the triangle fragment shader module" );
	} else {
		fmt::print( "Triangle fragment shader successfully loaded" );
	}

	VkShaderModule triangleVertexShader;
	if ( !vkutil::load_shader_module( "../shaders/colored_triangle.vert.spv", device, &triangleVertexShader ) ) {
		fmt::print( "Error when building the triangle vertex shader module" );
	} else {
		fmt::print( "Triangle vertex shader successfully loaded" );
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
	pipelineBuilder.set_depth_format( VK_FORMAT_UNDEFINED );

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
		// 64,
		// 64,
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

void PrometheusInstance::immediate_submit( std::function< void( VkCommandBuffer cmd ) > && function ) {
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
