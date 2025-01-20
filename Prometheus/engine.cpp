#include "engine.h"

#include <SDL.h>
#include <SDL_vulkan.h>

#include <vk_initializers.h>
#include <vk_types.h>

// global access to the current engine
PrometheusInstance* currentInstance = nullptr;
PrometheusInstance& PrometheusInstance::Get() { return *currentInstance; }

void PrometheusInstance::Init() {
	// only one initialization allowed
	assert( currentInstance == nullptr );
	currentInstance = this;

	// initializing SDL
	SDL_Init( SDL_INIT_VIDEO );
	SDL_WindowFlags windowFlags = ( SDL_WindowFlags ) ( SDL_WINDOW_VULKAN );

	window = SDL_CreateWindow(
		"Vulkan Engine",
		SDL_WINDOWPOS_UNDEFINED,
		SDL_WINDOWPOS_UNDEFINED,
		windowExtent.width,
		windowExtent.height,
		windowFlags );

	// everything went fine
	isInitialized = true;
}

void PrometheusInstance::Draw() {
	// currently no op
}

void PrometheusInstance::MainLoop() {
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
			continue;
		}

		Draw();
	}

}

void PrometheusInstance::ShutDown() {
	if ( isInitialized ) {
		SDL_DestroyWindow( window );
	}

	// clear engine pointer
	currentInstance = nullptr;
}