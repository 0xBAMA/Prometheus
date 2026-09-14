#include "engine.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_vulkan.h>

#include <vk_types.h>
#include <vk_initializers.h>
#include <vk_descriptors.h>
#include <vk_pipelines.h>
#include <vk_loader.h>
#include <vk_images.h>

#include <iostream>
#include <random>
#include <array>
#include <thread>
#include <chrono>
#include <fstream>

// heightmap gen

struct Resource {
	enum class Type {
		ImageView,
		Buffer
	};

	Type type;

	Resource( VkBuffer buff ) {
		type = Type::Buffer;
		buffer = buff;
	}

	Resource( VkImageView image ) {
		type = Type::ImageView;
		imageView = image;
	}

	// potentially extend this later to include things like the sampler, too

	union {
		VkImageView imageView;
		VkBuffer buffer;
	};
};

enum pipelineType {
	COMPUTE,
	GRAPHICS,
};

struct descriptorItem {

	// the descriptor index in the descriptor set
	uint32_t index = 0;

	// expressing the specific type of descriptor
	// usually one of:
	// VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
	// VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
	// VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
	// VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
	VkDescriptorType type = VK_DESCRIPTOR_TYPE_MAX_ENUM;

	// for simplicity, this handles both image and buffer descriptors
	// std::function used so that the behavior can be dynamic at runtime
	std::function<Resource()> get;

	// IMAGE DESCRIPTOR
	VkSampler sampler;
	VkImageLayout layout;
	descriptorItem (
		uint32_t index_in,
		VkDescriptorType type_in,
		VkSampler sampler_in,
		std::function<Resource()> getLambda,
		VkImageLayout layout_in = VK_IMAGE_LAYOUT_GENERAL )
	{
		index = index_in;
		type = type_in;
		sampler = sampler_in;
		get = getLambda;
		layout = layout_in;
	}

	// BUFFER DESCRIPTOR
	size_t size = 0;
	size_t offset = 0;
	descriptorItem (
		uint32_t index_in,
		VkDescriptorType type_in,
		size_t size_in, size_t offset_in,
		std::function<Resource()> getLambda )
	{
		index = index_in;
		type = type_in;
		get = getLambda;
		size = size_in;
		offset = offset_in;
	}

	// so we can iterate through the list at runtime
	void write ( DescriptorWriter& writer ) {
		if ( type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ) {
			// writing an image descriptor
			writer.write_image( index, get().imageView, sampler, layout, type );
		} else {
			// writing a buffer descriptor
			writer.write_buffer( index, get().buffer, size, offset, type );
		}
		// if you want to handle other stuff (e.g. BVH stuff), this needs to handle that
	}
};

struct ComputeConfig {

	std::string name;
	std::vector<descriptorItem> descriptorSetLayout;

	std::string shaderPath;

	std::function< VkDescriptorSet( VkDescriptorSetLayout dsl ) > allocateDescriptorSet;
	std::function< void( VkCommandBuffer cmd ) > dispatch;
	std::function< void( VkCommandBuffer cmd ) > updatePushConstants;

	std::vector< VkImageMemoryBarrier2 > imageBarriers;
	std::vector< VkBufferMemoryBarrier2 > memoryBarriers;

};

struct RasterConfig {

	std::string name;
	std::vector<descriptorItem> descriptorSetLayout;

	std::string shaderPathVert;
	std::string shaderPathFrag;

	std::function< VkDescriptorSet( VkDescriptorSetLayout dsl ) > allocateDescriptorSet;
	std::function< void( VkCommandBuffer cmd ) > dispatch;
	std::function< void( VkCommandBuffer cmd ) > updatePushConstants;
	std::function< VkExtent2D() > getRenderResolution;

	std::vector< VkImageMemoryBarrier2 > imageBarriers;
	std::vector< VkBufferMemoryBarrier2 > memoryBarriers;

	// rasterizer config
	bool enableDepthTest = true;
	VkCompareOp depthOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
	float lineWidth = 1.0f;

	// what you're drawing...
	// polygon mode, default to VK_POLYGON_MODE_FILL
	// cull mode
	// multisampling mode... not critical right now

	// will need to add some things to configure blending

	AllocatedImage *depthImage;
	AllocatedImage *drawImage;

	VkPrimitiveTopology inputTopology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
};

struct ComputeEffect {
	// pipeline is the thing we use to invoke this shader pass
	VkPipeline pipeline;
	pipelineType type;

	// pipeline layout gives us what we need for sending push constants and buffer attachments
	VkPipelineLayout pipelineLayout;

	// this is the descriptor set layout for this particular compute effect (UBO + any SSBOs + any images/textures)
	VkDescriptorSetLayout descriptorSetLayout;
	VkDescriptorSet descriptorSet;

	// retained state for the push constants
	PushConstants pushConstants;

	// copied from the input config struct
	std::vector<descriptorItem> descriptors;

	// barriers needed for this pass
	std::vector< VkImageMemoryBarrier2 > imageBarriers;
	std::vector< VkBufferMemoryBarrier2 > memoryBarriers;

	// used for raster only
	AllocatedImage *depthImage;
	AllocatedImage *drawImage;

	// so we can have the main loop code local to the declaration
	std::function< void( VkCommandBuffer cmd ) > invoke;
	std::function< VkDescriptorSet( VkDescriptorSetLayout dsl ) > allocateDescriptorSet;
	std::function< void( VkCommandBuffer cmd ) > dispatch;
	std::function< void( VkCommandBuffer cmd ) > updatePushConstants;
	std::function< VkExtent2D() > getRenderResolution;

	VkDevice* devicePtr;

	void init ( VkDevice* device, DeletionQueue* mainDeletionQueue, const RasterConfig config ) {
		type = GRAPHICS;

		getRenderResolution = config.getRenderResolution;
		drawImage = config.drawImage;
		depthImage = config.depthImage;
		imageBarriers = config.imageBarriers;
		memoryBarriers = config.memoryBarriers;

		allocateDescriptorSet = config.allocateDescriptorSet;
		dispatch = config.dispatch;
		updatePushConstants = config.updatePushConstants;

		devicePtr = device;

		{ // the first thing this needs is the descriptor layout
			DescriptorLayoutBuilder builder;

			for ( auto& d : config.descriptorSetLayout )
				builder.add_binding( d.index, d.type ),
				descriptors.push_back( d );

			descriptorSetLayout = builder.build( *device, VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT );
			SetDebugName( VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, ( uint64_t ) descriptorSetLayout, ( config.name + " Descriptor Set Layout" ).c_str() );
		}
		{ // pipeline layout + compute pipeline
			VkPushConstantRange pushConstant{};
			pushConstant.offset = 0;
			pushConstant.size = sizeof( PushConstants );
			pushConstant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

			VkPipelineLayoutCreateInfo rasterLayout{};
			rasterLayout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
			rasterLayout.pNext = nullptr;
			rasterLayout.pSetLayouts = &descriptorSetLayout;
			rasterLayout.setLayoutCount = 1;
			rasterLayout.pPushConstantRanges = &pushConstant;
			rasterLayout.pushConstantRangeCount = 1;

			VK_CHECK( vkCreatePipelineLayout( *device, &rasterLayout, nullptr, &pipelineLayout ) );
			SetDebugName( VK_OBJECT_TYPE_PIPELINE_LAYOUT, ( uint64_t ) pipelineLayout, ( config.name + " Raster Pipeline Layout" ).c_str() );

			VkShaderModule fragShader;
			if ( !vkutil::load_shader_module( config.shaderPathFrag.c_str(), *device, &fragShader ) ) {
				fmt::print( "Error when building the {} Fragment shader module\n", config.name.c_str() );
			}
			SetDebugName( VK_OBJECT_TYPE_SHADER_MODULE, ( uint64_t ) fragShader, ( config.name + " Fragment Shader Module" ).c_str() );

			VkShaderModule vertexShader;
			if ( !vkutil::load_shader_module( config.shaderPathVert.c_str(), *device, &vertexShader ) ) {
				fmt::print( "Error when building the {} Vertex shader module\n", config.name.c_str() );
			}
			SetDebugName( VK_OBJECT_TYPE_SHADER_MODULE, ( uint64_t ) vertexShader, ( config.name + " Vertex Shader Module" ).c_str() );

			PipelineBuilder pipelineBuilder;
			pipelineBuilder._pipelineLayout = pipelineLayout;
			pipelineBuilder.set_shaders( vertexShader, fragShader );
			pipelineBuilder.set_input_topology( config.inputTopology );
			pipelineBuilder.set_polygon_mode( VK_POLYGON_MODE_FILL );
			pipelineBuilder.set_cull_mode( VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE );
			pipelineBuilder.set_multisampling_none(); // tbd, not core functionality for now
			pipelineBuilder.disable_blending();
			pipelineBuilder.set_line_width( config.lineWidth );
			pipelineBuilder.set_color_attachment_format( config.drawImage->imageFormat );
			pipelineBuilder.enable_depthtest( config.enableDepthTest, config.depthOp );
			if ( config.enableDepthTest )
				pipelineBuilder.set_depth_format( config.depthImage->imageFormat );
			pipeline = pipelineBuilder.build_pipeline( *device );
			SetDebugName( VK_OBJECT_TYPE_PIPELINE, ( uint64_t ) pipeline, ( config.name + " Raster Pipeline" ).c_str() );

			// cleanup
			vkDestroyShaderModule( *device, fragShader, nullptr );
			vkDestroyShaderModule( *device, vertexShader, nullptr );

			// deletors for the pipeline layout + pipeline
			mainDeletionQueue->push_function( [ & ] () {
				vkDestroyDescriptorSetLayout( *device, descriptorSetLayout, nullptr );
				vkDestroyPipelineLayout( *device, pipelineLayout, nullptr );
				vkDestroyPipeline( *device, pipeline, nullptr );
			});
		}
	}

	void init ( VkDevice* device, DeletionQueue* mainDeletionQueue, const ComputeConfig config ) {
		type = COMPUTE;
		allocateDescriptorSet = config.allocateDescriptorSet;
		dispatch = config.dispatch;
		updatePushConstants = config.updatePushConstants;

		devicePtr = device;
		{ // the first thing this needs is the descriptor layout
			DescriptorLayoutBuilder builder;

			for ( auto& d : config.descriptorSetLayout )
				builder.add_binding( d.index, d.type ),
				descriptors.push_back( d );

			descriptorSetLayout = builder.build( *device, VK_SHADER_STAGE_COMPUTE_BIT );
			SetDebugName( VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, ( uint64_t ) descriptorSetLayout, ( config.name + " Descriptor Set Layout" ).c_str() );
		}

		{ // second thing it needs to do is pipeline layout
			VkPushConstantRange pushConstant{};
			pushConstant.offset = 0;
			pushConstant.size = sizeof( PushConstants );
			pushConstant.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

			VkPipelineLayoutCreateInfo computeLayout{};
			computeLayout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
			computeLayout.pNext = nullptr;
			computeLayout.pSetLayouts = &descriptorSetLayout;
			computeLayout.setLayoutCount = 1;
			computeLayout.pPushConstantRanges = &pushConstant;
			computeLayout.pushConstantRangeCount = 1;

			VK_CHECK( vkCreatePipelineLayout( *device, &computeLayout, nullptr, &pipelineLayout ) );
			SetDebugName( VK_OBJECT_TYPE_PIPELINE_LAYOUT, ( uint64_t ) pipelineLayout, ( config.name + " Pipeline Layout" ).c_str() );

			VkShaderModule shaderModule;
			if ( !vkutil::load_shader_module( config.shaderPath.c_str(), *device, &shaderModule ) ) {
				fmt::print( "Error when building the {} Compute Shader\n", config.name );
			}
			SetDebugName( VK_OBJECT_TYPE_SHADER_MODULE, ( uint64_t ) shaderModule, ( config.name + " Shader Module" ).c_str() );

			VkPipelineShaderStageCreateInfo stageinfo{};
			stageinfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			stageinfo.pNext = nullptr;
			stageinfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
			stageinfo.module = shaderModule;
			stageinfo.pName = "main"; // potentially parameterize this in the future

			VkComputePipelineCreateInfo computePipelineCreateInfo{};
			computePipelineCreateInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
			computePipelineCreateInfo.pNext = nullptr;
			computePipelineCreateInfo.layout = pipelineLayout;
			computePipelineCreateInfo.stage = stageinfo;

			VK_CHECK( vkCreateComputePipelines( *device, VK_NULL_HANDLE, 1, &computePipelineCreateInfo, nullptr, &pipeline ) );
			SetDebugName( VK_OBJECT_TYPE_PIPELINE, ( uint64_t ) pipeline, ( config.name + " Compute Pipeline" ).c_str() );
			vkDestroyShaderModule( *device, shaderModule, nullptr );

			// deletors for the pipeline layout + pipeline
			( *mainDeletionQueue ).push_function( [ & ] () {
				vkDestroyDescriptorSetLayout( *device, descriptorSetLayout, nullptr );
				vkDestroyPipelineLayout( *device, pipelineLayout, nullptr );
				vkDestroyPipeline( *device, pipeline, nullptr );
			});
		}
	}

	void bindPipelineAndDescriptorSetsCompute( VkCommandBuffer cmd ) {
		vkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline );
		vkCmdBindDescriptorSets( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &descriptorSet, 0, nullptr );
	}

	void bindPipelineAndDescriptorSetsGraphics( VkCommandBuffer cmd ) {
		vkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline );
		vkCmdBindDescriptorSets( cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &descriptorSet, 0, nullptr );
	}

	void beginRendering( VkCommandBuffer cmd ) {
		VkExtent2D extent = getRenderResolution();
		VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info( drawImage->imageView, nullptr, VK_IMAGE_LAYOUT_GENERAL );
		VkRenderingAttachmentInfo depthAttachment = vkinit::attachment_info( depthImage->imageView, nullptr, VK_IMAGE_LAYOUT_GENERAL );
		VkRenderingInfo renderInfo = vkinit::rendering_info( extent, &colorAttachment, &depthAttachment );

		// start up the rasterizer
		vkCmdBeginRendering( cmd, &renderInfo );

		// set dynamic viewport and scissor
		VkViewport viewport = { .x = 0, .y = 0, .minDepth = 0.0f, .maxDepth = 1.0f };
		viewport.width = extent.width;
		viewport.height = extent.height;
		vkCmdSetViewport( cmd, 0, 1, &viewport );

		VkRect2D scissor = { .offset = { 0, 0 } };
		scissor.extent = extent;
		vkCmdSetScissor( cmd, 0, 1, &scissor );
	}

	void endRendering( VkCommandBuffer cmd ) {
		vkCmdEndRendering( cmd );
	}

	void barriers ( VkCommandBuffer cmd ) {
		VkDependencyInfo barrierDependency {
			.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
			.bufferMemoryBarrierCount = uint32_t( memoryBarriers.size() ),
			.pBufferMemoryBarriers = memoryBarriers.data(),
			.imageMemoryBarrierCount = uint32_t( imageBarriers.size() ),
			.pImageMemoryBarriers = imageBarriers.data(),
		};

		vkCmdPipelineBarrier2( cmd, &barrierDependency );
	}

	void invoke2( VkCommandBuffer cmd ) {
		descriptorSet = allocateDescriptorSet( descriptorSetLayout );
		{
			DescriptorWriter writer;
			for ( auto& d : descriptors )
				d.write( writer );
			writer.update_set( *devicePtr, descriptorSet );
		}

		// setup for buffers etc
		if ( type == COMPUTE ) {
			bindPipelineAndDescriptorSetsCompute( cmd );
		} else if ( type == GRAPHICS ) {
			bindPipelineAndDescriptorSetsGraphics( cmd );
			beginRendering( cmd );
		}
		updatePushConstants( cmd );

		// invoke the actual pass + barriers
		dispatch( cmd );

		if ( type == GRAPHICS ) {
			endRendering( cmd );
		}
		barriers( cmd );
	}

};

