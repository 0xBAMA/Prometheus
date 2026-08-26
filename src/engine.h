#pragma once

#include <iostream>
#include <chrono>
#include <thread>
#include <random>

#include <vk_types.h>
#include <vk_descriptors.h>
#include <vk_pipelines.h>
#include <vk_loader.h>

#include "lightManager.h"
#include "timer.h"

#ifndef PI_DEFINED
#define PI_DEFINED
const float pi = 3.14159265358979323846f;
const float tau = 2.0f * pi;
const float sqrtpi = 1.7724538509f;
#endif

struct BLASCreateInfo {
	VkGeometryFlagsKHR geometryFlags = {};
	VkBuildAccelerationStructureFlagsKHR buildFlags = {};

	VkFormat vertexFormat        = VK_FORMAT_R32G32B32_SFLOAT;
	VkDeviceAddress vertexBuffer = 0;
	VkDeviceAddress indexBuffer  = 0;
	VkDeviceSize vertexStride    = 0;
	uint32_t numVertices         = 0;
	VkIndexType indexType        = VK_INDEX_TYPE_UINT32;
	uint32_t numIndices          = 0;
};

struct BLASRecord {
	// Vulkan API stuff
	VkAccelerationStructureKHR AShandle;
	AllocatedBuffer ASBuffer;

	// Address of the acceleration structure
	VkDeviceSize address_;

	// record of parameters used to create this BLAS
	BLASCreateInfo createInfo;
};

struct TLASInstance {
	VkTransformMatrixKHR transform = {};
	uint32_t instanceCustomIndex : 24 = 0;
	uint32_t mask : 8 = 0;
	uint32_t shaderBindingTableOffset : 24 = 0;
	VkGeometryInstanceFlagBitsKHR flags : 8 = {};
	VkDeviceAddress BLASAddress = 0;
};
static_assert(sizeof(TLASInstance) == sizeof(VkAccelerationStructureInstanceKHR));

struct TLASCreateInfo {
	VkGeometryFlagsKHR geometryFlags = {};
	VkBuildAccelerationStructureFlagsKHR buildFlags = {};

	AllocatedBuffer instanceBuffer;
};

struct TLASRecord {
	VkAccelerationStructureKHR handle;
	// Buffer holding the actual AS data
	AllocatedBuffer buffer;
	// Address of the acceleration structure
	VkDeviceSize address;
	// std::optional<DescriptorInfo> descriptorInfo_;

	TLASCreateInfo createInfo;
};

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
	VkFence renderFence;

	// command buffer + allocator
	VkCommandPool commandPool;
	VkCommandBuffer mainCommandBuffer;

	// handling frame-local resources
	DeletionQueue deletionQueue;

	// descriptor pool management
	DescriptorAllocatorGrowable frameDescriptors;

	// timestamp query pool
	VkQueryPool queryPools;
};

// this is the CPU side representation... GPU just gets 16 floats
struct geometryStruct {
	float values[ 16 ] = { 0.0f }; // tbd how this is going to be accessed

	// 16 floats generatlizes across the primitives needed:
		// line segment
		// circular arc
		// parabola
		// ...

	// this could operate as a per-primitive dirty flag, so that they can be removed from the grid prior to re-splatting (if grid is maintained)
	// bool touchedSinceLastUpdate = true;
};

// common configuration across all shaders
struct GlobalData {
	glm::uvec2 floatBufferResolution;
	glm::uvec2 presentBufferResolution;

	glm::vec4 mouseLoc;

	int numRays{ 64 * 100 };
	int numBounces{ 256 };

	int frameNumber{ 0 };
	int reset{ 0 };
	int framesSinceReset{ 0 };

	float brightnessScalar{ 1.0f };
	float resolutionScalar{ 1.0f };

	// for the BVH
	float gridScalar = 1.0f; // how big to make the grid?
	glm::ivec2 gridDims;

	// for mapping into the geometry buffer
	uint32_t numPrimitives{ 0 };		// setting where the pointer into the primitive buffer is
	uint32_t maxPrimitives{ 10000000 };	// setting the max primitive count (100k is 5x more than I've done so far... aka "plenty")
};

// smallest scope CPU->GPU passing of information
struct PushConstants {
	uint32_t wangSeed;
};

struct raySegment {
	float wavelength;
	float brightness;
	glm::vec2 a;	// first point
	glm::vec2 b;	// second point
};

struct debugLinePoint {
	vec4 position; // xy has position
	vec4 color;
};

struct debugStringConfig {
	// writing a string of N characters to the image
	vec2 debugStringWriteLocation;
	vec4 debugStringFillColor;
	vec4 debugStringBackgroundColor;
	uint32_t debugStringFontPick;
	uint32_t debugStringLength;
	float debugStringDepth;
	uint8_t debugStringData[ 1024 ];
};

constexpr unsigned int FRAME_OVERLAP = 2;
constexpr bool useValidationLayers = true;

struct ComputeEffect {
	// pipeline is the thing we use to invoke this shader pass
	VkPipeline pipeline;

	// pipeline layout gives us what we need for sending push constants and buffer attachments
	VkPipelineLayout pipelineLayout;

	// this is the descriptor set layout for this particular compute effect (UBO + any SSBOs + any images/textures)
	VkDescriptorSetLayout descriptorSetLayout;
	VkDescriptorSet descriptorSet;

	// retained state for the push constants
	PushConstants pushConstants;

	// so we can have the main loop code local to the declaration
	std::function< void( VkCommandBuffer cmd ) > invoke;
};

inline uint32_t genWangSeed () {
	static thread_local std::mt19937 seedRNG( [] {
	// RNG ( mostly for generating GPU-side RNG seed)
		std::random_device rd;
		std::seed_seq seq{  rd(), rd(), rd(), rd(), rd(), rd(), rd(), rd() };
		return std::mt19937( seq );
	} () );

	// float x = std::uniform_real_distribution< float >( min, max )( seedRNG );
	return std::uniform_int_distribution< uint32_t >{}( seedRNG );
}

class PrometheusInstance {
public:
	// RT state
	TLASRecord mainTLAS;
	AllocatedBuffer IBObuffer;
	AllocatedBuffer VBObuffer;

	std::vector< BLASRecord > BLASRecords;
	void addBLAS ( BLASCreateInfo createInfo, string name );

	// testing the hardware RT setup
	ComputeEffect HRTTest;

	// profiling data
	timerManager_t timer;
	int timestampPeriod;

	bool showMenu = true;

	// for saving scene configs
	char currentExportFilename[ 256 ]{ "filename" };

	uint32_t lastPreset;
	std::vector< uint32_t > presets;

// data/storage resources
	AllocatedBuffer GlobalUBO;
	GlobalData globalData; // goes into the UBO

	// the simulation buffer resolution
	VkExtent2D ImageBufferResolution;
	AllocatedImage XYZImage;

	// ray state
	AllocatedBuffer rayBuffer;

	// debug line state
	AllocatedBuffer debugLineDrawBuffer;
	int debugLineDrawNumLines{ 8 }; // start with scratch for mouse crosshair

	// debug string state -> user can keep index and overwrite the string scratch memory at runtime
	std::vector< debugStringConfig > debugStrings;
	AllocatedBuffer debugStringConfigBuffer; // the string config structs get copied to this buffer

	// using reverse Z, GREATER_OR_EQUAL depth compare op
	static constexpr float debugDrawMaxDepth{ 0.0f }; // farthest from screen
	static constexpr float debugDrawMinDepth{ 1.0f }; // closest to screen
	static constexpr float debugDrawMidDepth{ 0.5f }; // midpoint

	// text rendering, with pixel location + select from the list of available font LUTs (tinyfont, fatfont, code page 437)
	int addDebugString ( vec2 position, std::string displayText, vec3 color, int fontSelect, float zDepth = debugDrawMidDepth );

	// 2D line segment
	int addDebugDrawLine ( vec2 a, vec2 b, vec3 color, float zDepthA = debugDrawMidDepth, float zDepthB = debugDrawMidDepth );

	// 2D bounding box helper, draws 4 lines
	int addDebugDrawBox ( vec2 min, vec2 max, vec3 color, float zDepth = debugDrawMidDepth );

	// main compute shaders
	ComputeEffect Raytrace;
	ComputeEffect Accumulate;
	ComputeEffect DebugLineDraw;
	ComputeEffect DebugStringDraw;

	// for precomputing the grid acceleration structure on the GPU
	ComputeEffect BBoxPrecompute;
	ComputeEffect UncompactedGridPrecompute;

	// there are three buffers used for the grid AS at runtime ( prefix, grid, geometry )
		// and two that are used for precomputation ( bbox, grid precompute )
	bool geometryListDirty = true;		// triggering the rebuild of GPU structures

	AllocatedBuffer PrefixBuffer;		// containing the prefix sums + counts for indexing the grid buffer
	AllocatedBuffer GridBuffer;			// containing a packed list of each cell's contents (requires prefix buffer to operate)
	AllocatedBuffer GeometryBuffer;		// containing the 16-float representations of the geometry
	AllocatedBuffer BBoxBuffer;			// containing the scratch memory for the primitives' bounding boxes
	AllocatedBuffer UncompactedGridBuffer; // scratch memory for GPU precomputation of grid

	// font LUT textures
	AllocatedImage font_codepage437;
	AllocatedImage font_fatfont;
	AllocatedImage font_tinyfont;
	AllocatedImage depthImageCache; // the raster depth is copied here for the text to use

	// eventually these also need a material
	void addSegment ( vec2 a, vec2 b, int material, bool invert = false );
	void addArc ( vec2 center, float radius, float thetaStart, float thetaEnd, int material, bool invert = false );
	// void addParabola ( vec2 center, );

	// there is really only one function associated with this, which manages the buffer rebuild
	void bufferRebuildGPU (); // gotta go fast

	// as far as editing features, this will require:
		// mode select between geometry and lights
		// geometry selection + drag boxes + multiselect with control
		// escape to deselect all
		// so this keeps a list of selected geometry, which is just keeping integer indexes into geometryList, above
	// operations on the selected geometry is still tbd, but will include things like translating on x and y, drawing a gizmo with the debug lines

	// putting the image on the screen
	ComputeEffect BufferPresent;

	// abusing the ComputeEffect struct for a raster pipeline
	AllocatedImage lineColorAttachment;
	ComputeEffect lineRaster;

	// light manager
	LightManager lightManager;

	// Textures for the light scheme
	AllocatedImage PreviewAtlas;	// keeps all the spectrum + xrite chip previews, imgui::Image can specify min and max UVs to show
	AllocatedImage SpectrumISImage;	// keeps the iCDFs of the light emission spectra - this is indexed the same as the emitters, max 256
	AllocatedImage PickISImage;		// keeps the uint8 indices of the lights. Normalized random sampling, nearest filter, to pick - presence is weighted by brightness
		// 0 is mouse, 1-255 are custom user lights as configured in the menu -> this is a nice limit, for what we're doing here

	// gathered up parameters from the list of lights
	AllocatedBuffer LightParametersBuffer; // uses the same indexing as the pick importance sampling + spectrum importance sampling list

	// main loop gather function, updates textures + buffer
	void lightManagerMaintenance();

	// engine triggers
	bool resizeRequest { false };
	bool isInitialized { false };
	bool stopRendering { false };
	int frameNumber { 0 };

	// resources related to the BVH

	void initDefaultData ();
	// for buffer setup
	AllocatedBuffer createBuffer( size_t allocSize, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage, string label = "" );
	void destroyBuffer( const AllocatedBuffer& buffer );

	// basic Vulkan necessities, environmental handles
	VkInstance instance;						// Vulkan library handle
	VkDebugUtilsMessengerEXT debugMessenger;	// debug output messenger
	VkPhysicalDevice physicalDevice;			// GPU handle for the physical device in use
	VkDevice device;							// the abstract device that we interact with
	VkSurfaceKHR surface;						// the Vulkan window surface

	// an image to draw into and eventually pass to the swapchain
	AllocatedImage drawImage;
	AllocatedImage depthImage;
	VkExtent2D drawExtent;
	float renderScale = 1.0f;

	// some helper functions for allocating textures
	AllocatedImage createImage ( VkExtent3D size, VkFormat format, VkImageUsageFlags usage, bool mipmapped = false ); // storage image type
	AllocatedImage createImage ( void* data, VkExtent3D size, VkFormat format, VkImageUsageFlags usage, bool mipmapped = false ); // loaded from disk
	void updateImage ( AllocatedImage& image, void* data, int bytesPerTexel );
	void screenshot (); // save the contents of the framebuffer
	void destroyImage ( const AllocatedImage& img );

	// and some default textures
	AllocatedImage whiteImage;
	AllocatedImage blackImage;
	AllocatedImage greyImage;

	// and default sampler types
	VkSampler defaultSamplerLinear;
	VkSampler defaultSamplerNearest;

	// our frameData struct, which contains command pool/buffer + sync primitive handles
	frameData_t frameData[ FRAME_OVERLAP ];
	frameData_t& getCurrentFrame () { return frameData[ frameNumber % FRAME_OVERLAP ]; }

	VkFence immediateFence;
	VkCommandBuffer immediateCommandBuffer;
	VkCommandPool immediateCommandPool;
	void immediateSubmit( std::function< void( VkCommandBuffer cmd ) > && function );

	DescriptorAllocatorGrowable globalDescriptorAllocator;

	VkDescriptorSet drawImageDescriptors;
	VkDescriptorSetLayout drawImageDescriptorLayout;

	// the queue that we submit work to
	VkQueue graphicsQueue;
	uint32_t graphicsQueueFamilyIndex;

	// window size, swapchain size
	VkExtent2D windowExtent { 0,0 };
	VkExtent2D swapchainExtent;

	// swapchain handles
	VkSwapchainKHR swapchain;
	VkFormat swapchainImageFormat;
	std::vector< VkImage > swapchainImages;
	std::vector< VkImageView > swapchainImageViews;
	std::vector< VkSemaphore > swapchainPresentSemaphores;

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
	void initComputePasses ();
	void initImgui ();
	void initResources ();
	void initBVH ();
	void initLights();

	// main loop helpers
	void drawImgui ( VkCommandBuffer cmd, VkImageView targetImageView );

	// swapchain helpers
	void resizeSwapchain ();
	void createSwapchain ( uint32_t w, uint32_t h );
	void destroySwapchain ();
};