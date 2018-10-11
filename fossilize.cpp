/* Copyright (c) 2018 Hans-Kristian Arntzen
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <stddef.h>
#include "fossilize.hpp"
#include <stdexcept>
#include <algorithm>
#include <unordered_map>
#include <queue>
#include <string.h>
#include "varint.hpp"
#include "layer/utils.hpp"

#define RAPIDJSON_HAS_STDSTRING 1
#include "rapidjson/document.h"
#include "rapidjson/prettywriter.h"

using namespace std;
using namespace rapidjson;

namespace Fossilize
{
#define FOSSILIZE_THROW(x) throw ::Fossilize::Exception(x)

#define FOSSILIZE_MAGIC "FOSSILIZE0000001"
#define FOSSILIZE_JSON_MAGIC "JSON    "
#define FOSSILIZE_SPIRV_MAGIC "SPIR-V  "
#define FOSSILIZE_MAGIC_LEN 16

enum
{
	FOSSILIZE_FORMAT_VERSION = 1
};

class Hasher
{
public:
	explicit Hasher(Hash h)
		: h(h)
	{
	}

	Hasher() = default;

	template <typename T>
	inline void data(const T *data, size_t size)
	{
		size /= sizeof(*data);
		for (size_t i = 0; i < size; i++)
			h = (h * 0x100000001b3ull) ^ data[i];
	}

	inline void u32(uint32_t value)
	{
		h = (h * 0x100000001b3ull) ^ value;
	}

	inline void s32(int32_t value)
	{
		u32(uint32_t(value));
	}

	inline void f32(float value)
	{
		union
		{
			float f32;
			uint32_t u32;
		} u;
		u.f32 = value;
		u32(u.u32);
	}

	inline void u64(uint64_t value)
	{
		u32(value & 0xffffffffu);
		u32(value >> 32);
	}

	template <typename T>
	inline void pointer(T *ptr)
	{
		u64(reinterpret_cast<uintptr_t>(ptr));
	}

	inline void string(const char *str)
	{
		char c;
		u32(0xff);
		while ((c = *str++) != '\0')
			u32(uint8_t(c));
	}

	inline void string(const std::string &str)
	{
		u32(0xff);
		for (auto &c : str)
			u32(uint8_t(c));
	}

	inline Hash get() const
	{
		return h;
	}

private:
	Hash h = 0xcbf29ce484222325ull;
};

template <typename T>
struct HashedInfo
{
	Hash hash;
	T info;
};

struct StateReplayer::Impl
{
	void parse(StateCreatorInterface &iface, ResolverInterface &resolver, const void *buffer, size_t size);
	ScratchAllocator allocator;

	std::unordered_map<Hash, VkSampler> replayed_samplers;
	std::unordered_map<Hash, VkDescriptorSetLayout> replayed_descriptor_set_layouts;
	std::unordered_map<Hash, VkPipelineLayout> replayed_pipeline_layouts;
	std::unordered_map<Hash, VkShaderModule> replayed_shader_modules;
	std::unordered_map<Hash, VkRenderPass> replayed_render_passes;
	std::unordered_map<Hash, VkPipeline> replayed_compute_pipelines;
	std::unordered_map<Hash, VkPipeline> replayed_graphics_pipelines;

	void parse_samplers(StateCreatorInterface &iface, const Value &samplers);
	void parse_descriptor_set_layouts(StateCreatorInterface &iface, const Value &layouts);
	void parse_pipeline_layouts(StateCreatorInterface &iface, const Value &layouts);
	void parse_shader_modules(StateCreatorInterface &iface, const Value &modules);
	void parse_render_passes(StateCreatorInterface &iface, const Value &passes);
	void parse_compute_pipelines(StateCreatorInterface &iface, ResolverInterface &resolver, const Value &pipelines);
	void parse_graphics_pipelines(StateCreatorInterface &iface, ResolverInterface &resolver, const Value &pipelines);
	VkPushConstantRange *parse_push_constant_ranges(const Value &ranges);
	VkDescriptorSetLayout *parse_set_layouts(const Value &layouts);
	VkDescriptorSetLayoutBinding *parse_descriptor_set_bindings(const Value &bindings);
	VkSampler *parse_immutable_samplers(const Value &samplers);
	VkAttachmentDescription *parse_render_pass_attachments(const Value &attachments);
	VkSubpassDependency *parse_render_pass_dependencies(const Value &dependencies);
	VkSubpassDescription *parse_render_pass_subpasses(const Value &subpass);
	VkAttachmentReference *parse_attachment(const Value &value);
	VkAttachmentReference *parse_attachments(const Value &attachments);
	VkSpecializationInfo *parse_specialization_info(const Value &spec_info);
	VkSpecializationMapEntry *parse_map_entries(const Value &map_entries);
	VkViewport *parse_viewports(const Value &viewports);
	VkRect2D *parse_scissors(const Value &scissors);
	VkPipelineVertexInputStateCreateInfo *parse_vertex_input_state(const Value &state);
	VkPipelineColorBlendStateCreateInfo *parse_color_blend_state(const Value &state);
	VkPipelineDepthStencilStateCreateInfo *parse_depth_stencil_state(const Value &state);
	VkPipelineRasterizationStateCreateInfo *parse_rasterization_state(const Value &state);
	VkPipelineInputAssemblyStateCreateInfo *parse_input_assembly_state(const Value &state);
	VkPipelineMultisampleStateCreateInfo *parse_multisample_state(const Value &state);
	VkPipelineViewportStateCreateInfo *parse_viewport_state(const Value &state);
	VkPipelineDynamicStateCreateInfo *parse_dynamic_state(const Value &state);
	VkPipelineTessellationStateCreateInfo *parse_tessellation_state(const Value &state);
	VkPipelineShaderStageCreateInfo *parse_stages(StateCreatorInterface &iface, ResolverInterface &resolver, const Value &stages);
	VkVertexInputAttributeDescription *parse_vertex_attributes(const Value &attributes);
	VkVertexInputBindingDescription *parse_vertex_bindings(const Value &bindings);
	VkPipelineColorBlendAttachmentState *parse_blend_attachments(const Value &attachments);
	uint32_t *parse_uints(const Value &attachments);
	const char *duplicate_string(const char *str, size_t len);

	template <typename T>
	T *copy(const T *src, size_t count);
};

struct WorkItem
{
	uint64_t handle;
	void* create_info;
};

struct StateRecorder::Impl
{
	ScratchAllocator allocator;

	std::unordered_map<Hash, VkDescriptorSetLayoutCreateInfo> descriptor_sets;
	std::unordered_map<Hash, VkPipelineLayoutCreateInfo> pipeline_layouts;
	std::unordered_map<Hash, VkShaderModuleCreateInfo> shader_modules;
	std::unordered_map<Hash, VkGraphicsPipelineCreateInfo> graphics_pipelines;
	std::unordered_map<Hash, VkComputePipelineCreateInfo> compute_pipelines;
	std::unordered_map<Hash, VkRenderPassCreateInfo> render_passes;
	std::unordered_map<Hash, VkSamplerCreateInfo> samplers;

	std::unordered_map<VkDescriptorSetLayout, Hash> descriptor_set_layout_to_index;
	std::unordered_map<VkPipelineLayout, Hash> pipeline_layout_to_index;
	std::unordered_map<VkShaderModule, Hash> shader_module_to_index;
	std::unordered_map<VkPipeline, Hash> graphics_pipeline_to_index;
	std::unordered_map<VkPipeline, Hash> compute_pipeline_to_index;
	std::unordered_map<VkRenderPass, Hash> render_pass_to_index;
	std::unordered_map<VkSampler, Hash> sampler_to_index;

	VkDescriptorSetLayoutCreateInfo *copy_descriptor_set_layout(const VkDescriptorSetLayoutCreateInfo *create_info);
	VkPipelineLayoutCreateInfo *copy_pipeline_layout(const VkPipelineLayoutCreateInfo *create_info);
	VkShaderModuleCreateInfo *copy_shader_module(const VkShaderModuleCreateInfo *create_info);
	VkGraphicsPipelineCreateInfo *copy_graphics_pipeline(const VkGraphicsPipelineCreateInfo *create_info);
	VkComputePipelineCreateInfo *copy_compute_pipeline(const VkComputePipelineCreateInfo *create_info);
	VkSamplerCreateInfo *copy_sampler(const VkSamplerCreateInfo *create_info);
	VkRenderPassCreateInfo *copy_render_pass(const VkRenderPassCreateInfo *create_info);

	VkSpecializationInfo *copy_specialization_info(const VkSpecializationInfo *info);

	VkSampler remap_sampler_handle(VkSampler sampler) const;
	VkDescriptorSetLayout remap_descriptor_set_layout_handle(VkDescriptorSetLayout layout) const;
	VkPipelineLayout remap_pipeline_layout_handle(VkPipelineLayout layout) const;
	VkRenderPass remap_render_pass_handle(VkRenderPass render_pass) const;
	VkShaderModule remap_shader_module_handle(VkShaderModule shader_module) const;
	VkPipeline remap_compute_pipeline_handle(VkPipeline pipeline) const;
	VkPipeline remap_graphics_pipeline_handle(VkPipeline pipeline) const;

	void remap_descriptor_set_layout_ci(VkDescriptorSetLayoutCreateInfo *create_info);
	void remap_pipeline_layout_ci(VkPipelineLayoutCreateInfo *create_info);
	void remap_shader_module_ci(VkShaderModuleCreateInfo *create_info);
	void remap_graphics_pipeline_ci(VkGraphicsPipelineCreateInfo *create_info);
	void remap_compute_pipeline_ci(VkComputePipelineCreateInfo *create_info);
	void remap_sampler_ci(VkSamplerCreateInfo *create_info);
	void remap_render_pass_ci(VkRenderPassCreateInfo *create_info);

	std::mutex record_lock;
	std::condition_variable record_cv;
	std::queue<WorkItem> record_queue;
	std::thread worker_thread;

	static void record_task(StateRecorder *recorder);

	std::mutex serialization_lock;
	std::string serialization_path;

	template <typename T>
	T *copy(const T *src, size_t count);
};

// reinterpret_cast does not work reliably on MSVC 2013 for Vulkan objects.
template <typename T, typename U>
static inline T api_object_cast(U obj)
{
	static_assert(sizeof(T) == sizeof(U), "Objects are not of same size.");
	return (T)obj;
}

namespace Hashing
{
Hash compute_hash_sampler(const StateRecorder &, const VkSamplerCreateInfo &sampler)
{
	Hasher h;

	h.u32(sampler.flags);
	h.f32(sampler.maxAnisotropy);
	h.f32(sampler.mipLodBias);
	h.f32(sampler.minLod);
	h.f32(sampler.maxLod);
	h.u32(sampler.minFilter);
	h.u32(sampler.magFilter);
	h.u32(sampler.mipmapMode);
	h.u32(sampler.compareEnable);
	h.u32(sampler.compareOp);
	h.u32(sampler.anisotropyEnable);
	h.u32(sampler.addressModeU);
	h.u32(sampler.addressModeV);
	h.u32(sampler.addressModeW);
	h.u32(sampler.borderColor);
	h.u32(sampler.unnormalizedCoordinates);

	return h.get();
}

Hash compute_hash_descriptor_set_layout(const StateRecorder &recorder, const VkDescriptorSetLayoutCreateInfo &layout)
{
	Hasher h;

	h.u32(layout.bindingCount);
	h.u32(layout.flags);
	for (uint32_t i = 0; i < layout.bindingCount; i++)
	{
		auto &binding = layout.pBindings[i];
		h.u32(binding.binding);
		h.u32(binding.descriptorCount);
		h.u32(binding.descriptorType);
		h.u32(binding.stageFlags);

		if (binding.pImmutableSamplers &&
			(binding.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
		    binding.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER))
		{
			for (uint32_t j = 0; j < binding.descriptorCount; j++)
				h.u64(recorder.get_hash_for_sampler(binding.pImmutableSamplers[j]));
		}
	}

	return h.get();
}

Hash compute_hash_pipeline_layout(const StateRecorder &recorder, const VkPipelineLayoutCreateInfo &layout)
{
	Hasher h;

	h.u32(layout.setLayoutCount);
	for (uint32_t i = 0; i < layout.setLayoutCount; i++)
	{
		if (layout.pSetLayouts[i])
			h.u64(recorder.get_hash_for_descriptor_set_layout(layout.pSetLayouts[i]));
		else
			h.u32(0);
	}

	h.u32(layout.pushConstantRangeCount);
	for (uint32_t i = 0; i < layout.pushConstantRangeCount; i++)
	{
		auto &push = layout.pPushConstantRanges[i];
		h.u32(push.stageFlags);
		h.u32(push.size);
		h.u32(push.offset);
	}

	h.u32(layout.flags);

	return h.get();
}

Hash compute_hash_shader_module(const StateRecorder &, const VkShaderModuleCreateInfo &create_info)
{
	Hasher h;
	h.data(create_info.pCode, create_info.codeSize);
	h.u32(create_info.flags);
	return h.get();
}

static void hash_specialization_info(Hasher &h, const VkSpecializationInfo &spec)
{
	h.data(static_cast<const uint8_t *>(spec.pData), spec.dataSize);
	h.u64(spec.dataSize);
	h.u32(spec.mapEntryCount);
	for (uint32_t i = 0; i < spec.mapEntryCount; i++)
	{
		h.u32(spec.pMapEntries[i].offset);
		h.u64(spec.pMapEntries[i].size);
		h.u32(spec.pMapEntries[i].constantID);
	}
}

Hash compute_hash_graphics_pipeline(const StateRecorder &recorder, const VkGraphicsPipelineCreateInfo &create_info)
{
	Hasher h;

	h.u32(create_info.flags);

	if (create_info.basePipelineHandle != VK_NULL_HANDLE)
	{
		h.u64(recorder.get_hash_for_graphics_pipeline_handle(create_info.basePipelineHandle));
		h.s32(create_info.basePipelineIndex);
	}

	h.u64(recorder.get_hash_for_pipeline_layout(create_info.layout));
	h.u64(recorder.get_hash_for_render_pass(create_info.renderPass));
	h.u32(create_info.subpass);
	h.u32(create_info.stageCount);

	bool dynamic_stencil_compare = false;
	bool dynamic_stencil_reference = false;
	bool dynamic_stencil_write_mask = false;
	bool dynamic_depth_bounds = false;
	bool dynamic_depth_bias = false;
	bool dynamic_line_width = false;
	bool dynamic_blend_constants = false;
	bool dynamic_scissor = false;
	bool dynamic_viewport = false;
	if (create_info.pDynamicState)
	{
		auto &state = *create_info.pDynamicState;
		h.u32(state.dynamicStateCount);
		h.u32(state.flags);
		for (uint32_t i = 0; i < state.dynamicStateCount; i++)
		{
			h.u32(state.pDynamicStates[i]);
			switch (state.pDynamicStates[i])
			{
			case VK_DYNAMIC_STATE_DEPTH_BIAS:
				dynamic_depth_bias = true;
				break;
			case VK_DYNAMIC_STATE_DEPTH_BOUNDS:
				dynamic_depth_bounds = true;
				break;
			case VK_DYNAMIC_STATE_STENCIL_WRITE_MASK:
				dynamic_stencil_write_mask = true;
				break;
			case VK_DYNAMIC_STATE_STENCIL_REFERENCE:
				dynamic_stencil_reference = true;
				break;
			case VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK:
				dynamic_stencil_compare = true;
				break;
			case VK_DYNAMIC_STATE_BLEND_CONSTANTS:
				dynamic_blend_constants = true;
				break;
			case VK_DYNAMIC_STATE_SCISSOR:
				dynamic_scissor = true;
				break;
			case VK_DYNAMIC_STATE_VIEWPORT:
				dynamic_viewport = true;
				break;
			case VK_DYNAMIC_STATE_LINE_WIDTH:
				dynamic_line_width = true;
				break;
			default:
				break;
			}
		}
	}
	else
		h.u32(0);

	if (create_info.pDepthStencilState)
	{
		auto &ds = *create_info.pDepthStencilState;
		h.u32(ds.flags);
		h.u32(ds.depthBoundsTestEnable);
		h.u32(ds.depthCompareOp);
		h.u32(ds.depthTestEnable);
		h.u32(ds.depthWriteEnable);
		h.u32(ds.front.compareOp);
		h.u32(ds.front.depthFailOp);
		h.u32(ds.front.failOp);
		h.u32(ds.front.passOp);
		h.u32(ds.back.compareOp);
		h.u32(ds.back.depthFailOp);
		h.u32(ds.back.failOp);
		h.u32(ds.back.passOp);
		h.u32(ds.stencilTestEnable);

		if (!dynamic_depth_bounds && ds.depthBoundsTestEnable)
		{
			h.f32(ds.minDepthBounds);
			h.f32(ds.maxDepthBounds);
		}

		if (ds.stencilTestEnable)
		{
			if (!dynamic_stencil_compare)
			{
				h.u32(ds.front.compareMask);
				h.u32(ds.back.compareMask);
			}

			if (!dynamic_stencil_reference)
			{
				h.u32(ds.front.reference);
				h.u32(ds.back.reference);
			}

			if (!dynamic_stencil_write_mask)
			{
				h.u32(ds.front.writeMask);
				h.u32(ds.back.writeMask);
			}
		}
	}
	else
		h.u32(0);

	if (create_info.pInputAssemblyState)
	{
		auto &ia = *create_info.pInputAssemblyState;
		h.u32(ia.flags);
		h.u32(ia.primitiveRestartEnable);
		h.u32(ia.topology);
	}
	else
		h.u32(0);

	if (create_info.pRasterizationState)
	{
		auto &rs = *create_info.pRasterizationState;
		h.u32(rs.flags);
		h.u32(rs.cullMode);
		h.u32(rs.depthClampEnable);
		h.u32(rs.frontFace);
		h.u32(rs.rasterizerDiscardEnable);
		h.u32(rs.polygonMode);
		h.u32(rs.depthBiasEnable);

		if (rs.depthBiasEnable && !dynamic_depth_bias)
		{
			h.f32(rs.depthBiasClamp);
			h.f32(rs.depthBiasSlopeFactor);
			h.f32(rs.depthBiasConstantFactor);
		}

		if (!dynamic_line_width)
			h.f32(rs.lineWidth);
	}
	else
		h.u32(0);

	if (create_info.pMultisampleState)
	{
		auto &ms = *create_info.pMultisampleState;
		h.u32(ms.flags);
		h.u32(ms.alphaToCoverageEnable);
		h.u32(ms.alphaToOneEnable);
		h.f32(ms.minSampleShading);
		h.u32(ms.rasterizationSamples);
		h.u32(ms.sampleShadingEnable);
		if (ms.pSampleMask)
		{
			uint32_t elems = (ms.rasterizationSamples + 31) / 32;
			for (uint32_t i = 0; i < elems; i++)
				h.u32(ms.pSampleMask[i]);
		}
		else
			h.u32(0);
	}

	if (create_info.pViewportState)
	{
		auto &vp = *create_info.pViewportState;
		h.u32(vp.flags);
		h.u32(vp.scissorCount);
		h.u32(vp.viewportCount);
		if (!dynamic_scissor)
		{
			for (uint32_t i = 0; i < vp.scissorCount; i++)
			{
				h.s32(vp.pScissors[i].offset.x);
				h.s32(vp.pScissors[i].offset.y);
				h.u32(vp.pScissors[i].extent.width);
				h.u32(vp.pScissors[i].extent.height);
			}
		}

		if (!dynamic_viewport)
		{
			for (uint32_t i = 0; i < vp.viewportCount; i++)
			{
				h.f32(vp.pViewports[i].x);
				h.f32(vp.pViewports[i].y);
				h.f32(vp.pViewports[i].width);
				h.f32(vp.pViewports[i].height);
				h.f32(vp.pViewports[i].minDepth);
				h.f32(vp.pViewports[i].maxDepth);
			}
		}
	}
	else
		h.u32(0);

	if (create_info.pVertexInputState)
	{
		auto &vi = *create_info.pVertexInputState;
		h.u32(vi.flags);
		h.u32(vi.vertexAttributeDescriptionCount);
		h.u32(vi.vertexBindingDescriptionCount);

		for (uint32_t i = 0; i < vi.vertexAttributeDescriptionCount; i++)
		{
			h.u32(vi.pVertexAttributeDescriptions[i].offset);
			h.u32(vi.pVertexAttributeDescriptions[i].binding);
			h.u32(vi.pVertexAttributeDescriptions[i].format);
			h.u32(vi.pVertexAttributeDescriptions[i].location);
		}

		for (uint32_t i = 0; i < vi.vertexBindingDescriptionCount; i++)
		{
			h.u32(vi.pVertexBindingDescriptions[i].binding);
			h.u32(vi.pVertexBindingDescriptions[i].inputRate);
			h.u32(vi.pVertexBindingDescriptions[i].stride);
		}
	}
	else
		h.u32(0);

	if (create_info.pColorBlendState)
	{
		auto &b = *create_info.pColorBlendState;
		h.u32(b.flags);
		h.u32(b.attachmentCount);
		h.u32(b.logicOpEnable);
		h.u32(b.logicOp);

		bool need_blend_constants = false;

		for (uint32_t i = 0; i < b.attachmentCount; i++)
		{
			h.u32(b.pAttachments[i].blendEnable);
			if (b.pAttachments[i].blendEnable)
			{
				h.u32(b.pAttachments[i].colorWriteMask);
				h.u32(b.pAttachments[i].alphaBlendOp);
				h.u32(b.pAttachments[i].colorBlendOp);
				h.u32(b.pAttachments[i].dstAlphaBlendFactor);
				h.u32(b.pAttachments[i].srcAlphaBlendFactor);
				h.u32(b.pAttachments[i].dstColorBlendFactor);
				h.u32(b.pAttachments[i].srcColorBlendFactor);

				if (b.pAttachments[i].dstAlphaBlendFactor == VK_BLEND_FACTOR_CONSTANT_ALPHA ||
				    b.pAttachments[i].dstAlphaBlendFactor == VK_BLEND_FACTOR_CONSTANT_COLOR ||
				    b.pAttachments[i].srcAlphaBlendFactor == VK_BLEND_FACTOR_CONSTANT_ALPHA ||
				    b.pAttachments[i].srcAlphaBlendFactor == VK_BLEND_FACTOR_CONSTANT_COLOR ||
					b.pAttachments[i].dstColorBlendFactor == VK_BLEND_FACTOR_CONSTANT_ALPHA ||
					b.pAttachments[i].dstColorBlendFactor == VK_BLEND_FACTOR_CONSTANT_COLOR ||
					b.pAttachments[i].srcColorBlendFactor == VK_BLEND_FACTOR_CONSTANT_ALPHA ||
					b.pAttachments[i].srcColorBlendFactor == VK_BLEND_FACTOR_CONSTANT_COLOR)
				{
					need_blend_constants = true;
				}
			}
			else
				h.u32(0);
		}

		if (need_blend_constants && !dynamic_blend_constants)
			for (auto &blend_const : b.blendConstants)
				h.f32(blend_const);
	}
	else
		h.u32(0);

	if (create_info.pTessellationState)
	{
		auto &tess = *create_info.pTessellationState;
		h.u32(tess.flags);
		h.u32(tess.patchControlPoints);
	}
	else
		h.u32(0);

	for (uint32_t i = 0; i < create_info.stageCount; i++)
	{
		auto &stage = create_info.pStages[i];
		h.u32(stage.flags);
		h.string(stage.pName);
		h.u32(stage.stage);
		h.u64(recorder.get_hash_for_shader_module(stage.module));
		if (stage.pSpecializationInfo)
			hash_specialization_info(h, *stage.pSpecializationInfo);
		else
			h.u32(0);
	}

	return h.get();
}

Hash compute_hash_compute_pipeline(const StateRecorder &recorder, const VkComputePipelineCreateInfo &create_info)
{
	Hasher h;

	h.u64(recorder.get_hash_for_pipeline_layout(create_info.layout));
	h.u32(create_info.flags);

	if (create_info.basePipelineHandle != VK_NULL_HANDLE)
	{
		h.u64(recorder.get_hash_for_compute_pipeline_handle(create_info.basePipelineHandle));
		h.s32(create_info.basePipelineIndex);
	}
	else
		h.u32(0);

	h.u64(recorder.get_hash_for_shader_module(create_info.stage.module));
	h.string(create_info.stage.pName);
	h.u32(create_info.stage.flags);
	h.u32(create_info.stage.stage);

	if (create_info.stage.pSpecializationInfo)
		hash_specialization_info(h, *create_info.stage.pSpecializationInfo);
	else
		h.u32(0);

	return h.get();
}

static void hash_attachment(Hasher &h, const VkAttachmentDescription &att)
{
	h.u32(att.flags);
	h.u32(att.initialLayout);
	h.u32(att.finalLayout);
	h.u32(att.format);
	h.u32(att.loadOp);
	h.u32(att.storeOp);
	h.u32(att.stencilLoadOp);
	h.u32(att.stencilStoreOp);
	h.u32(att.samples);
}

static void hash_dependency(Hasher &h, const VkSubpassDependency &dep)
{
	h.u32(dep.dependencyFlags);
	h.u32(dep.dstAccessMask);
	h.u32(dep.srcAccessMask);
	h.u32(dep.srcSubpass);
	h.u32(dep.dstSubpass);
	h.u32(dep.srcStageMask);
	h.u32(dep.dstStageMask);
}

static void hash_subpass(Hasher &h, const VkSubpassDescription &subpass)
{
	h.u32(subpass.flags);
	h.u32(subpass.colorAttachmentCount);
	h.u32(subpass.inputAttachmentCount);
	h.u32(subpass.preserveAttachmentCount);
	h.u32(subpass.pipelineBindPoint);

	for (uint32_t i = 0; i < subpass.preserveAttachmentCount; i++)
		h.u32(subpass.pPreserveAttachments[i]);

	for (uint32_t i = 0; i < subpass.colorAttachmentCount; i++)
	{
		h.u32(subpass.pColorAttachments[i].attachment);
		h.u32(subpass.pColorAttachments[i].layout);
	}

	for (uint32_t i = 0; i < subpass.inputAttachmentCount; i++)
	{
		h.u32(subpass.pInputAttachments[i].attachment);
		h.u32(subpass.pInputAttachments[i].layout);
	}

	if (subpass.pResolveAttachments)
	{
		for (uint32_t i = 0; i < subpass.colorAttachmentCount; i++)
		{
			h.u32(subpass.pResolveAttachments[i].attachment);
			h.u32(subpass.pResolveAttachments[i].layout);
		}
	}

	if (subpass.pDepthStencilAttachment)
	{
		h.u32(subpass.pDepthStencilAttachment->attachment);
		h.u32(subpass.pDepthStencilAttachment->layout);
	}
	else
		h.u32(0);
}

Hash compute_hash_render_pass(const StateRecorder &, const VkRenderPassCreateInfo &create_info)
{
	Hasher h;

	h.u32(create_info.attachmentCount);
	h.u32(create_info.dependencyCount);
	h.u32(create_info.subpassCount);

	for (uint32_t i = 0; i < create_info.attachmentCount; i++)
	{
		auto &att = create_info.pAttachments[i];
		hash_attachment(h, att);
	}

	for (uint32_t i = 0; i < create_info.dependencyCount; i++)
	{
		auto &dep = create_info.pDependencies[i];
		hash_dependency(h, dep);
	}

	for (uint32_t i = 0; i < create_info.subpassCount; i++)
	{
		auto &subpass = create_info.pSubpasses[i];
		hash_subpass(h, subpass);
	}

	return h.get();
}
}

static uint8_t *decode_base64(ScratchAllocator &allocator, const char *data, size_t length)
{
	auto *buf = static_cast<uint8_t *>(allocator.allocate_raw(length, 16));
	auto *ptr = buf;

	const auto base64_index = [](char c) -> uint32_t {
		if (c >= 'A' && c <= 'Z')
			return uint32_t(c - 'A');
		else if (c >= 'a' && c <= 'z')
			return uint32_t(c - 'a') + 26;
		else if (c >= '0' && c <= '9')
			return uint32_t(c - '0') + 52;
		else if (c == '+')
			return 62;
		else if (c == '/')
			return 63;
		else
			return 0;
	};

	for (uint64_t i = 0; i < length; )
	{
		char c0 = *data++;
		if (c0 == '\0')
			break;
		char c1 = *data++;
		if (c1 == '\0')
			break;
		char c2 = *data++;
		if (c2 == '\0')
			break;
		char c3 = *data++;
		if (c3 == '\0')
			break;

		uint32_t values =
				(base64_index(c0) << 18) |
				(base64_index(c1) << 12) |
				(base64_index(c2) << 6) |
				(base64_index(c3) << 0);

		unsigned outbytes = 3;
		if (c2 == '=' && c3 == '=')
		{
			outbytes = 1;
			*ptr++ = uint8_t(values >> 16);
		}
		else if (c3 == '=')
		{
			outbytes = 2;
			*ptr++ = uint8_t(values >> 16);
			*ptr++ = uint8_t(values >> 8);
		}
		else
		{
			*ptr++ = uint8_t(values >> 16);
			*ptr++ = uint8_t(values >> 8);
			*ptr++ = uint8_t(values >> 0);
		}

		i += outbytes;
	}

	return buf;
}

static uint64_t string_to_uint64(const char* str) {
	uint64_t value;
	sscanf(str, "%" SCNx64, &value);
	return value;
}

const char *StateReplayer::Impl::duplicate_string(const char *str, size_t len)
{
	auto *c = allocator.allocate_n<char>(len + 1);
	memcpy(c, str, len);
	c[len] = '\0';
	return c;
}

VkSampler *StateReplayer::Impl::parse_immutable_samplers(const Value &samplers)
{
	auto *samps = allocator.allocate_n<VkSampler>(samplers.Size());
	auto *ret = samps;
	for (auto itr = samplers.Begin(); itr != samplers.End(); ++itr, samps++)
	{
		auto index = string_to_uint64(itr->GetString());
		if (index > 0)
			*samps = replayed_samplers[index];
	}

	return ret;
}

VkDescriptorSetLayoutBinding *StateReplayer::Impl::parse_descriptor_set_bindings(const Value &bindings)
{
	auto *set_bindings = allocator.allocate_n_cleared<VkDescriptorSetLayoutBinding>(bindings.Size());
	auto *ret = set_bindings;
	for (auto itr = bindings.Begin(); itr != bindings.End(); ++itr, set_bindings++)
	{
		auto &b = *itr;
		set_bindings->binding = b["binding"].GetUint();
		set_bindings->descriptorCount = b["descriptorCount"].GetUint();
		set_bindings->descriptorType = static_cast<VkDescriptorType>(b["descriptorType"].GetUint());
		set_bindings->stageFlags = b["stageFlags"].GetUint();
		if (b.HasMember("immutableSamplers"))
			set_bindings->pImmutableSamplers = parse_immutable_samplers(b["immutableSamplers"]);
	}
	return ret;
}

VkPushConstantRange *StateReplayer::Impl::parse_push_constant_ranges(const Value &ranges)
{
	auto *infos = allocator.allocate_n_cleared<VkPushConstantRange>(ranges.Size());
	auto *ret = infos;

	for (auto itr = ranges.Begin(); itr != ranges.End(); ++itr, infos++)
	{
		auto &obj = *itr;
		infos->stageFlags = obj["stageFlags"].GetUint();
		infos->offset = obj["offset"].GetUint();
		infos->size = obj["size"].GetUint();
	}

	return ret;
}

VkDescriptorSetLayout *StateReplayer::Impl::parse_set_layouts(const Value &layouts)
{
	auto *infos = allocator.allocate_n_cleared<VkDescriptorSetLayout>(layouts.Size());
	auto *ret = infos;

	for (auto itr = layouts.Begin(); itr != layouts.End(); ++itr, infos++)
	{
		auto index = string_to_uint64(itr->GetString());
		if (index > 0)
			*infos = replayed_descriptor_set_layouts[index];
	}

	return ret;
}

void StateReplayer::Impl::parse_shader_modules(StateCreatorInterface &iface, const Value &modules)
{
	auto *infos = allocator.allocate_n_cleared<VkShaderModuleCreateInfo>(modules.MemberCount());

	unsigned index = 0;
	for (auto itr = modules.MemberBegin(); itr != modules.MemberEnd(); ++itr, index++)
	{
		Hash hash = string_to_uint64(itr->name.GetString());
		if (replayed_shader_modules.count(hash))
			continue;
		auto &obj = itr->value;
		auto &info = infos[index];
		info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		info.flags = obj["flags"].GetUint();
		info.codeSize = obj["codeSize"].GetUint64();
		info.pCode = reinterpret_cast<uint32_t*>(decode_base64(allocator, obj["code"].GetString(), info.codeSize));
		if (!iface.enqueue_create_shader_module(hash, &info, &replayed_shader_modules[hash]))
			FOSSILIZE_THROW("Failed to create shader module.");
	}
	iface.wait_enqueue();
}

void StateReplayer::Impl::parse_pipeline_layouts(StateCreatorInterface &iface, const Value &layouts)
{
	auto *infos = allocator.allocate_n_cleared<VkPipelineLayoutCreateInfo>(layouts.MemberCount());

	unsigned index = 0;
	for (auto itr = layouts.MemberBegin(); itr != layouts.MemberEnd(); ++itr, index++)
	{
		Hash hash = string_to_uint64(itr->name.GetString());
		if (replayed_pipeline_layouts.count(hash))
			continue;
		auto &obj = itr->value;
		auto &info = infos[index];
		info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;

		info.flags = obj["flags"].GetUint();

		if (obj.HasMember("pushConstantRanges"))
		{
			info.pushConstantRangeCount = obj["pushConstantRanges"].Size();
			info.pPushConstantRanges = parse_push_constant_ranges(obj["pushConstantRanges"]);
		}

		if (obj.HasMember("setLayouts"))
		{
			info.setLayoutCount = obj["setLayouts"].Size();
			info.pSetLayouts = parse_set_layouts(obj["setLayouts"]);
		}

		if (!iface.enqueue_create_pipeline_layout(hash, &info, &replayed_pipeline_layouts[hash]))
			FOSSILIZE_THROW("Failed to create pipeline layout.");
	}
	iface.wait_enqueue();
}

void StateReplayer::Impl::parse_descriptor_set_layouts(StateCreatorInterface &iface, const Value &layouts)
{
	auto *infos = allocator.allocate_n_cleared<VkDescriptorSetLayoutCreateInfo>(layouts.MemberCount());

	unsigned index = 0;
	for (auto itr = layouts.MemberBegin(); itr != layouts.MemberEnd(); ++itr, index++)
	{
		Hash hash = string_to_uint64(itr->name.GetString());
		if (replayed_descriptor_set_layouts.count(hash))
			continue;
		auto &obj = itr->value;
		auto &info = infos[index];
		info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;

		info.flags = obj["flags"].GetUint();
		if (obj.HasMember("bindings"))
		{
			auto &bindings = obj["bindings"];
			info.bindingCount = bindings.Size();
			auto *allocated_bindings = parse_descriptor_set_bindings(bindings);
			info.pBindings = allocated_bindings;
		}

		if (!iface.enqueue_create_descriptor_set_layout(hash, &info, &replayed_descriptor_set_layouts[hash]))
			FOSSILIZE_THROW("Failed to create descriptor set layout.");
	}
	iface.wait_enqueue();
}

void StateReplayer::Impl::parse_samplers(StateCreatorInterface &iface, const Value &samplers)
{
	auto *infos = allocator.allocate_n_cleared<VkSamplerCreateInfo>(samplers.MemberCount());

	unsigned index = 0;
	for (auto itr = samplers.MemberBegin(); itr != samplers.MemberEnd(); ++itr)
	{
		Hash hash = string_to_uint64(itr->name.GetString());
		if (replayed_samplers.count(hash))
			continue;
		auto &obj = itr->value;
		auto &info = infos[index];
		info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;

		info.addressModeU = static_cast<VkSamplerAddressMode>(obj["addressModeU"].GetUint());
		info.addressModeV = static_cast<VkSamplerAddressMode>(obj["addressModeV"].GetUint());
		info.addressModeW = static_cast<VkSamplerAddressMode>(obj["addressModeW"].GetUint());
		info.anisotropyEnable = obj["anisotropyEnable"].GetUint();
		info.borderColor = static_cast<VkBorderColor>(obj["borderColor"].GetUint());
		info.compareEnable = obj["compareEnable"].GetUint();
		info.compareOp = static_cast<VkCompareOp>(obj["compareOp"].GetUint());
		info.flags = obj["flags"].GetUint();
		info.magFilter = static_cast<VkFilter>(obj["magFilter"].GetUint());
		info.minFilter = static_cast<VkFilter>(obj["minFilter"].GetUint());
		info.maxAnisotropy = obj["maxAnisotropy"].GetFloat();
		info.mipmapMode = static_cast<VkSamplerMipmapMode>(obj["mipmapMode"].GetUint());
		info.maxLod = obj["maxLod"].GetFloat();
		info.minLod = obj["minLod"].GetFloat();
		info.mipLodBias = obj["mipLodBias"].GetFloat();
		info.unnormalizedCoordinates = obj["unnormalizedCoordinates"].GetUint();

		if (!iface.enqueue_create_sampler(hash, &info, &replayed_samplers[hash]))
			FOSSILIZE_THROW("Failed to create sampler.");
	}
	iface.wait_enqueue();
}

VkAttachmentDescription *StateReplayer::Impl::parse_render_pass_attachments(const Value &attachments)
{
	auto *infos = allocator.allocate_n_cleared<VkAttachmentDescription>(attachments.Size());
	auto *ret = infos;

	for (auto itr = attachments.Begin(); itr != attachments.End(); ++itr, infos++)
	{
		auto &obj = *itr;
		infos->flags = obj["flags"].GetUint();
		infos->finalLayout = static_cast<VkImageLayout>(obj["finalLayout"].GetUint());
		infos->initialLayout = static_cast<VkImageLayout>(obj["initialLayout"].GetUint());
		infos->format = static_cast<VkFormat>(obj["format"].GetUint());
		infos->loadOp = static_cast<VkAttachmentLoadOp>(obj["loadOp"].GetUint());
		infos->storeOp = static_cast<VkAttachmentStoreOp>(obj["storeOp"].GetUint());
		infos->stencilLoadOp = static_cast<VkAttachmentLoadOp>(obj["stencilLoadOp"].GetUint());
		infos->stencilStoreOp = static_cast<VkAttachmentStoreOp>(obj["stencilStoreOp"].GetUint());
		infos->samples = static_cast<VkSampleCountFlagBits>(obj["samples"].GetUint());
	}

	return ret;
}

VkSubpassDependency *StateReplayer::Impl::parse_render_pass_dependencies(const Value &dependencies)
{
	auto *infos = allocator.allocate_n_cleared<VkSubpassDependency>(dependencies.Size());
	auto *ret = infos;

	for (auto itr = dependencies.Begin(); itr != dependencies.End(); ++itr, infos++)
	{
		auto &obj = *itr;
		infos->dependencyFlags = obj["dependencyFlags"].GetUint();
		infos->dstAccessMask = obj["dstAccessMask"].GetUint();
		infos->srcAccessMask = obj["srcAccessMask"].GetUint();
		infos->dstStageMask = obj["dstStageMask"].GetUint();
		infos->srcStageMask = obj["srcStageMask"].GetUint();
		infos->srcSubpass = obj["srcSubpass"].GetUint();
		infos->dstSubpass = obj["dstSubpass"].GetUint();
	}

	return ret;
}

VkAttachmentReference *StateReplayer::Impl::parse_attachment(const Value &value)
{
	auto *ret = allocator.allocate_cleared<VkAttachmentReference>();
	ret->attachment = value["attachment"].GetUint();
	ret->layout = static_cast<VkImageLayout>(value["layout"].GetUint());
	return ret;
}

VkAttachmentReference *StateReplayer::Impl::parse_attachments(const Value &attachments)
{
	auto *refs = allocator.allocate_n_cleared<VkAttachmentReference>(attachments.Size());
	auto *ret = refs;

	for (auto itr = attachments.Begin(); itr != attachments.End(); ++itr, refs++)
	{
		auto &value = *itr;
		refs->attachment = value["attachment"].GetUint();
		refs->layout = static_cast<VkImageLayout>(value["layout"].GetUint());
	}
	return ret;
}

uint32_t *StateReplayer::Impl::parse_uints(const Value &uints)
{
	auto *u32s = allocator.allocate_n<uint32_t>(uints.Size());
	auto *ret = u32s;
	for (auto itr = uints.Begin(); itr != uints.End(); ++itr, u32s++)
		*u32s = itr->GetUint();
	return ret;
}

VkSubpassDescription *StateReplayer::Impl::parse_render_pass_subpasses(const Value &subpasses)
{
	auto *infos = allocator.allocate_n_cleared<VkSubpassDescription>(subpasses.Size());
	auto *ret = infos;

	for (auto itr = subpasses.Begin(); itr != subpasses.End(); ++itr, infos++)
	{
		auto &obj = *itr;
		infos->flags = obj["flags"].GetUint();
		infos->pipelineBindPoint = static_cast<VkPipelineBindPoint>(obj["pipelineBindPoint"].GetUint());

		if (obj.HasMember("depthStencilAttachment"))
			infos->pDepthStencilAttachment = parse_attachment(obj["depthStencilAttachment"]);

		if (obj.HasMember("resolveAttachments"))
			infos->pResolveAttachments = parse_attachments(obj["resolveAttachments"]);

		if (obj.HasMember("inputAttachments"))
		{
			infos->inputAttachmentCount = obj["inputAttachments"].Size();
			infos->pInputAttachments = parse_attachments(obj["inputAttachments"]);
		}

		if (obj.HasMember("colorAttachments"))
		{
			infos->colorAttachmentCount = obj["colorAttachments"].Size();
			infos->pColorAttachments = parse_attachments(obj["colorAttachments"]);
		}

		if (obj.HasMember("preserveAttachments"))
		{
			infos->preserveAttachmentCount = obj["preserveAttachments"].Size();
			infos->pPreserveAttachments = parse_uints(obj["preserveAttachments"]);
		}
	}

	return ret;
}

void StateReplayer::Impl::parse_render_passes(StateCreatorInterface &iface, const Value &passes)
{
	auto *infos = allocator.allocate_n_cleared<VkRenderPassCreateInfo>(passes.MemberCount());

	unsigned index = 0;
	for (auto itr = passes.MemberBegin(); itr != passes.MemberEnd(); ++itr, index++)
	{
		Hash hash = string_to_uint64(itr->name.GetString());
		if (replayed_samplers.count(hash))
			continue;
		auto &obj = itr->value;
		auto &info = infos[index];
		info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;

		info.flags = obj["flags"].GetUint();

		if (obj.HasMember("attachments"))
		{
			info.attachmentCount = obj["attachments"].Size();
			info.pAttachments = parse_render_pass_attachments(obj["attachments"]);
		}

		if (obj.HasMember("dependencies"))
		{
			info.dependencyCount = obj["dependencies"].Size();
			info.pDependencies = parse_render_pass_dependencies(obj["dependencies"]);
		}

		if (obj.HasMember("subpasses"))
		{
			info.subpassCount = obj["subpasses"].Size();
			info.pSubpasses = parse_render_pass_subpasses(obj["subpasses"]);
		}

		if (!iface.enqueue_create_render_pass(hash, &info, &replayed_render_passes[hash]))
			FOSSILIZE_THROW("Failed to create render pass.");
	}

	iface.wait_enqueue();
}

VkSpecializationMapEntry *StateReplayer::Impl::parse_map_entries(const Value &map_entries)
{
	auto *entries = allocator.allocate_n_cleared<VkSpecializationMapEntry>(map_entries.Size());
	auto *ret = entries;

	for (auto itr = map_entries.Begin(); itr != map_entries.End(); ++itr, entries++)
	{
		auto &obj = *itr;
		entries->constantID = obj["constantID"].GetUint();
		entries->offset = obj["offset"].GetUint();
		entries->size = obj["size"].GetUint();
	}

	return ret;
}

VkSpecializationInfo *StateReplayer::Impl::parse_specialization_info(const Value &spec_info)
{
	auto *spec = allocator.allocate_cleared<VkSpecializationInfo>();
	spec->dataSize = spec_info["dataSize"].GetUint();
	spec->pData = decode_base64(allocator, spec_info["data"].GetString(), spec->dataSize);
	if (spec_info.HasMember("mapEntries"))
	{
		spec->mapEntryCount = spec_info["mapEntries"].Size();
		spec->pMapEntries = parse_map_entries(spec_info["mapEntries"]);
	}
	return spec;
}

void StateReplayer::Impl::parse_compute_pipelines(StateCreatorInterface &iface, ResolverInterface &resolver, const Value &pipelines)
{
	auto *infos = allocator.allocate_n_cleared<VkComputePipelineCreateInfo>(pipelines.MemberCount());

	unsigned index = 0;
	for (auto itr = pipelines.MemberBegin(); itr != pipelines.MemberEnd(); ++itr, index++)
	{
		Hash hash = string_to_uint64(itr->name.GetString());
		if (replayed_compute_pipelines.count(hash))
			continue;
		auto &obj = itr->value;
		auto &info = infos[index];
		info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
		info.flags = obj["flags"].GetUint();
		info.basePipelineIndex = obj["basePipelineIndex"].GetUint();

		auto pipeline = string_to_uint64(obj["basePipelineHandle"].GetString());
		if (pipeline > 0)
		{
			iface.wait_enqueue();
			auto pipeline_iter = replayed_compute_pipelines.find(pipeline);
			if (pipeline_iter == replayed_compute_pipelines.end())
			{
				auto external_state = resolver.resolve(pipeline);
				if (external_state.empty())
					FOSSILIZE_THROW("Failed to find referenced compute pipeline");
				this->parse(iface, resolver, external_state.data(), external_state.size());
				pipeline_iter = replayed_compute_pipelines.find(pipeline);
				if (pipeline_iter == replayed_compute_pipelines.end())
					FOSSILIZE_THROW("Failed to find referenced compute pipeline");
			}
			info.basePipelineHandle = pipeline_iter->second;
		}

		auto layout = string_to_uint64(obj["layout"].GetString());
		if (layout > 0)
			info.layout = replayed_pipeline_layouts[layout];

		auto &stage = obj["stage"];
		info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		info.stage.stage = static_cast<VkShaderStageFlagBits>(stage["stage"].GetUint());

		auto module = string_to_uint64(stage["module"].GetString());
		if (module > 0)
		{
			auto module_iter = replayed_shader_modules.find(module);
			if (module_iter == replayed_shader_modules.end())
			{
				auto external_state = resolver.resolve(module);
				if (external_state.empty())
					FOSSILIZE_THROW("Failed to find referenced shader");
				this->parse(iface, resolver, external_state.data(), external_state.size());
				module_iter = replayed_shader_modules.find(module);
				if (module_iter == replayed_shader_modules.end())
					FOSSILIZE_THROW("Failed find referenced shader module");
			}
			info.stage.module = module_iter->second;
		}

		info.stage.pName = duplicate_string(stage["name"].GetString(), stage["name"].GetStringLength());
		if (stage.HasMember("specializationInfo"))
			info.stage.pSpecializationInfo = parse_specialization_info(stage["specializationInfo"]);

		if (!iface.enqueue_create_compute_pipeline(hash, &info, &replayed_compute_pipelines[hash]))
			FOSSILIZE_THROW("Failed to create compute pipeline.");
	}
	iface.wait_enqueue();
}

VkVertexInputAttributeDescription *StateReplayer::Impl::parse_vertex_attributes(const rapidjson::Value &attributes)
{
	auto *attribs = allocator.allocate_n_cleared<VkVertexInputAttributeDescription>(attributes.Size());
	auto *ret = attribs;

	for (auto itr = attributes.Begin(); itr != attributes.End(); ++itr, attribs++)
	{
		auto &obj = *itr;
		attribs->location = obj["location"].GetUint();
		attribs->binding = obj["binding"].GetUint();
		attribs->offset = obj["offset"].GetUint();
		attribs->format = static_cast<VkFormat>(obj["format"].GetUint());
	}

	return ret;
}

VkVertexInputBindingDescription *StateReplayer::Impl::parse_vertex_bindings(const rapidjson::Value &bindings)
{
	auto *binds = allocator.allocate_n_cleared<VkVertexInputBindingDescription>(bindings.Size());
	auto *ret = binds;

	for (auto itr = bindings.Begin(); itr != bindings.End(); ++itr, binds++)
	{
		auto &obj = *itr;
		binds->binding = obj["binding"].GetUint();
		binds->inputRate = static_cast<VkVertexInputRate>(obj["inputRate"].GetUint());
		binds->stride = obj["stride"].GetUint();
	}

	return ret;
}

VkPipelineVertexInputStateCreateInfo *StateReplayer::Impl::parse_vertex_input_state(const rapidjson::Value &vi)
{
	auto *state = allocator.allocate_cleared<VkPipelineVertexInputStateCreateInfo>();
	state->sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	state->flags = vi["flags"].GetUint();

	if (vi.HasMember("attributes"))
	{
		state->vertexAttributeDescriptionCount = vi["attributes"].Size();
		state->pVertexAttributeDescriptions = parse_vertex_attributes(vi["attributes"]);
	}

	if (vi.HasMember("bindings"))
	{
		state->vertexBindingDescriptionCount = vi["bindings"].Size();
		state->pVertexBindingDescriptions = parse_vertex_bindings(vi["bindings"]);
	}

	return state;
}

VkPipelineDepthStencilStateCreateInfo *StateReplayer::Impl::parse_depth_stencil_state(const rapidjson::Value &ds)
{
	auto *state = allocator.allocate_cleared<VkPipelineDepthStencilStateCreateInfo>();
	state->sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	state->flags = ds["flags"].GetUint();

	state->depthBoundsTestEnable = ds["depthBoundsTestEnable"].GetUint();
	state->depthCompareOp = static_cast<VkCompareOp>(ds["depthCompareOp"].GetUint());
	state->depthTestEnable = ds["depthTestEnable"].GetUint();
	state->depthWriteEnable = ds["depthWriteEnable"].GetUint();
	state->minDepthBounds = ds["minDepthBounds"].GetFloat();
	state->maxDepthBounds = ds["maxDepthBounds"].GetFloat();
	state->stencilTestEnable = ds["stencilTestEnable"].GetUint();
	state->front.compareMask = ds["front"]["compareMask"].GetUint();
	state->front.compareOp = static_cast<VkCompareOp>(ds["front"]["compareOp"].GetUint());
	state->front.depthFailOp = static_cast<VkStencilOp>(ds["front"]["depthFailOp"].GetUint());
	state->front.passOp = static_cast<VkStencilOp>(ds["front"]["passOp"].GetUint());
	state->front.failOp = static_cast<VkStencilOp>(ds["front"]["failOp"].GetUint());
	state->front.reference = ds["front"]["reference"].GetUint();
	state->front.writeMask = ds["front"]["writeMask"].GetUint();
	state->back.compareMask = ds["back"]["compareMask"].GetUint();
	state->back.compareOp = static_cast<VkCompareOp>(ds["back"]["compareOp"].GetUint());
	state->back.depthFailOp = static_cast<VkStencilOp>(ds["back"]["depthFailOp"].GetUint());
	state->back.passOp = static_cast<VkStencilOp>(ds["back"]["passOp"].GetUint());
	state->back.failOp = static_cast<VkStencilOp>(ds["back"]["failOp"].GetUint());
	state->back.reference = ds["back"]["reference"].GetUint();
	state->back.writeMask = ds["back"]["writeMask"].GetUint();

	return state;
}

VkPipelineRasterizationStateCreateInfo *StateReplayer::Impl::parse_rasterization_state(const rapidjson::Value &rs)
{
	auto *state = allocator.allocate_cleared<VkPipelineRasterizationStateCreateInfo>();
	state->sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	state->flags = rs["flags"].GetUint();

	state->cullMode = static_cast<VkCullModeFlags>(rs["cullMode"].GetUint());
	state->depthBiasClamp = rs["depthBiasClamp"].GetFloat();
	state->depthBiasConstantFactor = rs["depthBiasConstantFactor"].GetFloat();
	state->depthBiasSlopeFactor = rs["depthBiasSlopeFactor"].GetFloat();
	state->lineWidth = rs["lineWidth"].GetFloat();
	state->rasterizerDiscardEnable = rs["rasterizerDiscardEnable"].GetUint();
	state->depthBiasEnable = rs["depthBiasEnable"].GetUint();
	state->depthClampEnable = rs["depthClampEnable"].GetUint();
	state->polygonMode = static_cast<VkPolygonMode>(rs["polygonMode"].GetUint());
	state->frontFace = static_cast<VkFrontFace>(rs["frontFace"].GetUint());

	return state;
}

VkPipelineTessellationStateCreateInfo *StateReplayer::Impl::parse_tessellation_state(const rapidjson::Value &tess)
{
	auto *state = allocator.allocate_cleared<VkPipelineTessellationStateCreateInfo>();
	state->sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO;
	state->flags = tess["flags"].GetUint();
	state->patchControlPoints = tess["patchControlPoints"].GetUint();
	return state;
}

VkPipelineInputAssemblyStateCreateInfo *StateReplayer::Impl::parse_input_assembly_state(const rapidjson::Value &ia)
{
	auto *state = allocator.allocate_cleared<VkPipelineInputAssemblyStateCreateInfo>();
	state->sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	state->flags = ia["flags"].GetUint();
	state->primitiveRestartEnable = ia["primitiveRestartEnable"].GetUint();
	state->topology = static_cast<VkPrimitiveTopology>(ia["topology"].GetUint());
	return state;
}

VkPipelineColorBlendAttachmentState *StateReplayer::Impl::parse_blend_attachments(const rapidjson::Value &attachments)
{
	auto *att = allocator.allocate_n_cleared<VkPipelineColorBlendAttachmentState>(attachments.Size());
	auto *ret = att;

	for (auto itr = attachments.Begin(); itr != attachments.End(); ++itr, att++)
	{
		auto &obj = *itr;
		att->blendEnable = obj["blendEnable"].GetUint();
		att->colorWriteMask = obj["colorWriteMask"].GetUint();
		att->alphaBlendOp = static_cast<VkBlendOp>(obj["alphaBlendOp"].GetUint());
		att->colorBlendOp = static_cast<VkBlendOp>(obj["colorBlendOp"].GetUint());
		att->srcColorBlendFactor = static_cast<VkBlendFactor>(obj["srcColorBlendFactor"].GetUint());
		att->dstColorBlendFactor = static_cast<VkBlendFactor>(obj["dstColorBlendFactor"].GetUint());
		att->srcAlphaBlendFactor = static_cast<VkBlendFactor>(obj["srcAlphaBlendFactor"].GetUint());
		att->dstAlphaBlendFactor = static_cast<VkBlendFactor>(obj["dstAlphaBlendFactor"].GetUint());
	}

	return ret;
}

VkPipelineColorBlendStateCreateInfo *StateReplayer::Impl::parse_color_blend_state(const rapidjson::Value &blend)
{
	auto *state = allocator.allocate_cleared<VkPipelineColorBlendStateCreateInfo>();
	state->sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	state->flags = blend["flags"].GetUint();

	state->logicOp = static_cast<VkLogicOp>(blend["logicOp"].GetUint());
	state->logicOpEnable = blend["logicOpEnable"].GetUint();
	for (unsigned i = 0; i < 4; i++)
		state->blendConstants[i] = blend["blendConstants"][i].GetFloat();

	if (blend.HasMember("attachments"))
	{
		state->attachmentCount = blend["attachments"].Size();
		state->pAttachments = parse_blend_attachments(blend["attachments"]);
	}

	return state;
}

VkPipelineMultisampleStateCreateInfo *StateReplayer::Impl::parse_multisample_state(const rapidjson::Value &ms)
{
	auto *state = allocator.allocate_cleared<VkPipelineMultisampleStateCreateInfo>();

	state->sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	state->flags = ms["flags"].GetUint();

	state->alphaToCoverageEnable = ms["alphaToCoverageEnable"].GetUint();
	state->alphaToOneEnable = ms["alphaToOneEnable"].GetUint();
	state->minSampleShading = ms["minSampleShading"].GetFloat();
	if (ms.HasMember("sampleMask"))
		state->pSampleMask = parse_uints(ms["sampleMask"]);
	state->sampleShadingEnable = ms["sampleShadingEnable"].GetUint();
	state->rasterizationSamples = static_cast<VkSampleCountFlagBits>(ms["rasterizationSamples"].GetUint());

	return state;
}

VkPipelineDynamicStateCreateInfo *StateReplayer::Impl::parse_dynamic_state(const rapidjson::Value &dyn)
{
	auto *state = allocator.allocate_cleared<VkPipelineDynamicStateCreateInfo>();

	state->sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	state->flags = dyn["flags"].GetUint();

	if (dyn.HasMember("dynamicState"))
	{
		state->dynamicStateCount = dyn["dynamicState"].Size();
		static_assert(sizeof(VkDynamicState) == sizeof(uint32_t), "Enum size is not 32-bit.");
		state->pDynamicStates = reinterpret_cast<VkDynamicState *>(parse_uints(dyn["dynamicState"]));
	}

	return state;
}

VkViewport *StateReplayer::Impl::parse_viewports(const rapidjson::Value &viewports)
{
	auto *vps = allocator.allocate_n_cleared<VkViewport>(viewports.Size());
	auto *ret = vps;

	for (auto itr = viewports.Begin(); itr != viewports.End(); ++itr, vps++)
	{
		auto &obj = *itr;
		vps->x = obj["x"].GetFloat();
		vps->y = obj["y"].GetFloat();
		vps->width = obj["width"].GetFloat();
		vps->height = obj["height"].GetFloat();
		vps->minDepth = obj["minDepth"].GetFloat();
		vps->maxDepth = obj["maxDepth"].GetFloat();
	}

	return ret;
}

VkRect2D *StateReplayer::Impl::parse_scissors(const rapidjson::Value &scissors)
{
	auto *sci = allocator.allocate_n_cleared<VkRect2D>(scissors.Size());
	auto *ret = sci;

	for (auto itr = scissors.Begin(); itr != scissors.End(); ++itr, sci++)
	{
		auto &obj = *itr;
		sci->offset.x = obj["x"].GetInt();
		sci->offset.y = obj["y"].GetInt();
		sci->extent.width = obj["width"].GetUint();
		sci->extent.height = obj["height"].GetUint();
	}

	return ret;
}

VkPipelineViewportStateCreateInfo *StateReplayer::Impl::parse_viewport_state(const rapidjson::Value &vp)
{
	auto *state = allocator.allocate_cleared<VkPipelineViewportStateCreateInfo>();

	state->sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	state->flags = vp["flags"].GetUint();

	state->scissorCount = vp["scissorCount"].GetUint();
	if (vp.HasMember("scissors"))
		state->pScissors = parse_scissors(vp["scissors"]);

	state->viewportCount = vp["viewportCount"].GetUint();
	if (vp.HasMember("viewports"))
		state->pViewports = parse_viewports(vp["viewports"]);

	return state;
}

VkPipelineShaderStageCreateInfo *StateReplayer::Impl::parse_stages(StateCreatorInterface &iface, ResolverInterface &resolver, const rapidjson::Value &stages)
{
	auto *state = allocator.allocate_n_cleared<VkPipelineShaderStageCreateInfo>(stages.Size());
	auto *ret = state;

	for (auto itr = stages.Begin(); itr != stages.End(); ++itr, state++)
	{
		auto &obj = *itr;
		state->sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		state->flags = obj["flags"].GetUint();
		state->stage = static_cast<VkShaderStageFlagBits>(obj["stage"].GetUint());
		state->pName = duplicate_string(obj["name"].GetString(), obj["name"].GetStringLength());
		if (obj.HasMember("specializationInfo"))
			state->pSpecializationInfo = parse_specialization_info(obj["specializationInfo"]);

		auto module = string_to_uint64(obj["module"].GetString());
		if (module > 0)
		{
			auto module_iter = replayed_shader_modules.find(module);
			if (module_iter == replayed_shader_modules.end())
			{
				auto external_state = resolver.resolve(module);
				if (external_state.empty())
					FOSSILIZE_THROW("Failed to find referenced shader");
				this->parse(iface, resolver, external_state.data(), external_state.size());
				module_iter = replayed_shader_modules.find(module);
				if (module_iter == replayed_shader_modules.end())
					FOSSILIZE_THROW("Failed to find referenced shader module");
			}
			state->module = module_iter->second;
		}
	}

	return ret;
}

void StateReplayer::Impl::parse_graphics_pipelines(StateCreatorInterface &iface, ResolverInterface &resolver, const Value &pipelines)
{
	auto *infos = allocator.allocate_n_cleared<VkGraphicsPipelineCreateInfo>(pipelines.MemberCount());

	unsigned index = 0;
	for (auto itr = pipelines.MemberBegin(); itr != pipelines.MemberEnd(); ++itr, index++)
	{
		Hash hash = string_to_uint64(itr->name.GetString());
		if (replayed_graphics_pipelines.count(hash))
			continue;
		auto &obj = itr->value;
		auto &info = infos[index];
		info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
		info.flags = obj["flags"].GetUint();
		info.basePipelineIndex = obj["basePipelineIndex"].GetUint();

		auto pipeline = string_to_uint64(obj["basePipelineHandle"].GetString());
		if (pipeline > 0)
		{
			iface.wait_enqueue();
			auto pipeline_iter = replayed_graphics_pipelines.find(pipeline);
			if (pipeline_iter == replayed_graphics_pipelines.end())
			{
				auto external_state = resolver.resolve(pipeline);
				if (external_state.empty())
					FOSSILIZE_THROW("Failed to find referenced graphics pipeline");
				this->parse(iface, resolver, external_state.data(), external_state.size());
				pipeline_iter = replayed_graphics_pipelines.find(pipeline);
				if (pipeline_iter == replayed_graphics_pipelines.end())
					FOSSILIZE_THROW("Failed to find referenced graphics pipeline");
			}
			info.basePipelineHandle = pipeline_iter->second;
		}

		auto layout = string_to_uint64(obj["layout"].GetString());
		if (layout > 0)
			info.layout = replayed_pipeline_layouts[layout];

		auto render_pass = string_to_uint64(obj["renderPass"].GetString());
		if (render_pass > 0)
			info.renderPass = replayed_render_passes[render_pass];

		info.subpass = obj["subpass"].GetUint();

		if (obj.HasMember("stages"))
		{
			info.stageCount = obj["stages"].Size();
			info.pStages = parse_stages(iface, resolver, obj["stages"]);
		}

		if (obj.HasMember("rasterizationState"))
			info.pRasterizationState = parse_rasterization_state(obj["rasterizationState"]);
		if (obj.HasMember("tessellationState"))
			info.pTessellationState = parse_tessellation_state(obj["tessellationState"]);
		if (obj.HasMember("colorBlendState"))
			info.pColorBlendState = parse_color_blend_state(obj["colorBlendState"]);
		if (obj.HasMember("depthStencilState"))
			info.pDepthStencilState = parse_depth_stencil_state(obj["depthStencilState"]);
		if (obj.HasMember("dynamicState"))
			info.pDynamicState = parse_dynamic_state(obj["dynamicState"]);
		if (obj.HasMember("viewportState"))
			info.pViewportState = parse_viewport_state(obj["viewportState"]);
		if (obj.HasMember("multisampleState"))
			info.pMultisampleState = parse_multisample_state(obj["multisampleState"]);
		if (obj.HasMember("inputAssemblyState"))
			info.pInputAssemblyState = parse_input_assembly_state(obj["inputAssemblyState"]);
		if (obj.HasMember("vertexInputState"))
			info.pVertexInputState = parse_vertex_input_state(obj["vertexInputState"]);

		if (!iface.enqueue_create_graphics_pipeline(hash, &info, &replayed_graphics_pipelines[hash]))
			FOSSILIZE_THROW("Failed to create graphics pipeline.");
	}

	iface.wait_enqueue();
}

StateReplayer::StateReplayer()
{
	impl.reset(new Impl);
}

StateReplayer::~StateReplayer()
{
}

ScratchAllocator &StateReplayer::get_allocator()
{
	return impl->allocator;
}

void StateReplayer::parse(StateCreatorInterface &iface, ResolverInterface &resolver, const void *buffer, size_t size)
{
	impl->parse(iface, resolver, buffer, size);
}

void StateReplayer::Impl::parse(StateCreatorInterface &iface, ResolverInterface &resolver, const void *buffer_, size_t size)
{
	Document doc;
	doc.Parse(reinterpret_cast<const char *>(buffer_), size);

	if (doc.HasParseError())
		FOSSILIZE_THROW("JSON parse error.");

	if (doc["version"].GetInt() != FOSSILIZE_FORMAT_VERSION)
		FOSSILIZE_THROW("JSON version mismatches.");

	if (doc.HasMember("shaderModules"))
		parse_shader_modules(iface, doc["shaderModules"]);
	else
		iface.set_num_shader_modules(0);

	if (doc.HasMember("samplers"))
		parse_samplers(iface, doc["samplers"]);
	else
		iface.set_num_samplers(0);

	if (doc.HasMember("setLayouts"))
		parse_descriptor_set_layouts(iface, doc["setLayouts"]);
	else
		iface.set_num_descriptor_set_layouts(0);

	if (doc.HasMember("pipelineLayouts"))
		parse_pipeline_layouts(iface, doc["pipelineLayouts"]);
	else
		iface.set_num_pipeline_layouts(0);

	if (doc.HasMember("renderPasses"))
		parse_render_passes(iface, doc["renderPasses"]);
	else
		iface.set_num_render_passes(0);

	if (doc.HasMember("computePipelines"))
		parse_compute_pipelines(iface, resolver, doc["computePipelines"]);
	else
		iface.set_num_compute_pipelines(0);

	if (doc.HasMember("graphicsPipelines"))
		parse_graphics_pipelines(iface, resolver, doc["graphicsPipelines"]);
	else
		iface.set_num_graphics_pipelines(0);
}

template <typename T>
T *StateReplayer::Impl::copy(const T *src, size_t count)
{
	auto *new_data = allocator.allocate_n<T>(count);
	if (new_data)
		std::copy(src, src + count, new_data);
	return new_data;
}

template <typename T>
T *StateRecorder::Impl::copy(const T *src, size_t count)
{
	auto *new_data = allocator.allocate_n<T>(count);
	if (new_data)
		std::copy(src, src + count, new_data);
	return new_data;
}

ScratchAllocator::Block::Block(size_t size)
{
	blob.resize(size);
}

void ScratchAllocator::add_block(size_t minimum_size)
{
	if (minimum_size < 64 * 1024)
		minimum_size = 64 * 1024;
	blocks.emplace_back(minimum_size);
}

void *ScratchAllocator::allocate_raw_cleared(size_t size, size_t alignment)
{
	void *ret = allocate_raw(size, alignment);
	if (ret)
		memset(ret, 0, size);
	return ret;
}

void *ScratchAllocator::allocate_raw(size_t size, size_t alignment)
{
	if (blocks.empty())
		add_block(size + alignment);

	auto &block = blocks.back();

	size_t offset = (block.offset + alignment - 1) & ~(alignment - 1);
	size_t required_size = offset + size;
	if (required_size <= block.blob.size())
	{
		void *ret = block.blob.data() + offset;
		block.offset = required_size;
		return ret;
	}

	add_block(size + alignment);
	return allocate_raw(size, alignment);
}

ScratchAllocator &StateRecorder::get_allocator()
{
	return impl->allocator;
}

void StateRecorder::record_descriptor_set_layout(VkDescriptorSetLayout set_layout, const VkDescriptorSetLayoutCreateInfo &create_info)
{
	std::lock_guard<std::mutex> lock(impl->record_lock);
	impl->record_queue.push({ api_object_cast<uint64_t>(set_layout), reinterpret_cast<void *>(impl->copy_descriptor_set_layout(&create_info)) });
	impl->record_cv.notify_one();
}

void StateRecorder::record_pipeline_layout(VkPipelineLayout pipeline_layout, const VkPipelineLayoutCreateInfo &create_info)
{
	std::lock_guard<std::mutex> lock(impl->record_lock);
	impl->record_queue.push({ api_object_cast<uint64_t>(pipeline_layout), reinterpret_cast<void *>(impl->copy_pipeline_layout(&create_info)) });
	impl->record_cv.notify_one();
}

void StateRecorder::record_sampler(VkSampler sampler, const VkSamplerCreateInfo &create_info)
{
	if (create_info.pNext)
		FOSSILIZE_THROW("pNext in VkSamplerCreateInfo not supported.");
	std::lock_guard<std::mutex> lock(impl->record_lock);
	impl->record_queue.push({ api_object_cast<uint64_t>(sampler), reinterpret_cast<void *>(impl->copy_sampler(&create_info)) });
	impl->record_cv.notify_one();
}

void StateRecorder::record_graphics_pipeline(VkPipeline pipeline, const VkGraphicsPipelineCreateInfo &create_info)
{
	if (create_info.pNext)
		FOSSILIZE_THROW("pNext in VkGraphicsPipelineCreateInfo not supported.");
	std::lock_guard<std::mutex> lock(impl->record_lock);
	impl->record_queue.push({ api_object_cast<uint64_t>(pipeline), reinterpret_cast<void *>(impl->copy_graphics_pipeline(&create_info)) });
	impl->record_cv.notify_one();
}

void StateRecorder::record_compute_pipeline(VkPipeline pipeline, const VkComputePipelineCreateInfo &create_info)
{
	if (create_info.pNext)
		FOSSILIZE_THROW("pNext in VkComputePipelineCreateInfo not supported.");
	std::lock_guard<std::mutex> lock(impl->record_lock);
	impl->record_queue.push({ api_object_cast<uint64_t>(pipeline), reinterpret_cast<void *>(impl->copy_compute_pipeline(&create_info)) });
	impl->record_cv.notify_one();
}

void StateRecorder::record_render_pass(VkRenderPass render_pass, const VkRenderPassCreateInfo &create_info)
{
	if (create_info.pNext)
		FOSSILIZE_THROW("pNext in VkRenderPassCreateInfo not supported.");
	std::lock_guard<std::mutex> lock(impl->record_lock);
	impl->record_queue.push({ api_object_cast<uint64_t>(render_pass), reinterpret_cast<void *>(impl->copy_render_pass(&create_info)) });
	impl->record_cv.notify_one();
}

void StateRecorder::record_shader_module(VkShaderModule module, const VkShaderModuleCreateInfo &create_info)
{
	if (create_info.pNext)
		FOSSILIZE_THROW("pNext in VkShaderModuleCreateInfo not supported.");
	std::lock_guard<std::mutex> lock(impl->record_lock);
	impl->record_queue.push({ api_object_cast<uint64_t>(module), reinterpret_cast<void *>(impl->copy_shader_module(&create_info)) });
	impl->record_cv.notify_one();
}

void StateRecorder::record_end()
{
	// Signal end of recording with empty work item
	std::lock_guard lock(impl->record_lock);
	impl->record_queue.push({ 0, nullptr });
	impl->record_cv.notify_one();
}

Hash StateRecorder::get_hash_for_compute_pipeline_handle(VkPipeline pipeline) const
{
	auto itr = impl->compute_pipeline_to_index.find(pipeline);
	if (itr == end(impl->compute_pipeline_to_index))
		FOSSILIZE_THROW("Handle is not registered.");
	else
		return itr->second;
}

Hash StateRecorder::get_hash_for_graphics_pipeline_handle(VkPipeline pipeline) const
{
	auto itr = impl->graphics_pipeline_to_index.find(pipeline);
	if (itr == end(impl->graphics_pipeline_to_index))
		FOSSILIZE_THROW("Handle is not registered.");
	else
		return itr->second;
}

Hash StateRecorder::get_hash_for_sampler(VkSampler sampler) const
{
	auto itr = impl->sampler_to_index.find(sampler);
	if (itr == end(impl->sampler_to_index))
		FOSSILIZE_THROW("Handle is not registered.");
	else
		return itr->second;
}

Hash StateRecorder::get_hash_for_shader_module(VkShaderModule module) const
{
	auto itr = impl->shader_module_to_index.find(module);
	if (itr == end(impl->shader_module_to_index))
		FOSSILIZE_THROW("Handle is not registered.");
	else
		return itr->second;
}

Hash StateRecorder::get_hash_for_pipeline_layout(VkPipelineLayout layout) const
{
	auto itr = impl->pipeline_layout_to_index.find(layout);
	if (itr == end(impl->pipeline_layout_to_index))
		FOSSILIZE_THROW("Handle is not registered.");
	else
		return itr->second;
}

Hash StateRecorder::get_hash_for_descriptor_set_layout(VkDescriptorSetLayout layout) const
{
	auto itr = impl->descriptor_set_layout_to_index.find(layout);
	if (itr == end(impl->descriptor_set_layout_to_index))
		FOSSILIZE_THROW("Handle is not registered.");
	else
		return itr->second;
}

Hash StateRecorder::get_hash_for_render_pass(VkRenderPass render_pass) const
{
	auto itr = impl->render_pass_to_index.find(render_pass);
	if (itr == end(impl->render_pass_to_index))
		FOSSILIZE_THROW("Handle is not registered.");
	else
		return itr->second;
}

VkShaderModuleCreateInfo *StateRecorder::Impl::copy_shader_module(const VkShaderModuleCreateInfo *create_info)
{
	auto *info = copy(create_info, 1);
	info->pCode = copy(info->pCode, info->codeSize / sizeof(uint32_t));
	return info;
}

VkSamplerCreateInfo *StateRecorder::Impl::copy_sampler(const VkSamplerCreateInfo *create_info)
{
	return copy(create_info, 1);
}

VkDescriptorSetLayoutCreateInfo *StateRecorder::Impl::copy_descriptor_set_layout(
	const VkDescriptorSetLayoutCreateInfo *create_info)
{
	auto *info = copy(create_info, 1);
	info->pBindings = copy(info->pBindings, info->bindingCount);

	for (uint32_t i = 0; i < info->bindingCount; i++)
	{
		auto &b = const_cast<VkDescriptorSetLayoutBinding &>(info->pBindings[i]);
		if (b.pImmutableSamplers &&
		    (b.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER ||
		     b.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER))
		{
			b.pImmutableSamplers = copy(b.pImmutableSamplers, b.descriptorCount);
		}
	}

	return info;
}

VkPipelineLayoutCreateInfo *StateRecorder::Impl::copy_pipeline_layout(const VkPipelineLayoutCreateInfo *create_info)
{
	auto *info = copy(create_info, 1);
	info->pPushConstantRanges = copy(info->pPushConstantRanges, info->pushConstantRangeCount);
	info->pSetLayouts = copy(info->pSetLayouts, info->setLayoutCount);
	return info;
}

VkSpecializationInfo *StateRecorder::Impl::copy_specialization_info(const VkSpecializationInfo *info)
{
	auto *ret = copy(info, 1);
	ret->pMapEntries = copy(ret->pMapEntries, ret->mapEntryCount);
	ret->pData = copy(static_cast<const uint8_t *>(ret->pData), ret->dataSize);
	return ret;
}

VkComputePipelineCreateInfo *StateRecorder::Impl::copy_compute_pipeline(const VkComputePipelineCreateInfo *create_info)
{
	auto *info = copy(create_info, 1);
	if (info->stage.pSpecializationInfo)
		info->stage.pSpecializationInfo = copy_specialization_info(info->stage.pSpecializationInfo);
	if (info->stage.pNext)
		FOSSILIZE_THROW("pNext in VkPipelineShaderStageCreateInfo not supported.");
	info->stage.pName = copy(info->stage.pName, strlen(info->stage.pName) + 1);
	return info;
}

VkGraphicsPipelineCreateInfo *StateRecorder::Impl::copy_graphics_pipeline(const VkGraphicsPipelineCreateInfo *create_info)
{
	auto *info = copy(create_info, 1);

	info->pStages = copy(info->pStages, info->stageCount);
	if (info->pTessellationState)
	{
		if (info->pTessellationState->pNext)
			FOSSILIZE_THROW("pNext in VkPipelineTessellationStateCreateInfo not supported.");
		info->pTessellationState = copy(info->pTessellationState, 1);
	}

	if (info->pColorBlendState)
	{
		if (info->pColorBlendState->pNext)
			FOSSILIZE_THROW("pNext in VkPipelineColorBlendStateCreateInfo not supported.");
		info->pColorBlendState = copy(info->pColorBlendState, 1);
	}

	if (info->pVertexInputState)
	{
		if (info->pColorBlendState->pNext)
			FOSSILIZE_THROW("pNext in VkPipelineTessellationStateCreateInfo not supported.");
		info->pVertexInputState = copy(info->pVertexInputState, 1);
	}

	if (info->pMultisampleState)
	{
		if (info->pMultisampleState->pNext)
			FOSSILIZE_THROW("pNext in VkPipelineMultisampleStateCreateInfo not supported.");
		info->pMultisampleState = copy(info->pMultisampleState, 1);
	}

	if (info->pVertexInputState)
	{
		if (info->pVertexInputState->pNext)
			FOSSILIZE_THROW("pNext in VkPipelineVertexInputStateCreateInfo not supported.");
		info->pVertexInputState = copy(info->pVertexInputState, 1);
	}

	if (info->pViewportState)
	{
		if (info->pViewportState->pNext)
			FOSSILIZE_THROW("pNext in VkPipelineViewportStateCreateInfo not supported.");
		info->pViewportState = copy(info->pViewportState, 1);
	}

	if (info->pInputAssemblyState)
	{
		if (info->pInputAssemblyState->pNext)
			FOSSILIZE_THROW("pNext in VkPipelineInputAssemblyStateCreateInfo not supported.");
		info->pInputAssemblyState = copy(info->pInputAssemblyState, 1);
	}

	if (info->pDepthStencilState)
	{
		if (info->pDepthStencilState->pNext)
			FOSSILIZE_THROW("pNext in VkPipelineDepthStencilStateCreateInfo not supported.");
		info->pDepthStencilState = copy(info->pDepthStencilState, 1);
	}

	if (info->pRasterizationState)
	{
		if (info->pRasterizationState->pNext)
			FOSSILIZE_THROW("pNext in VkPipelineRasterizationCreateInfo not supported.");
		info->pRasterizationState = copy(info->pRasterizationState, 1);
	}

	if (info->pDynamicState)
	{
		if (info->pDynamicState->pNext)
			FOSSILIZE_THROW("pNext in VkPipelineDynamicStateCreateInfo not supported.");
		info->pDynamicState = copy(info->pDynamicState, 1);
	}

	for (uint32_t i = 0; i < info->stageCount; i++)
	{
		auto &stage = const_cast<VkPipelineShaderStageCreateInfo &>(info->pStages[i]);
		if (stage.pNext)
			FOSSILIZE_THROW("pNext in VkPipelineShaderStageCreateInfo not supported.");
		stage.pName = copy(stage.pName, strlen(stage.pName) + 1);
		if (stage.pSpecializationInfo)
			stage.pSpecializationInfo = copy_specialization_info(stage.pSpecializationInfo);
	}

	if (info->pColorBlendState)
	{
		auto &blend = const_cast<VkPipelineColorBlendStateCreateInfo &>(*info->pColorBlendState);
		blend.pAttachments = copy(blend.pAttachments, blend.attachmentCount);
	}

	if (info->pVertexInputState)
	{
		auto &vs = const_cast<VkPipelineVertexInputStateCreateInfo &>(*info->pVertexInputState);
		vs.pVertexAttributeDescriptions = copy(vs.pVertexAttributeDescriptions, vs.vertexAttributeDescriptionCount);
		vs.pVertexBindingDescriptions = copy(vs.pVertexBindingDescriptions, vs.vertexBindingDescriptionCount);
	}

	if (info->pMultisampleState)
	{
		auto &ms = const_cast<VkPipelineMultisampleStateCreateInfo &>(*info->pMultisampleState);
		if (ms.pSampleMask)
			ms.pSampleMask = copy(ms.pSampleMask, (ms.rasterizationSamples + 31) / 32);
	}

	if (info->pDynamicState)
	{
		const_cast<VkPipelineDynamicStateCreateInfo *>(info->pDynamicState)->pDynamicStates =
				copy(info->pDynamicState->pDynamicStates, info->pDynamicState->dynamicStateCount);
	}

	return info;
}

VkRenderPassCreateInfo *StateRecorder::Impl::copy_render_pass(const VkRenderPassCreateInfo *create_info)
{
	auto *info = copy(create_info, 1);
	info->pAttachments = copy(info->pAttachments, info->attachmentCount);
	info->pSubpasses = copy(info->pSubpasses, info->subpassCount);
	info->pDependencies = copy(info->pDependencies, info->dependencyCount);

	for (uint32_t i = 0; i < info->subpassCount; i++)
	{
		auto &sub = const_cast<VkSubpassDescription &>(info->pSubpasses[i]);
		if (sub.pDepthStencilAttachment)
			sub.pDepthStencilAttachment = copy(sub.pDepthStencilAttachment, 1);
		if (sub.pColorAttachments)
			sub.pColorAttachments = copy(sub.pColorAttachments, sub.colorAttachmentCount);
		if (sub.pResolveAttachments)
			sub.pResolveAttachments = copy(sub.pResolveAttachments, sub.colorAttachmentCount);
		if (sub.pInputAttachments)
			sub.pInputAttachments = copy(sub.pInputAttachments, sub.inputAttachmentCount);
		if (sub.pPreserveAttachments)
			sub.pPreserveAttachments = copy(sub.pPreserveAttachments, sub.preserveAttachmentCount);
	}

	return info;
}

VkSampler StateRecorder::Impl::remap_sampler_handle(VkSampler sampler) const
{
	auto itr = sampler_to_index.find(sampler);
	if (itr == end(sampler_to_index))
		FOSSILIZE_THROW("Cannot find sampler in hashmap.");
	return api_object_cast<VkSampler>(uint64_t(itr->second));
}

VkDescriptorSetLayout StateRecorder::Impl::remap_descriptor_set_layout_handle(VkDescriptorSetLayout layout) const
{
	auto itr = descriptor_set_layout_to_index.find(layout);
	if (itr == end(descriptor_set_layout_to_index))
		FOSSILIZE_THROW("Cannot find descriptor set layout in hashmap.");
	return api_object_cast<VkDescriptorSetLayout>(uint64_t(itr->second));
}

VkPipelineLayout StateRecorder::Impl::remap_pipeline_layout_handle(VkPipelineLayout layout) const
{
	auto itr = pipeline_layout_to_index.find(layout);
	if (itr == end(pipeline_layout_to_index))
		FOSSILIZE_THROW("Cannot find pipeline layout in hashmap.");
	return api_object_cast<VkPipelineLayout>(uint64_t(itr->second));
}

VkShaderModule StateRecorder::Impl::remap_shader_module_handle(VkShaderModule module) const
{
	auto itr = shader_module_to_index.find(module);
	if (itr == end(shader_module_to_index))
		FOSSILIZE_THROW("Cannot find shader module in hashmap.");
	return api_object_cast<VkShaderModule>(uint64_t(itr->second));
}

VkRenderPass StateRecorder::Impl::remap_render_pass_handle(VkRenderPass render_pass) const
{
	auto itr = render_pass_to_index.find(render_pass);
	if (itr == end(render_pass_to_index))
		FOSSILIZE_THROW("Cannot find render pass in hashmap.");
	return api_object_cast<VkRenderPass>(uint64_t(itr->second));
}

VkPipeline StateRecorder::Impl::remap_graphics_pipeline_handle(VkPipeline pipeline) const
{
	auto itr = graphics_pipeline_to_index.find(pipeline);
	if (itr == end(graphics_pipeline_to_index))
		FOSSILIZE_THROW("Cannot find graphics pipeline in hashmap.");
	return api_object_cast<VkPipeline>(uint64_t(itr->second));
}

VkPipeline StateRecorder::Impl::remap_compute_pipeline_handle(VkPipeline pipeline) const
{
	auto itr = compute_pipeline_to_index.find(pipeline);
	if (itr == end(compute_pipeline_to_index))
		FOSSILIZE_THROW("Cannot find compute pipeline in hashmap.");
	return api_object_cast<VkPipeline>(uint64_t(itr->second));
}

void StateRecorder::Impl::remap_descriptor_set_layout_ci(VkDescriptorSetLayoutCreateInfo *info)
{
	for (uint32_t i = 0; i < info->bindingCount; i++)
	{
		auto &b = const_cast<VkDescriptorSetLayoutBinding &>(info->pBindings[i]);
		if (b.pImmutableSamplers &&
		    (b.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER ||
		     b.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER))
		{
			auto *samplers = const_cast<VkSampler *>(b.pImmutableSamplers);
			for (uint32_t j = 0; j < b.descriptorCount; j++)
				samplers[j] = remap_sampler_handle(samplers[j]);
		}
	}
}

void StateRecorder::Impl::remap_pipeline_layout_ci(VkPipelineLayoutCreateInfo *info)
{
	for (uint32_t i = 0; i < info->setLayoutCount; i++)
		const_cast<VkDescriptorSetLayout *>(info->pSetLayouts)[i] = remap_descriptor_set_layout_handle(info->pSetLayouts[i]);
}

void StateRecorder::Impl::remap_shader_module_ci(VkShaderModuleCreateInfo *info)
{
	// nothing to do
}

void StateRecorder::Impl::remap_graphics_pipeline_ci(VkGraphicsPipelineCreateInfo *info)
{
	info->renderPass = remap_render_pass_handle(info->renderPass);
	info->layout = remap_pipeline_layout_handle(info->layout);
	if (info->basePipelineHandle != VK_NULL_HANDLE)
		info->basePipelineHandle = remap_graphics_pipeline_handle(info->basePipelineHandle);

	for (uint32_t i = 0; i < info->stageCount; i++)
	{
		auto &stage = const_cast<VkPipelineShaderStageCreateInfo &>(info->pStages[i]);
		stage.module = remap_shader_module_handle(stage.module);
	}
}

void StateRecorder::Impl::remap_compute_pipeline_ci(VkComputePipelineCreateInfo *info)
{
	info->stage.module = remap_shader_module_handle(info->stage.module);

	if (info->basePipelineHandle != VK_NULL_HANDLE)
		info->basePipelineHandle = remap_compute_pipeline_handle(info->basePipelineHandle);

	info->layout = remap_pipeline_layout_handle(info->layout);
}

void StateRecorder::Impl::remap_sampler_ci(VkSamplerCreateInfo *info)
{
	// nothing to do
}

void StateRecorder::Impl::remap_render_pass_ci(VkRenderPassCreateInfo *create_info)
{
	// nothing to do
}

static void write_buffer(const std::string &json_dir, Hash hash, const vector<uint8_t> &bytes)
{
	try
	{
		char filename[22]; // 16 digits + ".json" + null
		sprintf(filename, "%016" PRIX64 ".json", hash);
		auto path = json_dir + filename;
		FILE *file = fopen(path.c_str(), "wb");
		if (file)
		{
			if (fwrite(bytes.data(), 1, bytes.size(), file) != bytes.size())
				LOGE("Failed to write serialized state to disk.\n");
			fclose(file);
		}
		else
		{
			LOGE("Failed to open file for writing: \"%s\".\n", path.c_str());
		}
	}
	catch (const std::exception &e)
	{
		LOGE("Failed to serialize: \"%s\".\n", e.what());
	}
}

void StateRecorder::Impl::record_task(StateRecorder *recorder) {
	for(;;)
	{
		std::unique_lock<std::mutex> lock(recorder->impl->record_lock);
		recorder->impl->record_cv.wait(lock, [&] {return !recorder->impl->record_queue.empty(); });
		auto record_item = recorder->impl->record_queue.front();
		recorder->impl->record_queue.pop();
		lock.unlock();

		if (!record_item.create_info)
			return;

		try
		{
			switch (*reinterpret_cast<VkStructureType*>(record_item.create_info))
			{
			case VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO:
			{
				auto *create_info = reinterpret_cast<VkSamplerCreateInfo *>(record_item.create_info);
				auto hash = Hashing::compute_hash_sampler(*recorder, *create_info);
				recorder->impl->sampler_to_index[(VkSampler)record_item.handle] = hash;
				if (!recorder->impl->samplers.count(hash))
					recorder->impl->samplers[hash] = *create_info;
				break;
			}
			case VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO:
			{
				auto *create_info = reinterpret_cast<VkDescriptorSetLayoutCreateInfo *>(record_item.create_info);
				auto hash = Hashing::compute_hash_descriptor_set_layout(*recorder, *create_info);
				recorder->impl->descriptor_set_layout_to_index[(VkDescriptorSetLayout)record_item.handle] = hash;
				if (!recorder->impl->descriptor_sets.count(hash))
				{
					recorder->impl->remap_descriptor_set_layout_ci(create_info);
					recorder->impl->descriptor_sets[hash] = *create_info;
				}
				break;
			}
			case VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO:
			{
				auto *create_info = reinterpret_cast<VkPipelineLayoutCreateInfo *>(record_item.create_info);
				auto hash = Hashing::compute_hash_pipeline_layout(*recorder, *create_info);
				recorder->impl->pipeline_layout_to_index[(VkPipelineLayout)record_item.handle] = hash;
				if (!recorder->impl->pipeline_layouts.count(hash))
				{
					recorder->impl->remap_pipeline_layout_ci(create_info);
					recorder->impl->pipeline_layouts[hash] = *create_info;
				}
				break;
			}
			case VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO:
			{
				auto *create_info = reinterpret_cast<VkRenderPassCreateInfo *>(record_item.create_info);
				auto hash = Hashing::compute_hash_render_pass(*recorder, *create_info);
				recorder->impl->render_pass_to_index[(VkRenderPass)record_item.handle] = hash;
				if (!recorder->impl->render_passes.count(hash))
					recorder->impl->render_passes[hash] = *create_info;
				break;
			}
			case VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO:
			{
				auto *create_info = reinterpret_cast<VkShaderModuleCreateInfo *>(record_item.create_info);
				auto hash = Hashing::compute_hash_shader_module(*recorder, *create_info);
				recorder->impl->shader_module_to_index[(VkShaderModule)record_item.handle] = hash;
				if (!recorder->impl->shader_modules.count(hash))
				{
					recorder->impl->shader_modules[hash] = *create_info;
					lock_guard<mutex> lock(recorder->impl->serialization_lock);
					write_buffer(recorder->impl->serialization_path, hash, recorder->serialize_shader_module(hash));
				}
				break;
			}
			case VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO:
			{
				auto *create_info = reinterpret_cast<VkGraphicsPipelineCreateInfo *>(record_item.create_info);
				auto hash = Hashing::compute_hash_graphics_pipeline(*recorder, *create_info);
				recorder->impl->graphics_pipeline_to_index[(VkPipeline)record_item.handle] = hash;
				if (!recorder->impl->graphics_pipelines.count(hash))
				{
					recorder->impl->remap_graphics_pipeline_ci(create_info);
					recorder->impl->graphics_pipelines[hash] = *create_info;
					lock_guard<mutex> lock(recorder->impl->serialization_lock);
					write_buffer(recorder->impl->serialization_path, hash, recorder->serialize_graphics_pipeline(hash));
				}
				break;
			}
			case VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO:
			{
				auto *create_info = reinterpret_cast<VkComputePipelineCreateInfo *>(record_item.create_info);
				auto hash = Hashing::compute_hash_compute_pipeline(*recorder, *create_info);
				recorder->impl->compute_pipeline_to_index[(VkPipeline)record_item.handle] = hash;
				if (!recorder->impl->compute_pipelines.count(hash))
				{
					recorder->impl->remap_compute_pipeline_ci(create_info);
					recorder->impl->compute_pipelines[hash] = *create_info;
					lock_guard<mutex> lock(recorder->impl->serialization_lock);
					write_buffer(recorder->impl->serialization_path, hash, recorder->serialize_compute_pipeline(hash));
				}
				break;
			}
			default:
				break;
			}
		}
		catch (const std::exception &e)
		{
			LOGE("%s\n", e.what());
		}
	}
}

static char base64(uint32_t v)
{
	if (v == 63)
		return '/';
	else if (v == 62)
		return '+';
	else if (v >= 52)
		return char('0' + (v - 52));
	else if (v >= 26)
		return char('a' + (v - 26));
	else
		return char('A' + v);
}

static std::string encode_base64(const void *data_, size_t size)
{
	auto *data = static_cast<const uint8_t *>(data_);
	size_t num_chars = 4 * ((size + 2) / 3);
	std::string ret;
	ret.reserve(num_chars);

	for (size_t i = 0; i < size; i += 3)
	{
		uint32_t code = data[i] << 16;
		if (i + 1 < size)
			code |= data[i + 1] << 8;
		if (i + 2 < size)
			code |= data[i + 2] << 0;

		auto c0 = base64((code >> 18) & 63);
		auto c1 = base64((code >> 12) & 63);
		auto c2 = base64((code >>  6) & 63);
		auto c3 = base64((code >>  0) & 63);

		auto outbytes = std::min(size - i, size_t(3));
		if (outbytes == 1)
		{
			c2 = '=';
			c3 = '=';
		}
		else if (outbytes == 2)
			c3 = '=';

		ret.push_back(c0);
		ret.push_back(c1);
		ret.push_back(c2);
		ret.push_back(c3);
	}

	return ret;
}

template <typename Allocator>
static Value uint64_string(const uint64_t value, Allocator &alloc)
{
	char str[17]; // 16 digits + null
	sprintf(str, "%016" PRIX64, value);
	return Value(str, alloc);
}

template <typename Allocator>
static Value json_value(const VkSamplerCreateInfo& sampler, Allocator& alloc)
{
	Value s(kObjectType);
	s.AddMember("flags", sampler.flags, alloc);
	s.AddMember("minFilter", sampler.minFilter, alloc);
	s.AddMember("magFilter", sampler.magFilter, alloc);
	s.AddMember("maxAnisotropy", sampler.maxAnisotropy, alloc);
	s.AddMember("compareOp", sampler.compareOp, alloc);
	s.AddMember("anisotropyEnable", sampler.anisotropyEnable, alloc);
	s.AddMember("mipmapMode", sampler.mipmapMode, alloc);
	s.AddMember("addressModeU", sampler.addressModeU, alloc);
	s.AddMember("addressModeV", sampler.addressModeV, alloc);
	s.AddMember("addressModeW", sampler.addressModeW, alloc);
	s.AddMember("borderColor", sampler.borderColor, alloc);
	s.AddMember("unnormalizedCoordinates", sampler.unnormalizedCoordinates, alloc);
	s.AddMember("compareEnable", sampler.compareEnable, alloc);
	s.AddMember("mipLodBias", sampler.mipLodBias, alloc);
	s.AddMember("minLod", sampler.minLod, alloc);
	s.AddMember("maxLod", sampler.maxLod, alloc);
	return s;
}

template <typename Allocator>
static Value json_value(const VkDescriptorSetLayoutCreateInfo& layout, Allocator& alloc)
{
	Value l(kObjectType);
	l.AddMember("flags", layout.flags, alloc);

	Value bindings(kArrayType);
	for (uint32_t i = 0; i < layout.bindingCount; i++)
	{
		auto &b = layout.pBindings[i];
		Value binding(kObjectType);
		binding.AddMember("descriptorType", b.descriptorType, alloc);
		binding.AddMember("descriptorCount", b.descriptorCount, alloc);
		binding.AddMember("stageFlags", b.stageFlags, alloc);
		binding.AddMember("binding", b.binding, alloc);
		if (b.pImmutableSamplers)
		{
			Value immutables(kArrayType);
			for (uint32_t j = 0; j < b.descriptorCount; j++)
				immutables.PushBack(uint64_string(api_object_cast<uint64_t>(b.pImmutableSamplers[j]), alloc), alloc);
			binding.AddMember("immutableSamplers", immutables, alloc);
		}
		bindings.PushBack(binding, alloc);
	}
	l.AddMember("bindings", bindings, alloc);
	return l;
}

template <typename Allocator>
static Value json_value(const VkPipelineLayoutCreateInfo& layout, Allocator& alloc)
{
	Value p(kObjectType);
	p.AddMember("flags", layout.flags, alloc);
	Value push(kArrayType);
	for (uint32_t i = 0; i < layout.pushConstantRangeCount; i++)
	{
		Value range(kObjectType);
		range.AddMember("stageFlags", layout.pPushConstantRanges[i].stageFlags, alloc);
		range.AddMember("size", layout.pPushConstantRanges[i].size, alloc);
		range.AddMember("offset", layout.pPushConstantRanges[i].offset, alloc);
		push.PushBack(range, alloc);
	}
	p.AddMember("pushConstantRanges", push, alloc);

	Value set_layouts(kArrayType);
	for (uint32_t i = 0; i < layout.setLayoutCount; i++)
		set_layouts.PushBack(uint64_string(api_object_cast<uint64_t>(layout.pSetLayouts[i]), alloc), alloc);
	p.AddMember("setLayouts", set_layouts, alloc);
	return p;
}

template <typename Allocator>
static Value json_value(const VkShaderModuleCreateInfo& module, Allocator& alloc)
{
	Value m(kObjectType);
	m.AddMember("flags", module.flags, alloc);
	m.AddMember("codeSize", module.codeSize, alloc);
	m.AddMember("code", encode_base64(module.pCode, module.codeSize), alloc);
	return m;
}

template <typename Allocator>
static Value json_value(const VkRenderPassCreateInfo& pass, Allocator& alloc)
{
	Value p(kObjectType);
	p.AddMember("flags", pass.flags, alloc);

	Value deps(kArrayType);
	Value subpasses(kArrayType);
	Value attachments(kArrayType);

	if (pass.pDependencies)
	{
		for (uint32_t i = 0; i < pass.dependencyCount; i++)
		{
			auto &d = pass.pDependencies[i];
			Value dep(kObjectType);
			dep.AddMember("dependencyFlags", d.dependencyFlags, alloc);
			dep.AddMember("dstAccessMask", d.dstAccessMask, alloc);
			dep.AddMember("srcAccessMask", d.srcAccessMask, alloc);
			dep.AddMember("dstStageMask", d.dstStageMask, alloc);
			dep.AddMember("srcStageMask", d.srcStageMask, alloc);
			dep.AddMember("dstSubpass", d.dstSubpass, alloc);
			dep.AddMember("srcSubpass", d.srcSubpass, alloc);
			deps.PushBack(dep, alloc);
		}
		p.AddMember("dependencies", deps, alloc);
	}

	if (pass.pAttachments)
	{
		for (uint32_t i = 0; i < pass.attachmentCount; i++)
		{
			auto &a = pass.pAttachments[i];
			Value att(kObjectType);

			att.AddMember("flags", a.flags, alloc);
			att.AddMember("format", a.format, alloc);
			att.AddMember("finalLayout", a.finalLayout, alloc);
			att.AddMember("initialLayout", a.initialLayout, alloc);
			att.AddMember("loadOp", a.loadOp, alloc);
			att.AddMember("storeOp", a.storeOp, alloc);
			att.AddMember("samples", a.samples, alloc);
			att.AddMember("stencilLoadOp", a.stencilLoadOp, alloc);
			att.AddMember("stencilStoreOp", a.stencilStoreOp, alloc);

			attachments.PushBack(att, alloc);
		}
		p.AddMember("attachments", attachments, alloc);
	}

	for (uint32_t i = 0; i < pass.subpassCount; i++)
	{
		auto &sub = pass.pSubpasses[i];
		Value p(kObjectType);
		p.AddMember("flags", sub.flags, alloc);
		p.AddMember("pipelineBindPoint", sub.pipelineBindPoint, alloc);

		if (sub.pPreserveAttachments)
		{
			Value preserves(kArrayType);
			for (uint32_t j = 0; j < sub.preserveAttachmentCount; j++)
				preserves.PushBack(sub.pPreserveAttachments[j], alloc);
			p.AddMember("preserveAttachments", preserves, alloc);
		}

		if (sub.pInputAttachments)
		{
			Value inputs(kArrayType);
			for (uint32_t j = 0; j < sub.inputAttachmentCount; j++)
			{
				Value input(kObjectType);
				auto &ia = sub.pInputAttachments[j];
				input.AddMember("attachment", ia.attachment, alloc);
				input.AddMember("layout", ia.layout, alloc);
				inputs.PushBack(input, alloc);
			}
			p.AddMember("inputAttachments", inputs, alloc);
		}

		if (sub.pColorAttachments)
		{
			Value colors(kArrayType);
			for (uint32_t j = 0; j < sub.colorAttachmentCount; j++)
			{
				Value color(kObjectType);
				auto &c = sub.pColorAttachments[j];
				color.AddMember("attachment", c.attachment, alloc);
				color.AddMember("layout", c.layout, alloc);
				colors.PushBack(color, alloc);
			}
			p.AddMember("colorAttachments", colors, alloc);
		}

		if (sub.pResolveAttachments)
		{
			Value resolves(kArrayType);
			for (uint32_t j = 0; j < sub.colorAttachmentCount; j++)
			{
				Value resolve(kObjectType);
				auto &r = sub.pResolveAttachments[j];
				resolve.AddMember("attachment", r.attachment, alloc);
				resolve.AddMember("layout", r.layout, alloc);
				resolves.PushBack(resolve, alloc);
			}
			p.AddMember("resolveAttachments", resolves, alloc);
		}

		if (sub.pDepthStencilAttachment)
		{
			Value depth_stencil(kObjectType);
			depth_stencil.AddMember("attachment", sub.pDepthStencilAttachment->attachment, alloc);
			depth_stencil.AddMember("layout", sub.pDepthStencilAttachment->layout, alloc);
			p.AddMember("depthStencilAttachment", depth_stencil, alloc);
		}

		subpasses.PushBack(p, alloc);
	}
	p.AddMember("subpasses", subpasses, alloc);
	return p;
}

template <typename Allocator>
static Value json_value(const VkComputePipelineCreateInfo& pipe, Allocator& alloc)
{
	Value p(kObjectType);
	p.AddMember("flags", pipe.flags, alloc);
	p.AddMember("layout", uint64_string(api_object_cast<uint64_t>(pipe.layout), alloc), alloc);
	p.AddMember("basePipelineHandle", uint64_string(api_object_cast<uint64_t>(pipe.basePipelineHandle), alloc), alloc);
	p.AddMember("basePipelineIndex", pipe.basePipelineIndex, alloc);
	Value stage(kObjectType);
	stage.AddMember("flags", pipe.stage.flags, alloc);
	stage.AddMember("stage", pipe.stage.stage, alloc);
	stage.AddMember("module", uint64_string(api_object_cast<uint64_t>(pipe.stage.module), alloc), alloc);
	stage.AddMember("name", StringRef(pipe.stage.pName), alloc);
	if (pipe.stage.pSpecializationInfo)
	{
		Value spec(kObjectType);
		spec.AddMember("dataSize", pipe.stage.pSpecializationInfo->dataSize, alloc);
		spec.AddMember("data",
					   encode_base64(pipe.stage.pSpecializationInfo->pData,
									 pipe.stage.pSpecializationInfo->dataSize), alloc);
		Value map_entries(kArrayType);
		for (uint32_t i = 0; i < pipe.stage.pSpecializationInfo->mapEntryCount; i++)
		{
			auto &e = pipe.stage.pSpecializationInfo->pMapEntries[i];
			Value map_entry(kObjectType);
			map_entry.AddMember("offset", e.offset, alloc);
			map_entry.AddMember("size", e.size, alloc);
			map_entry.AddMember("constantID", e.constantID, alloc);
			map_entries.PushBack(map_entry, alloc);
		}
		spec.AddMember("mapEntries", map_entries, alloc);
		stage.AddMember("specializationInfo", spec, alloc);
	}
	p.AddMember("stage", stage, alloc);
	return p;
}

template <typename Allocator>
static Value json_value(const VkGraphicsPipelineCreateInfo& pipe, Allocator& alloc)
{
	Value p(kObjectType);
	p.AddMember("flags", pipe.flags, alloc);
	p.AddMember("basePipelineHandle", uint64_string(api_object_cast<uint64_t>(pipe.basePipelineHandle), alloc), alloc);
	p.AddMember("basePipelineIndex", pipe.basePipelineIndex, alloc);
	p.AddMember("layout", uint64_string(api_object_cast<uint64_t>(pipe.layout), alloc), alloc);
	p.AddMember("renderPass", uint64_string(api_object_cast<uint64_t>(pipe.renderPass), alloc), alloc);
	p.AddMember("subpass", pipe.subpass, alloc);

	if (pipe.pTessellationState)
	{
		Value tess(kObjectType);
		tess.AddMember("flags", pipe.pTessellationState->flags, alloc);
		tess.AddMember("patchControlPoints", pipe.pTessellationState->patchControlPoints, alloc);
		p.AddMember("tessellationState", tess, alloc);
	}

	if (pipe.pDynamicState)
	{
		Value dyn(kObjectType);
		dyn.AddMember("flags", pipe.pDynamicState->flags, alloc);
		Value dynamics(kArrayType);
		for (uint32_t i = 0; i < pipe.pDynamicState->dynamicStateCount; i++)
			dynamics.PushBack(pipe.pDynamicState->pDynamicStates[i], alloc);
		dyn.AddMember("dynamicState", dynamics, alloc);
		p.AddMember("dynamicState", dyn, alloc);
	}

	if (pipe.pMultisampleState)
	{
		Value ms(kObjectType);
		ms.AddMember("flags", pipe.pMultisampleState->flags, alloc);
		ms.AddMember("rasterizationSamples", pipe.pMultisampleState->rasterizationSamples, alloc);
		ms.AddMember("sampleShadingEnable", pipe.pMultisampleState->sampleShadingEnable, alloc);
		ms.AddMember("minSampleShading", pipe.pMultisampleState->minSampleShading, alloc);
		ms.AddMember("alphaToOneEnable", pipe.pMultisampleState->alphaToOneEnable, alloc);
		ms.AddMember("alphaToCoverageEnable", pipe.pMultisampleState->alphaToCoverageEnable, alloc);

		Value sm(kArrayType);
		if (pipe.pMultisampleState->pSampleMask)
		{
			auto entries = uint32_t(pipe.pMultisampleState->rasterizationSamples + 31) / 32;
			for (uint32_t i = 0; i < entries; i++)
				sm.PushBack(pipe.pMultisampleState->pSampleMask[i], alloc);
			ms.AddMember("sampleMask", sm, alloc);
		}

		p.AddMember("multisampleState", ms, alloc);
	}

	if (pipe.pVertexInputState)
	{
		Value vi(kObjectType);

		Value attribs(kArrayType);
		Value bindings(kArrayType);
		vi.AddMember("flags", pipe.pVertexInputState->flags, alloc);

		for (uint32_t i = 0; i < pipe.pVertexInputState->vertexAttributeDescriptionCount; i++)
		{
			auto &a = pipe.pVertexInputState->pVertexAttributeDescriptions[i];
			Value attrib(kObjectType);
			attrib.AddMember("location", a.location, alloc);
			attrib.AddMember("binding", a.binding, alloc);
			attrib.AddMember("offset", a.offset, alloc);
			attrib.AddMember("format", a.format, alloc);
			attribs.PushBack(attrib, alloc);
		}

		for (uint32_t i = 0; i < pipe.pVertexInputState->vertexBindingDescriptionCount; i++)
		{
			auto &b = pipe.pVertexInputState->pVertexBindingDescriptions[i];
			Value binding(kObjectType);
			binding.AddMember("binding", b.binding, alloc);
			binding.AddMember("stride", b.stride, alloc);
			binding.AddMember("inputRate", b.inputRate, alloc);
			bindings.PushBack(binding, alloc);
		}
		vi.AddMember("attributes", attribs, alloc);
		vi.AddMember("bindings", bindings, alloc);

		p.AddMember("vertexInputState", vi, alloc);
	}

	if (pipe.pRasterizationState)
	{
		Value rs(kObjectType);
		rs.AddMember("flags", pipe.pRasterizationState->flags, alloc);
		rs.AddMember("depthBiasConstantFactor", pipe.pRasterizationState->depthBiasConstantFactor, alloc);
		rs.AddMember("depthBiasSlopeFactor", pipe.pRasterizationState->depthBiasSlopeFactor, alloc);
		rs.AddMember("depthBiasClamp", pipe.pRasterizationState->depthBiasClamp, alloc);
		rs.AddMember("depthBiasEnable", pipe.pRasterizationState->depthBiasEnable, alloc);
		rs.AddMember("depthClampEnable", pipe.pRasterizationState->depthClampEnable, alloc);
		rs.AddMember("polygonMode", pipe.pRasterizationState->polygonMode, alloc);
		rs.AddMember("rasterizerDiscardEnable", pipe.pRasterizationState->rasterizerDiscardEnable, alloc);
		rs.AddMember("frontFace", pipe.pRasterizationState->frontFace, alloc);
		rs.AddMember("lineWidth", pipe.pRasterizationState->lineWidth, alloc);
		rs.AddMember("cullMode", pipe.pRasterizationState->cullMode, alloc);
		p.AddMember("rasterizationState", rs, alloc);
	}

	if (pipe.pInputAssemblyState)
	{
		Value ia(kObjectType);
		ia.AddMember("flags", pipe.pInputAssemblyState->flags, alloc);
		ia.AddMember("topology", pipe.pInputAssemblyState->topology, alloc);
		ia.AddMember("primitiveRestartEnable", pipe.pInputAssemblyState->primitiveRestartEnable, alloc);
		p.AddMember("inputAssemblyState", ia, alloc);
	}

	if (pipe.pColorBlendState)
	{
		Value cb(kObjectType);
		cb.AddMember("flags", pipe.pColorBlendState->flags, alloc);
		cb.AddMember("logicOp", pipe.pColorBlendState->logicOp, alloc);
		cb.AddMember("logicOpEnable", pipe.pColorBlendState->logicOpEnable, alloc);
		Value blend_constants(kArrayType);
		for (auto &c : pipe.pColorBlendState->blendConstants)
			blend_constants.PushBack(c, alloc);
		cb.AddMember("blendConstants", blend_constants, alloc);
		Value attachments(kArrayType);
		for (uint32_t i = 0; i < pipe.pColorBlendState->attachmentCount; i++)
		{
			auto &a = pipe.pColorBlendState->pAttachments[i];
			Value att(kObjectType);
			att.AddMember("dstAlphaBlendFactor", a.dstAlphaBlendFactor, alloc);
			att.AddMember("srcAlphaBlendFactor", a.srcAlphaBlendFactor, alloc);
			att.AddMember("dstColorBlendFactor", a.dstColorBlendFactor, alloc);
			att.AddMember("srcColorBlendFactor", a.srcColorBlendFactor, alloc);
			att.AddMember("colorWriteMask", a.colorWriteMask, alloc);
			att.AddMember("alphaBlendOp", a.alphaBlendOp, alloc);
			att.AddMember("colorBlendOp", a.colorBlendOp, alloc);
			att.AddMember("blendEnable", a.blendEnable, alloc);
			attachments.PushBack(att, alloc);
		}
		cb.AddMember("attachments", attachments, alloc);
		p.AddMember("colorBlendState", cb, alloc);
	}

	if (pipe.pViewportState)
	{
		Value vp(kObjectType);
		vp.AddMember("flags", pipe.pViewportState->flags, alloc);
		vp.AddMember("viewportCount", pipe.pViewportState->viewportCount, alloc);
		vp.AddMember("scissorCount", pipe.pViewportState->scissorCount, alloc);
		if (pipe.pViewportState->pViewports)
		{
			Value viewports(kArrayType);
			for (uint32_t i = 0; i < pipe.pViewportState->viewportCount; i++)
			{
				Value viewport(kObjectType);
				viewport.AddMember("x", pipe.pViewportState->pViewports[i].x, alloc);
				viewport.AddMember("y", pipe.pViewportState->pViewports[i].y, alloc);
				viewport.AddMember("width", pipe.pViewportState->pViewports[i].width, alloc);
				viewport.AddMember("height", pipe.pViewportState->pViewports[i].height, alloc);
				viewport.AddMember("minDepth", pipe.pViewportState->pViewports[i].minDepth, alloc);
				viewport.AddMember("maxDepth", pipe.pViewportState->pViewports[i].maxDepth, alloc);
				viewports.PushBack(viewport, alloc);
			}
			vp.AddMember("viewports", viewports, alloc);
		}

		if (pipe.pViewportState->pScissors)
		{
			Value scissors(kArrayType);
			for (uint32_t i = 0; i < pipe.pViewportState->scissorCount; i++)
			{
				Value scissor(kObjectType);
				scissor.AddMember("x", pipe.pViewportState->pScissors[i].offset.x, alloc);
				scissor.AddMember("y", pipe.pViewportState->pScissors[i].offset.y, alloc);
				scissor.AddMember("width", pipe.pViewportState->pScissors[i].extent.width, alloc);
				scissor.AddMember("height", pipe.pViewportState->pScissors[i].extent.height, alloc);
				scissors.PushBack(scissor, alloc);
			}
			vp.AddMember("scissors", scissors, alloc);
		}
		p.AddMember("viewportState", vp, alloc);
	}

	if (pipe.pDepthStencilState)
	{
		Value ds(kObjectType);
		ds.AddMember("flags", pipe.pDepthStencilState->flags, alloc);
		ds.AddMember("stencilTestEnable", pipe.pDepthStencilState->stencilTestEnable, alloc);
		ds.AddMember("maxDepthBounds", pipe.pDepthStencilState->maxDepthBounds, alloc);
		ds.AddMember("minDepthBounds", pipe.pDepthStencilState->minDepthBounds, alloc);
		ds.AddMember("depthBoundsTestEnable", pipe.pDepthStencilState->depthBoundsTestEnable, alloc);
		ds.AddMember("depthWriteEnable", pipe.pDepthStencilState->depthWriteEnable, alloc);
		ds.AddMember("depthTestEnable", pipe.pDepthStencilState->depthTestEnable, alloc);
		ds.AddMember("depthCompareOp", pipe.pDepthStencilState->depthCompareOp, alloc);

		const auto serialize_stencil = [&](Value &v, const VkStencilOpState &state) {
			v.AddMember("compareOp", state.compareOp, alloc);
			v.AddMember("writeMask", state.writeMask, alloc);
			v.AddMember("reference", state.reference, alloc);
			v.AddMember("compareMask", state.compareMask, alloc);
			v.AddMember("passOp", state.passOp, alloc);
			v.AddMember("failOp", state.failOp, alloc);
			v.AddMember("depthFailOp", state.depthFailOp, alloc);
		};
		Value front(kObjectType);
		Value back(kObjectType);
		serialize_stencil(front, pipe.pDepthStencilState->front);
		serialize_stencil(back, pipe.pDepthStencilState->back);
		ds.AddMember("front", front, alloc);
		ds.AddMember("back", back, alloc);
		p.AddMember("depthStencilState", ds, alloc);
	}

	Value stages(kArrayType);
	for (uint32_t i = 0; i < pipe.stageCount; i++)
	{
		auto &s = pipe.pStages[i];
		Value stage(kObjectType);
		stage.AddMember("flags", s.flags, alloc);
		stage.AddMember("name", StringRef(s.pName), alloc);
		stage.AddMember("module", uint64_string(api_object_cast<uint64_t>(s.module), alloc), alloc);
		stage.AddMember("stage", s.stage, alloc);
		if (s.pSpecializationInfo)
		{
			Value spec(kObjectType);
			spec.AddMember("dataSize", s.pSpecializationInfo->dataSize, alloc);
			spec.AddMember("data",
						   encode_base64(s.pSpecializationInfo->pData,
										 s.pSpecializationInfo->dataSize), alloc);
			Value map_entries(kArrayType);
			for (uint32_t i = 0; i < s.pSpecializationInfo->mapEntryCount; i++)
			{
				auto &e = s.pSpecializationInfo->pMapEntries[i];
				Value map_entry(kObjectType);
				map_entry.AddMember("offset", e.offset, alloc);
				map_entry.AddMember("size", e.size, alloc);
				map_entry.AddMember("constantID", e.constantID, alloc);
				map_entries.PushBack(map_entry, alloc);
			}
			spec.AddMember("mapEntries", map_entries, alloc);
			stage.AddMember("specializationInfo", spec, alloc);
		}
		stages.PushBack(stage, alloc);
	}
	p.AddMember("stages", stages, alloc);
	return p;
}

template <typename ObjType, typename CreateType, typename AllocType>
static inline const CreateType* serialize_obj(ObjType obj, std::unordered_map<Hash, CreateType>& ci_map, Value& json_map, AllocType& alloc)
{
	auto iter = ci_map.find(api_object_cast<Hash>(obj));
	if (iter != ci_map.end()) {
		auto hash = uint64_string(api_object_cast<uint64_t>(obj), alloc);
		if (!json_map.HasMember(hash)) {
			json_map.AddMember(hash, json_value(iter->second, alloc), alloc);
		}
		return &iter->second;
	}
	return nullptr;
}

vector<uint8_t> StateRecorder::serialize_graphics_pipeline(Hash hash) const
{
	Document doc;
	doc.SetObject();
	auto &alloc = doc.GetAllocator();

	Value samplers(kObjectType);
	Value set_layouts(kObjectType);
	Value pipeline_layouts(kObjectType);
	Value shader_modules(kObjectType);
	Value render_passes(kObjectType);
	Value graphics_pipelines(kObjectType);

	if (auto pipe = serialize_obj(hash, impl->graphics_pipelines, graphics_pipelines, alloc))
	{
		if (auto pipeline_layout = serialize_obj(pipe->layout, impl->pipeline_layouts, pipeline_layouts, alloc))
		{
			for (uint32_t i = 0; i < pipeline_layout->setLayoutCount; i++)
			{
				if (auto set_layout = serialize_obj(pipeline_layout->pSetLayouts[i], impl->descriptor_sets, set_layouts, alloc))
				{
					for (uint32_t j = 0; j < set_layout->bindingCount; j++)
					{
						auto& binding = set_layout->pBindings[j];
						if (binding.pImmutableSamplers &&
							(binding.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
								binding.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER))
						{
							for (uint32_t k = 0; k < binding.descriptorCount; k++)
								serialize_obj(binding.pImmutableSamplers[k], impl->samplers, samplers, alloc);
						}
					}
				}
			}
		}

		serialize_obj(pipe->renderPass, impl->render_passes, render_passes, alloc);
	}

	doc.AddMember("version", FOSSILIZE_FORMAT_VERSION, alloc);
	doc.AddMember("samplers", samplers, alloc);
	doc.AddMember("setLayouts", set_layouts, alloc);
	doc.AddMember("pipelineLayouts", pipeline_layouts, alloc);
	doc.AddMember("renderPasses", render_passes, alloc);
	doc.AddMember("graphicsPipelines", graphics_pipelines, alloc);

	StringBuffer buffer;
	PrettyWriter<StringBuffer> writer(buffer);
	doc.Accept(writer);

	vector<uint8_t> serialize_buffer(buffer.GetSize());
	memcpy(serialize_buffer.data(), buffer.GetString(), buffer.GetSize());
	return serialize_buffer;
}

vector<uint8_t> StateRecorder::serialize_compute_pipeline(Hash hash) const
{
	Document doc;
	doc.SetObject();
	auto &alloc = doc.GetAllocator();

	Value samplers(kObjectType);
	Value set_layouts(kObjectType);
	Value pipeline_layouts(kObjectType);
	Value shader_modules(kObjectType);
	Value compute_pipelines(kObjectType);
	
	if (auto pipe = serialize_obj(hash, impl->compute_pipelines, compute_pipelines, alloc))
	{
		if (auto pipeline_layout = serialize_obj(pipe->layout, impl->pipeline_layouts, pipeline_layouts, alloc))
		{
			for (uint32_t i = 0; i < pipeline_layout->setLayoutCount; i++)
			{
				if (auto set_layout = serialize_obj(pipeline_layout->pSetLayouts[i], impl->descriptor_sets, set_layouts, alloc))
				{
					for (uint32_t j = 0; j < set_layout->bindingCount; j++)
					{
						auto& binding = set_layout->pBindings[j];
						if (binding.pImmutableSamplers &&
							(binding.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
								binding.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER))
						{
							for (uint32_t k = 0; k < binding.descriptorCount; k++)
								serialize_obj(binding.pImmutableSamplers[k], impl->samplers, samplers, alloc);
						}
					}
				}
			}
		}
	}

	doc.AddMember("version", FOSSILIZE_FORMAT_VERSION, alloc);
	doc.AddMember("samplers", samplers, alloc);
	doc.AddMember("setLayouts", set_layouts, alloc);
	doc.AddMember("pipelineLayouts", pipeline_layouts, alloc);
	doc.AddMember("computePipelines", compute_pipelines, alloc);

	StringBuffer buffer;
	PrettyWriter<StringBuffer> writer(buffer);
	doc.Accept(writer);

	vector<uint8_t> serialize_buffer(buffer.GetSize());
	memcpy(serialize_buffer.data(), buffer.GetString(), buffer.GetSize());
	return serialize_buffer;
}

vector<uint8_t> StateRecorder::serialize_shader_module(Hash hash) const
{
	Document doc;
	doc.SetObject();
	auto &alloc = doc.GetAllocator();

	Value shader_modules(kObjectType);
	if (auto pipe = serialize_obj(hash, impl->shader_modules, shader_modules, alloc))

	doc.AddMember("version", FOSSILIZE_FORMAT_VERSION, alloc);
	doc.AddMember("shaderModules", shader_modules, alloc);

	StringBuffer buffer;
	PrettyWriter<StringBuffer> writer(buffer);
	doc.Accept(writer);

	vector<uint8_t> serialize_buffer(buffer.GetSize());
	memcpy(serialize_buffer.data(), buffer.GetString(), buffer.GetSize());
	return serialize_buffer;
}

vector<uint8_t> StateRecorder::serialize() const
{
	uint64_t varint_spirv_offset = 0;

	Document doc;
	doc.SetObject();
	auto &alloc = doc.GetAllocator();

	doc.AddMember("version", FOSSILIZE_FORMAT_VERSION, alloc);

	Value samplers(kObjectType);
	for (auto &sampler : impl->samplers)
	{
		auto s = json_value(sampler.second, alloc);
		samplers.AddMember(uint64_string(sampler.first, alloc), json_value(sampler.second, alloc), alloc);
	}
	doc.AddMember("samplers", samplers, alloc);

	Value set_layouts(kObjectType);
	for (auto &layout : impl->descriptor_sets)
	{
		auto l = json_value(layout.second, alloc);
		set_layouts.AddMember(uint64_string(layout.first, alloc), l, alloc);
	}
	doc.AddMember("setLayouts", set_layouts, alloc);

	Value pipeline_layouts(kObjectType);
	for (auto &layout : impl->pipeline_layouts)
	{
		auto p = json_value(layout.second, alloc);
		pipeline_layouts.AddMember(uint64_string(layout.first, alloc), p, alloc);
	}
	doc.AddMember("pipelineLayouts", pipeline_layouts, alloc);

	Value shader_modules(kObjectType);
	for (auto &module : impl->shader_modules)
	{
		auto m = json_value(module.second, alloc);
		shader_modules.AddMember(uint64_string(module.first, alloc), m, alloc);
	}
	doc.AddMember("shaderModules", shader_modules, alloc);

	Value render_passes(kObjectType);
	for (auto &pass : impl->render_passes)
	{
		auto p = json_value(pass.second, alloc);
		render_passes.AddMember(uint64_string(pass.first, alloc), p, alloc);
	}
	doc.AddMember("renderPasses", render_passes, alloc);

	Value compute_pipelines(kObjectType);
	for (auto &pipe : impl->compute_pipelines)
	{
		auto p = json_value(pipe.second, alloc);
		compute_pipelines.AddMember(uint64_string(pipe.first, alloc), p, alloc);
	}
	doc.AddMember("computePipelines", compute_pipelines, alloc);

	Value graphics_pipelines(kObjectType);
	for (auto &pipe : impl->graphics_pipelines)
	{
		auto p = json_value(pipe.second, alloc);
		graphics_pipelines.AddMember(uint64_string(pipe.first, alloc), p, alloc);
	}
	doc.AddMember("graphicsPipelines", graphics_pipelines, alloc);

	StringBuffer buffer;
	PrettyWriter<StringBuffer> writer(buffer);
	doc.Accept(writer);

	vector<uint8_t> serialize_buffer(buffer.GetSize());
	memcpy(serialize_buffer.data(), buffer.GetString(), buffer.GetSize());
	return serialize_buffer;
}

void StateRecorder::set_serialization_path(const std::string &serialization_path)
{
	std::lock_guard lock(impl->serialization_lock);
	impl->serialization_path = serialization_path;
}

StateRecorder::StateRecorder()
{
	impl.reset(new Impl);
	impl->worker_thread = std::thread(&StateRecorder::Impl::record_task, this);
}

StateRecorder::~StateRecorder()
{
	record_end();
	impl->worker_thread.join();
}


}
