#pragma once

#include <vk_types.h>
#include <unordered_map>
#include <filesystem>

struct GeoSurface {
	uint32_t startIndex;
	uint32_t count;
};

//forward declaration
class PrometheusInstance;