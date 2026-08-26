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

};

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

	// copied from the input config struct
	std::vector<descriptorItem> descriptors;

	// so we can have the main loop code local to the declaration
	std::function< void( VkCommandBuffer cmd ) > invoke;

	void init ( VkDevice* device, DeletionQueue* mainDeletionQueue,  const ComputeConfig config ) {
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
};

