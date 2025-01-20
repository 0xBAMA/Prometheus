#pragma once

#include <iostream>
#include <chrono>
#include <thread>

#include <vk_types.h>

class PrometheusInstance {
public:
	bool isInitialized{ false };
	int frameNumber{ 0 };
	bool stopRendering{ false };

	VkExtent2D windowExtent{ 1700 , 900 };
	struct SDL_Window* window{ nullptr };
	static PrometheusInstance& Get();

	void Init();
	void Draw();
	void MainLoop();
	void ShutDown();
};