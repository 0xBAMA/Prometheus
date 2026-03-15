#pragma once

#include <iostream>
#include <chrono>
#include <thread>

#include <vk_types.h>
#include <vk_descriptors.h>
#include <vk_pipelines.h>
#include <vk_loader.h>

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
};

struct ComputePushConstants {
	glm::vec4 data1;
	glm::vec4 data2;
	glm::vec4 data3;
	glm::vec4 data4;
};

struct BasicGPUSceneData {
	glm::mat4 view;
	glm::mat4 proj;
	glm::mat4 viewproj;
	glm::vec4 ambientColor;
	glm::vec4 sunlightDirection; // w for power
	glm::vec4 sunlightColor;
};

struct ComputeEffect {
	const char* name;

	VkPipeline pipeline;
	VkPipelineLayout layout;

	// need to figure out adding buffers + textures here
		// this is core functionality, with configurable descriptors for the particular effects

	ComputePushConstants data;
};

struct GLTFMetallic_Roughness {
// material wrapper defined in vk_types
	MaterialPipeline opaquePipeline;
	MaterialPipeline transparentPipeline;

	VkDescriptorSetLayout materialLayout;

	struct MaterialConstants {
		glm::vec4 colorFactors;
		glm::vec4 metal_rough_factors;
		//padding, we need it anyway for uniform buffers
		glm::vec4 extra[ 14 ];
	};

	struct MaterialResources {
		AllocatedImage colorImage;
		VkSampler colorSampler;
		AllocatedImage metalRoughImage;
		VkSampler metalRoughSampler;
		VkBuffer dataBuffer;
		uint32_t dataBufferOffset;
	};

	DescriptorWriter writer;

	void buildPipelines ( PrometheusInstance* engine );
	void clearResources ( VkDevice device );

	MaterialInstance writeMaterial ( VkDevice device, MaterialPass pass, const MaterialResources& resources, DescriptorAllocatorGrowable& descriptorAllocator );
};

struct RenderObject {
	uint32_t indexCount;
	uint32_t firstIndex;
	VkBuffer indexBuffer;

	MaterialInstance* material;

	glm::mat4 transform;
	VkDeviceAddress vertexBufferAddress;
};

struct DrawContext {
	std::vector< RenderObject > OpaqueSurfaces;
};

struct MeshNode : public Node {
	std::shared_ptr< MeshAsset > mesh;
	virtual void Draw ( const glm::mat4& topMatrix, DrawContext& ctx ) override;
};

constexpr unsigned int FRAME_OVERLAP = 2;
constexpr bool useValidationLayers = true;

class PrometheusInstance {
public:

	bool resizeRequest { false };
	bool isInitialized { false };
	bool stopRendering { false };
	int frameNumber { 0 };

	// for drawing a triangle
	VkPipelineLayout trianglePipelineLayout;
	VkPipeline trianglePipeline;

	// for drawing a mesh
	VkPipelineLayout meshPipelineLayout;
	VkPipeline meshPipeline;
	GPUMeshBuffers rectangle;
	void initDefaultData ();
	GPUMeshBuffers uploadMesh ( std::span<uint32_t> indices, std::span<Vertex> vertices );
	std::vector<std::shared_ptr<MeshAsset>> testMeshes;

	// GLTF mesh drawing
	MaterialInstance defaultData;
	GLTFMetallic_Roughness metalRoughMaterial;

	// for buffer setup
	AllocatedBuffer createBuffer( size_t allocSize, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage );
	void destroyBuffer( const AllocatedBuffer& buffer );

	// also contains retained state for push constants
	std::vector< ComputeEffect > computeEffects;

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
	void destroyImage ( const AllocatedImage& img );

	// and some default textures
	AllocatedImage whiteImage;
	AllocatedImage blackImage;
	AllocatedImage greyImage;
	AllocatedImage errorCheckerboardImage;

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

	BasicGPUSceneData sceneData;
	DescriptorAllocatorGrowable globalDescriptorAllocator;
	VkDescriptorSetLayout gpuSceneDataDescriptorLayout;

	VkDescriptorSet drawImageDescriptors;
	VkDescriptorSetLayout drawImageDescriptorLayout;

	VkDescriptorSetLayout singleImageDescriptorLayout;

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
	void initPipelines ();
	void initBackgroundPipelines ();
	void initTrianglePipeline ();
	void initMeshPipeline ();
	void initImgui ();

	// main loop helpers
	void drawImgui ( VkCommandBuffer cmd, VkImageView targetImageView );
	void drawGeometry ( VkCommandBuffer cmd );
	void drawBackground ( VkCommandBuffer cmd ) const;

	// swapchain helpers
	void resizeSwapchain ();
	void createSwapchain ( uint32_t w, uint32_t h );
	void destroySwapchain ();
};