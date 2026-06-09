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
	initBVH();
	initDescriptors();
	initComputePasses();
	initImgui();
	initDefaultData();
	initLights();

	// everything went fine
	isInitialized = true;
}

//============================================================================================================================
// Draw
//============================================================================================================================
void PrometheusInstance::Draw () {
	// wait until the gpu has finished rendering the last frame. Timeout of 3 seconds
	VK_CHECK( vkWaitForFences( device, 1, &getCurrentFrame().renderFence, true, 3000000000 ) );

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

	// write directly from the memory on the PrometheusInstance
	GlobalData* uniformData = ( GlobalData * ) GlobalUBO.allocation->GetMappedData();
	*uniformData = globalData;

	// reset the reset flag
	if ( globalData.reset != 0 ) {
		globalData.reset = 0;
		globalData.framesSinceReset = 0;
	}

	// if ( geometryListDirty ) {
		// bufferRebuildGPU();
	// }

	// start the command buffer recording
	VK_CHECK( vkBeginCommandBuffer( cmd, &cmdBeginInfo ) );

	// reset timers...
	timerManager->cmd = &cmd;
	timerManager->pool = &getCurrentFrame().queryPools;
	timerManager->reset();

	// put the core images into a general format
	vkutil::transition_image( cmd, XYZImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL );
	vkutil::transition_image( cmd, drawImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL );
	vkutil::transition_imageD( cmd, depthImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL );
	vkutil::transition_image( cmd, depthImageCache.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL );
	vkutil::transition_image( cmd, lineColorAttachment.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL );

	vkutil::transition_image( cmd, font_codepage437.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL );
	vkutil::transition_image( cmd, font_fatfont.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL );
	vkutil::transition_image( cmd, font_tinyfont.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL );

	vkutil::transition_image( cmd, PreviewAtlas.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL );
	vkutil::transition_image( cmd, PickISImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL );
	vkutil::transition_image( cmd, SpectrumISImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL );

	/*
	{ // compute shader to do one update of the raytrace process
		scopedTimer start( "Raytrace" );
		Raytrace.invoke( cmd );
	}

	{ // line drawing
		scopedTimer start( "Line Raster" );
		lineRaster.invoke( cmd );
	}

	{ // accumulate the result into a buffer
		scopedTimer start( "Accumulate" );
		Accumulate.invoke( cmd );
	}
	*/

	{ // placeholder test for building BLAS + TLAS + doing ray queries in a shader
		scopedTimer start( "Hardware RT Test" );
		HRTTest.invoke( cmd );
	}

	{ // compute shader to accumulate the raster result + put the resolved final image into the drawImage...
		scopedTimer start( "Present" );
		BufferPresent.invoke( cmd );
	}

	{ // do the debug line draw over top of the final LDR color
		scopedTimer start( "Debug Line Draw" );
		DebugLineDraw.invoke( cmd );
	}

	{ // do the debug string draw
		scopedTimer start( "Debug String Draw" );
		DebugStringDraw.invoke( cmd );
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

			if ( e.type == SDL_EVENT_QUIT ) {
				quit = true;
			}

			if ( e.type == SDL_EVENT_KEY_UP && e.key.scancode == SDL_SCANCODE_ESCAPE ) {
				quit = true;
			}

			if ( e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_M ) {
				showMenu = !showMenu;
			}

			if ( e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_N ) {
				lightManager.MouseLightToUserLight();
				globalData.reset = 1;
			}

			const bool shift = SDL_GetModState() & SDL_KMOD_LSHIFT;
			const float amount = shift ? 0.1f : 0.01f;

			if ( e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_EQUALS ) {
				globalData.brightnessScalar *= 1.0f + amount;
			}

			if ( e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_MINUS ) {
				globalData.brightnessScalar /= 1.0f + amount;
			}

			if ( e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_K ) {
				lightManager.clearList();
				globalData.reset = 1;
			}

			const bool* kb = SDL_GetKeyboardState( NULL );
			// if ( kb[ SDL_SCANCODE_RIGHT ] || kb[ SDL_SCANCODE_D ] ) {
				// globalData.rotation = glm::rotate( globalData.rotation, amount, glm::vec3( 0.0f, 1.0f, 0.0f ) );
				// globalData.reset = 1;
			// }
			if ( kb[ SDL_SCANCODE_R ] ) {
				globalData.reset = true;
			}

			if ( kb[ SDL_SCANCODE_D ] ) {
				globalData.reset = true;
				lightManager.MouseLight->parameters.rotation -= amount;
			}

			if ( kb[ SDL_SCANCODE_A ] ) {
				globalData.reset = true;
				lightManager.MouseLight->parameters.rotation += amount;
			}

			if ( kb[ SDL_SCANCODE_T ] && shift ) {
				screenshot();
			}
		}

		static glm::vec2 lastMousePos = glm::vec2( 0.0f );
		if ( distance( lastMousePos, globalData.mouseLoc.xy() ) > 8.0f ) {
			globalData.reset = true;
			lastMousePos = globalData.mouseLoc.xy();
		}

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

				if ( ImGui::Begin( "Edit" ) ) {

					ImGui::SliderFloat( "Brightness Scale", &globalData.brightnessScalar, 0.3f, 5.0f, "%.5f", ImGuiSliderFlags_Logarithmic ); // this should also apply to the raster step + accumulate step
					ImGui::SliderFloat( "Resolution Scale", &renderScale, 0.05f, 1.0f ); // this should also apply to the raster step + accumulate step
					ImGui::Separator();
					ImGui::Separator();
					static bool lsVisible = true;
					if ( ImGui::CollapsingHeader( "Show/Hide Load/Save Dialog", &lsVisible ) ) {
						static std::chrono::time_point< std::chrono::system_clock > tLastFileListUpdate = std::chrono::system_clock::now();

						static std::vector< std::string > savesList;
						if ( savesList.size() == 0 ) { // get the list
							struct pathLeafString {
								std::string operator()( const std::filesystem::directory_entry &entry ) const {
									return entry.path().string();
								}
							};
							std::filesystem::path p( "../lightingConfigs/" );
							std::filesystem::directory_iterator start( p );
							std::filesystem::directory_iterator end;
							std::transform( start, end, std::back_inserter( savesList ), pathLeafString() );
							std::sort( savesList.begin(), savesList.end() ); // sort these alphabetic
							tLastFileListUpdate = std::chrono::system_clock::now();
						}

						#define LISTBOX_SIZE_MAX 256
						const char *listboxItems[ LISTBOX_SIZE_MAX ];
						uint32_t i;
						for ( i = 0; i < LISTBOX_SIZE_MAX && i < savesList.size(); ++i ) {
							listboxItems[ i ] = savesList[ i ].c_str();
						}

						ImGui::Text( "Files In /lightingConfigs/" );
						static int listboxSelected = 0;
						ImGui::ListBox( " ", &listboxSelected, listboxItems, i, 24 );

						if ( ImGui::Button( " Load " ) ) {
							// LoadLightConfig( savesList[ listboxSelected ] );
							YAML::Node root = YAML::LoadFile( "../lightingConfigs/" + savesList[ listboxSelected ] );

							if ( root[ "globalBrightness" ] ) {
								globalData.brightnessScalar = root[ "globalBrightness" ].as< float >();
							}

							// clear the light list
							lightManager.clearList();

							// load the config specified
							YAML::Node lightsNode = root[ "lights" ];
							if ( lightsNode && lightsNode.IsSequence() ) {
								// list of lights in the file
								for ( const auto& node : lightsNode ) {
									Light l;

									l.parameters.position.x = node[ "positionX" ].as<float>();
									l.parameters.position.y = node[ "positionY" ].as<float>();
									l.parameters.rotation = node[ "rotation" ].as<float>();
									l.parameters.angleScalar = node[ "angleScalar" ].as<float>();
									l.parameters.cauchyMix = node[ "cauchyMix" ].as<float>();
									l.parameters.repeats = node[ "repeats" ].as<int>();
									l.parameters.emitterSpacing = node[ "emitterSpacing" ].as<float>();
									l.parameters.width = node[ "width" ].as<float>();

									// light source
									l.PDFPick = node[ "lightSource" ].as<int>();

									// gels / filter stack
									if ( node[ "gels" ] ) {
										l.filterStack = node[ "gels" ].as<std::vector<int>>();
									}

									l.dirtyFlag = true;
									lightManager.lights.push_back( l );
								}
							}
							globalData.reset = 1;
						}

						// triggering the thing every 10 seconds
						if ( ( tLastFileListUpdate - std::chrono::system_clock::now() ) > 10s ) {
							savesList.clear();
						}

						ImGui::SameLine();
						ImGui::InputText( "##SaveFile", currentExportFilename, IM_ARRAYSIZE( currentExportFilename ) );
						ImGui::SameLine();
						if ( ImGui::Button( " Save " ) ) {

							// output the light list
							YAML::Node outputNode;
							outputNode[ "globalBrightness" ] = globalData.brightnessScalar;
							for ( auto& l : lightManager.lights ) {
								YAML::Node node;
								node[ "positionX" ] = l.parameters.position.x;
								node[ "positionY" ] = l.parameters.position.y;
								node[ "rotation" ] = l.parameters.rotation;
								node[ "angleScalar" ] = l.parameters.angleScalar;
								node[ "cauchyMix" ] = l.parameters.cauchyMix;
								node[ "repeats" ] = l.parameters.repeats;
								node[ "emitterSpacing" ] = l.parameters.emitterSpacing;
								node[ "width" ] = l.parameters.width;

								node[ "lightSource" ] = l.PDFPick;
								node[ "gels" ] = l.filterStack;

								outputNode[ "lights" ].push_back( node );
							}
							std::ofstream fout( "../lightingConfigs/" + std::string( currentExportFilename ) + ".yaml" );
							fout << outputNode;

							savesList.clear(); // triggers rebuild of list
						}
					}

					/*
					static ImTextureID myTextureID = ( ImTextureID ) ImGui_ImplVulkan_AddTexture(
						defaultSamplerLinear,
						lineColorAttachment.imageView,
						VK_IMAGE_LAYOUT_GENERAL
					);
					ImGui::Image( myTextureID, ImVec2( 386, 256 ) );
					*/

					/*
					if ( ImGui::Button( "Add Preset" ) ) {
						// add the new one
						presets.push_back( lastPreset );

						// overwrite the file
						YAML::Node outputNode;
						for ( auto& p: presets ) {
							outputNode.push_back( p );
						}
						std::ofstream fout( "../src/presets.yaml" );
						fout << outputNode;
					}
					*/

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

		.add_required_extension( "VK_KHR_maintenance9" ) // for VK_QUERY_POOL_CREATE_RESET_BIT_KHR
		.add_required_extension( "VK_KHR_acceleration_structure" )
		.add_required_extension_features( accelFeatures )

		.add_required_extension( "VK_KHR_ray_query" )
		.add_required_extension_features( rayQueryFeatures )

		.add_required_extension( "VK_KHR_deferred_host_operations" )
		.add_required_extension( "VK_KHR_ray_tracing_position_fetch" )
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
		{ VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 6 },
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
			{ VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 4 },
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
	// create the buffer for the UBO
	{
		GlobalUBO = createBuffer( sizeof( GlobalData ), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU );
		SetDebugName( VK_OBJECT_TYPE_BUFFER, ( uint64_t ) GlobalUBO.buffer, "Global Data UBO" );
	}

	// create the accumulator texture
	{
		VkExtent3D bufferExtent = { ImageBufferResolution.width, ImageBufferResolution.height, 1 };
		XYZImage = createImage( bufferExtent, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT );
		SetDebugName( VK_OBJECT_TYPE_IMAGE, ( uint64_t ) XYZImage.image, "Accumulator" );
	}

	// create the raster attachments
	{
		lineColorAttachment = createImage( { ImageBufferResolution.width, ImageBufferResolution.height, 1 }, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT );
		SetDebugName( VK_OBJECT_TYPE_IMAGE, ( uint64_t ) lineColorAttachment.image, "Line Color Attachment" );
	}

	// buffer to hold geometry data + buffers for the grid precompute
	{
		// constant size allocations for Geo + BBoxes -> based on set maximum number of primtives
		GeometryBuffer = createBuffer( globalData.maxPrimitives * sizeof( geometryStruct ), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO );
		SetDebugName( VK_OBJECT_TYPE_BUFFER, ( uint64_t ) GeometryBuffer.buffer, "BVH Geometry Buffer" );
		BBoxBuffer = createBuffer( globalData.maxPrimitives * 4 * sizeof( float ), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO );
		SetDebugName( VK_OBJECT_TYPE_BUFFER, ( uint64_t ) BBoxBuffer.buffer, "BBox Buffer" );

		// this buffer is based on the current screen resolution
		// grid scaling + resize logic
		globalData.gridDims = glm::ivec2(
			( std::floor( ImageBufferResolution.width / globalData.gridScalar ) + 1 ),
			( std::floor( ImageBufferResolution.height / globalData.gridScalar ) + 1 ) );

		size_t uncompactedBufferSize = globalData.gridDims.x * globalData.gridDims.y * 16 * sizeof( int32_t );
		UncompactedGridBuffer = createBuffer( uncompactedBufferSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU );
		SetDebugName( VK_OBJECT_TYPE_BUFFER, ( uint64_t ) UncompactedGridBuffer.buffer, "Uncompacted Grid Buffer" );
	}

	// buffer for the rays
	{
		rayBuffer = createBuffer( globalData.numBounces * globalData.numRays * sizeof( raySegment ), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY );
		SetDebugName( VK_OBJECT_TYPE_BUFFER, ( uint64_t ) rayBuffer.buffer, "Ray Segment Buffer" );
	}

	{
		LightParametersBuffer = createBuffer( 256 * sizeof( LightEmitterParameters ), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU );
		SetDebugName( VK_OBJECT_TYPE_BUFFER, ( uint64_t ) LightParametersBuffer.buffer, "Light Parameter UBO" );
	}

	{ // buffer for debug line drawing
		debugLineDrawBuffer = createBuffer( ( 1 << 16 ) * sizeof( debugLinePoint ), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU );
		SetDebugName( VK_OBJECT_TYPE_BUFFER, ( uint64_t ) debugLineDrawBuffer.buffer, "Debug Line SSBO" );
	}

	{ // SSBO for the text renderer
		debugStringConfigBuffer = createBuffer( 1024 * sizeof( debugStringConfig ), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO );
		SetDebugName( VK_OBJECT_TYPE_BUFFER, ( uint64_t ) debugStringConfigBuffer.buffer, "Debug Text SSBO" );
	}

	{ // actually need to blit depth to another target, unfortunately
		depthImageCache = createImage( depthImage.imageExtent, VK_FORMAT_R32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT );
		SetDebugName( VK_OBJECT_TYPE_IMAGE, ( uint64_t ) depthImageCache.image, "Debug Text Depth" );
	}

	{ // Load font LUTs from disk...
		// code page 437
		int w, h, channels;
		unsigned char * data = stbi_load( "../fontLUTs/codepage437.png", &w, &h, &channels, 0 );
		VkExtent3D extent = { uint32_t( w ), uint32_t( h ), 1 };
		font_codepage437 = createImage( data, extent, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_USAGE_SAMPLED_BIT );
		stbi_image_free( data );

		// fatfont
		data = stbi_load( "../fontLUTs/fatFont.png", &w, &h, &channels, 0 );
		extent = { uint32_t( w ), uint32_t( h ), 1 };
		font_fatfont = createImage( data, extent, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_USAGE_SAMPLED_BIT );
		stbi_image_free( data );

		// tinyfont
		data = stbi_load( "../fontLUTs/tinyFont.png", &w, &h, &channels, 0 );
		extent = { uint32_t( w ), uint32_t( h ), 1 };
		font_tinyfont = createImage( data, extent, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_USAGE_SAMPLED_BIT );
		stbi_image_free( data );
	}

	// placeholder init
	std::mt19937 seedRNG( [] {
		std::random_device rd;
		std::seed_seq seq{  rd(), rd(), rd(), rd(), rd(), rd(), rd(), rd() };
		return std::mt19937( seq );
	} () );

	// it would be great to be able to add a line + text renderer
		// with state, so I can call reset when I want (or never)

	// in particular here, I want to draw boxes around the grids here, and randomize them
		// including the labels for the different diameters and the range of theta covered

	// it would also be nice to have a Z component, because then I've got a way to order things
		// this is something that could be shared between the two passes (text + lines) and would
		// actually enable full 3D rendering of lines if you really wanted to do that... neat

	// addDebugString();
	// addDebugDrawLine();
	// addDebugDrawBox();

	/*
	// float sizeRamp = 1.5f;
	for ( int xB = 300; xB < ImageBufferResolution.width - 300; xB += 400 ) {
		for ( int yB = 300; yB < ImageBufferResolution.height - 300; yB += 300 ) {

			const float sizeRamp = std::uniform_real_distribution< float >( 5.0f, 150.0f )( seedRNG );
			vec2 basePoint = vec2( xB, yB );
			for ( float xO = 0.0f; xO < 2.0f * 168.0f; xO += sizeRamp ) {
				for ( float yO = 0.0f; yO < 200.0f; yO += sizeRamp ) {
					vec2 offset = vec2( xO, yO );

					const int m = std::uniform_int_distribution< int >( 12, 14 )( seedRNG );
					// addArc( basePoint + offset, sizeRamp * 0.45, pi / 2.0f,  pi + pi / 2.0f, m );
					addArc( basePoint + offset, sizeRamp * 0.45, 0.0f,  pi * 2.0f, 12 );
				}
			}

			addDebugDrawBox( vec2( xB, yB ), vec2( xB + 2.0f * 168.0f, yB + 200.0f ), vec3( 1.0f ), 0.5f );
			addDebugString( vec2( xB + 168.0f, yB + 100.0f ), "Arc Test R=" + std::to_string( sizeRamp ), vec3( 0.618f ), 0 );

			// sizeRamp *= 1.2f;
		}
		// if ( sizeRamp > 100.0f ) break;
	}

	fmt::print( "Created {} primitives\n", globalData.numPrimitives );
	*/

	// make sure to clean up at the end
	mainDeletionQueue.push_function([ & ] () {
		// destroying buffers
		destroyBuffer( GlobalUBO );
		destroyBuffer( rayBuffer );
		destroyBuffer( LightParametersBuffer );
		destroyBuffer( debugLineDrawBuffer );
		destroyBuffer( debugStringConfigBuffer );
		destroyBuffer( GeometryBuffer );
		destroyBuffer( PrefixBuffer );
		destroyBuffer( GridBuffer );
		destroyBuffer( BBoxBuffer );
		destroyBuffer( UncompactedGridBuffer );


		// destroying images
		destroyImage( XYZImage );
		destroyImage( lineColorAttachment );
		destroyImage( PreviewAtlas );
		destroyImage( SpectrumISImage );
		destroyImage( PickISImage );
		destroyImage( font_codepage437 );
		destroyImage( font_fatfont );
		destroyImage( font_tinyfont );
		destroyImage( depthImageCache );
	});
}

void PrometheusInstance::addBLAS ( BLASCreateInfo createInfo, string name ) {
	assert(createInfo.numIndices >= 3);
	//assert(createInfo.numIndices % 3 == 0);
	assert(createInfo.numVertices >= 3);
	assert(createInfo.vertexBuffer != 0);
	assert(createInfo.indexBuffer != 0);
	//const uint32_t maxVertex = uint32_t(createInfo.vertexBuffer->SizeBytes() / createInfo.vertexStride - 1);

	BLASRecords.emplace_back();

	const uint32_t primitiveCount = createInfo.numIndices / 3;

	const VkAccelerationStructureGeometryKHR geometryInfo = {
	  .sType        = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
	  .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
	  .geometry =
		{
		  .triangles =
			{
			  .sType        = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
			  .vertexFormat = createInfo.vertexFormat,
			  .vertexData   = {.deviceAddress = createInfo.vertexBuffer},
			  .vertexStride = createInfo.vertexStride,
			  .maxVertex    = createInfo.numVertices - 1,
			  .indexType    = createInfo.indexType,
			  .indexData    = {.deviceAddress = createInfo.indexBuffer},
			},
		},
	  .flags = createInfo.geometryFlags,
	};

	//const uint32_t primitiveCount = uint32_t(createInfo.indexBuffer->SizeBytes() / sizeof(uint32_t) / 3);

	VkAccelerationStructureBuildGeometryInfoKHR buildInfo = {
	  .sType         = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
	  .type          = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
	  .flags         = static_cast<VkBuildAccelerationStructureFlagsKHR>(createInfo.buildFlags),
	  .mode          = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
	  .geometryCount = 1,
	  .pGeometries   = &geometryInfo,
	};

	VkAccelerationStructureBuildSizesInfoKHR buildSizeInfo = {
	  .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR,
	};
	vkGetAccelerationStructureBuildSizesKHR( device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo, &primitiveCount, &buildSizeInfo);

	AllocatedBuffer BLASBuffer = createBuffer( buildSizeInfo.accelerationStructureSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, VMA_MEMORY_USAGE_AUTO, name + " BLAS Buffer" );

	const VkAccelerationStructureCreateInfoKHR blasInfo = {
	  .sType  = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
	  .buffer = BLASBuffer.buffer,
	  .size   = buildSizeInfo.accelerationStructureSize,
	  .type   = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
	};

	VkAccelerationStructureKHR blas;
	VK_CHECK( vkCreateAccelerationStructureKHR( device, &blasInfo, nullptr, &blas ) );

	AllocatedBuffer scratchBuildBuffer = createBuffer( buildSizeInfo.buildScratchSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, VMA_MEMORY_USAGE_AUTO, name + " BLAS Scratch Buffer" );

	immediateSubmit( [&] ( VkCommandBuffer commandBuffer ) {
		buildInfo.dstAccelerationStructure = blas;
		buildInfo.scratchData              = { scratchBuildBuffer.deviceAddress };

		auto m = VkMemoryBarrier2{
				 .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
				 .srcStageMask  = VK_PIPELINE_STAGE_2_NONE,
				 .srcAccessMask = VK_ACCESS_2_NONE,
				 .dstStageMask  = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
				 .dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
		};
		auto  dep =  VkDependencyInfo{
			.sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
			.memoryBarrierCount = 1,
			.pMemoryBarriers    = &m
		  };
		vkCmdPipelineBarrier2( commandBuffer, &dep );
		VkAccelerationStructureBuildRangeInfoKHR buildRangeInfo = {.primitiveCount = primitiveCount};

		const VkAccelerationStructureBuildRangeInfoKHR* rangeInfos[] = { &buildRangeInfo };
		vkCmdBuildAccelerationStructuresKHR(commandBuffer, 1, &buildInfo, rangeInfos );
		// TODO: better barrier
		auto m2 = VkMemoryBarrier2{
				 .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
				 .srcStageMask  = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
				 .srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
				 .dstStageMask  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
				 .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
			};
		auto dep2 = VkDependencyInfo{
			.sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
			.memoryBarrierCount = 1,
			.pMemoryBarriers    = &m2
		  };
		vkCmdPipelineBarrier2( commandBuffer, &dep2 );
	  });

	if (createInfo.buildFlags & VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR)
	{
	  const VkQueryPoolCreateInfo queryPoolInfo = {
		.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
		.queryType  = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR,
		.queryCount = 1,
	  };

	  VkQueryPool compactedSizeQuery;
	  VK_CHECK(vkCreateQueryPool( device, &queryPoolInfo, nullptr, &compactedSizeQuery ) );

	  immediateSubmit( [&](VkCommandBuffer commandBuffer)
		{
		  vkCmdResetQueryPool(commandBuffer, compactedSizeQuery, 0, 1);
		  vkCmdWriteAccelerationStructuresPropertiesKHR(commandBuffer, 1, &blas, VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, compactedSizeQuery, 0);
		});

	  uint64_t compactedSize;
	  VK_CHECK(vkGetQueryPoolResults( device,
		compactedSizeQuery,
		0,
		1,
		sizeof(uint64_t),
		&compactedSize,
		sizeof(uint64_t),
		VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
	  vkDestroyQueryPool( device, compactedSizeQuery, nullptr);

	  AllocatedBuffer compactBLASBuffer = createBuffer( compactedSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO, name + " Compact BLAS Buffer");

	  VkAccelerationStructureCreateInfoKHR compactBlasInfo = {
		.sType  = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
		.buffer = compactBLASBuffer.buffer,
		.size   = compactedSize,
		.type   = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
	  };

	  VkAccelerationStructureKHR compactBlas;
	  VK_CHECK(vkCreateAccelerationStructureKHR( device, &compactBlasInfo, nullptr, &compactBlas));

	  immediateSubmit( [&](VkCommandBuffer commandBuffer)
		{
	  	auto m = VkMemoryBarrier2{
				   .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
				   .srcStageMask  = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
				   .srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
				   .dstStageMask  = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
				   .dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR,
			  };
	  	auto dep = VkDependencyInfo{
			  .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
			  .memoryBarrierCount = 1,
			  .pMemoryBarriers    = &m,
			};
	    vkCmdPipelineBarrier2(commandBuffer, &dep );
	  	auto cas = VkCopyAccelerationStructureInfoKHR{.sType = VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR,
			  .src                                                    = blas,
			  .dst                                                    = compactBlas,
			  .mode                                                   = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR};
		  vkCmdCopyAccelerationStructureKHR(commandBuffer, &cas );
	    // TODO: better barrier
	  	m =VkMemoryBarrier2{
									  .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
									  .srcStageMask  = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
									  .srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
									  .dstStageMask  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
									  .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
								  };
	  	dep = VkDependencyInfo{
								  .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
								  .memoryBarrierCount = 1,
								  .pMemoryBarriers    = &m,
							  };
	    vkCmdPipelineBarrier2(commandBuffer, &dep);
		});

	  vkDestroyAccelerationStructureKHR( device, blas, nullptr );
	  BLASBuffer = std::move( compactBLASBuffer );
	  blas = compactBlas;
	}

	BLASRecords[ BLASRecords.size() - 1 ].ASBuffer = std::move( BLASBuffer );

	VkAccelerationStructureDeviceAddressInfoKHR addressInfo = {
	  .sType                 = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
	  .accelerationStructure = blas,
	};
	BLASRecords[ BLASRecords.size() - 1 ].address_ = vkGetAccelerationStructureDeviceAddressKHR( device, &addressInfo );

	auto debugnameinfo = VkDebugUtilsObjectNameInfoEXT{
		.sType        = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
		.objectType   = VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR,
		.objectHandle = reinterpret_cast<uint64_t>(blas),
		.pObjectName  = name.c_str(),
	  };
	vkSetDebugUtilsObjectNameEXT( device, &debugnameinfo );

	BLASRecords[ BLASRecords.size() - 1 ].AShandle = blas;

	mainDeletionQueue.push_function([ & ] () {
		auto handle = BLASRecords[ BLASRecords.size() - 1 ].AShandle;
		vkDestroyAccelerationStructureKHR( device, handle, nullptr );
	});
}

void PrometheusInstance::initBVH () {
	TLASCreateInfo createInfo;
	std::string name = "MAIN TLAS";

	// creating the initial mesh
	{ // creating the heightmap
		BLASCreateInfo createInfoBLAS;
		const uint32_t dim = 1024;
			long unsigned int seed = std::chrono::system_clock::now().time_since_epoch().count();

			std::default_random_engine engine{ seed };
			std::uniform_real_distribution< float > distribution{ 0.0f, 1.0f };

			// #define TILE
			#ifdef TILE
				const auto size = dim;
			#else
				const auto size = dim + 1; // for no_wrap
			#endif

			const auto edge = size - 1;
			std::vector< std::vector< float > > data;
			data.resize( size );
			for ( uint32_t i = 0; i < size; i++ ) {
				data[ i ].resize( size );
			}

			// data[ 0 ][ 0 ] = data[ edge ][ 0 ] = data[ 0 ][ edge ] = data[ edge ][ edge ] = 0.25f;

			std::uniform_real_distribution< float > distribution_init{ 0.0f, 1.5f };
			data[ 0 ][ 0 ] = distribution_init( engine );
			data[ edge ][ 0 ] = distribution_init( engine );
			data[ 0 ][ edge ] = distribution_init( engine );
			data[ edge ][ edge ] = distribution_init( engine );

#ifdef TILE
			heightfield::diamond_square_wrap
		#else
			heightfield::diamond_square_no_wrap
		#endif
			(size,
				// random
				[ &engine, &distribution ]( float range ) {
					return distribution( engine ) * range;
				},
				// variance
				[]( int level ) -> float {
					return std::pow( 0.5f, level );
					// return static_cast<float>( std::numeric_limits<float>::max() / 2 ) * std::pow(0.5f, level);
					// return static_cast<float>(std::numeric_limits<float>::max()/1.6) * std::pow(0.5f, level);
				},
				// at
				[ &data ]( int x, int y ) -> float& {
					return data[ x ][ y ];
				}
			);

		// we have the thing in data[ x ][ y ], need it in a linearized format (+ integer conversion and tracking the normalization factor)
		std::vector< vec3 > vboData;
		auto gridToWorld = [ & ]( int i ) -> float {
			return remap( i, 0, dim, -1.0f, 1.0f );
		};

		for ( size_t x = 0; x < dim - 1; x++ ) {
			for ( size_t y = 0; y < dim - 1; y++ ) {
				float aH = std::clamp( data[ x ][ y ], 0.0f, 5.0f );
				float bH = std::clamp( data[ x + 1 ][ y ], 0.0f, 5.0f );
				float cH = std::clamp( data[ x ][ y + 1 ], 0.0f, 5.0f );
				float dH = std::clamp( data[ x + 1 ][ y + 1 ], 0.0f, 5.0f );

			/* need to create 2 triangles, total 6 verts
				A ( 0 )	@=======@ B ( 1 )
						|      /|
						|     / |
						|    /  |
						|   /   |
						|  /    |
						| /     |
						|/      |
				C ( 2 )	@=======@ D ( 3 ) --> X */

				// ABC
				vboData.emplace_back( vec3( gridToWorld( x ), gridToWorld( y ), aH ) );
				vboData.emplace_back( vec3( gridToWorld( x + 1 ), gridToWorld( y ), bH ) );
				vboData.emplace_back( vec3( gridToWorld( x ), gridToWorld( y + 1 ), cH ) );

				// CBD
				vboData.emplace_back( vec3( gridToWorld( x ), gridToWorld( y + 1 ), cH ) );
				vboData.emplace_back( vec3( gridToWorld( x + 1 ), gridToWorld( y ), bH ) );
				vboData.emplace_back( vec3( gridToWorld( x + 1 ), gridToWorld( y + 1 ), dH ) );
			}
		}

		// index buffer is required -> create the trivial IBO needeed here
		std::vector< uint32_t > iboData( vboData.size() );
		std::iota( iboData.begin(), iboData.end(), 0 );

		// uploading instance buffer and vertex buffer data
		IBObuffer = createBuffer( sizeof( uint32_t ) * iboData.size(), VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_2_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, VMA_MEMORY_USAGE_AUTO, "HWRT IBO" );
		VBObuffer = createBuffer( sizeof( vec3 ) * iboData.size(), VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_2_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, VMA_MEMORY_USAGE_AUTO, "HWRT VBO" );

		// copy the data into the buffers
		vec3 * VBOPtr = ( vec3 * ) VBObuffer.allocation->GetMappedData();
		memcpy( VBOPtr, &vboData[ 0 ], vboData.size() * sizeof( vec3 ) );
		uint32_t * IBOPtr = ( uint32_t * ) IBObuffer.allocation->GetMappedData();
		memcpy( IBOPtr, &iboData[ 0 ], iboData.size() * sizeof( uint32_t ) );

		// filling out the create struct with the information about the mesh
		createInfoBLAS.numVertices = createInfoBLAS.numIndices = iboData.size();
		createInfoBLAS.vertexStride = sizeof( float ) * 3;
		createInfoBLAS.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
		createInfoBLAS.indexBuffer = IBObuffer.deviceAddress;
		createInfoBLAS.vertexBuffer = VBObuffer.deviceAddress;
		createInfoBLAS.indexType = VK_INDEX_TYPE_UINT32;

		// VkGeometryFlagsKHR geometryFlags = {};
		// VkBuildAccelerationStructureFlagsKHR buildFlags = {};

		// add to the list
		addBLAS( createInfoBLAS, "Heightmap Mesh" );
	}

	// mesh should now be prepped for BVH build
		// setting up a single instance...
	createInfo.geometryFlags = VK_GEOMETRY_OPAQUE_BIT_KHR;
	createInfo.buildFlags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_DATA_ACCESS_KHR;
	createInfo.instanceBuffer = createBuffer( sizeof( TLASInstance ), VK_BUFFER_USAGE_2_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO );

	// writing instance data to the buffer
	TLASInstance * instances = ( TLASInstance * ) createInfo.instanceBuffer.allocation->GetMappedData();
	instances[ 0 ].BLASAddress = BLASRecords[ 0 ].address_;
	instances[ 0 ].transform = {
		1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f
	};

	const VkAccelerationStructureGeometryKHR geometryInfo = {
      .sType        = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
      .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
      .geometry     = {.instances =
                         {
                           .sType           = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
                           .arrayOfPointers = false,
                           .data            = createInfo.instanceBuffer.deviceAddress,
                     }},
      .flags        = static_cast<VkGeometryFlagsKHR>(createInfo.geometryFlags),
    };

    const uint32_t instanceCount = uint32_t(createInfo.instanceBuffer.allocation->GetSize() / sizeof(TLASInstance));

    VkAccelerationStructureBuildGeometryInfoKHR buildInfo = {
      .sType         = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
      .type          = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
      .flags         = static_cast<VkBuildAccelerationStructureFlagsKHR>(createInfo.buildFlags),
      .mode          = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
      .geometryCount = 1,
      .pGeometries   = &geometryInfo,
    };

    VkAccelerationStructureBuildSizesInfoKHR buildSizeInfo = {
      .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR,
    };
    vkGetAccelerationStructureBuildSizesKHR( device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo, &instanceCount, &buildSizeInfo);
	AllocatedBuffer tlasBuffer = createBuffer( buildSizeInfo.accelerationStructureSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, VMA_MEMORY_USAGE_AUTO, name + " TLAS Buffer" );

    VkAccelerationStructureCreateInfoKHR tlasInfo = {
      .sType  = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
      .buffer = tlasBuffer.buffer,
      .size   = buildSizeInfo.accelerationStructureSize,
      .type   = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
    };

    VkAccelerationStructureKHR tlas;
    VK_CHECK(vkCreateAccelerationStructureKHR(device, &tlasInfo, nullptr, &tlas ) );
	AllocatedBuffer scratchBuildBuffer = createBuffer( buildSizeInfo.buildScratchSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, VMA_MEMORY_USAGE_AUTO, name + " TLAS Scratch Buffer" );

    immediateSubmit([&](VkCommandBuffer commandBuffer)
      {
        buildInfo.dstAccelerationStructure = tlas;
        buildInfo.scratchData              = {scratchBuildBuffer.deviceAddress};

    	auto m =VkMemoryBarrier2{
				 .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
				 .srcStageMask  = VK_PIPELINE_STAGE_2_NONE,
				 .srcAccessMask = VK_ACCESS_2_NONE,
				 .dstStageMask  = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
				 .dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
			};
    	auto b = VkDependencyInfo{
			.sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
			.memoryBarrierCount = 1,
			.pMemoryBarriers    = &m,
    		};
        vkCmdPipelineBarrier2(commandBuffer, &b );
        VkAccelerationStructureBuildRangeInfoKHR buildRangeInfo = {.primitiveCount = instanceCount};
    	const VkAccelerationStructureBuildRangeInfoKHR* rangeInfos[] = { &buildRangeInfo };
        vkCmdBuildAccelerationStructuresKHR(commandBuffer, 1, &buildInfo, rangeInfos );

        // TODO: better barrier
    	m = VkMemoryBarrier2{
				 .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
				 .srcStageMask  = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
				 .srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
				 .dstStageMask  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
				 .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
			};
    	auto dep = VkDependencyInfo{
			.sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
			.memoryBarrierCount = 1,
			.pMemoryBarriers    = &m
		  };
        vkCmdPipelineBarrier2(commandBuffer, &dep);
      });

    mainTLAS.buffer = std::move(tlasBuffer);

    VkAccelerationStructureDeviceAddressInfoKHR addressInfo = {
      .sType                 = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
      .accelerationStructure = tlas,
    };
    mainTLAS.address = vkGetAccelerationStructureDeviceAddressKHR(device, &addressInfo);

	auto m = VkDebugUtilsObjectNameInfoEXT{
		.sType        = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
		.objectType   = VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR,
		.objectHandle = reinterpret_cast<uint64_t>(tlas),
		.pObjectName  = name.c_str(),
	  };
    vkSetDebugUtilsObjectNameEXT(device, &m);
    mainTLAS.handle = tlas;

	// fmt::print( "created tlas {}", ( int ) tlas );

    // descriptorInfo_ = Fvog::GetDevice().AllocateAccelerationStructureDescriptor(tlas);
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
	{ // testing hardware RT
		{ // descriptor layout
			DescriptorLayoutBuilder builder;
			builder.add_binding( 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ); // global config UBO
			builder.add_binding( 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ); // the accumulator
			builder.add_binding( 2, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR ); // handle for the hardware RT object

			HRTTest.descriptorSetLayout = builder.build( device, VK_SHADER_STAGE_COMPUTE_BIT );
			SetDebugName( VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, ( uint64_t ) HRTTest.descriptorSetLayout, "Hardware Raytrace Descriptor Set Layout" );
		}

		{ // pipeline layout + compute pipeline
			VkPushConstantRange pushConstant{};
			pushConstant.offset = 0;
			pushConstant.size = sizeof( PushConstants );
			pushConstant.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

			VkPipelineLayoutCreateInfo computeLayout{};
			computeLayout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
			computeLayout.pNext = nullptr;
			computeLayout.pSetLayouts = &HRTTest.descriptorSetLayout;
			computeLayout.setLayoutCount = 1;
			computeLayout.pPushConstantRanges = &pushConstant;
			computeLayout.pushConstantRangeCount = 1;

			VK_CHECK( vkCreatePipelineLayout( device, &computeLayout, nullptr, &HRTTest.pipelineLayout ) );
			SetDebugName( VK_OBJECT_TYPE_PIPELINE_LAYOUT, ( uint64_t ) HRTTest.pipelineLayout, "Hardware Raytrace Pipeline Layout" );

			VkShaderModule RaytraceShader;
			if ( !vkutil::load_shader_module("../shaders/HRTTest.comp.glsl.spv", device, &RaytraceShader ) ) {
				fmt::print( "Error when building the Hardware Raytrace Compute Shader\n" );
			}
			SetDebugName( VK_OBJECT_TYPE_SHADER_MODULE, ( uint64_t ) RaytraceShader, "Hardware Raytrace Shader Module" );

			VkPipelineShaderStageCreateInfo stageinfo{};
			stageinfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			stageinfo.pNext = nullptr;
			stageinfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
			stageinfo.module = RaytraceShader;
			stageinfo.pName = "main";

			VkComputePipelineCreateInfo computePipelineCreateInfo{};
			computePipelineCreateInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
			computePipelineCreateInfo.pNext = nullptr;
			computePipelineCreateInfo.layout = HRTTest.pipelineLayout;
			computePipelineCreateInfo.stage = stageinfo;

			VK_CHECK( vkCreateComputePipelines( device, VK_NULL_HANDLE, 1, &computePipelineCreateInfo, nullptr, &HRTTest.pipeline ) );
			SetDebugName( VK_OBJECT_TYPE_PIPELINE, ( uint64_t ) HRTTest.pipeline, "Hardware Raytrace Compute Pipeline" );
			vkDestroyShaderModule( device, RaytraceShader, nullptr );

			// deletors for the pipeline layout + pipeline
			mainDeletionQueue.push_function( [ & ] () {
				vkDestroyDescriptorSetLayout( device, HRTTest.descriptorSetLayout, nullptr );
				vkDestroyPipelineLayout( device, HRTTest.pipelineLayout, nullptr );
				vkDestroyPipeline( device, HRTTest.pipeline, nullptr );
			});
		}

		// invoke() lambda
		HRTTest.invoke = [ & ] ( VkCommandBuffer cmd ){
			// dynamic descriptor allocation, to bind a texture
			HRTTest.descriptorSet = getCurrentFrame().frameDescriptors.allocate( device, HRTTest.descriptorSetLayout );
			{
				DescriptorWriter writer;
				writer.write_buffer( 0, GlobalUBO.buffer, sizeof( GlobalData ), 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER );
				writer.write_image( 1, XYZImage.imageView, defaultSamplerNearest, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE );
				writer.write_acceleration_structure( 2, &mainTLAS.handle );
				writer.update_set( device, HRTTest.descriptorSet );
			}

			vkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, HRTTest.pipeline );

			// bind the descriptor set, as just recorded
			vkCmdBindDescriptorSets( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, HRTTest.pipelineLayout, 0, 1, &HRTTest.descriptorSet, 0, nullptr );

			// get a new wang RNG seed
			HRTTest.pushConstants.wangSeed = genWangSeed();

			// send the current value of the push constants
			vkCmdPushConstants( cmd, HRTTest.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof( PushConstants ), &HRTTest.pushConstants );

			// dispatch for all the pixels
			vkCmdDispatch( cmd, ( drawExtent.width + 15 ) / 16, ( drawExtent.height + 15 ) / 16, 1 );

			VkImageMemoryBarrier2 barrierC = makeImageBarrier( XYZImage.image, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT );
			VkDependencyInfo barrierDependency {
				.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
				.imageMemoryBarrierCount = 1,
				.pImageMemoryBarriers = &barrierC
			};

			vkCmdPipelineBarrier2( cmd, &barrierDependency );
		};
	}

	if ( false ) { // Raytrace update
		{ // descriptor layout
			DescriptorLayoutBuilder builder;
			builder.add_binding( 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ); // global config UBO
			builder.add_binding( 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ); // the ray buffer
			builder.add_binding( 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ); // the iCDF texture for light spectra
			builder.add_binding( 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ); // the discrete IS texture for lights
			builder.add_binding( 4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ); // the parameters for the light emitters
			// buffers for the BVH
			builder.add_binding( 5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ); // the geometry buffer
			builder.add_binding( 6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ); // the prefix buffer
			builder.add_binding( 7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ); // the grid buffer
			Raytrace.descriptorSetLayout = builder.build( device, VK_SHADER_STAGE_COMPUTE_BIT );
			SetDebugName( VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, ( uint64_t ) Raytrace.descriptorSetLayout, "Raytrace Descriptor Set Layout" );
		}

		{ // pipeline layout + compute pipeline
			VkPushConstantRange pushConstant{};
			pushConstant.offset = 0;
			pushConstant.size = sizeof( PushConstants );
			pushConstant.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

			VkPipelineLayoutCreateInfo computeLayout{};
			computeLayout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
			computeLayout.pNext = nullptr;
			computeLayout.pSetLayouts = &Raytrace.descriptorSetLayout;
			computeLayout.setLayoutCount = 1;
			computeLayout.pPushConstantRanges = &pushConstant;
			computeLayout.pushConstantRangeCount = 1;

			VK_CHECK( vkCreatePipelineLayout( device, &computeLayout, nullptr, &Raytrace.pipelineLayout ) );
			SetDebugName( VK_OBJECT_TYPE_PIPELINE_LAYOUT, ( uint64_t ) Raytrace.pipelineLayout, "Raytrace Pipeline Layout" );

			VkShaderModule RaytraceShader;
			if ( !vkutil::load_shader_module("../shaders/raytrace.comp.glsl.spv", device, &RaytraceShader ) ) {
				fmt::print( "Error when building the Raytrace Compute Shader\n" );
			}
			SetDebugName( VK_OBJECT_TYPE_SHADER_MODULE, ( uint64_t ) RaytraceShader, "Raytrace Shader Module" );

			VkPipelineShaderStageCreateInfo stageinfo{};
			stageinfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			stageinfo.pNext = nullptr;
			stageinfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
			stageinfo.module = RaytraceShader;
			stageinfo.pName = "main";

			VkComputePipelineCreateInfo computePipelineCreateInfo{};
			computePipelineCreateInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
			computePipelineCreateInfo.pNext = nullptr;
			computePipelineCreateInfo.layout = Raytrace.pipelineLayout;
			computePipelineCreateInfo.stage = stageinfo;

			VK_CHECK( vkCreateComputePipelines( device, VK_NULL_HANDLE, 1, &computePipelineCreateInfo, nullptr, &Raytrace.pipeline ) );
			SetDebugName( VK_OBJECT_TYPE_PIPELINE, ( uint64_t ) Raytrace.pipeline, "Raytrace Compute Pipeline" );
			vkDestroyShaderModule( device, RaytraceShader, nullptr );

			// deletors for the pipeline layout + pipeline
			mainDeletionQueue.push_function( [ & ] () {
				vkDestroyDescriptorSetLayout( device, Raytrace.descriptorSetLayout, nullptr );
				vkDestroyPipelineLayout( device, Raytrace.pipelineLayout, nullptr );
				vkDestroyPipeline( device, Raytrace.pipeline, nullptr );
			});
		}

		// invoke() lambda
		Raytrace.invoke = [ & ] ( VkCommandBuffer cmd ){
			// dynamic descriptor allocation, to bind a texture
			Raytrace.descriptorSet = getCurrentFrame().frameDescriptors.allocate( device, Raytrace.descriptorSetLayout );
			{
				DescriptorWriter writer;
				writer.write_buffer( 0, GlobalUBO.buffer, sizeof( GlobalData ), 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER );
				writer.write_buffer( 1, rayBuffer.buffer, globalData.numBounces * globalData.numRays * sizeof( raySegment ), 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER );
				writer.write_image( 2, SpectrumISImage.imageView, defaultSamplerLinear, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER );
				writer.write_image( 3, PickISImage.imageView, defaultSamplerNearest, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER );
				writer.write_buffer( 4, LightParametersBuffer.buffer, 256 * sizeof( LightEmitterParameters ), 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER );
				// BVH buffers
				writer.write_buffer( 5, GeometryBuffer.buffer, VK_WHOLE_SIZE, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER );
				writer.write_buffer( 6, PrefixBuffer.buffer, VK_WHOLE_SIZE, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER );
				writer.write_buffer( 7, GridBuffer.buffer, VK_WHOLE_SIZE, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER );
				writer.update_set( device, Raytrace.descriptorSet );
			}

			vkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, Raytrace.pipeline );

			// bind the descriptor set, as just recorded
			vkCmdBindDescriptorSets( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, Raytrace.pipelineLayout, 0, 1, &Raytrace.descriptorSet, 0, nullptr );

			// get a new wang RNG seed
			Raytrace.pushConstants.wangSeed = genWangSeed();

			// send the current value of the push constants
			vkCmdPushConstants( cmd, Raytrace.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof( PushConstants ), &Raytrace.pushConstants );

			// dispatch for all the pixels
			vkCmdDispatch( cmd, globalData.numRays / 64, 1, 1 );

			VkBufferMemoryBarrier2 bufferBarrier = makeBufferBarrier( rayBuffer.buffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT );
			VkDependencyInfo barrierDependency {
				.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
				.bufferMemoryBarrierCount = 1,
				.pBufferMemoryBarriers = &bufferBarrier,
			};

			vkCmdPipelineBarrier2( cmd, &barrierDependency );
		};
	}

	if ( false ) { // Line Rasterization
		{ // descriptor layout
			// we're eventually going to just want 32-bit uint IDs out of this process, but for now I think color makes sense...
				// we of course also need depth for the z-testing.

			// Color and Depth Attachments are part of the rendering state, and are not specified as part of the descriptor set or descriptor set layout

			DescriptorLayoutBuilder builder;
			builder.add_binding( 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ); // global config UBO
			builder.add_binding( 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ); // Ray state buffer
			lineRaster.descriptorSetLayout = builder.build( device, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT );
			SetDebugName( VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, ( uint64_t ) lineRaster.descriptorSetLayout, "Line Raster Descriptor Set Layout" );
		}

		{ // pipeline layout + pipeline build
			VkPushConstantRange pushConstant{};
			pushConstant.offset = 0;
			pushConstant.size = sizeof( PushConstants );
			pushConstant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

			VkPipelineLayoutCreateInfo rasterLayout{};
			rasterLayout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
			rasterLayout.pNext = nullptr;
			rasterLayout.pSetLayouts = &lineRaster.descriptorSetLayout;
			rasterLayout.setLayoutCount = 1;
			rasterLayout.pPushConstantRanges = &pushConstant;
			rasterLayout.pushConstantRangeCount = 1;

			VK_CHECK( vkCreatePipelineLayout( device, &rasterLayout, nullptr, &lineRaster.pipelineLayout ) );
			SetDebugName( VK_OBJECT_TYPE_PIPELINE_LAYOUT, ( uint64_t ) lineRaster.pipelineLayout, "Line Raster Pipeline Layout" );

			VkShaderModule lineFragShader;
			if ( !vkutil::load_shader_module( "../shaders/lineDraw.frag.glsl.spv", device, &lineFragShader ) ) {
				fmt::print( "Error when building the Line Draw Fragment shader module\n" );
			}
			SetDebugName( VK_OBJECT_TYPE_SHADER_MODULE, ( uint64_t ) lineFragShader, "Line Fragment Shader Module" );

			VkShaderModule lineVertexShader;
			if ( !vkutil::load_shader_module( "../shaders/lineDraw.vert.glsl.spv", device, &lineVertexShader ) ) {
				fmt::print( "Error when building the Line Draw Vertex shader module\n" );
			}
			SetDebugName( VK_OBJECT_TYPE_SHADER_MODULE, ( uint64_t ) lineVertexShader, "Line Vertex Shader Module" );

			PipelineBuilder pipelineBuilder;
			pipelineBuilder._pipelineLayout = lineRaster.pipelineLayout;
			pipelineBuilder.set_shaders( lineVertexShader, lineFragShader );
			pipelineBuilder.set_input_topology( VK_PRIMITIVE_TOPOLOGY_LINE_LIST );
			pipelineBuilder.set_polygon_mode( VK_POLYGON_MODE_FILL );
			pipelineBuilder.set_cull_mode( VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE );
			pipelineBuilder.set_multisampling_none();
			pipelineBuilder.enable_blending_additive();
			pipelineBuilder.disable_depthtest();
			pipelineBuilder.set_color_attachment_format( lineColorAttachment.imageFormat );
			lineRaster.pipeline = pipelineBuilder.build_pipeline( device );
			SetDebugName( VK_OBJECT_TYPE_PIPELINE, ( uint64_t ) lineRaster.pipeline, "Line Raster Pipeline" );

			// cleanup
			vkDestroyShaderModule( device, lineFragShader, nullptr );
			vkDestroyShaderModule( device, lineVertexShader, nullptr );

			mainDeletionQueue.push_function( [ & ] ()  {
				vkDestroyDescriptorSetLayout( device, lineRaster.descriptorSetLayout, nullptr );
				vkDestroyPipeline( device, lineRaster.pipeline, nullptr );
				vkDestroyPipelineLayout( device, lineRaster.pipelineLayout, nullptr );
			});
		}

		lineRaster.invoke = [ & ] ( VkCommandBuffer cmd ) {
			// additive raster for the agent locations
			VkClearValue clearColor = { 0.0f, 0.0f, 0.0f, 1.0f };
			VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info( lineColorAttachment.imageView, &clearColor, VK_IMAGE_LAYOUT_GENERAL );
			VkRenderingInfo renderInfo = vkinit::rendering_info( ImageBufferResolution, &colorAttachment, nullptr );

			vkCmdBeginRendering( cmd, &renderInfo );
			vkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, lineRaster.pipeline );

			// dynamic descriptor allocation
			lineRaster.descriptorSet = getCurrentFrame().frameDescriptors.allocate( device, lineRaster.descriptorSetLayout );
			{
				DescriptorWriter writer;
				writer.write_buffer( 0, GlobalUBO.buffer, sizeof( GlobalData ), 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER );
				writer.write_buffer( 1, rayBuffer.buffer, globalData.numBounces * globalData.numRays * sizeof( raySegment ), 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER );
				writer.update_set( device, lineRaster.descriptorSet );
			}

			vkCmdBindDescriptorSets( cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, lineRaster.pipelineLayout, 0, 1, &lineRaster.descriptorSet, 0, nullptr );

			//set dynamic viewport and scissor
			VkViewport viewport = {};
			viewport.x = 0;
			viewport.y = 0;
			viewport.width = float( ImageBufferResolution.width * renderScale );
			viewport.height = float( ImageBufferResolution.height * renderScale );
			viewport.minDepth = 0.0f;
			viewport.maxDepth = 1.0f;
			vkCmdSetViewport( cmd, 0, 1, &viewport );

			VkRect2D scissor = {};
			scissor.offset.x = 0;
			scissor.offset.y = 0;
			scissor.extent.width = ImageBufferResolution.width;
			scissor.extent.height = ImageBufferResolution.height;
			vkCmdSetScissor( cmd, 0, 1, &scissor );

			// draw all the agents as points
			vkCmdBindDescriptorSets( cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,  lineRaster.pipelineLayout, 0, 1, &lineRaster.descriptorSet, 0, nullptr );
			vkCmdPushConstants( cmd, lineRaster.pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof( PushConstants ), &lineRaster.pushConstants );

			// launch a draw command to do the fullscreen triangle
			vkCmdDraw( cmd, 2 * ( globalData.numRays * globalData.numBounces ), 1, 0, 0 );
			vkCmdEndRendering( cmd );

			const float fillValue = 0.0f;
			const uint32_t& fillValueU32 = reinterpret_cast< const uint32_t& >( fillValue );
			vkCmdFillBuffer( cmd, rayBuffer.buffer, 0, globalData.numBounces * globalData.numRays * sizeof( raySegment ), fillValueU32 );

			VkImageMemoryBarrier2 barrierC = makeImageBarrier( lineColorAttachment.image, VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT );
			VkBufferMemoryBarrier2 barrierB = makeBufferBarrier( rayBuffer.buffer, VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, VK_ACCESS_2_SHADER_READ_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT );

			VkDependencyInfo barrierDependency {
				.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
				.bufferMemoryBarrierCount = 1,
				.pBufferMemoryBarriers = &barrierB,
				.imageMemoryBarrierCount = 1,
				.pImageMemoryBarriers = &barrierC
			};

			vkCmdPipelineBarrier2( cmd, &barrierDependency );
		};
	}

	if ( false ) { // Accumulate
		{ // descriptor layout
			DescriptorLayoutBuilder builder;
			builder.add_binding( 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ); // global config UBO
			builder.add_binding( 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ); // draw image
			builder.add_binding( 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ); // XYZ Buffer
			Accumulate.descriptorSetLayout = builder.build( device, VK_SHADER_STAGE_COMPUTE_BIT );
			SetDebugName( VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, ( uint64_t ) Accumulate.descriptorSetLayout, "Accumulate Descriptor Set Layout" );
		}

		{ // pipeline layout + compute pipeline
			VkPushConstantRange pushConstant{};
			pushConstant.offset = 0;
			pushConstant.size = sizeof( PushConstants );
			pushConstant.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

			VkPipelineLayoutCreateInfo computeLayout{};
			computeLayout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
			computeLayout.pNext = nullptr;
			computeLayout.pSetLayouts = &Accumulate.descriptorSetLayout;
			computeLayout.setLayoutCount = 1;
			computeLayout.pPushConstantRanges = &pushConstant;
			computeLayout.pushConstantRangeCount = 1;

			VK_CHECK( vkCreatePipelineLayout( device, &computeLayout, nullptr, &Accumulate.pipelineLayout ) );
			SetDebugName( VK_OBJECT_TYPE_PIPELINE_LAYOUT, ( uint64_t ) Accumulate.pipelineLayout, "Accumulate Pipeline Layout" );

			VkShaderModule AccumulateShader;
			if ( !vkutil::load_shader_module("../shaders/accumulate.comp.glsl.spv", device, &AccumulateShader ) ) {
				fmt::print( "Error when building the Accumulate Compute Shader\n" );
			}
			SetDebugName( VK_OBJECT_TYPE_SHADER_MODULE, ( uint64_t ) AccumulateShader, "Accumulate Shader Module" );

			VkPipelineShaderStageCreateInfo stageinfo{};
			stageinfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			stageinfo.pNext = nullptr;
			stageinfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
			stageinfo.module = AccumulateShader;
			stageinfo.pName = "main";

			VkComputePipelineCreateInfo computePipelineCreateInfo{};
			computePipelineCreateInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
			computePipelineCreateInfo.pNext = nullptr;
			computePipelineCreateInfo.layout = Accumulate.pipelineLayout;
			computePipelineCreateInfo.stage = stageinfo;

			VK_CHECK( vkCreateComputePipelines( device, VK_NULL_HANDLE, 1, &computePipelineCreateInfo, nullptr, &Accumulate.pipeline ) );
			SetDebugName( VK_OBJECT_TYPE_PIPELINE, ( uint64_t ) Accumulate.pipeline, "Accumulate Compute Pipeline" );
			vkDestroyShaderModule( device, AccumulateShader, nullptr );

			// deletors for the pipeline layout + pipeline
			mainDeletionQueue.push_function( [ & ] () {
				vkDestroyDescriptorSetLayout( device, Accumulate.descriptorSetLayout, nullptr );
				vkDestroyPipelineLayout( device, Accumulate.pipelineLayout, nullptr );
				vkDestroyPipeline( device, Accumulate.pipeline, nullptr );
			});
		}

		// invoke() lambda
		Accumulate.invoke = [ & ]( VkCommandBuffer cmd ) {
			Accumulate.descriptorSet = getCurrentFrame().frameDescriptors.allocate( device, Accumulate.descriptorSetLayout );
			{
				DescriptorWriter writer;
				writer.write_buffer( 0, GlobalUBO.buffer, sizeof( GlobalData ), 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER );
				writer.write_image( 1, lineColorAttachment.imageView, defaultSamplerLinear, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER );
				writer.write_image( 2, XYZImage.imageView, defaultSamplerNearest, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE );
				writer.update_set( device, Accumulate.descriptorSet );
			}

			vkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, Accumulate.pipeline );

			// bind the descriptor set, as just recorded
			vkCmdBindDescriptorSets( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, Accumulate.pipelineLayout, 0, 1, &Accumulate.descriptorSet, 0, nullptr );

			// get a new wang RNG seed
			Accumulate.pushConstants.wangSeed = genWangSeed();

			// send the current value of the push constants
			vkCmdPushConstants( cmd, Accumulate.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof( PushConstants ), &Accumulate.pushConstants );

			// and the actual compute dispatch for pixels - this is sized for the full buffer
			vkCmdDispatch( cmd, ( ImageBufferResolution.width + 15 ) / 16, ( ImageBufferResolution.height + 15 ) / 16, 1 );

			VkImageMemoryBarrier2 barrierC = makeImageBarrier( XYZImage.image, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT );
			VkDependencyInfo barrierDependency {
				.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
				.imageMemoryBarrierCount = 1,
				.pImageMemoryBarriers = &barrierC
			};

			vkCmdPipelineBarrier2( cmd, &barrierDependency );
		};
	}

	{ // Debug Text Draw
		{ // descriptor layout
			DescriptorLayoutBuilder builder;
			builder.add_binding( 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ); // global config UBO
			builder.add_binding( 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ); // string config UBO

			// font LUTs
			builder.add_binding( 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ); // code page 437
			builder.add_binding( 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ); // fatfont
			builder.add_binding( 4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ); // tinyfont

			DebugStringDraw.descriptorSetLayout = builder.build( device, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT );
			SetDebugName( VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, ( uint64_t ) DebugStringDraw.descriptorSetLayout, "Debug String Draw Descriptor Set Layout" );
		}

		{ // pipeline layout + compute pipeline
			VkPushConstantRange pushConstant{};
			pushConstant.offset = 0;
			pushConstant.size = sizeof( PushConstants );
			pushConstant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

			VkPipelineLayoutCreateInfo computeLayout{};
			computeLayout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
			computeLayout.pNext = nullptr;
			computeLayout.pSetLayouts = &DebugStringDraw.descriptorSetLayout;
			computeLayout.setLayoutCount = 1;
			computeLayout.pPushConstantRanges = &pushConstant;
			computeLayout.pushConstantRangeCount = 1;

			VK_CHECK( vkCreatePipelineLayout( device, &computeLayout, nullptr, &DebugStringDraw.pipelineLayout ) );
			SetDebugName( VK_OBJECT_TYPE_PIPELINE_LAYOUT, ( uint64_t ) DebugStringDraw.pipelineLayout, "Debug String Draw Pipeline Layout" );

			VkShaderModule stringFragShader;
			if ( !vkutil::load_shader_module( "../shaders/debugStringDraw.frag.glsl.spv", device, &stringFragShader ) ) {
				fmt::print( "Error when building the Debug String Draw Fragment shader module\n" );
			}
			SetDebugName( VK_OBJECT_TYPE_SHADER_MODULE, ( uint64_t ) stringFragShader, "Debug String Fragment Shader Module" );

			VkShaderModule stringVertexShader;
			if ( !vkutil::load_shader_module( "../shaders/debugStringDraw.vert.glsl.spv", device, &stringVertexShader ) ) {
				fmt::print( "Error when building the Debug String Draw Vertex shader module\n" );
			}
			SetDebugName( VK_OBJECT_TYPE_SHADER_MODULE, ( uint64_t ) stringVertexShader, "Debug String Vertex Shader Module" );

			PipelineBuilder pipelineBuilder;
			pipelineBuilder._pipelineLayout = DebugStringDraw.pipelineLayout;
			pipelineBuilder.set_shaders( stringVertexShader, stringFragShader );
			pipelineBuilder.set_input_topology( VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST );
			pipelineBuilder.set_polygon_mode( VK_POLYGON_MODE_FILL );
			pipelineBuilder.set_cull_mode( VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE );
			pipelineBuilder.set_multisampling_none();
			pipelineBuilder.disable_blending();
			pipelineBuilder.enable_depthtest( true, VK_COMPARE_OP_GREATER_OR_EQUAL );
			pipelineBuilder.set_color_attachment_format( drawImage.imageFormat );
			pipelineBuilder.set_depth_format( depthImage.imageFormat );
			DebugStringDraw.pipeline = pipelineBuilder.build_pipeline( device );
			SetDebugName( VK_OBJECT_TYPE_PIPELINE, ( uint64_t ) DebugStringDraw.pipeline, "Debug String Raster Pipeline" );

			// cleanup
			vkDestroyShaderModule( device, stringFragShader, nullptr );
			vkDestroyShaderModule( device, stringVertexShader, nullptr );

			// deletors for the pipeline layout + pipeline
			mainDeletionQueue.push_function( [ & ] () {
				vkDestroyDescriptorSetLayout( device, DebugStringDraw.descriptorSetLayout, nullptr );
				vkDestroyPipelineLayout( device, DebugStringDraw.pipelineLayout, nullptr );
				vkDestroyPipeline( device, DebugStringDraw.pipeline, nullptr );
			});
		}

		// invoke() lambda
		DebugStringDraw.invoke = [ & ] ( VkCommandBuffer cmd ) {
			// skip if there are no strings to draw
			if ( debugStrings.size() != 0 ) {
				// dynamic descriptor allocation, to bind a texture
				DebugStringDraw.descriptorSet = getCurrentFrame().frameDescriptors.allocate( device, DebugStringDraw.descriptorSetLayout );
				{
					DescriptorWriter writer;
					writer.write_buffer( 0, GlobalUBO.buffer, sizeof( GlobalData ), 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER );

					// the config UBO
					writer.write_buffer( 1, debugStringConfigBuffer.buffer, 1024 * sizeof( debugStringConfig ), 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER );

					// the font LUTs
					writer.write_image( 2, font_codepage437.imageView, defaultSamplerNearest, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER );
					writer.write_image( 3, font_fatfont.imageView, defaultSamplerNearest, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER );
					writer.write_image( 4, font_tinyfont.imageView, defaultSamplerNearest, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER );

					writer.update_set( device, DebugStringDraw.descriptorSet );
				}

				VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info( drawImage.imageView, nullptr, VK_IMAGE_LAYOUT_GENERAL );
				VkRenderingAttachmentInfo depthAttachment = vkinit::attachment_info( depthImage.imageView, nullptr, VK_IMAGE_LAYOUT_GENERAL );
				VkRenderingInfo renderInfo = vkinit::rendering_info( ImageBufferResolution, &colorAttachment, &depthAttachment );

				vkCmdBeginRendering( cmd, &renderInfo );
				vkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, DebugStringDraw.pipeline );

				//set dynamic viewport and scissor
				VkViewport viewport = {};
				viewport.x = 0;
				viewport.y = 0;
				viewport.width = float( ImageBufferResolution.width * renderScale );
				viewport.height = float( ImageBufferResolution.height * renderScale );
				viewport.minDepth = 0.0f;
				viewport.maxDepth = 1.0f;
				vkCmdSetViewport( cmd, 0, 1, &viewport );

				VkRect2D scissor = {};
				scissor.offset.x = 0;
				scissor.offset.y = 0;
				scissor.extent.width = ImageBufferResolution.width;
				scissor.extent.height = ImageBufferResolution.height;
				vkCmdSetScissor( cmd, 0, 1, &scissor );

				// copy the string configs into the buffer
				debugStringConfig * debugStringConfigGPU = ( debugStringConfig * ) debugStringConfigBuffer.allocation->GetMappedData();

				// copy the data for the strings to the GPU
				memcpy( debugStringConfigGPU, &debugStrings[ 0 ], debugStrings.size() * sizeof( debugStringConfig ) );

				// draw line segments
				vkCmdBindDescriptorSets( cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,  DebugStringDraw.pipelineLayout, 0, 1, &DebugStringDraw.descriptorSet, 0, nullptr );
				vkCmdPushConstants( cmd, DebugStringDraw.pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof( PushConstants ), &DebugStringDraw.pushConstants );

				// launch a draw command to do the fullscreen triangle
				vkCmdDraw( cmd, debugStrings.size() * 6, 1, 0, 0 );
				vkCmdEndRendering( cmd );

				VkImageMemoryBarrier2 barrierC[] = {
					makeImageBarrier( drawImage.image, VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, VK_ACCESS_2_SHADER_READ_BIT ),
					makeImageBarrierD( depthImage.image, VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, VK_ACCESS_2_SHADER_READ_BIT )
				};

				VkDependencyInfo barrierDependency {
					.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
					.imageMemoryBarrierCount = 2,
					.pImageMemoryBarriers = barrierC
				};

				vkCmdPipelineBarrier2( cmd, &barrierDependency );
			}
		};
	}

	{ //debug line drawing layer, need to be able to draw boxes for debugging and for user geometry manipulation widgets
		{ // descriptor layout
			DescriptorLayoutBuilder builder;
			builder.add_binding( 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ); // global config UBO
			builder.add_binding( 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ); // buffer with line information
			DebugLineDraw.descriptorSetLayout = builder.build( device,  VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT );
			SetDebugName( VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, ( uint64_t ) DebugLineDraw.descriptorSetLayout, "Debug Line Raster Descriptor Set Layout" );
		}

		{ // pipeline layout + pipeline build
			VkPushConstantRange pushConstant{};
			pushConstant.offset = 0;
			pushConstant.size = sizeof( PushConstants );
			pushConstant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

			VkPipelineLayoutCreateInfo rasterLayout{};
			rasterLayout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
			rasterLayout.pNext = nullptr;
			rasterLayout.pSetLayouts = &DebugLineDraw.descriptorSetLayout;
			rasterLayout.setLayoutCount = 1;
			rasterLayout.pPushConstantRanges = &pushConstant;
			rasterLayout.pushConstantRangeCount = 1;

			VK_CHECK( vkCreatePipelineLayout( device, &rasterLayout, nullptr, &DebugLineDraw.pipelineLayout ) );
			SetDebugName( VK_OBJECT_TYPE_PIPELINE_LAYOUT, ( uint64_t ) DebugLineDraw.pipelineLayout, "Debug Line Raster Pipeline Layout" );

			VkShaderModule lineFragShader;
			if ( !vkutil::load_shader_module( "../shaders/debugLineDraw.frag.glsl.spv", device, &lineFragShader ) ) {
				fmt::print( "Error when building the Debug Line Draw Fragment shader module\n" );
			}
			SetDebugName( VK_OBJECT_TYPE_SHADER_MODULE, ( uint64_t ) lineFragShader, "Debug Line Fragment Shader Module" );

			VkShaderModule lineVertexShader;
			if ( !vkutil::load_shader_module( "../shaders/debugLineDraw.vert.glsl.spv", device, &lineVertexShader ) ) {
				fmt::print( "Error when building the Debug Line Draw Vertex shader module\n" );
			}
			SetDebugName( VK_OBJECT_TYPE_SHADER_MODULE, ( uint64_t ) lineVertexShader, "Debug Line Vertex Shader Module" );

			PipelineBuilder pipelineBuilder;
			pipelineBuilder._pipelineLayout = DebugLineDraw.pipelineLayout;
			pipelineBuilder.set_shaders( lineVertexShader, lineFragShader );
			pipelineBuilder.set_input_topology( VK_PRIMITIVE_TOPOLOGY_LINE_LIST );
			pipelineBuilder.set_polygon_mode( VK_POLYGON_MODE_FILL );
			pipelineBuilder.set_cull_mode( VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE );
			pipelineBuilder.set_multisampling_none();
			pipelineBuilder.disable_blending();
			pipelineBuilder.enable_depthtest( true, VK_COMPARE_OP_GREATER_OR_EQUAL );
			pipelineBuilder.set_color_attachment_format( drawImage.imageFormat );
			pipelineBuilder.set_depth_format( depthImage.imageFormat );
			DebugLineDraw.pipeline = pipelineBuilder.build_pipeline( device );
			SetDebugName( VK_OBJECT_TYPE_PIPELINE, ( uint64_t ) DebugLineDraw.pipeline, "Debug Line Raster Pipeline" );

			// cleanup
			vkDestroyShaderModule( device, lineFragShader, nullptr );
			vkDestroyShaderModule( device, lineVertexShader, nullptr );

			mainDeletionQueue.push_function( [ & ] ()  {
				vkDestroyDescriptorSetLayout( device, DebugLineDraw.descriptorSetLayout, nullptr );
				vkDestroyPipeline( device, DebugLineDraw.pipeline, nullptr );
				vkDestroyPipelineLayout( device, DebugLineDraw.pipelineLayout, nullptr );
			});
		}

		DebugLineDraw.invoke = [ & ] ( VkCommandBuffer cmd ) {

			// need to update the lines in the buffer
			debugLinePoint* linePointData = ( debugLinePoint * ) debugLineDrawBuffer.allocation->GetMappedData();

			// this needs to show a couple of things:
				// outlines showing the individual bounding boxes of the selected objects...
				// show a glyph for each light, at the light position, indicating direction, width, angle...
				// ...

			{ // mouse position crosshair
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

			VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info( drawImage.imageView, nullptr, VK_IMAGE_LAYOUT_GENERAL );
			VkRenderingAttachmentInfo depthAttachment = vkinit::attachment_info( depthImage.imageView, nullptr, VK_IMAGE_LAYOUT_GENERAL );
			VkRenderingInfo renderInfo = vkinit::rendering_info( ImageBufferResolution, &colorAttachment, &depthAttachment );

			const VkClearDepthStencilValue depthClearValue = { 0.0f, 0 };
			const VkImageSubresourceRange range = {
				.aspectMask =  VK_IMAGE_ASPECT_DEPTH_BIT,
				.baseMipLevel = 0,
				.levelCount = 1,
				.baseArrayLayer = 0,
				.layerCount = 1,
			};
			vkCmdClearDepthStencilImage( cmd, depthImage.image, VK_IMAGE_LAYOUT_GENERAL, &depthClearValue, 1, &range );

			vkCmdBeginRendering( cmd, &renderInfo );
			vkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, DebugLineDraw.pipeline );

			// dynamic descriptor allocation
			DebugLineDraw.descriptorSet = getCurrentFrame().frameDescriptors.allocate( device, DebugLineDraw.descriptorSetLayout );
			{
				DescriptorWriter writer;
				writer.write_buffer( 0, GlobalUBO.buffer, sizeof( GlobalData ), 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER );
				writer.write_buffer( 1, debugLineDrawBuffer.buffer, ( 1 << 16 ) * sizeof( debugLinePoint ), 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER );
				writer.update_set( device, DebugLineDraw.descriptorSet );
			}

			vkCmdBindDescriptorSets( cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, DebugLineDraw.pipelineLayout, 0, 1, &DebugLineDraw.descriptorSet, 0, nullptr );

			//set dynamic viewport and scissor
			VkViewport viewport = {};
			viewport.x = 0;
			viewport.y = 0;
			viewport.width = float( ImageBufferResolution.width * renderScale );
			viewport.height = float( ImageBufferResolution.height * renderScale );
			viewport.minDepth = 0.0f;
			viewport.maxDepth = 1.0f;
			vkCmdSetViewport( cmd, 0, 1, &viewport );

			VkRect2D scissor = {};
			scissor.offset.x = 0;
			scissor.offset.y = 0;
			scissor.extent.width = ImageBufferResolution.width;
			scissor.extent.height = ImageBufferResolution.height;
			vkCmdSetScissor( cmd, 0, 1, &scissor );

			// draw line segments
			vkCmdBindDescriptorSets( cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,  DebugLineDraw.pipelineLayout, 0, 1, &DebugLineDraw.descriptorSet, 0, nullptr );
			vkCmdPushConstants( cmd, DebugLineDraw.pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof( PushConstants ), &DebugLineDraw.pushConstants );

			// launch a draw command to do the fullscreen triangle
			vkCmdDraw( cmd, ( 1 << 16 ), 1, 0, 0 );
			vkCmdEndRendering( cmd );

			VkImageMemoryBarrier2 barrierC[] = {
				makeImageBarrier( drawImage.image, VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, VK_ACCESS_2_SHADER_READ_BIT ),
				makeImageBarrierD( depthImage.image, VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, VK_ACCESS_2_SHADER_READ_BIT )
			};

			VkDependencyInfo barrierDependency {
				.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
				.imageMemoryBarrierCount = 2,
				.pImageMemoryBarriers = barrierC
			};

			vkCmdPipelineBarrier2( cmd, &barrierDependency );
		};
	}

	{ // Present
		{ // descriptor layout
			DescriptorLayoutBuilder builder;
			builder.add_binding( 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ); // global config UBO
			builder.add_binding( 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ); // draw image
			builder.add_binding( 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ); // accumulation Buffer -> linear filter
			BufferPresent.descriptorSetLayout = builder.build( device, VK_SHADER_STAGE_COMPUTE_BIT );
			SetDebugName( VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, ( uint64_t ) BufferPresent.descriptorSetLayout, "Buffer Present Descriptor Set Layout" );
		}

		{ // pipeline layout + compute pipeline
			VkPushConstantRange pushConstant{};
			pushConstant.offset = 0;
			pushConstant.size = sizeof( PushConstants );
			pushConstant.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

			VkPipelineLayoutCreateInfo computeLayout{};
			computeLayout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
			computeLayout.pNext = nullptr;
			computeLayout.pSetLayouts = &BufferPresent.descriptorSetLayout;
			computeLayout.setLayoutCount = 1;
			computeLayout.pPushConstantRanges = &pushConstant;
			computeLayout.pushConstantRangeCount = 1;

			VK_CHECK( vkCreatePipelineLayout( device, &computeLayout, nullptr, &BufferPresent.pipelineLayout ) );
			SetDebugName( VK_OBJECT_TYPE_PIPELINE_LAYOUT, ( uint64_t ) BufferPresent.pipelineLayout, "Buffer Present Pipeline Layout" );

			VkShaderModule BufferPresentShader;
			if ( !vkutil::load_shader_module("../shaders/bufferPresent.comp.glsl.spv", device, &BufferPresentShader ) ) {
				fmt::print( "Error when building the Buffer Present Compute Shader\n" );
			}
			SetDebugName( VK_OBJECT_TYPE_SHADER_MODULE, ( uint64_t ) BufferPresentShader, "Buffer Present Shader Module" );

			VkPipelineShaderStageCreateInfo stageinfo{};
			stageinfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			stageinfo.pNext = nullptr;
			stageinfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
			stageinfo.module = BufferPresentShader;
			stageinfo.pName = "main";

			VkComputePipelineCreateInfo computePipelineCreateInfo{};
			computePipelineCreateInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
			computePipelineCreateInfo.pNext = nullptr;
			computePipelineCreateInfo.layout = BufferPresent.pipelineLayout;
			computePipelineCreateInfo.stage = stageinfo;

			VK_CHECK( vkCreateComputePipelines( device, VK_NULL_HANDLE, 1, &computePipelineCreateInfo, nullptr, &BufferPresent.pipeline ) );
			SetDebugName( VK_OBJECT_TYPE_PIPELINE, ( uint64_t ) BufferPresent.pipeline, "Buffer Present Compute Pipeline" );
			vkDestroyShaderModule( device, BufferPresentShader, nullptr );

			// deletors for the pipeline layout + pipeline
			mainDeletionQueue.push_function( [ & ] () {
				vkDestroyDescriptorSetLayout( device, BufferPresent.descriptorSetLayout, nullptr );
				vkDestroyPipelineLayout( device, BufferPresent.pipelineLayout, nullptr );
				vkDestroyPipeline( device, BufferPresent.pipeline, nullptr );
			});
		}

		// invoke() lambda
		BufferPresent.invoke = [ & ]( VkCommandBuffer cmd ) {
			BufferPresent.descriptorSet = getCurrentFrame().frameDescriptors.allocate( device, BufferPresent.descriptorSetLayout );
			{
				DescriptorWriter writer;
				writer.write_buffer( 0, GlobalUBO.buffer, sizeof( GlobalData ), 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER );
				writer.write_image( 1, drawImage.imageView, defaultSamplerNearest, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE );
				writer.write_image( 2, XYZImage.imageView, defaultSamplerLinear, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER );
				writer.update_set( device, BufferPresent.descriptorSet );
			}

			vkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, BufferPresent.pipeline );

			// bind the descriptor set, as just recorded
			vkCmdBindDescriptorSets( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, BufferPresent.pipelineLayout, 0, 1, &BufferPresent.descriptorSet, 0, nullptr );

			// get a new wang RNG seed
			BufferPresent.pushConstants.wangSeed = genWangSeed();

			// send the current value of the push constants
			vkCmdPushConstants( cmd, BufferPresent.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof( PushConstants ), &BufferPresent.pushConstants );

			// and the actual compute dispatch for the simulation agents
			vkCmdDispatch( cmd, ( drawExtent.width + 15 ) / 16, ( drawExtent.height + 15 ) / 16, 1 );
		};
	}

	{ // BBox precompute
		{ // descriptor layout
			DescriptorLayoutBuilder builder;
			builder.add_binding( 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ); // global config UBO
			builder.add_binding( 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ); // geometry buffer
			builder.add_binding( 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ); // bbox buffer
			BBoxPrecompute.descriptorSetLayout = builder.build( device, VK_SHADER_STAGE_COMPUTE_BIT );
			SetDebugName( VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, ( uint64_t ) BBoxPrecompute.descriptorSetLayout, "BBox Precompute Descriptor Set Layout" );
		}

		{ // pipeline layout + compute pipeline
			VkPushConstantRange pushConstant{};
			pushConstant.offset = 0;
			pushConstant.size = sizeof( PushConstants );
			pushConstant.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

			VkPipelineLayoutCreateInfo computeLayout{};
			computeLayout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
			computeLayout.pNext = nullptr;
			computeLayout.pSetLayouts = &BBoxPrecompute.descriptorSetLayout;
			computeLayout.setLayoutCount = 1;
			computeLayout.pPushConstantRanges = &pushConstant;
			computeLayout.pushConstantRangeCount = 1;

			VK_CHECK( vkCreatePipelineLayout( device, &computeLayout, nullptr, &BBoxPrecompute.pipelineLayout ) );
			SetDebugName( VK_OBJECT_TYPE_PIPELINE_LAYOUT, ( uint64_t ) BBoxPrecompute.pipelineLayout, "BBox Precompute Pipeline Layout" );

			VkShaderModule bboxShader;
			if ( !vkutil::load_shader_module("../shaders/gridPrecomputeBBox.comp.glsl.spv", device, &bboxShader ) ) {
				fmt::print( "Error when building the BBox Precompute Compute Shader\n" );
			}
			SetDebugName( VK_OBJECT_TYPE_SHADER_MODULE, ( uint64_t ) bboxShader, "BBox Precompute Shader Module" );

			VkPipelineShaderStageCreateInfo stageinfo{};
			stageinfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			stageinfo.pNext = nullptr;
			stageinfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
			stageinfo.module = bboxShader;
			stageinfo.pName = "main";

			VkComputePipelineCreateInfo computePipelineCreateInfo{};
			computePipelineCreateInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
			computePipelineCreateInfo.pNext = nullptr;
			computePipelineCreateInfo.layout = BBoxPrecompute.pipelineLayout;
			computePipelineCreateInfo.stage = stageinfo;

			VK_CHECK( vkCreateComputePipelines( device, VK_NULL_HANDLE, 1, &computePipelineCreateInfo, nullptr, &BBoxPrecompute.pipeline ) );
			SetDebugName( VK_OBJECT_TYPE_PIPELINE, ( uint64_t ) BBoxPrecompute.pipeline, "BBox Precompute Compute Pipeline" );
			vkDestroyShaderModule( device, bboxShader, nullptr );

			// deletors for the pipeline layout + pipeline
			mainDeletionQueue.push_function( [ & ] () {
				vkDestroyDescriptorSetLayout( device, BBoxPrecompute.descriptorSetLayout, nullptr );
				vkDestroyPipelineLayout( device, BBoxPrecompute.pipelineLayout, nullptr );
				vkDestroyPipeline( device, BBoxPrecompute.pipeline, nullptr );
			});
		}

		// invoke() lambda
		BBoxPrecompute.invoke = [ & ] ( VkCommandBuffer cmd ){
			// dynamic descriptor allocation, to bind a texture
			BBoxPrecompute.descriptorSet = getCurrentFrame().frameDescriptors.allocate( device, BBoxPrecompute.descriptorSetLayout );
			{
				DescriptorWriter writer;
				writer.write_buffer( 0, GlobalUBO.buffer, sizeof( GlobalData ), 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER );
				writer.write_buffer( 1, GeometryBuffer.buffer, VK_WHOLE_SIZE, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER );
				writer.write_buffer( 2, BBoxBuffer.buffer, VK_WHOLE_SIZE, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER );
				writer.update_set( device, BBoxPrecompute.descriptorSet );
			}

			vkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, BBoxPrecompute.pipeline );

			// bind the descriptor set, as just recorded
			vkCmdBindDescriptorSets( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, BBoxPrecompute.pipelineLayout, 0, 1, &BBoxPrecompute.descriptorSet, 0, nullptr );

			// get a new wang RNG seed
			BBoxPrecompute.pushConstants.wangSeed = genWangSeed();

			// send the current value of the push constants
			vkCmdPushConstants( cmd, BBoxPrecompute.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof( PushConstants ), &BBoxPrecompute.pushConstants );

			// dispatch for all primitives
			vkCmdDispatch( cmd, ( globalData.numPrimitives + 63 ) / 64, 1, 1 );

			VkBufferMemoryBarrier2 bufferBarrier = makeBufferBarrier( BBoxBuffer.buffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT );
			VkDependencyInfo barrierDependency {
				.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
				.bufferMemoryBarrierCount = 1,
				.pBufferMemoryBarriers = &bufferBarrier,
			};

			vkCmdPipelineBarrier2( cmd, &barrierDependency );
		};
	}

	{ // Grid eval precompute
		{ // descriptor layout
			DescriptorLayoutBuilder builder;
			builder.add_binding( 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ); // global config UBO
			builder.add_binding( 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ); // geometry buffer
			builder.add_binding( 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ); // bbox buffer
			builder.add_binding( 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ); // uncompacted grid buffer
			UncompactedGridPrecompute.descriptorSetLayout = builder.build( device, VK_SHADER_STAGE_COMPUTE_BIT );
			SetDebugName( VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, ( uint64_t ) UncompactedGridPrecompute.descriptorSetLayout, "Uncompacted Grid Precompute Descriptor Set Layout" );
		}

		{ // pipeline layout + compute pipeline
			VkPushConstantRange pushConstant{};
			pushConstant.offset = 0;
			pushConstant.size = sizeof( PushConstants );
			pushConstant.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

			VkPipelineLayoutCreateInfo computeLayout{};
			computeLayout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
			computeLayout.pNext = nullptr;
			computeLayout.pSetLayouts = &UncompactedGridPrecompute.descriptorSetLayout;
			computeLayout.setLayoutCount = 1;
			computeLayout.pPushConstantRanges = &pushConstant;
			computeLayout.pushConstantRangeCount = 1;

			VK_CHECK( vkCreatePipelineLayout( device, &computeLayout, nullptr, &UncompactedGridPrecompute.pipelineLayout ) );
			SetDebugName( VK_OBJECT_TYPE_PIPELINE_LAYOUT, ( uint64_t ) UncompactedGridPrecompute.pipelineLayout, "Uncompacted Grid Precompute Pipeline Layout" );

			VkShaderModule gridShader;
			if ( !vkutil::load_shader_module("../shaders/gridPrecomputePrimitive.comp.glsl.spv", device, &gridShader ) ) {
				fmt::print( "Error when building the Uncompacted Grid Precompute Compute Shader\n" );
			}
			SetDebugName( VK_OBJECT_TYPE_SHADER_MODULE, ( uint64_t ) gridShader, "Uncompacted Grid Precompute Shader Module" );

			VkPipelineShaderStageCreateInfo stageinfo{};
			stageinfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			stageinfo.pNext = nullptr;
			stageinfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
			stageinfo.module = gridShader;
			stageinfo.pName = "main";

			VkComputePipelineCreateInfo computePipelineCreateInfo{};
			computePipelineCreateInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
			computePipelineCreateInfo.pNext = nullptr;
			computePipelineCreateInfo.layout = UncompactedGridPrecompute.pipelineLayout;
			computePipelineCreateInfo.stage = stageinfo;

			VK_CHECK( vkCreateComputePipelines( device, VK_NULL_HANDLE, 1, &computePipelineCreateInfo, nullptr, &UncompactedGridPrecompute.pipeline ) );
			SetDebugName( VK_OBJECT_TYPE_PIPELINE, ( uint64_t ) UncompactedGridPrecompute.pipeline, "Uncompacted Grid Precompute Compute Pipeline" );
			vkDestroyShaderModule( device, gridShader, nullptr );

			// deletors for the pipeline layout + pipeline
			mainDeletionQueue.push_function( [ & ] () {
				vkDestroyDescriptorSetLayout( device, UncompactedGridPrecompute.descriptorSetLayout, nullptr );
				vkDestroyPipelineLayout( device, UncompactedGridPrecompute.pipelineLayout, nullptr );
				vkDestroyPipeline( device, UncompactedGridPrecompute.pipeline, nullptr );
			});
		}

		// invoke() lambda
		UncompactedGridPrecompute.invoke = [ & ] ( VkCommandBuffer cmd ){
			// dynamic descriptor allocation, to bind a texture
			UncompactedGridPrecompute.descriptorSet = getCurrentFrame().frameDescriptors.allocate( device, UncompactedGridPrecompute.descriptorSetLayout );
			{
				DescriptorWriter writer;
				writer.write_buffer( 0, GlobalUBO.buffer, sizeof( GlobalData ), 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER );
				writer.write_buffer( 1, GeometryBuffer.buffer, VK_WHOLE_SIZE, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER );
				writer.write_buffer( 2, BBoxBuffer.buffer, VK_WHOLE_SIZE, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER );
				writer.write_buffer( 3, UncompactedGridBuffer.buffer, VK_WHOLE_SIZE, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER );
				writer.update_set( device, UncompactedGridPrecompute.descriptorSet );
			}

			vkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, UncompactedGridPrecompute.pipeline );

			// bind the descriptor set, as just recorded
			vkCmdBindDescriptorSets( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, UncompactedGridPrecompute.pipelineLayout, 0, 1, &UncompactedGridPrecompute.descriptorSet, 0, nullptr );

			// get a new wang RNG seed
			UncompactedGridPrecompute.pushConstants.wangSeed = genWangSeed();

			// send the current value of the push constants
			vkCmdPushConstants( cmd, UncompactedGridPrecompute.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof( PushConstants ), &UncompactedGridPrecompute.pushConstants );

			// dispatch for all primitives
			vkCmdDispatch( cmd,( globalData.gridDims.x + 7 ) / 8 ,  ( globalData.gridDims.y + 7 ) / 8, 1 );

			VkBufferMemoryBarrier2 bufferBarrier = makeBufferBarrier( UncompactedGridBuffer.buffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT );
			VkDependencyInfo barrierDependency {
				.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
				.bufferMemoryBarrierCount = 1,
				.pBufferMemoryBarriers = &bufferBarrier,
			};

			vkCmdPipelineBarrier2( cmd, &barrierDependency );
		};
	}
}

void PrometheusInstance::bufferRebuildGPU () {
	// and two pipelines which exist only for this
	static bool initialized = false;

	unscopedTimer t ( "timer", true );

	// the geometry list now lives on the GPU, so we are able to skip the initial upload step

	{ // 1: immediate submit for GPU precompute
		t.tick();

		immediateSubmit( [ & ] ( VkCommandBuffer cmd ) {
			// dispatch bbox precompute for each primitive
			BBoxPrecompute.invoke( cmd );

			// dispatch grid eval for each grid cell
			UncompactedGridPrecompute.invoke( cmd );
		});

		t.tock();
		fmt::print( "precompute stage took {}ms\n", std::chrono::duration_cast< std::chrono::microseconds >( t.c.tStop - t.c.tStart ).count() / 1000.0f );
	}

// all this might be able to be skipped... it would mean using an uncompacted buffer during traversal, and I need to figure out if that's good or bad for perf

	std::vector < uint32_t > prefixValues;
	std::vector < uint32_t > gridValues;

	// allocating sufficient memory to store the
	const int numCells = globalData.gridDims.x * globalData.gridDims.y;
	prefixValues.resize( numCells * 2, 0 );
	gridValues.resize( numCells * 16, 0 );

	int gidx = 0;

	{ // 2: stepping through the mapped buffer by 16's (we only support up to 16 primitives per grid cell)
		t.tick();

		// UncompactedGridBuffer has the data, as prepared by the prior stage
			// stepping through by grid cells... 16 floats per
		int32_t * gridBuff = ( int32_t * ) UncompactedGridBuffer.allocation->GetMappedData();
		for ( int i = 0; i < numCells; ++i ) {
			int cellCount = gridBuff[ i * 16 ];

			// if ( cellCount != 0 )
				// fmt::print( "cell {} contains {} primitives", i, cellCount );

			// copy cell contents to the compacted buffer
			for ( int j = 0; j < cellCount; j++ ) {
				gridValues[ gidx ] = gridBuff[ i * 16 + j + 1 ];
				gidx++;
			}

			prefixValues[ 2 * i + 0 ] = gidx; // index
			prefixValues[ 2 * i + 1 ] = cellCount; // count
		}

		t.tock();
		fmt::print( "buffer process stage took {}ms\n", std::chrono::duration_cast< std::chrono::microseconds >( t.c.tStop - t.c.tStart ).count() / 1000.0f );
	}

	{ // 3: upload the buffers used by the runtime traversal
		t.tick();

		// create the buffers, with the current contents...
		size_t gbSize = gidx * sizeof( int32_t );
		GridBuffer		= createBuffer( gbSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO );
		SetDebugName( VK_OBJECT_TYPE_BUFFER, ( uint64_t ) GridBuffer.buffer, "BVH Grid Buffer" );

		size_t pbSize = numCells * 2 * sizeof( int32_t );
		PrefixBuffer	= createBuffer( pbSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO );
		SetDebugName( VK_OBJECT_TYPE_BUFFER, ( uint64_t ) PrefixBuffer.buffer, "BVH Prefix Buffer" );

		// transferring prepped data to the new buffers
		memcpy( GridBuffer.info.pMappedData, gridValues.data(), gbSize );
		memcpy( PrefixBuffer.info.pMappedData, prefixValues.data(), pbSize );

		t.tock();
		fmt::print( "final buffer upload stage took {}ms\n", std::chrono::duration_cast< std::chrono::microseconds >( t.c.tStop - t.c.tStart ).count() / 1000.0f );
	}

	// and we have latest data on the GPU
	geometryListDirty = false;
}

// adding primitives to the geometry list
void PrometheusInstance::addSegment ( vec2 a, vec2 b, int material, bool invert ) {
// segment mapping:
	// 0: a.x
	// 1: a.y
	// 2: b.x
	// 3: b.y
	// 4-12: unused
	// 13: material ID
	// 14: invert flag
	// 15: 0 -> line segment

	geometryStruct * geoData = ( geometryStruct * ) GeometryBuffer.allocation->GetMappedData();
	if ( globalData.numPrimitives < globalData.maxPrimitives ) {
		geoData[ globalData.numPrimitives ].values[ 0 ] = a.x;
		geoData[ globalData.numPrimitives ].values[ 1 ] = a.y;
		geoData[ globalData.numPrimitives ].values[ 2 ] = b.x;
		geoData[ globalData.numPrimitives ].values[ 3 ] = b.y;

		geoData[ globalData.numPrimitives ].values[ 13 ] = material;
		geoData[ globalData.numPrimitives ].values[ 14 ] = invert ? 1.0f : 0.0f;
		geoData[ globalData.numPrimitives ].values[ 15 ] = 0; // line segment identifier

		globalData.numPrimitives++;
	}
}

void PrometheusInstance::addArc ( vec2 center, float radius, float thetaStart, float thetaEnd, int material, bool invert ) {
// arc mapping:
	// 0: center.x
	// 1: center.y
	// 2: radius
	// 3: thetaMin
	// 4: thetaMax
	// 5-12: unused
	// 13: material ID
	// 14: invert flag
	// 15: 1 -> circular arc

	geometryStruct * geoData = ( geometryStruct * ) GeometryBuffer.allocation->GetMappedData();
	if ( globalData.numPrimitives < globalData.maxPrimitives ) {
		geoData[ globalData.numPrimitives ].values[ 0 ] = center.x;
		geoData[ globalData.numPrimitives ].values[ 1 ] = center.y;
		geoData[ globalData.numPrimitives ].values[ 2 ] = radius;

		float thetaMin = std::clamp( std::min( thetaStart, thetaEnd ), 0.0f, pi * 2.0f );
		float thetaMax = std::clamp( std::max( thetaStart, thetaEnd ), 0.0f, pi * 2.0f );
		geoData[ globalData.numPrimitives ].values[ 3 ] = thetaMin;
		geoData[ globalData.numPrimitives ].values[ 4 ] = thetaMax;

		geoData[ globalData.numPrimitives ].values[ 13 ] = material;
		geoData[ globalData.numPrimitives ].values[ 14 ] = invert ? 1.0f : 0.0f;
		geoData[ globalData.numPrimitives ].values[ 15 ] = 1; // ARC identifier

		globalData.numPrimitives++;
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

// 2D line segment
int PrometheusInstance::addDebugDrawLine ( vec2 a, vec2 b, vec3 color, float zDepthA, float zDepthB ) {
	// need to update the buffer with the new line
	debugLinePoint* linePointData = ( debugLinePoint * ) debugLineDrawBuffer.allocation->GetMappedData();

	linePointData[ debugLineDrawNumLines + 0 ].position = vec4( a, zDepthA, 1.0f );
	linePointData[ debugLineDrawNumLines + 0 ].color = vec4( color, 1.0f );
	linePointData[ debugLineDrawNumLines + 1 ].position = vec4( b, zDepthB, 1.0f );
	linePointData[ debugLineDrawNumLines + 1 ].color = vec4( color, 1.0f );

	debugLineDrawNumLines += 2;
	return debugLineDrawNumLines;
}

// 2D bounding box helper, draws 4 lines
int PrometheusInstance::addDebugDrawBox ( vec2 min, vec2 max, vec3 color, float zDepth ) {
	addDebugDrawLine( min, vec2( min.x, max.y ), color, zDepth, zDepth );
	addDebugDrawLine( min, vec2( max.x, min.y ), color, zDepth, zDepth );
	addDebugDrawLine( max, vec2( min.x, max.y ), color, zDepth, zDepth );
	addDebugDrawLine( max, vec2( max.x, min.y ), color, zDepth, zDepth );

	return debugLineDrawNumLines;
}

void PrometheusInstance::lightManagerMaintenance () {
	// three resources need to be kept up:
		// spectral sampling IS
		// light pick IS
		// light parameters buffer

	static bool firstTime = true;

	static int lastSeenNumLights = 0;
	uint8_t numLights = lightManager.lights.size() + 1;

	// if we see a change in the light list, we need to rebuild
	if ( lastSeenNumLights != numLights ) {
		if ( !firstTime ) {
			// delete the existing textures
			destroyImage( PreviewAtlas );
			destroyImage( SpectrumISImage );
			destroyImage( PickISImage );
		}
		// create the new textures at current sizes
		PreviewAtlas = createImage( { 554, 64u * numLights, 1 }, VK_FORMAT_R8G8B8A8_UNORM,  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT );
		SetDebugName( VK_OBJECT_TYPE_IMAGE, ( uint64_t ) PreviewAtlas.image, "Preview Atlas" );

		SpectrumISImage = createImage( { 1024, numLights, 1 }, VK_FORMAT_R32_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT );
		SetDebugName( VK_OBJECT_TYPE_IMAGE, ( uint64_t ) SpectrumISImage.image, "Spectral IS Texture" );

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
	emitterParams[ 0 ] = lightManager.MouseLight->parameters;
	for ( int i = 0; i < lightManager.lights.size(); i++ ) {
		emitterParams[ i + 1 ] = lightManager.lights[ i ].parameters;
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
	std::string filenameS = std::string( timeDateString() + ".png" );
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
	vkDestroyImageView( device, img.imageView, nullptr );
	vmaDestroyImage( allocator, img.image, img.allocation );
}

void PrometheusInstance::initDefaultData () {

	YAML::Node config = YAML::LoadFile( "../src/presets.yaml" );
	size_t numEntries = config.size();
	for ( size_t i = 0; i < numEntries; i++ ) {
		presets.push_back( config[ i ].as< uint32_t >() );
	}

// TEXTURES
	// 3 default textures, white, grey, black. 1 pixel each
	uint32_t white = glm::packUnorm4x8( glm::vec4( 1.0f, 1.0f, 1.0f, 1.0f ) );
	whiteImage = createImage( ( void * ) &white, VkExtent3D{ 1, 1, 1 }, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT );

	uint32_t grey = glm::packUnorm4x8(glm::vec4( 0.66f, 0.66f, 0.66f, 1 ) );
	greyImage = createImage( ( void * ) &grey, VkExtent3D{ 1, 1, 1 }, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT );

	uint32_t black = glm::packUnorm4x8(glm::vec4(0, 0, 0, 0 ) );
	blackImage = createImage( ( void * ) &black, VkExtent3D{ 1, 1, 1 }, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT );

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
