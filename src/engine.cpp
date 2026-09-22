#include "engine.h"

// #include <SDL.h>
// #include <SDL_vulkan.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_vulkan.h>

#include <vk_types.h>
#include <vk_initializers.h>
#include <vk_images.h>

#include <third_party/volk/volk.h>

#include "VkBootstrap.h"
#include <array>
#include <thread>
#include <chrono>
#include <fstream>

using namespace std::chrono_literals;

#define VMA_IMPLEMENTATION
#include <fastgltf/types.hpp>

#include "vk_mem_alloc.h"

#include <third_party/imgui/imgui.h>
#include <third_party/imgui/imgui_impl_sdl3.h>
#include <third_party/imgui/imgui_impl_vulkan.h>
#include <third_party/imgui/LegitProfiler/ImGuiProfilerRenderer.h>

#include <third_party/yaml-cpp/include/yaml-cpp/yaml.h>

#include <glm/gtx/transform.hpp>
#include <glm/gtc/packing.hpp>

#include <third_party/stb/stb_image_write.h>

// heightmap gen
#include <third_party/diamondSquare/diamondSquare.h>

//============================================================================================================================
//============================================================================================================================
// Initialization
//============================================================================================================================
void PrometheusInstance::Init () {
	// initializing SDL
	// SDL_SetHint( SDL_HINT_VIDEO_HDR_ENABLED, "1" );
	SDL_Init( SDL_INIT_VIDEO );
	SDL_WindowFlags windowFlags = ( SDL_WindowFlags ) ( SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_FULLSCREEN );

	SDL_Rect viewRect;
	int numDisplays;
	SDL_DisplayID *displays = SDL_GetDisplays( &numDisplays );
	SDL_GetDisplayBounds( displays[ 0 ], &viewRect );

	// accumulator image is going to be 1:1 with the swapchain image
	// ImageBufferResolution.width = windowExtent.width = 3 * viewRect.w / 4;
	// ImageBufferResolution.height = windowExtent.height = 3 * viewRect.h / 4;
	ImageBufferResolution.width = windowExtent.width = viewRect.w;
	ImageBufferResolution.height = windowExtent.height = viewRect.h;

	window = SDL_CreateWindow(
		"Prometheus",
		windowExtent.width,
		windowExtent.height,
		windowFlags );

	initVulkan();
	initSwapchain();
	initCommandStructures();
	initSyncStructures();
	initResources();
	initDescriptors();
	initImgui();
	initDefaultData();
	initComputePasses();
	initLights();

	// addDebugString( vec2( 16 ), "HELLO THIS IS A DEBUG STRING", vec3( 1.0f ), 0 );

	// everything went fine
	isInitialized = true;
}

//============================================================================================================================
// Draw
//============================================================================================================================
void PrometheusInstance::Draw () {
	// wait until the gpu has finished rendering the last frame. Timeout of u
	VK_CHECK( vkWaitForFences( device, 1, &getCurrentFrame().renderFence, true, UINT64_MAX ) );

	// we want to take this opportunity to now reset the deletion queue, since this fence marks the completion
	getCurrentFrame().deletionQueue.flush(); // of all operations which could be using the data...
	getCurrentFrame().frameDescriptors.clear_pools( device ); // mark the allocated descriptors as available

	// and now reset that fence so we can use it again, to signal this frame's completion
	VK_CHECK( vkResetFences( device, 1, &getCurrentFrame().renderFence ) );

	//request image from the swapchain
	uint32_t swapchainImageIndex;
	VkResult e = vkAcquireNextImageKHR( device, swapchain, 1000000000, getCurrentFrame().swapchainSemaphore, VK_NULL_HANDLE, &swapchainImageIndex );
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
	drawExtent.height = uint32_t( std::min( swapchainExtent.height, drawImage.imageExtent.height ) * renderScale );
	drawExtent.width = uint32_t( std::min( swapchainExtent.width, drawImage.imageExtent.width ) * renderScale );

	// update the UBO contents
	static float mouseX, mouseY;
	auto ret = SDL_GetMouseState( &mouseX, &mouseY );
	globalData.mouseLoc.x = mouseX;
	globalData.mouseLoc.y = mouseY;
	globalData.mouseLoc.z = ( ret & SDL_BUTTON_LEFT && !ImGui::GetIO().WantCaptureMouse ) ? 1.0f : 0.0f;
	globalData.mouseLoc.w = ( ret & SDL_BUTTON_RIGHT && !ImGui::GetIO().WantCaptureMouse ) ? 1.0f : 0.0f;
	globalData.floatBufferResolution = glm::uvec2( ImageBufferResolution.width, ImageBufferResolution.height );
	globalData.presentBufferResolution = glm::uvec2( drawExtent.width, drawExtent.height );
	globalData.frameNumber = frameNumber;
	globalData.framesSinceReset++;
	globalData.resolutionScalar = renderScale;

	// this will also be used to draw the map overlay
	globalData.basisX = basisX;
	globalData.basisY = basisY;
	globalData.basisZ = basisZ;
	globalData.viewerPosition = viewerPosition;
	globalData.FoV = FoV;
	globalData.bounces = bounces;
	globalData.raymarchMaxSteps = raymarchMaxSteps;
	globalData.raymarchUnderstep = raymarchUnderstep;
	globalData.raymarchMaxDistance = raymarchMaxDistance;
	globalData.epsilon = epsilon;
	globalData.numLights = lightManager.numLights;
	globalData.mapMode = mapConfig.mapActive ? 1 : 0; // tbd if we use this to send more data
	globalData.mapMatrix = mapConfig.orientation;

	// write directly from the memory on the PrometheusInstance
	GlobalData* uniformData = ( GlobalData * ) GlobalUBO.allocation->GetMappedData();
	*uniformData = globalData;

	// reset the reset flag
	if ( globalData.reset != 0 ) {
		globalData.reset = 0;
		globalData.framesSinceReset = 0;
	}

	// start the command buffer recording
	VK_CHECK( vkBeginCommandBuffer( cmd, &cmdBeginInfo ) );

	// reset timers...
	timerManager->cmd = &cmd;
	timerManager->pool = &getCurrentFrame().queryPools;
	timerManager->reset();

	// put the core images into a general format
	vkutil::transition_image( cmd, Accumulator.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL );
	vkutil::transition_image( cmd, drawImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL );
	vkutil::transition_imageD( cmd, depthImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL );
	vkutil::transition_image( cmd, mapDrawImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL );
	vkutil::transition_imageD( cmd, mapDepthImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL );

	vkutil::transition_image( cmd, font_codepage437.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL );
	vkutil::transition_image( cmd, font_fatfont.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL );
	vkutil::transition_image( cmd, font_tinyfont.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL );

	vkutil::transition_image( cmd, PreviewAtlas.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL );
	vkutil::transition_image( cmd, PickISImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL );
	vkutil::transition_image( cmd, SpectrumISImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL );
	vkutil::transition_image( cmd, SpectrumPDFImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL );
	vkutil::transition_image( cmd, jakobLUTImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL );

	vkutil::transition_image( cmd, AdamColor.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL );
	vkutil::transition_image( cmd, AdamCount.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL );

	if ( mapConfig.mapActive ) {

		// drawing the map
		scopedTimer start( "Map Draw" );
		mapOpaque.invoke2( cmd );

		// copying raster result to the framebuffer
		// vkutil::transition_image( cmd, Accumulator.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL );
		// vkutil::transition_image( cmd, mapDrawImage.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL );
		// vkutil::copy_image_to_image( cmd, mapDrawImage.image, Accumulator.image, { uint32_t( mapConfig.mapRes.x ), uint32_t( mapConfig.mapRes.y ) }, { uint32_t( ImageBufferResolution.width * renderScale ), uint32_t( ImageBufferResolution.height * renderScale ) });
		// vkutil::transition_image( cmd, Accumulator.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL );
		// vkutil::transition_image( cmd, mapDrawImage.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL );
		mapCopy.invoke2( cmd );

	} else {

		// running the pathtracer
		scopedTimer start( "Test 1" );
		testPipe.invoke2( cmd );

	}

	{ // compute shader to accumulate the raster result + put the resolved final image into the drawImage...
		scopedTimer start( "Present" );
		BufferPresent.invoke2( cmd );
	}

	if ( screenshotRequested ) { // decrement to zero
		if ( !--screenshotRequested )
			screenshot();
	} else {
		{ // do the debug line draw over top of the final LDR color
			scopedTimer start( "Debug Line Draw" );
			DebugLineDraw.invoke2( cmd );
		}

		{ // do the debug string draw
			scopedTimer start( "Debug String Draw" );
			DebugStringDraw.invoke2( cmd );
		}
	}

	// transition the images for the copy
	vkutil::transition_image( cmd, drawImage.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL );
	vkutil::transition_image( cmd, swapchainImages[ swapchainImageIndex ], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL );

	// execute a copy from the draw image into the swapchain
	vkutil::copy_image_to_image( cmd, drawImage.image, swapchainImages[ swapchainImageIndex ], drawExtent, swapchainExtent );

	// set swapchain image layout to Attachment Optimal so we can draw it
	vkutil::transition_image( cmd, swapchainImages[ swapchainImageIndex ], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL );

	// draw imgui into the swapchain image
	drawImgui( cmd, swapchainImageViews[ swapchainImageIndex ] );

	// transition the image from layout general to ready-for-swapchain-handoff
	vkutil::transition_image( cmd, swapchainImages[ swapchainImageIndex ], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR );

	// Kill recording, and put it in "executable" state
	VK_CHECK( vkEndCommandBuffer( cmd ) );

	// prepare the timing results for next frame...
	timerManager->gather();

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
		while ( SDL_PollEvent( &e ) ) {
			ImGui_ImplSDL3_ProcessEvent( &e );

			// I want to move to the input handler I wrote as soon as possible
			const bool* kb = SDL_GetKeyboardState( NULL );
			const SDL_Keymod k		= SDL_GetModState();
			const bool shift		= ( k & SDL_KMOD_SHIFT );
			const bool alt			= ( k & SDL_KMOD_ALT );
			const bool control		= ( k & SDL_KMOD_CTRL );
			const bool caps			= ( k & SDL_KMOD_CAPS );
			const bool super		= ( k & SDL_KMOD_GUI );

			if ( e.type == SDL_EVENT_QUIT ) {
				quit = true;
			}

			if ( e.type == SDL_EVENT_KEY_UP && e.key.scancode == SDL_SCANCODE_ESCAPE ) {
				quit = true;
			}

			if ( e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_M ) {
				showMenu = !showMenu;
			}

		// RESOLUTION SCALING
			// "P" for "Print"
			if ( e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_P ) {
				globalData.reset = 1;
				renderScale = 1.0f;
			}

			// "O" for "Observe"
			if ( e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_O ) {
				globalData.reset = 1;
				renderScale = 0.3f;
			}

			// managing the map orientation
			static glm::mat4 mapOrientation = glm::mat4( 1.0f );
			if ( e.type == SDL_EVENT_MOUSE_MOTION && mapConfig.mapActive && ( e.motion.state & SDL_BUTTON_LEFT ) ) {
				// this should do to manage the orientation via click and drag
				if ( !ImGui::GetIO().WantCaptureMouse ) {
					mapOrientation = glm::rotate( mapOrientation, e.motion.xrel * 0.001f, glm::mat3( glm::inverse( mapOrientation ) ) * vec3( 0.0f, 1.0f, 0.0f ) );
					mapOrientation = glm::rotate( mapOrientation, -e.motion.yrel * 0.001f, glm::mat3( glm::inverse( mapOrientation ) ) * vec3( 1.0f, 0.0f, 0.0f ) );
					globalData.reset = true;
				}
			}
			float baseScalar = 1.0f / ( length( globalData.sceneExtents ) );
			mapConfig.orientation = glm::translate( glm::mat4( 1.0f ), vec3( 0.0f, 0.0f, 0.5f ) )
				* glm::scale( glm::mat4( 1.0f ), vec3( baseScalar * ( mapConfig.mapRes.y / mapConfig.mapRes.x ), baseScalar, 0.45f * baseScalar ) )
				* mapOrientation;

			if ( kb[ SDL_SCANCODE_R ] ) {
				globalData.reset = true;
			}

			if ( kb[ SDL_SCANCODE_T ] && shift ) {
				// screenshot();
				screenshotRequested = FRAME_OVERLAP;
			}

			if ( e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_SPACE ) {
				mapConfig.mapActive = !mapConfig.mapActive;
				if ( mapConfig.mapActive ) {
					// clear the accumulator
					globalData.reset = true;
					SDL_Delay( 100 );
				}
			}

			{ // placeholder interactive camera from Daedalus
				// quaternion based rotation via retained state in the basis vectors
				const float scalar = shift ? 0.1f : ( control ? 0.0005f : 0.02f );
				if ( kb[ SDL_SCANCODE_W ] ) {
					glm::quat rot = glm::angleAxis( scalar, basisX ); // basisX is the axis, therefore remains untransformed
					basisY = ( rot * vec4( basisY, 0.0f ) ).xyz();
					basisZ = ( rot * vec4( basisZ, 0.0f ) ).xyz();
				}
				if ( kb[ SDL_SCANCODE_S ] ) {
					glm::quat rot = glm::angleAxis( -scalar, basisX );
					basisY = ( rot * vec4( basisY, 0.0f ) ).xyz();
					basisZ = ( rot * vec4( basisZ, 0.0f ) ).xyz();
				}
				if ( kb[ SDL_SCANCODE_A ] ) {
					glm::quat rot = glm::angleAxis( -scalar, basisY ); // same as above, but basisY is the axis
					basisX = ( rot * vec4( basisX, 0.0f ) ).xyz();
					basisZ = ( rot * vec4( basisZ, 0.0f ) ).xyz();
				}
				if ( kb[ SDL_SCANCODE_D ] ) {
					glm::quat rot = glm::angleAxis( scalar, basisY );
					basisX = ( rot * vec4( basisX, 0.0f ) ).xyz();
					basisZ = ( rot * vec4( basisZ, 0.0f ) ).xyz();
				}
				if ( kb[ SDL_SCANCODE_Q ] ) {
					glm::quat rot = glm::angleAxis( scalar, basisZ ); // and again for basisZ
					basisX = ( rot * vec4( basisX, 0.0f ) ).xyz();
					basisY = ( rot * vec4( basisY, 0.0f ) ).xyz();
				}
				if ( kb[ SDL_SCANCODE_E ] ) {
					glm::quat rot = glm::angleAxis( -scalar, basisZ );
					basisX = ( rot * vec4( basisX, 0.0f ) ).xyz();
					basisY = ( rot * vec4( basisY, 0.0f ) ).xyz();
				}

				// zoom in and out with plus/minus
				if ( kb[ SDL_SCANCODE_MINUS ] ) {
					FoV += scalar;
				}
				if ( kb[ SDL_SCANCODE_EQUALS ] ) {
					FoV -= scalar;
				}

				// f to reset basis, shift + f to reset basis and home to origin
				if ( kb[ SDL_SCANCODE_F ] ) {
					if ( shift ) viewerPosition = vec3( 0.0f, 0.0f, 0.0f );
					basisX = vec3( 1.0f, 0.0f, 0.0f );
					basisY = vec3( 0.0f, 1.0f, 0.0f );
					basisZ = vec3( 0.0f, 0.0f, 1.0f );
				}
				if ( kb[ SDL_SCANCODE_UP ] )		viewerPosition += 10.0f * scalar * basisZ;
				if ( kb[ SDL_SCANCODE_DOWN ] )		viewerPosition -= 10.0f * scalar * basisZ;
				if ( kb[ SDL_SCANCODE_RIGHT ] )		viewerPosition += 10.0f * scalar * basisX;
				if ( kb[ SDL_SCANCODE_LEFT ] )		viewerPosition -= 10.0f * scalar * basisX;
				if ( kb[ SDL_SCANCODE_PAGEDOWN ] )	viewerPosition += 10.0f * scalar * basisY;
				if ( kb[ SDL_SCANCODE_PAGEUP ] )	viewerPosition -= 10.0f * scalar * basisY;
			}
		}

		/*
		static glm::vec2 lastMousePos = glm::vec2( 0.0f );
		if ( distance( lastMousePos, globalData.mouseLoc.xy() ) > 8.0f ) {
			globalData.reset = true;
			lastMousePos = globalData.mouseLoc.xy();
		}
		*/

		// handling minimized application
		if ( stopRendering ) {
			// throttle the speed to avoid busy loop
			std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
		} else {
			// imgui new frame
			ImGui_ImplVulkan_NewFrame();
			ImGui_ImplSDL3_NewFrame();
			ImGui::NewFrame();

			if ( ImGui::GetIO().WantCaptureMouse ) {
				if ( !SDL_CursorVisible() ) {
					SDL_ShowCursor();
				}
			} else {
				if ( SDL_CursorVisible() ) {
					SDL_HideCursor();
				}
			}

			// some imgui UI to test
			// ImGui::ShowDemoWindow();

			if ( showMenu ) {
				// showing the profiler - 1 frame delay, so we have to wait for frame 1 for the first results
				if ( frameNumber != 0 ) {
					int color = 0;
					std::vector< legit::ProfilerTask > tasks_CPU;
					std::vector< legit::ProfilerTask > tasks_GPU;

					for ( size_t i = 0; i < timerManager->timingResults.size(); i++ ) {
						color++;
						color = color % legit::Colors::colorList.size();
						legit::ProfilerTask pt_CPU;
						legit::ProfilerTask pt_GPU;

						// calculate start and end times
						pt_CPU.startTime = timerManager->timingResults[ i ].tStartCPU / 1000.0f;
						pt_CPU.endTime = timerManager->timingResults[ i ].tStopCPU / 1000.0f;
						pt_CPU.name = timerManager->timingResults[ i ].label;
						pt_CPU.color = legit::Colors::colorList[ color ]; // do better
						tasks_CPU.push_back( pt_CPU );

						pt_GPU.startTime = timerManager->timingResults[ i ].tStartGPU / 1000.0f;
						pt_GPU.endTime = timerManager->timingResults[ i ].tStopGPU / 1000.0f;
						pt_GPU.name = timerManager->timingResults[ i ].label;
						pt_GPU.color = legit::Colors::colorList[ color ]; // do better
						tasks_GPU.push_back( pt_GPU );
					}

					static ImGuiUtils::ProfilersWindow profilerWindow; // add new profiling data and render
					profilerWindow.cpuGraph.LoadFrameData( &tasks_CPU[ 0 ], tasks_CPU.size() );
					profilerWindow.gpuGraph.LoadFrameData( &tasks_GPU[ 0 ], tasks_GPU.size() );
					profilerWindow.Render(); // GPU graph is presented on top, CPU on bottom
				}

				static bool open = true;
				if ( ImGui::Begin( "Edit", &open, ImGuiWindowFlags_NoNavInputs ) ) {

					ImGui::SliderFloat( "Brightness Scale", &globalData.brightnessScalar, 0.3f, 5.0f, "%.5f", ImGuiSliderFlags_Logarithmic ); // this should also apply to the raster step + accumulate step
					ImGui::SliderFloat( "Resolution Scale", &renderScale, 0.05f, 1.0f ); // this should also apply to the raster step + accumulate step
					ImGui::Separator();
					ImGui::Separator();
					static bool lsVisible = true;

					lightManager.ImGuiDrawLightList();
				}
				ImGui::End();
			}

			// make imgui calculate internal draw structures
			ImGui::Render();

			// some stuff to do, if we need to update buffers or textures associated with the lights
			lightManagerMaintenance();

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

	VK_CHECK(volkInitialize());

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

	volkLoadInstance( instance );

	// create a surface to render to
	SDL_Vulkan_CreateSurface( window, instance, NULL, &surface );

	//vulkan 1.3 features
	VkPhysicalDeviceVulkan13Features features13{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
	features13.dynamicRendering = true;
	features13.synchronization2 = true;

	//vulkan 1.2 features
	VkPhysicalDeviceVulkan12Features features12{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
	features12.bufferDeviceAddress = true;
	features12.descriptorIndexing = true;
	features12.scalarBlockLayout = true;
	features12.uniformAndStorageBuffer8BitAccess = true;

	//vulkan 1.0 features
	VkPhysicalDeviceFeatures features{};
	features.wideLines = true;

	VkPhysicalDeviceAccelerationStructureFeaturesKHR accelFeatures{
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR
	};
	accelFeatures.accelerationStructure = VK_TRUE;

	VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures{};
	rayQueryFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
	rayQueryFeatures.rayQuery = VK_TRUE;

	//use vkbootstrap to select a gpu.
	//We want a gpu that can write to the SDL surface and supports vulkan 1.3 with the correct features
	vkb::PhysicalDeviceSelector selector{ vkb_inst };
	vkb::PhysicalDevice physicalDeviceSelect = selector
		.set_minimum_version( 1, 3 )
		.set_required_features_13( features13 )
		.set_required_features_12( features12 )
		.set_required_features( features )

		.add_required_extension( "VK_KHR_maintenance9" ) // for VK_QUERY_POOL_CREATE_RESET_BIT_KHR
		// .add_required_extension( "VK_EXT_depth_range_unrestricted" )

	// a lot of these were for the hardware RT stuff
		// .add_required_extension( "VK_KHR_acceleration_structure" )
		// .add_required_extension_features( accelFeatures )

		// .add_required_extension( "VK_KHR_ray_query" )
		// .add_required_extension_features( rayQueryFeatures )

		// .add_required_extension( "VK_KHR_deferred_host_operations" )
		// .add_required_extension( "VK_KHR_ray_tracing_position_fetch" )

		.set_surface( surface )
		.select()
		.value();

	//create the final vulkan device
	vkb::DeviceBuilder deviceBuilder{ physicalDeviceSelect };
	vkb::Device vkbDevice = deviceBuilder.build().value();

	// Get the VkDevice handle used in the rest of a vulkan application
	device = vkbDevice.device;
	physicalDevice = physicalDeviceSelect.physical_device;
	volkLoadDevice( device );

	// reporting some platform info
	VkPhysicalDeviceProperties temp;
	vkGetPhysicalDeviceProperties( vkbDevice.physical_device, &temp );
	{
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
		fmt::print( "Timestamp Period: {}\n", temp.limits.timestampPeriod );

		// line raster stuff
		VkPhysicalDeviceFeatures feat;
		vkGetPhysicalDeviceFeatures( physicalDeviceSelect , &feat );
		fmt::print( "Value of widelines is {}\n", feat.wideLines );
		fmt::print( "Line Width Granularity is {} and range is {} to {}", temp.limits.lineWidthGranularity, temp.limits.lineWidthRange[ 0 ], temp.limits.lineWidthRange[ 1 ] );
		fmt::print( "\n\n" );
	}

	// use vkbootstrap to get a Graphics queue
	graphicsQueue = vkbDevice.get_queue( vkb::QueueType::graphics ).value();
	graphicsQueueFamilyIndex = vkbDevice.get_queue_index( vkb::QueueType::graphics ).value();

	// value for the timestamp period
	timestampPeriod = temp.limits.timestampPeriod;
	if ( timestampPeriod != 0 && temp.limits.timestampComputeAndGraphics ) {
		// timestamps supporteed
		fmt::print( "Timestamps at {}ns Resolution\n\n", timestampPeriod );
	} else {
		fmt::print( "Timestamps Unsupported\n\n" );
	}

	VmaVulkanFunctions funcs{};
	funcs.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
	funcs.vkGetDeviceProcAddr   = vkGetDeviceProcAddr;

	// initialize the memory allocator
	VmaAllocatorCreateInfo allocatorInfo = {};
	allocatorInfo.physicalDevice = physicalDevice;
	allocatorInfo.device = device;
	allocatorInfo.instance = instance;
	allocatorInfo.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
	allocatorInfo.pVulkanFunctions = &funcs;
	vmaCreateAllocator( &allocatorInfo, &allocator );

	// populate the global allocator pointer
	vmaGlobalAllocatorPtr = &allocator;

	// populate the global device pointer
	globalVkDevicePtr = &device;

	// populate the global instance pointer
	globalVkInstancePtr = &instance;

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
	// setting up the remainder of the timestamp infrastructure
	timer.device = &device;
	timer.timestampPeriod = timestampPeriod;
	timerManager = &timer;

	VkFenceCreateInfo fenceCreateInfo = vkinit::fence_create_info( VK_FENCE_CREATE_SIGNALED_BIT );
	VkSemaphoreCreateInfo semaphoreCreateInfo = vkinit::semaphore_create_info();
	for ( int i = 0; i < FRAME_OVERLAP; i++ ) {
	// we need to create one fence ( frame end mark )
		VK_CHECK( vkCreateFence( device, &fenceCreateInfo, nullptr, &frameData[ i ].renderFence ) );

	// and two semaphores: swapchain image ready, and render finished
		VK_CHECK( vkCreateSemaphore( device, &semaphoreCreateInfo, nullptr, &frameData[ i ].swapchainSemaphore ) );

	// and space for the timestamps (32 pairs as a max for now shouldn't be an issue)
		VkQueryPoolCreateInfo query_pool_info{};
		query_pool_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
		query_pool_info.flags = VK_QUERY_POOL_CREATE_RESET_BIT_KHR;
		query_pool_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
		query_pool_info.queryCount = timer.maxQueries;
		VK_CHECK( vkCreateQueryPool( device, &query_pool_info, nullptr, &frameData[ i ].queryPools ) );

		mainDeletionQueue.push_function( [ = ] () {
			vkDestroyQueryPool( device, frameData[ i ].queryPools, nullptr );
		});
	}

	swapchainPresentSemaphores.resize( swapchainImages.size() );
	for ( size_t i = 0; i < swapchainImages.size(); i++ ) {
		VK_CHECK( vkCreateSemaphore( device, &semaphoreCreateInfo, nullptr, &swapchainPresentSemaphores[ i ] ) );
	}

	VK_CHECK( vkCreateFence( device, &fenceCreateInfo, nullptr, &immediateFence ) );
	mainDeletionQueue.push_function( [ = ] ()  { vkDestroyFence( device, immediateFence, nullptr ); } );

	// will also need several barriers for the compute/graphics operations
}

void PrometheusInstance::initDescriptors  () {
	//create a descriptor pool that will hold 10 sets with some different contents
	std::vector< DescriptorAllocatorGrowable::PoolSizeRatio > sizes = {
		{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 6 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 6 },
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 6 },
		{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 6 },
		// { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 6 },
	};

	globalDescriptorAllocator.init( device, 10, sizes );

	//make sure both the descriptor allocator and the new layout get cleaned up properly
	mainDeletionQueue.push_function( [ & ] () {
		globalDescriptorAllocator.destroy_pools( device );
	});

	for ( int i = 0; i < FRAME_OVERLAP; i++ ) {
		// create a descriptor pool
		std::vector< DescriptorAllocatorGrowable::PoolSizeRatio > frameSizes = {
			{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3 },
			{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 },
			{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3 },
			{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4 },
			// { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 4 },
		};

		frameData[ i ].frameDescriptors = DescriptorAllocatorGrowable{};
		frameData[ i ].frameDescriptors.init( device, 1000, frameSizes );

		mainDeletionQueue.push_function([ &, i ]() {
			frameData[ i ].frameDescriptors.destroy_pools( device );
		});
	}
}

void PrometheusInstance::initResources () {

	// API resource allocation:
	GlobalUBO = createBuffer( sizeof( GlobalData ), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU, "Global Data UBO" );
	Accumulator = createImage( { ImageBufferResolution.width, ImageBufferResolution.height, 1 }, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, "Accumulator" );
	LightParametersBuffer = createBuffer( 256 * sizeof( LightEmitterParameters ), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU, "Light Parameter UBO" );
	mapDrawImage = createImage( { uint32_t( mapConfig.mapRes.x ), uint32_t( mapConfig.mapRes.y ), 1 }, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, "Map Color Image" );
	mapDepthImage = createImage( { uint32_t( mapConfig.mapRes.x ), uint32_t( mapConfig.mapRes.y ), 1 }, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, "Map Depth Image" );
	rayBuffer = createBuffer( 64 * numRays, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO, "Ray Buffer" );

	// placeholder, making sure it works
	uint32_t width = 1024;
	uint32_t height = 1024;
	uint32_t numPixels = width * height;
	uint32_t* zeroesU = ( uint32_t * ) malloc( numPixels * 4 * sizeof( uint32_t ) );
	float* zeroesF = ( float * ) malloc( numPixels * 4 * 4 * sizeof( uint32_t ) );

	AdamCount = createImage( zeroesU, { 1024, 1024, 1 }, VK_FORMAT_R32_UINT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 4, "Adam Count", true );
	AdamColor = createImage( zeroesF, { 1024, 1024, 1 }, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 16, "Adam Color", true );

	free( zeroesF );
	free( zeroesU );

	// data storage for the debug layers
	debugLineDrawBuffer = createBuffer( ( 1 << 16 ) * sizeof( debugLinePoint ), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU, "Debug Line SSBO" );
	debugStringConfigBuffer = createBuffer( 1024 * sizeof( debugStringConfig ), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO, "Debug Text SSBO" );

	{ // Load font LUTs from disk...
		// code page 437
		int w, h, channels;
		unsigned char * data = stbi_load( "../fontLUTs/codepage437.png", &w, &h, &channels, 0 );
		VkExtent3D extent = { uint32_t( w ), uint32_t( h ), 1 };
		font_codepage437 = createImage( data, extent, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_USAGE_SAMPLED_BIT, 4, "Codepage 437 LUT" );
		stbi_image_free( data );

		// fatfont
		data = stbi_load( "../fontLUTs/fatFont.png", &w, &h, &channels, 0 );
		extent = { uint32_t( w ), uint32_t( h ), 1 };
		font_fatfont = createImage( data, extent, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_USAGE_SAMPLED_BIT, 4, "Fatfont LUT" );
		stbi_image_free( data );

		// tinyfont
		data = stbi_load( "../fontLUTs/tinyFont.png", &w, &h, &channels, 0 );
		extent = { uint32_t( w ), uint32_t( h ), 1 };
		font_tinyfont = createImage( data, extent, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_USAGE_SAMPLED_BIT, 4, "TinyFont LUT" );
		stbi_image_free( data );
	}

	// make sure to clean up at the end
	mainDeletionQueue.push_function([ & ] () {
		// destroying buffers
		destroyBuffer( GlobalUBO );
		destroyBuffer( LightParametersBuffer );
		destroyBuffer( debugLineDrawBuffer );
		destroyBuffer( debugStringConfigBuffer );

		// destroying images
		destroyImage( Accumulator );
		destroyImage( PreviewAtlas );
		destroyImage( SpectrumISImage );
		destroyImage( SpectrumPDFImage );
		destroyImage( PickISImage );
		destroyImage( font_codepage437 );
		destroyImage( font_fatfont );
		destroyImage( font_tinyfont );
	});
}

static inline VkImageMemoryBarrier2 makeImageBarrier ( VkImage img, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess ) {
	return VkImageMemoryBarrier2 {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
		.srcStageMask = srcStage,
		.srcAccessMask = srcAccess,
		.dstStageMask = dstStage,
		.dstAccessMask = dstAccess,
		.oldLayout = VK_IMAGE_LAYOUT_GENERAL,
		.newLayout = VK_IMAGE_LAYOUT_GENERAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = img,
		.subresourceRange = {
			VK_IMAGE_ASPECT_COLOR_BIT, 0,
			VK_REMAINING_MIP_LEVELS,
			0,
			VK_REMAINING_ARRAY_LAYERS
		}
	};
}

static inline VkImageMemoryBarrier2 makeImageBarrierD ( VkImage img, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess ) {
	return VkImageMemoryBarrier2 {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
		.srcStageMask = srcStage,
		.srcAccessMask = srcAccess,
		.dstStageMask = dstStage,
		.dstAccessMask = dstAccess,
		.oldLayout = VK_IMAGE_LAYOUT_GENERAL,
		.newLayout = VK_IMAGE_LAYOUT_GENERAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = img,
		.subresourceRange = {
			VK_IMAGE_ASPECT_DEPTH_BIT, 0,
			VK_REMAINING_MIP_LEVELS,
			0,
			VK_REMAINING_ARRAY_LAYERS
		}
	};
}

static VkBufferMemoryBarrier2 makeBufferBarrier ( VkBuffer buf, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess ) {
	return VkBufferMemoryBarrier2 {
		.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
		.srcStageMask = srcStage,
		.srcAccessMask = srcAccess,
		.dstStageMask = dstStage,
		.dstAccessMask = dstAccess,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.buffer = buf,
		.offset = 0,
		.size = VK_WHOLE_SIZE
	};
}

void PrometheusInstance::initComputePasses () {

	renderScale = 0.3f;

	{ // RAYTRACE UBERSHADER
		ComputeConfig config;
		config.name = "Test 1";
		config.descriptorSetLayout = {
			{ 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, sizeof( GlobalData ), 0,
				[ & ] () { return Resource( GlobalUBO.buffer ); } },

			// ACCUMULATOR IMAGE
			{ 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, defaultSamplerNearest,
				[ & ] () { return Resource( Accumulator.imageView ); } },

			// sRGB -> REFLECTANCE LUT
			{ 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, defaultSamplerNearest,
				[ & ] () {return Resource( jakobLUTImage.imageView ); } },

			// PARAMETERS FOR THE CURRENTLY CONFIGURED SET OF LIGHTS
			{ 3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_WHOLE_SIZE, 0,
				[ & ] () { return Resource( LightParametersBuffer.buffer ); } },

			// IMPORTANCE SAMPLING + WEIGHTING TEXTURES FOR THE LIGHTS
			{ 4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, defaultSamplerLinear,
				[ & ] () {return Resource( SpectrumPDFImage.imageView ); } },
			{ 5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, defaultSamplerLinear,
				[ & ] () {return Resource( SpectrumISImage.imageView ); } },
			{ 6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, defaultSamplerNearest,
				[ & ] () {return Resource( PickISImage.imageView ); } },
		};
		config.allocateDescriptorSet = [&]( VkDescriptorSetLayout dsl ) {
			return getCurrentFrame().frameDescriptors.allocate( device, dsl );
		};

		config.updatePushConstants = [&]( VkCommandBuffer cmd ) {
			testPipe.pushConstants.wangSeed = genWangSeed();
			vkCmdPushConstants( cmd, testPipe.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof( PushConstants ), &testPipe.pushConstants );
		};

		config.shaderPath = "../shaders/test.comp.glsl.spv";
		config.dispatch = [&]( VkCommandBuffer cmd ) {
			vkCmdDispatch( cmd, ( ( drawExtent.width ) + 15 ) / 16, ( ( drawExtent.height ) + 15 ) / 16, 1 );
		};

		// creating the actual API resources
		testPipe.init( &device, &mainDeletionQueue, config );
	}

	{ // Wavefront Camera Ray Gen
		ComputeConfig config;
		config.name = "Phoenix Ray Gen";

		config.descriptorSetLayout = {
			{ 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, sizeof( GlobalData ), 0,
				[ & ] () { return Resource( GlobalUBO.buffer ); } },

			// THE RAY BUFFER
			{ 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_WHOLE_SIZE, 0,
				[ & ] () { return Resource( rayBuffer.buffer ); } },

			// wavelength importance sampling buffer (film sensitivity)

		};

		config.allocateDescriptorSet = [&]( VkDescriptorSetLayout dsl ) {
			return getCurrentFrame ().frameDescriptors.allocate( device, dsl );
		};

		config.updatePushConstants = [&]( VkCommandBuffer cmd ) {
			cameraGen.pushConstants.wangSeed = genWangSeed();
			vkCmdPushConstants( cmd, cameraGen.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof( PushConstants ), &cameraGen.pushConstants );
		};

		config.shaderPath = "../shaders/camera.comp.glsl.spv";
		config.dispatch = [&]( VkCommandBuffer cmd ) {
			vkCmdDispatch( cmd, ( numRays ) / 256, 1, 1 );
		};

		cameraGen.init( &device, &mainDeletionQueue, config );
	}

	{ // Wavefront Ray Intersect
		ComputeConfig config;
		config.name = "Phoenix Ray Intersect";

		config.descriptorSetLayout = {
			{ 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, sizeof( GlobalData ), 0,
				[ & ] () { return Resource( GlobalUBO.buffer ); } },

			// THE RAY BUFFER
			{ 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_WHOLE_SIZE, 0,
				[ & ] () { return Resource( rayBuffer.buffer ); } },

			// sRGB -> REFLECTANCE LUT
			{ 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, defaultSamplerNearest,
				[ & ] () {return Resource( jakobLUTImage.imageView ); } },

			// PARAMETERS FOR THE CURRENTLY CONFIGURED SET OF LIGHTS
			{ 3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_WHOLE_SIZE, 0,
				[ & ] () { return Resource( LightParametersBuffer.buffer ); } },

			// IMPORTANCE SAMPLING + WEIGHTING TEXTURES FOR THE LIGHTS
			{ 4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, defaultSamplerLinear,
				[ & ] () {return Resource( SpectrumPDFImage.imageView ); } },
			{ 5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, defaultSamplerLinear,
				[ & ] () {return Resource( SpectrumISImage.imageView ); } },
			{ 6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, defaultSamplerNearest,
				[ & ] () {return Resource( PickISImage.imageView ); } },

			// any other buffers associated with intersection (BVH, etc)

		};

		config.allocateDescriptorSet = [&]( VkDescriptorSetLayout dsl ) {
			return getCurrentFrame().frameDescriptors.allocate( device, dsl );
		};

		config.updatePushConstants = [&]( VkCommandBuffer cmd ) {
			intersect.pushConstants.wangSeed = genWangSeed();
			vkCmdPushConstants( cmd, intersect.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof( PushConstants ), &intersect.pushConstants );
		};

		config.shaderPath = "../shaders/intersect.comp.glsl.spv";
		config.dispatch = [&]( VkCommandBuffer cmd ) {
			vkCmdDispatch( cmd, ( numRays ) / 256, 1, 1 );
		};

		intersect.init( &device, &mainDeletionQueue, config );
	}

	{ // Wavefront Ray Shading
		ComputeConfig config;
		config.name = "Phoenix Ray Shade";

		config.descriptorSetLayout = {
			{ 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, sizeof( GlobalData ), 0,
				[ & ] () { return Resource( GlobalUBO.buffer ); } },

			// THE RAY BUFFER
			{ 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_WHOLE_SIZE, 0,
				[ & ] () { return Resource( rayBuffer.buffer ); } },

			// sRGB -> REFLECTANCE LUT
			{ 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, defaultSamplerNearest,
				[ & ] () {return Resource( jakobLUTImage.imageView ); } },

			// PARAMETERS FOR THE CURRENTLY CONFIGURED SET OF LIGHTS
			{ 3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_WHOLE_SIZE, 0,
				[ & ] () { return Resource( LightParametersBuffer.buffer ); } },

			// IMPORTANCE SAMPLING + WEIGHTING TEXTURES FOR THE LIGHTS
			{ 4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, defaultSamplerLinear,
				[ & ] () {return Resource( SpectrumPDFImage.imageView ); } },
			{ 5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, defaultSamplerLinear,
				[ & ] () {return Resource( SpectrumISImage.imageView ); } },
			{ 6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, defaultSamplerNearest,
				[ & ] () {return Resource( PickISImage.imageView ); } },

			// need to replace the accumulation with Adam accumulator buffers
		};

		config.allocateDescriptorSet = [&]( VkDescriptorSetLayout dsl ) {
			return getCurrentFrame().frameDescriptors.allocate( device, dsl );
		};

		config.updatePushConstants = [&]( VkCommandBuffer cmd ) {
			shading.pushConstants.wangSeed = genWangSeed();
			vkCmdPushConstants( cmd, shading.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof( PushConstants ), &shading.pushConstants );
		};

		config.shaderPath = "../shaders/shading.comp.glsl.spv";
		config.dispatch = [&]( VkCommandBuffer cmd ) {
			vkCmdDispatch( cmd, ( numRays ) / 256, 1, 1 );
		};

		shading.init( &device, &mainDeletionQueue, config );
	}

	// pipeline for Adam propagation

	// pipeline to sample the Adam buffers into the accumulator

	{
		RasterConfig config;
		config.name = "Map Opaque Draw";
		config.descriptorSetLayout = {
			{ 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, sizeof( GlobalData ), 0,
				[ & ] () { return Resource( GlobalUBO.buffer ); } },

			// PARAMETERS FOR THE CURRENTLY CONFIGURED SET OF LIGHTS
			{ 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_WHOLE_SIZE, 0,
				[ & ] () { return Resource( LightParametersBuffer.buffer ); } },
		};
		config.allocateDescriptorSet = [&]( VkDescriptorSetLayout dsl ) {
			return getCurrentFrame().frameDescriptors.allocate( device, dsl );
		};

		// SHADERS
		config.shaderPathFrag = "../shaders/mapOpaque.frag.glsl.spv";
		config.shaderPathVert = "../shaders/mapOpaque.vert.glsl.spv";

		// FBO CONFIG
		config.drawImage = &mapDrawImage;
		config.depthImage = &mapDepthImage;
		config.clearColor = true;
		config.clearDepth = true;
		// config.lineWidth = 4.618f;
		config.getRenderResolution = [&]() {
			return VkExtent2D {
				uint32_t( mapConfig.mapRes.x ),
				uint32_t( mapConfig.mapRes.y ),
			};
		};

		// BARRIERS (post-draw)
		// config.imageBarriers = {
			// raster result made available ( color + depth )
			// makeImageBarrier( mapDrawImage.image, VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, VK_ACCESS_2_SHADER_READ_BIT ),
			// makeImageBarrierD( mapDepthImage.image, VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, VK_ACCESS_2_SHADER_READ_BIT )
		// };

		// DRAW
		config.inputTopology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
		config.updatePushConstants = [&]( VkCommandBuffer cmd ) {
			mapOpaque.pushConstants.wangSeed = genWangSeed();
			vkCmdPushConstants( cmd, mapOpaque.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof( PushConstants ), &mapOpaque.pushConstants );
		};
		config.dispatch = [&]( VkCommandBuffer cmd ) {
			if ( mapConfig.mapActive ) {
				// using some placeholder values
				// 30 verts for the bounding box and lines through the origin
				// 3 basis vectors -> each consists of 2? verts
				// N lights -> each consists of ? verts

				int count = 30 + 3 * 2 + lightManager.lights.size() * 256; // tbd how many vertices per light
				vkCmdSetLineWidth( cmd, 5.0f );
				vkCmdDraw( cmd, count, 1, 0, 0 );
			}
		};

		mapOpaque.init( &device, &mainDeletionQueue, config );
	}

	{
		ComputeConfig config;
		config.name = "Map Copy";
		config.descriptorSetLayout = {
			{ 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, sizeof( GlobalData ), 0,
				[ & ] () { return Resource( GlobalUBO.buffer ); } },

			{ 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, defaultSamplerLinear,
				[ & ] () { return Resource( mapDrawImage.imageView ); } },

			{ 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, defaultSamplerLinear,
				[ & ] () { return Resource( Accumulator.imageView ); } }
		};
		config.allocateDescriptorSet = [&]( VkDescriptorSetLayout dsl ) {
			return getCurrentFrame().frameDescriptors.allocate( device, dsl );
		};

		config.shaderPath = "../shaders/mapCopy.comp.glsl.spv";
		config.updatePushConstants = [&]( VkCommandBuffer cmd ) {
			// get a new wang RNG seed + send the current value of the push constants
			mapCopy.pushConstants.wangSeed = genWangSeed();
			vkCmdPushConstants( cmd, mapCopy.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof( PushConstants ), &mapCopy.pushConstants );
		};
		config.dispatch = [&]( VkCommandBuffer cmd ) {
			vkCmdDispatch( cmd, ( drawExtent.width + 15 ) / 16, ( drawExtent.height + 15 ) / 16, 1 );
		};

		mapCopy.init( &device, &mainDeletionQueue, config );
	}

	{
		RasterConfig config;
		config.name = "Debug String Draw";
		config.descriptorSetLayout = {
			{ 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, sizeof( GlobalData ), 0,
				[ & ] () { return Resource( GlobalUBO.buffer ); } },

			// STRINGS
			{ 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_WHOLE_SIZE, 0,
				[ & ] () { return Resource( debugStringConfigBuffer.buffer ); } },

			// FONT LUTS
			{ 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, defaultSamplerNearest,
				[ & ] () {return Resource(  font_codepage437.imageView ); } },
			{ 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, defaultSamplerNearest,
				[ & ] () {return Resource(  font_fatfont.imageView ); } },
			{ 4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, defaultSamplerNearest,
				[ & ] () {return Resource(  font_tinyfont.imageView ); } },
		};
		config.allocateDescriptorSet = [&]( VkDescriptorSetLayout dsl ) {
			return getCurrentFrame().frameDescriptors.allocate( device, dsl );
		};

		// SHADERS
		config.shaderPathFrag = "../shaders/debugStringDraw.frag.glsl.spv";
		config.shaderPathVert = "../shaders/debugStringDraw.vert.glsl.spv";

		// FBO CONFIG
		config.drawImage = &drawImage;
		config.depthImage = &depthImage;
		config.getRenderResolution = [&]() {
			return VkExtent2D {
				uint32_t( ImageBufferResolution.width * renderScale ),
				uint32_t( ImageBufferResolution.height * renderScale ),
			};
		};

		// BARRIERS (post-draw)
		config.imageBarriers = {
			// raster result made available ( color + depth )
			makeImageBarrier( drawImage.image, VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, VK_ACCESS_2_SHADER_READ_BIT ),
			makeImageBarrierD( depthImage.image, VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, VK_ACCESS_2_SHADER_READ_BIT )
		};

		// DRAW
		config.updatePushConstants = [&]( VkCommandBuffer cmd ) {
			DebugStringDraw.pushConstants.wangSeed = genWangSeed();
			vkCmdPushConstants( cmd, DebugStringDraw.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof( PushConstants ), &DebugStringDraw.pushConstants );
		};
		config.dispatch = [&]( VkCommandBuffer cmd ) {
			if ( debugStrings.size() != 0 ) {

				// copy latest strings to the GPU
				memcpy( debugStringConfigBuffer.allocation->GetMappedData(), &debugStrings[ 0 ], debugStrings.size() * sizeof( debugStringConfig ) );

				// 2 triangles per string
				vkCmdDraw( cmd, debugStrings.size() * 6, 1, 0, 0 );
			}
		};

		DebugStringDraw.init( &device, &mainDeletionQueue, config );
	}

	{
		RasterConfig config;
		config.name = "Debug Line Draw";
		config.descriptorSetLayout = {
			{ 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, sizeof( GlobalData ), 0,
				[ & ] () { return Resource( GlobalUBO.buffer ); } },

			// LINE DATA
			{ 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_WHOLE_SIZE, 0,
				[ & ] () { return Resource( debugLineDrawBuffer.buffer ); } },
		};
		config.allocateDescriptorSet = [&]( VkDescriptorSetLayout dsl ) {
			return getCurrentFrame().frameDescriptors.allocate( device, dsl );
		};

		// SHADERS
		config.shaderPathFrag = "../shaders/debugLineDraw.frag.glsl.spv";
		config.shaderPathVert = "../shaders/debugLineDraw.vert.glsl.spv";

		// FBO CONFIG
		config.drawImage = &drawImage;
		config.depthImage = &depthImage;
		config.getRenderResolution = [&]() {
			return VkExtent2D {
				uint32_t( ImageBufferResolution.width * renderScale ),
				uint32_t( ImageBufferResolution.height * renderScale ),
			};
		};

		// BARRIERS (post-draw)
		config.imageBarriers = {
			// raster result made available ( color + depth )
			makeImageBarrier( drawImage.image, VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, VK_ACCESS_2_SHADER_READ_BIT ),
			makeImageBarrierD( depthImage.image, VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, VK_ACCESS_2_SHADER_READ_BIT )
		};

		// DRAW
		config.inputTopology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
		config.updatePushConstants = [&]( VkCommandBuffer cmd ) {
			DebugLineDraw.pushConstants.wangSeed = genWangSeed();
			vkCmdPushConstants( cmd, DebugLineDraw.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof( PushConstants ), &DebugLineDraw.pushConstants );
		};
		config.dispatch = [&]( VkCommandBuffer cmd ) {

			// need to update the lines in the buffer
			debugLinePoint* linePointData = ( debugLinePoint * ) debugLineDrawBuffer.allocation->GetMappedData();

			// this needs to show a couple of things:
			// outlines showing the individual bounding boxes of the selected objects...
			// show a glyph for each light, at the light position, indicating direction, width, angle...
			// ...

			{ // mouse position crosshair, in a reserved location at the beginning of the buffer
				const int sO = 7;
				const int bO = 15;
				linePointData[ 0 ].position = vec4( globalData.mouseLoc.x + bO, globalData.mouseLoc.y, 0.5f, 1.0f );
				linePointData[ 1 ].position = vec4( globalData.mouseLoc.x + sO, globalData.mouseLoc.y, 0.5f, 1.0f );
				linePointData[ 2 ].position = vec4( globalData.mouseLoc.x - bO, globalData.mouseLoc.y, 0.5f, 1.0f );
				linePointData[ 3 ].position = vec4( globalData.mouseLoc.x - sO, globalData.mouseLoc.y, 0.5f, 1.0f );

				linePointData[ 4 ].position = vec4( globalData.mouseLoc.x, globalData.mouseLoc.y + bO, 0.5f, 1.0f );
				linePointData[ 5 ].position = vec4( globalData.mouseLoc.x, globalData.mouseLoc.y + sO, 0.5f, 1.0f );
				linePointData[ 6 ].position = vec4( globalData.mouseLoc.x, globalData.mouseLoc.y - bO, 0.5f, 1.0f );
				linePointData[ 7 ].position = vec4( globalData.mouseLoc.x, globalData.mouseLoc.y - sO, 0.5f, 1.0f );

				for ( int i = 0; i < 8; ++i ) {
					linePointData[ i ].color = vec4( 1.0f );
				}
			}

			// and then draw
			vkCmdSetLineWidth( cmd, 1.0f );
			vkCmdDraw( cmd, ( 1 << 16 ), 1, 0, 0 );
		};

		DebugLineDraw.init( &device, &mainDeletionQueue, config );
	}

	{
		ComputeConfig config;
		config.name = "Buffer Present";
		config.descriptorSetLayout = {
			{ 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, sizeof( GlobalData ), 0,
				[ & ] () { return Resource( GlobalUBO.buffer ); } },

			{ 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, defaultSamplerNearest,
				[ & ] () { return Resource( drawImage.imageView ); } },

			{ 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, defaultSamplerLinear,
				[ & ] () { return Resource( Accumulator.imageView ); } }
		};
		config.allocateDescriptorSet = [&]( VkDescriptorSetLayout dsl ) {
			return getCurrentFrame().frameDescriptors.allocate( device, dsl );
		};

		config.shaderPath = "../shaders/bufferPresent.comp.glsl.spv";
		config.updatePushConstants = [&]( VkCommandBuffer cmd ) {
			// get a new wang RNG seed + send the current value of the push constants
			BufferPresent.pushConstants.wangSeed = genWangSeed();
			vkCmdPushConstants( cmd, BufferPresent.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof( PushConstants ), &BufferPresent.pushConstants );
		};
		config.dispatch = [&]( VkCommandBuffer cmd ) {
			vkCmdDispatch( cmd, ( drawExtent.width + 15 ) / 16, ( drawExtent.height + 15 ) / 16, 1 );
		};

		BufferPresent.init( &device, &mainDeletionQueue, config );
	}
}

// text rendering, with pixel location + select from the list of available font LUTs (tinyfont, fatfont, code page 437)
int PrometheusInstance::addDebugString ( vec2 position, std::string displayText, vec3 color, int fontSelect, float zDepth ) {
	debugStringConfig s;

	// for runtime usage
	s.debugStringWriteLocation = position;
	s.debugStringDepth = zDepth;
	s.debugStringFillColor = vec4( color, 1.0f );
	s.debugStringBackgroundColor = vec4( 0.0f );
	s.debugStringFontPick = std::clamp( fontSelect, 0, 2 );
	s.debugStringLength = sprintf( ( char * ) s.debugStringData, "%s", displayText.c_str() );

	// add to the list of strings
	debugStrings.push_back( s );

	// fmt::print( "added new string {} at {} {}\n", string( ( char * ) debugStrings[ debugStrings.size() - 1 ].debugStringData ), position.x, position.y );

	// index of the string in the list
	return debugStrings.size() - 1;
}

void PrometheusInstance::updateString ( int index, vec2 position, std::string displayText, vec3 color, int fontSelect, float zDepth ){
	debugStringConfig& s = debugStrings[ index ];

	// for runtime usage
	s.debugStringWriteLocation = position;
	s.debugStringDepth = zDepth;
	s.debugStringFillColor = vec4( color, 1.0f );
	s.debugStringBackgroundColor = vec4( 0.0f );
	s.debugStringFontPick = std::clamp( fontSelect, 0, 2 );
	s.debugStringLength = sprintf( ( char * ) s.debugStringData, "%s", displayText.c_str() );
}

// 2D line segment
int PrometheusInstance::addDebugDrawLine ( vec2 a, vec2 b, vec3 color, float zDepthA, float zDepthB ) {
	// need to update the buffer with the new line
	debugLinePoint* linePointData = ( debugLinePoint * ) debugLineDrawBuffer.allocation->GetMappedData();

	linePointData[ debugLineDrawNumLines + 0 ].position = vec4( a, zDepthA, 1.0f );
	linePointData[ debugLineDrawNumLines + 0 ].color = vec4( color, 1.0f );
	linePointData[ debugLineDrawNumLines + 1 ].position = vec4( b, zDepthB, 1.0f );
	linePointData[ debugLineDrawNumLines + 1 ].color = vec4( color, 1.0f );

	debugLineDrawNumLines += 2;
	return debugLineDrawNumLines; // this is an allocation, and the memory can be reused
}

// 2D bounding box helper, draws 4 lines
int PrometheusInstance::addDebugDrawBox ( vec2 min, vec2 max, vec3 color, float zDepth ) {
	addDebugDrawLine( min, vec2( min.x, max.y ), color, zDepth, zDepth );
	addDebugDrawLine( min, vec2( max.x, min.y ), color, zDepth, zDepth );
	addDebugDrawLine( max, vec2( min.x, max.y ), color, zDepth, zDepth );
	addDebugDrawLine( max, vec2( max.x, min.y ), color, zDepth, zDepth );

	return debugLineDrawNumLines; // you can figure out from this, but it's not ideal
}

void PrometheusInstance::lightManagerMaintenance () {
	// three resources need to be kept up:
		// spectral sampling IS
		// light pick IS
		// light parameters buffer

	static bool firstTime = true;

	static int lastSeenNumLights = 0;
	uint8_t numLights = lightManager.lights.size();

	// if we see a change in the light list, we need to rebuild
	if ( lastSeenNumLights != numLights ) {
		vkDeviceWaitIdle( device );
		if ( !firstTime ) {
			// delete the existing textures
			destroyImage( PreviewAtlas );
			destroyImage( SpectrumISImage );
			destroyImage( SpectrumPDFImage );
			destroyImage( PickISImage );
		}
		// create the new textures at current sizes
		PreviewAtlas = createImage( { 554, 64u * numLights, 1 }, VK_FORMAT_R8G8B8A8_UNORM,  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT );
		SetDebugName( VK_OBJECT_TYPE_IMAGE, ( uint64_t ) PreviewAtlas.image, "Preview Atlas" );

		SpectrumISImage = createImage( { 1024, numLights, 1 }, VK_FORMAT_R32_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT );
		SetDebugName( VK_OBJECT_TYPE_IMAGE, ( uint64_t ) SpectrumISImage.image, "Spectral IS Texture" );

		SpectrumPDFImage = createImage( { 450, numLights, 1 }, VK_FORMAT_R32_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT );
		SetDebugName( VK_OBJECT_TYPE_IMAGE, ( uint64_t ) SpectrumPDFImage.image, "Spectral PDF Texture" );

		PickISImage = createImage( { 256, 256, 1 }, VK_FORMAT_R8_UINT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT );
		SetDebugName( VK_OBJECT_TYPE_IMAGE, ( uint64_t ) PickISImage.image, "Pick IS Texture" );

		firstTime = false;

		// we have memory allocated, now need to do the updates
		lightManager.needsUpdate = true;
		lastSeenNumLights = numLights;
	}

	if ( lightManager.needsUpdate ) {
		// ensure that we have up-to-date data prepared
		lightManager.Update();

		// and send this prepared texture data to the GPU
		updateImage( PreviewAtlas, lightManager.concatenatedPreviews.data(), 4 );	// data comes in as R8B8G8A8 (4 bytes)
		updateImage( SpectrumISImage, lightManager.iCDFTexture.data(), 4 );			// data comes in as R32 (4 bytes)
		updateImage( SpectrumPDFImage, lightManager.PDFTexture.data(), 4 );			// data comes in as R32 (4 bytes)
		updateImage( PickISImage, lightManager.pickTexture.data(), 1 );				// data comes in as R8 (1 byte)

		// setup for ImGui to draw texture on the menus
		textureID = ( ImTextureID ) ImGui_ImplVulkan_AddTexture(
			defaultSamplerNearest,
			PreviewAtlas.imageView,
			VK_IMAGE_LAYOUT_GENERAL
		);

		// wipe buffers
		globalData.reset = 1;
	}

	// and then we need to update the parameters buffer for the emitters
	LightEmitterParameters* emitterParams = ( LightEmitterParameters * ) LightParametersBuffer.allocation->GetMappedData();
	for ( int i = 0; i < lightManager.lights.size(); i++ ) {
		emitterParams[ i ] = lightManager.lights[ i ].parameters;
	}
}

AllocatedBuffer PrometheusInstance::createBuffer ( size_t allocSize, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage, string label ) {
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
	VK_CHECK( vmaCreateBuffer( allocator, &bufferInfo, &vmaallocInfo, &newBuffer.buffer, &newBuffer.allocation, &newBuffer.info ) );

	VkBufferDeviceAddressInfo deviceAddressInfo = {};
	deviceAddressInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
	deviceAddressInfo.buffer = newBuffer.buffer;
	newBuffer.deviceAddress = vkGetBufferDeviceAddress( *globalVkDevicePtr, &deviceAddressInfo  );

	if ( label != "" ) {
		SetDebugName( VK_OBJECT_TYPE_BUFFER, ( uint64_t ) newBuffer.buffer, label.c_str() );
	}
	return newBuffer;
}

void PrometheusInstance::destroyBuffer ( const AllocatedBuffer& buffer ) {
	vmaDestroyBuffer( allocator, buffer.buffer, buffer.allocation );
}

AllocatedImage PrometheusInstance::createImage ( VkExtent3D size, VkFormat format, VkImageUsageFlags usage, string label, bool mipmapped ) {
	AllocatedImage newImage;
	newImage.imageFormat = format;
	newImage.imageExtent = size;

	VkImageCreateInfo img_info = vkinit::image_create_info( format, usage, size );
	if ( mipmapped ) {
		newImage.numMips = img_info.mipLevels = static_cast<uint32_t>( std::floor( std::log2( std::max( size.width, size.height ) ) ) ) + 1;
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
	VkImageViewCreateInfo view_info = vkinit::imageview_create_info( format, newImage.image, aspectFlag, ( size.depth != 1 ) );
	view_info.subresourceRange.levelCount = img_info.mipLevels;

	VK_CHECK( vkCreateImageView( device, &view_info, nullptr, &newImage.imageView ) );

	if ( label != "" ) {
		SetDebugName( VK_OBJECT_TYPE_IMAGE, ( uint64_t ) newImage.image, label.c_str() );
		SetDebugName( VK_OBJECT_TYPE_IMAGE_VIEW, ( uint64_t ) newImage.imageView, label.c_str() );
	}

	return newImage;
}

AllocatedImage PrometheusInstance::createImage ( void* data, VkExtent3D size, VkFormat format, VkImageUsageFlags usage, int bytesPerPixel, string label, bool mipmapped ) {
	size_t dataSize = size.depth * size.width * size.height * bytesPerPixel;
	AllocatedBuffer uploadbuffer = createBuffer( dataSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU );

	// data from the void pointer, copied to the upload buffer
	memcpy( uploadbuffer.info.pMappedData, data, dataSize );

	// call to the read/write styled image creation function
	AllocatedImage new_image = createImage( size, format, usage | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, label, mipmapped );

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

		if ( mipmapped ) {
			vkutil::generate_mipmaps(cmd, new_image.image, VkExtent2D{ new_image.imageExtent.width, new_image.imageExtent.height }, format );
		} else {
			vkutil::transition_image(cmd, new_image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL );
		}
	});

	if ( mipmapped ) { // need to create the image views related to the mip layers
		// if the format is a depth format, we will need to have it use the correct aspect flag
		VkImageAspectFlags aspectFlag = VK_IMAGE_ASPECT_COLOR_BIT;
		if ( format == VK_FORMAT_D32_SFLOAT ) {
			aspectFlag = VK_IMAGE_ASPECT_DEPTH_BIT;
		}

		for ( int i = 1; i < new_image.numMips; i++ ) {
			// build a image-view for the image
			VkImageViewCreateInfo view_info = vkinit::imageview_create_info( format, new_image.image, aspectFlag, ( size.depth != 1 ) );
			view_info.subresourceRange.levelCount = 1;
			view_info.subresourceRange.baseMipLevel = i;
			view_info.subresourceRange.layerCount = 1;
			VK_CHECK( vkCreateImageView( device, &view_info, nullptr, &new_image.imageView[ i ] ) );
		}
	}

	// finished uploading, that data is now available
	destroyBuffer( uploadbuffer );

	return new_image;
}

void PrometheusInstance::updateImage( AllocatedImage& image, void* data, int bytesPerTexel ) {
	size_t dataSize = image.imageExtent.width * image.imageExtent.height * image.imageExtent.depth * bytesPerTexel;

	AllocatedBuffer uploadbuffer = createBuffer( dataSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU );

	memcpy( uploadbuffer.info.pMappedData, data, dataSize );

	immediateSubmit( [&]( VkCommandBuffer cmd ) {
		VkBufferImageCopy copyRegion = {};
		copyRegion.bufferOffset = 0;
		copyRegion.bufferRowLength = 0;
		copyRegion.bufferImageHeight = 0;

		copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copyRegion.imageSubresource.mipLevel = 0;
		copyRegion.imageSubresource.baseArrayLayer = 0;
		copyRegion.imageSubresource.layerCount = 1;
		copyRegion.imageExtent = image.imageExtent;

		vkutil::transition_image( cmd, image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL );
		vkCmdCopyBufferToImage( cmd, uploadbuffer.buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion );
		vkutil::transition_image( cmd, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL );
	} );

	destroyBuffer( uploadbuffer );
}

// this is a pretty specialized screenshot function, because it operates on the half floats stored in the draw image
void PrometheusInstance::screenshot() {
	std::string filenameS = std::string( "Phoenix-" + timeDateString() + ".png" );
	const char* filename = filenameS.c_str();
	AllocatedImage& image = drawImage;
	VkExtent3D size{ drawExtent.width, drawExtent.height, 1 };

	size_t pixelCount = size.width * size.height;
	size_t dataSize = pixelCount * 4 * sizeof( uint16_t );

	AllocatedBuffer readbackBuffer = createBuffer( dataSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU );

	immediateSubmit( [&]( VkCommandBuffer cmd ) {
		VkBufferImageCopy copyRegion = {};
		copyRegion.bufferOffset = 0;
		copyRegion.bufferRowLength = 0;
		copyRegion.bufferImageHeight = 0;
		copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copyRegion.imageSubresource.mipLevel = 0;
		copyRegion.imageSubresource.baseArrayLayer = 0;
		copyRegion.imageSubresource.layerCount = 1;
		copyRegion.imageExtent = size;

		vkCmdCopyImageToBuffer( cmd, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readbackBuffer.buffer, 1, &copyRegion );
	} );

	std::jthread writeThread = std::jthread( [ & ] ( ) {
		uint16_t* src = ( uint16_t* ) readbackBuffer.info.pMappedData;
		std::vector<uint8_t> out( pixelCount * 4 );

		for ( size_t i = 0; i < pixelCount; i++ ) {
			float r = glm::unpackHalf1x16( src[ i * 4 + 0 ] );
			float g = glm::unpackHalf1x16( src[ i * 4 + 1 ] );
			float b = glm::unpackHalf1x16( src[ i * 4 + 2 ] );

			r = glm::clamp( r, 0.0f, 1.0f );
			g = glm::clamp( g, 0.0f, 1.0f );
			b = glm::clamp( b, 0.0f, 1.0f );

			out[ i * 4 + 0 ] = ( uint8_t ) ( r * 255.0f );
			out[ i * 4 + 1 ] = ( uint8_t ) ( g * 255.0f );
			out[ i * 4 + 2 ] = ( uint8_t ) ( b * 255.0f );
			out[ i * 4 + 3 ] = 255;
		}

		stbi_write_png( filename, size.width, size.height, 4, out.data(), size.width * 4 );
		destroyBuffer( readbackBuffer );
	});
}

void PrometheusInstance::destroyImage ( const AllocatedImage& img ) {
	for ( int i = 0; i < img.numMips; i++ )
		vkDestroyImageView( device, img.imageView[ i ], nullptr );
	vmaDestroyImage( allocator, img.image, img.allocation );
}

void PrometheusInstance::initDefaultData () {

// TEXTURES
	// 3 default textures, white, grey, black. 1 pixel each
	uint32_t white = glm::packUnorm4x8( glm::vec4( 1.0f, 1.0f, 1.0f, 1.0f ) );
	whiteImage = createImage( ( void * ) &white, VkExtent3D{ 1, 1, 1 }, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT );

	uint32_t grey = glm::packUnorm4x8(glm::vec4( 0.66f, 0.66f, 0.66f, 1 ) );
	greyImage = createImage( ( void * ) &grey, VkExtent3D{ 1, 1, 1 }, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT );

	uint32_t black = glm::packUnorm4x8(glm::vec4(0, 0, 0, 0 ) );
	blackImage = createImage( ( void * ) &black, VkExtent3D{ 1, 1, 1 }, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT );

	{
		// unscopedTimer timer( "JAKOB", true );
		// timer.tick();
		// the sRGB to spectral LUT ( Jakob 2019 ) https://rgl.epfl.ch/publications/Jakob2019Spectral
		// creating a texture now with half precision floats, so that I can use
		RGB2Spec *model = rgb2spec_load( "../src/third_party/Jakob2019Spectral/supplement/tables/srgb.coeff" );

		uint32_t *jakobLUT; // one element per possible value in the sRGB space... 4x 16-bit becomes 2x uint32's per texel
		jakobLUT = ( uint32_t * ) malloc( 256 * 256 * 256 * sizeof( uint32_t ) * 2 );

		for ( size_t i = 0; i < 256 * 256 * 256; i++ ) {
			uint32_t r = i % 256;
			uint32_t g = ( i / 256 ) % 256;
			uint32_t b = ( i / ( 256 * 256 ) ) % 256;

			float rgb[ 3 ] = { r / 255.0f, g / 255.0f, b / 255.0f }, coeff[ 3 ];
			rgb2spec_fetch( model, rgb, coeff );

			jakobLUT[ 2 * i + 0 ] = glm::packHalf2x16( vec2( coeff[ 0 ], coeff[ 1 ] ) );
			jakobLUT[ 2 * i + 1 ] = glm::packHalf2x16( vec2( coeff[ 2 ], 0.0f ) );
		}
		jakobLUTImage = createImage( ( void * ) jakobLUT, VkExtent3D{ 256, 256, 256 }, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT, 8 );
		free( jakobLUT );
		// timer.tock();
		// fmt::print( "Jakob LUT loaded in {}ms", std::chrono::duration_cast<std::chrono::microseconds>(  timer.c.tStop - timer.c.tStart ).count() / 1000.0f );
	}

// SAMPLER OBJECTS
	VkSamplerCreateInfo sampl = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };

	sampl.magFilter = VK_FILTER_NEAREST;
	sampl.minFilter = VK_FILTER_NEAREST;
	vkCreateSampler( device, &sampl, nullptr, &defaultSamplerNearest );

	sampl.magFilter = VK_FILTER_LINEAR;
	sampl.minFilter = VK_FILTER_LINEAR;
	sampl.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampl.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampl.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	vkCreateSampler( device, &sampl, nullptr, &defaultSamplerLinear );

	mainDeletionQueue.push_function([&](){
		vkDestroySampler( device, defaultSamplerNearest,nullptr );
		vkDestroySampler( device, defaultSamplerLinear,nullptr );

		destroyImage( whiteImage );
		destroyImage( greyImage );
		destroyImage( blackImage );
		destroyImage( jakobLUTImage );
	});
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
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();

	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard; // optional

	ImGui::StyleColorsDark();

	// this initializes imgui for SDL
	ImGui_ImplSDL3_InitForVulkan( window );

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

	init_info.PipelineInfoMain = {};
	init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineRenderingCreateInfoKHR pipeline_rendering_info = {};
	pipeline_rendering_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
	pipeline_rendering_info.colorAttachmentCount = 1;
	pipeline_rendering_info.pColorAttachmentFormats = &swapchainImageFormat;
	init_info.PipelineInfoMain.PipelineRenderingCreateInfo = pipeline_rendering_info;

	ImGui_ImplVulkan_Init( &init_info );

	// add the destroy the imgui created structures
	mainDeletionQueue.push_function( [ = ] ()  {
		ImGui_ImplVulkan_Shutdown();
		vkDestroyDescriptorPool( device, imguiPool, nullptr );

		ImGui_ImplSDL3_Shutdown();
		ImGui::DestroyContext();
	});
}

void PrometheusInstance::initLights () {
	// setting up some of the global resources used by the lights
	lightManager.Initialize();
	lightManager.brightnessScalar = &globalData.brightnessScalar;
	lightManager.sceneSize = &globalData.sceneExtents;
	lightManager.AddLight( 1.0f ); // placeholder, since the mouse light is gone

	// AllocatedImage previewImage = createImage( { 450 + 104, 64, 1 }, VK_FORMAT_R8G8B8A8_SNORM, VK_IMAGE_USAGE_SAMPLED_BIT );

	// do the work to populate the textures initially
	lightManagerMaintenance();
}

//==============================================================================================
// swapchain helpers
//==============================================================================================
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
	depthImageUsages |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	depthImageUsages |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	depthImageUsages |= VK_IMAGE_USAGE_SAMPLED_BIT;
	VkImageCreateInfo dimg_info = vkinit::image_create_info( depthImage.imageFormat, depthImageUsages, drawImageExtent );
	//allocate and create the depth image
	vmaCreateImage( allocator, &dimg_info, &rimg_allocinfo, &depthImage.image, &depthImage.allocation, nullptr );
	// build a image-view for the draw image to use for rendering
	VkImageViewCreateInfo dview_info = vkinit::imageview_create_info( depthImage.imageFormat, depthImage.image, VK_IMAGE_ASPECT_DEPTH_BIT );
	VK_CHECK( vkCreateImageView( device, &dview_info, nullptr, &depthImage.imageView ) );

	SetDebugName( VK_OBJECT_TYPE_IMAGE, ( uint64_t ) drawImage.image, "Draw Image" );
	SetDebugName( VK_OBJECT_TYPE_IMAGE, ( uint64_t ) depthImage.image, "Depth Image" );

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
	for ( size_t i = 0; i < swapchainImageViews.size(); i++ ) {
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
	VK_CHECK( vkWaitForFences( device, 1, &immediateFence, true, 99999999999 ) );
}

void PrometheusInstance::drawImgui ( VkCommandBuffer cmd, VkImageView targetImageView ) {
	VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info( targetImageView, nullptr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL );
	VkRenderingInfo renderInfo = vkinit::rendering_info( swapchainExtent, &colorAttachment, nullptr );

	vkCmdBeginRendering( cmd, &renderInfo );
	ImGui_ImplVulkan_RenderDrawData( ImGui::GetDrawData(), cmd );
	vkCmdEndRendering( cmd );
}
