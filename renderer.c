#include "renderer.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#include <vulkan/vulkan.h>

#include "udmabuf.h"

struct buffer {
	VkBuffer buf;
	VkDeviceMemory mem;
};

struct renderer {
	struct {
		int width;
		int height;
		int output_count;
		bool use_udmabuf;
	} config;

	struct {
		int in;
		int out;
	} ctrl;

	struct {
		int memfd;
		size_t size;
		union {
			void *base;
			int udmabuf;
		};
	} heap;

	/* VK device */
	VkInstance instance;
	VkPhysicalDevice physical_dev;
	VkPhysicalDeviceMemoryProperties2 mem_props;
	VkDevice dev;
	VkQueue queue;

	struct {
		VkDeviceSize base_skip;
		VkDeviceSize ubo_size;
		VkDeviceSize output_size;

		/* by-products */

		VkExternalMemoryHandleTypeFlagBits handle_type;
		VkExternalMemoryBufferCreateInfo ext_buffer_info;

		VkDeviceSize ubo_used_size;
		VkExternalBufferProperties ubo_props;
		VkBufferCreateInfo ubo_info;
		VkMemoryRequirements2 ubo_reqs;

		VkDeviceSize output_used_size;
		VkExternalBufferProperties output_props;
		VkBufferCreateInfo output_info;
		VkMemoryRequirements2 output_reqs;
	} heap_layout;

	/* heap buffers */
	struct buffer ubo;
	struct buffer *outputs;

	struct {
		VkBuffer buf;
		VkDeviceMemory mem;
	} vb;

	struct {
		VkDescriptorPool pool;
		VkDescriptorSetLayout layout;
		VkDescriptorSet set;
	} desc;

	struct {
		VkRenderPass pass;

		VkImage img;
		VkDeviceMemory mem;
		VkImageView view;
		VkFramebuffer fb;
	} fb;

	struct {
		VkPipelineLayout layout;
		VkShaderModule vs;
		VkShaderModule fs;
		VkPipeline pipeline;
	} pipeline;

	struct {
		VkCommandPool pool;
		VkCommandBuffer *bufs;
	} cmd;
};

/* generated with vkcube build rules */
static const uint32_t renderer_vs_code[] = {
#include "renderer.vert.h"
};
static const uint32_t renderer_fs_code[] = {
#include "renderer.frag.h"
};

static void renderer_fatal(const char *msg)
{
	printf("RENDERER-FATAL: %s\n", msg);
	abort();
}

static void renderer_vk(VkResult result, const char *msg)
{
	if (result != VK_SUCCESS)
		renderer_fatal(msg);
}

static void renderer_init_heap(struct renderer *renderer, int memfd)
{
	off_t off = lseek(memfd, 0, SEEK_END);
	if (off < 0)
		renderer_fatal("failed to get memfd size");

	renderer->heap.memfd = memfd;
	renderer->heap.size = off;

	if (renderer->config.use_udmabuf) {
		renderer->heap.udmabuf = udmabuf_init();
		if (renderer->heap.udmabuf < 0)
			renderer_fatal("failed to initialize udmabuf");
	} else {
		renderer->heap.base = mmap(NULL, off, PROT_READ | PROT_WRITE,
				MAP_SHARED, renderer->heap.memfd, 0);
		if (renderer->heap.base == MAP_FAILED)
			renderer_fatal("failed to map memfd");
	}
}

static void renderer_init_vk_instance(struct renderer *renderer)
{
	uint32_t version;
	vkEnumerateInstanceVersion(&version);
	if (version < VK_MAKE_VERSION(1, 1, 0))
		renderer_fatal("no Vulkan 1.1 instance support");

	VkResult result = vkCreateInstance(
			&(VkInstanceCreateInfo) {
				.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
				.pApplicationInfo = &(VkApplicationInfo) {
					.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
					.apiVersion = version,
				},
			}, NULL, &renderer->instance);
	renderer_vk(result, "failed to create instance");
}

static void renderer_init_vk_physical_device(struct renderer *renderer)
{
	uint32_t count = 1;
	VkResult result = vkEnumeratePhysicalDevices(renderer->instance,
			&count, &renderer->physical_dev);
	if (result != VK_INCOMPLETE)
		renderer_vk(result, "failed to enumerate physical devices");

	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(renderer->physical_dev, &props);
	if (props.apiVersion < VK_MAKE_VERSION(1, 1, 0))
		renderer_fatal("no Vulkan 1.1 device support");

	renderer->mem_props = (VkPhysicalDeviceMemoryProperties2) {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2
	};
	vkGetPhysicalDeviceMemoryProperties2(renderer->physical_dev, &renderer->mem_props);
}

static void renderer_init_vk_device(struct renderer *renderer)
{
	const struct {
		const char *name;
		bool required;
	} ext_table[] = {
		{ "VK_KHR_external_memory_fd", renderer->config.use_udmabuf },
		{ "VK_EXT_external_memory_dma_buf", renderer->config.use_udmabuf },
		{ "VK_EXT_external_memory_host", !renderer->config.use_udmabuf },
		{ NULL },
	};

	uint32_t ext_count = 1;
	VkResult result = vkEnumerateDeviceExtensionProperties(renderer->physical_dev,
			NULL, &ext_count, NULL);
	renderer_vk(result, "failed to enumerate extension count");

	VkExtensionProperties *ext_props = malloc(sizeof(*ext_props) * ext_count);
	if (!ext_props)
		renderer_fatal("failed to allocate extension array");
	result = vkEnumerateDeviceExtensionProperties(renderer->physical_dev,
			NULL, &ext_count, ext_props);
	renderer_vk(result, "failed to enumerate extensions");

	const char *enabled_names[16];
	uint32_t enabled_count = 0;
	for (int i = 0; ext_table[i].name; i++) {
		if (!ext_table[i].required)
			continue;

		bool found = false;
		for (uint32_t j = 0; j < ext_count; j++) {
			if (!strcmp(ext_props[j].extensionName, ext_table[i].name)) {
				found = true;
				break;
			}
		}
		if (!found)
			renderer_fatal("missing extensions");

		enabled_names[enabled_count++] = ext_table[i].name;
	}

	free(ext_props);

	VkQueueFamilyProperties2 queue_props = {
		.sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2
	};
	uint32_t queue_count = 1;
	vkGetPhysicalDeviceQueueFamilyProperties2(renderer->physical_dev,
			&queue_count, &queue_props);
	if (!(queue_props.queueFamilyProperties.queueFlags & VK_QUEUE_GRAPHICS_BIT))
		renderer_fatal("queue family 0 does not support graphics");

	result = vkCreateDevice(renderer->physical_dev,
			&(VkDeviceCreateInfo) {
				.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
				.queueCreateInfoCount = 1,
				.pQueueCreateInfos = &(VkDeviceQueueCreateInfo) {
					.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
					.queueFamilyIndex = 0,
					.queueCount = 1,
					.pQueuePriorities = &(float) { 1.0f },
				},
				.enabledExtensionCount = enabled_count,
				.ppEnabledExtensionNames = enabled_names,
			}, NULL, &renderer->dev);
	renderer_vk(result, "failed to create device");

	vkGetDeviceQueue(renderer->dev, 0, 0, &renderer->queue);
}

static void renderer_get_heap_buffer_props(const struct renderer *renderer,
		VkDeviceSize size, VkBufferUsageFlags usage,
		VkDeviceSize mem_align,
		VkExternalBufferProperties *props,
		VkBufferCreateInfo *info,
		VkMemoryRequirements2 *reqs,
		VkDeviceSize *alloc)
{
	*props = (VkExternalBufferProperties) {
		.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES
	};
	vkGetPhysicalDeviceExternalBufferProperties(renderer->physical_dev,
			&(VkPhysicalDeviceExternalBufferInfo) {
				.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
				.usage = usage,
				.handleType = renderer->heap_layout.handle_type,
			}, props);

	if (!(props->externalMemoryProperties.externalMemoryFeatures &
				VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT))
		renderer_fatal("external memory not importable");

	*info = (VkBufferCreateInfo) {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.pNext = &renderer->heap_layout.ext_buffer_info,
		.size = size,
		.usage = usage,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};

	VkBuffer buf;
	VkResult result = vkCreateBuffer(renderer->dev, info, NULL, &buf);
	renderer_vk(result, "failed to create tmp buffer");

	*reqs = (VkMemoryRequirements2) { .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2 };
	vkGetBufferMemoryRequirements2(renderer->dev,
			&(VkBufferMemoryRequirementsInfo2) {
				.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2,
				.buffer = buf,
			}, reqs);

	vkDestroyBuffer(renderer->dev, buf, NULL);

	const size_t rem = reqs->memoryRequirements.size % mem_align;
	if (rem) {
		if (props->externalMemoryProperties.externalMemoryFeatures &
				VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT)
			renderer_fatal("conflicting size requirement from dedicated allocation");
		*alloc = reqs->memoryRequirements.size - rem + mem_align;
	} else {
		*alloc = size;
	}
}

static void renderer_alloc_heap_buffer(const struct renderer *renderer,
		struct buffer *buf, size_t offset, size_t size,
		const VkExternalBufferProperties *props,
		const VkBufferCreateInfo *info,
		const VkMemoryRequirements2 *reqs)
{
	VkResult result = vkCreateBuffer(renderer->dev, info, NULL, &buf->buf);
	renderer_vk(result, "failed to create buffer");

	VkImportMemoryFdInfoKHR fd_info = {
		.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
		.handleType = renderer->heap_layout.handle_type,
	};
	VkImportMemoryHostPointerInfoEXT ptr_info = {
		.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
		.handleType = renderer->heap_layout.handle_type,
	};
	uint32_t mem_types = reqs->memoryRequirements.memoryTypeBits;
	void *p_next;
	if (renderer->config.use_udmabuf) {
		/* the fd ownership will be transferred to Vulakn */
		fd_info.fd = udmabuf_create(renderer->heap.udmabuf, renderer->heap.memfd,
				offset, size);
		if (fd_info.fd < 0)
			renderer_fatal("failed to create udmabuf");

		VkMemoryFdPropertiesKHR fd_props = {
			.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR
		};
		PFN_vkGetMemoryFdPropertiesKHR getter =
			(PFN_vkGetMemoryFdPropertiesKHR)
			vkGetDeviceProcAddr(renderer->dev,
					"vkGetMemoryFdPropertiesKHR");
		result = getter(renderer->dev, fd_info.handleType, fd_info.fd, &fd_props);
		renderer_vk(result, "invalid dmabuf");

		mem_types &= fd_props.memoryTypeBits;
		p_next = &fd_info;
	} else {
		ptr_info.pHostPointer = renderer->heap.base + offset;

		VkMemoryHostPointerPropertiesEXT ptr_props = {
			.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT
		};
		PFN_vkGetMemoryHostPointerPropertiesEXT getter =
			(PFN_vkGetMemoryHostPointerPropertiesEXT)
			vkGetDeviceProcAddr(renderer->dev,
					"vkGetMemoryHostPointerPropertiesEXT");
		result = getter(renderer->dev, ptr_info.handleType,
				ptr_info.pHostPointer, &ptr_props);
		renderer_vk(result, "invalid memfd ptr");

		mem_types &= ptr_props.memoryTypeBits;
		p_next = &ptr_info;
	}

	if (!mem_types)
		renderer_fatal("no usable memory type");
	const uint32_t mem_type = ffs(mem_types) - 1;

	VkMemoryDedicatedAllocateInfo dedicated_info = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
		.buffer = buf->buf,
	};
	if (props->externalMemoryProperties.externalMemoryFeatures &
			VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT) {
		dedicated_info.pNext = p_next;
		p_next = &dedicated_info;
	}

	result = vkAllocateMemory(renderer->dev,
			&(VkMemoryAllocateInfo) {
				.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
				.pNext = p_next,
				.allocationSize = size,
				.memoryTypeIndex = mem_type,
			}, NULL, &buf->mem);
	renderer_vk(result, "failed to import memory");

	result = vkBindBufferMemory2(renderer->dev, 1,
			&(VkBindBufferMemoryInfo) {
				.sType = VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO,
				.buffer = buf->buf,
				.memory = buf->mem,
			});
	renderer_vk(result, "failed to bind memory");
}

static void renderer_init_heap_layout(struct renderer *renderer)
{
	VkDeviceSize mem_align;

	if (renderer->config.use_udmabuf) {
		mem_align = getpagesize();
		renderer->heap_layout.base_skip = 0;
		renderer->heap_layout.handle_type =
			VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
	} else {
		VkPhysicalDeviceExternalMemoryHostPropertiesEXT ext_mem_host_props = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT
		};
		vkGetPhysicalDeviceProperties2(renderer->physical_dev,
				&(VkPhysicalDeviceProperties2) {
					.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
					.pNext = &ext_mem_host_props,
				});
		mem_align = ext_mem_host_props.minImportedHostPointerAlignment;

		const VkDeviceSize rem = (uintptr_t) renderer->heap.base % mem_align;
		renderer->heap_layout.base_skip = rem ? mem_align - rem : 0;
		renderer->heap_layout.handle_type =
			VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
	}

	renderer->heap_layout.ext_buffer_info = (VkExternalMemoryBufferCreateInfo) {
		.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
		.handleTypes = renderer->heap_layout.handle_type,
	};

	/* vec4 */
	renderer->heap_layout.ubo_used_size = sizeof(float[4]);
	renderer_get_heap_buffer_props(renderer, renderer->heap_layout.ubo_used_size,
			VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, mem_align,
			&renderer->heap_layout.ubo_props,
			&renderer->heap_layout.ubo_info,
			&renderer->heap_layout.ubo_reqs,
			&renderer->heap_layout.ubo_size);

	/* B8G8R8A8 */
	renderer->heap_layout.output_used_size =
		renderer->config.width * renderer->config.height * 4;
	renderer_get_heap_buffer_props(renderer, renderer->heap_layout.output_used_size,
			VK_BUFFER_USAGE_TRANSFER_DST_BIT, mem_align,
			&renderer->heap_layout.output_props,
			&renderer->heap_layout.output_info,
			&renderer->heap_layout.output_reqs,
			&renderer->heap_layout.output_size);

	if (renderer->heap_layout.ubo_size + renderer->heap_layout.output_size *
			renderer->config.output_count > renderer->heap.size)
		renderer_fatal("heap size too small");
}

static void renderer_init_heap_buffers(struct renderer *renderer)
{
	renderer->outputs = malloc(sizeof(renderer->outputs[0]) *
			renderer->config.output_count);
	if (!renderer->outputs)
		renderer_fatal("failed to allocate output array");

	size_t offset = renderer->heap_layout.base_skip;
	renderer_alloc_heap_buffer(renderer, &renderer->ubo, offset,
			renderer->heap_layout.ubo_size,
			&renderer->heap_layout.ubo_props,
			&renderer->heap_layout.ubo_info,
			&renderer->heap_layout.ubo_reqs);
	offset += renderer->heap_layout.ubo_size;

	for (int i = 0; i < renderer->config.output_count; i++) {
		renderer_alloc_heap_buffer(renderer, &renderer->outputs[i], offset,
				renderer->heap_layout.output_size,
				&renderer->heap_layout.output_props,
				&renderer->heap_layout.output_info,
				&renderer->heap_layout.output_reqs);
		offset += renderer->heap_layout.output_size;
	}
}

static void renderer_init_vk_vertex_buffer(struct renderer *renderer)
{
	const float vertices[3][2] = {
		{ -1.0f, -1.0f },
		{  0.0f,  1.0f },
		{  1.0f, -1.0f },
	};

	VkResult result = vkCreateBuffer(renderer->dev,
			&(VkBufferCreateInfo) {
				.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
				.size = sizeof(vertices),
				.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
				.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
			}, NULL, &renderer->vb.buf);
	renderer_vk(result, "failed to create vertex buffer");

	VkMemoryRequirements2 reqs = { .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2 };
	vkGetBufferMemoryRequirements2(renderer->dev,
			&(VkBufferMemoryRequirementsInfo2) {
				.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2,
				.buffer = renderer->vb.buf,
			}, &reqs);

	result = vkAllocateMemory(renderer->dev,
			&(VkMemoryAllocateInfo) {
				.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
				.allocationSize = reqs.memoryRequirements.size,
				.memoryTypeIndex = ffs(reqs.memoryRequirements.memoryTypeBits) - 1,
			}, NULL, &renderer->vb.mem);
	renderer_vk(result, "failed to allocate vertex buffer memory");

	result = vkBindBufferMemory2(renderer->dev, 1,
			&(VkBindBufferMemoryInfo) {
				.sType = VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO,
				.buffer = renderer->vb.buf,
				.memory = renderer->vb.mem,
			});
	renderer_vk(result, "failed to bind vertex buffer memory");

	void *ptr;
	result = vkMapMemory(renderer->dev, renderer->vb.mem, 0, sizeof(vertices), 0, &ptr);
	renderer_vk(result, "failed to map vertex buffer");
	memcpy(ptr, vertices, sizeof(vertices));
	vkUnmapMemory(renderer->dev, renderer->vb.mem);
}

static void renderer_init_vk_descriptor_set(struct renderer *renderer)
{
	VkResult result = vkCreateDescriptorPool(renderer->dev,
			&(VkDescriptorPoolCreateInfo) {
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
				.maxSets = 1,
				.poolSizeCount = 1,
				.pPoolSizes = &(VkDescriptorPoolSize) {
					.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
					.descriptorCount = 1,
				},
			}, NULL, &renderer->desc.pool);
	renderer_vk(result, "failed to create descriptor pool");

	result = vkCreateDescriptorSetLayout(renderer->dev,
			&(VkDescriptorSetLayoutCreateInfo) {
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
				.bindingCount = 1,
				.pBindings = &(VkDescriptorSetLayoutBinding) {
					.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
					.descriptorCount = 1,
					.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
				},
			}, NULL, &renderer->desc.layout);
	renderer_vk(result, "failed to create descriptor set layout");

	result = vkAllocateDescriptorSets(renderer->dev,
			&(VkDescriptorSetAllocateInfo) {
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
				.descriptorPool = renderer->desc.pool,
				.descriptorSetCount = 1,
				.pSetLayouts = &renderer->desc.layout,
			}, &renderer->desc.set);
	renderer_vk(result, "failed to allocate descriptor set");

	vkUpdateDescriptorSets(renderer->dev, 1,
			&(VkWriteDescriptorSet) {
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = renderer->desc.set,
				.descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
				.pBufferInfo = &(VkDescriptorBufferInfo) {
					.buffer = renderer->ubo.buf,
					.range = renderer->heap_layout.ubo_used_size,
				},
			}, 0, NULL);
}

static void renderer_init_vk_framebuffer(struct renderer *renderer)
{
	const VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;

	VkResult result = vkCreateRenderPass(renderer->dev,
			&(VkRenderPassCreateInfo) {
				.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
				.attachmentCount = 1,
				.pAttachments = &(VkAttachmentDescription) {
					.format = format,
					.samples = VK_SAMPLE_COUNT_1_BIT,
					.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
					.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
					.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
					.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				},
				.subpassCount = 1,
				.pSubpasses = &(VkSubpassDescription) {
					.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
					.colorAttachmentCount = 1,
					.pColorAttachments = &(VkAttachmentReference) {
						.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
					}
				},
			}, NULL, &renderer->fb.pass);
	renderer_vk(result, "failed to create render pass");

	result = vkCreateImage(renderer->dev,
			&(VkImageCreateInfo) {
				.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
				.imageType = VK_IMAGE_TYPE_2D,
				.format = format,
				.extent = {
					.width = renderer->config.width,
					.height = renderer->config.height,
					.depth = 1,
				},
				.mipLevels = 1,
				.arrayLayers = 1,
				.samples = VK_SAMPLE_COUNT_1_BIT,
				.tiling = VK_IMAGE_TILING_OPTIMAL,
				.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
				         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
				 .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
				 .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			}, NULL, &renderer->fb.img);
	renderer_vk(result, "failed to create framebuffer image");

	VkMemoryRequirements2 reqs = { .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2 };
	vkGetImageMemoryRequirements2(renderer->dev,
			&(VkImageMemoryRequirementsInfo2) {
				.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
				.image = renderer->fb.img,
			}, &reqs);

	result = vkAllocateMemory(renderer->dev,
			&(VkMemoryAllocateInfo) {
				.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
				.allocationSize = reqs.memoryRequirements.size,
				.memoryTypeIndex = ffs(reqs.memoryRequirements.memoryTypeBits) - 1,
			}, NULL, &renderer->fb.mem);
	renderer_vk(result, "failed to allocate image memory");

	result = vkBindImageMemory2(renderer->dev, 1,
			&(VkBindImageMemoryInfo) {
				.sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO,
				.image = renderer->fb.img,
				.memory = renderer->fb.mem,
			});
	renderer_vk(result, "failed to bind image memory");

	result = vkCreateImageView(renderer->dev,
			&(VkImageViewCreateInfo) {
				.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
				.image = renderer->fb.img,
				.viewType = VK_IMAGE_VIEW_TYPE_2D,
				.format = format,
				.subresourceRange = {
					.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
					.levelCount = 1,
					.layerCount = 1,
				},
			}, NULL, &renderer->fb.view);
	renderer_vk(result, "failed to create framebuffer image view");

	result = vkCreateFramebuffer(renderer->dev,
			&(VkFramebufferCreateInfo) {
				.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
				.renderPass = renderer->fb.pass,
				.attachmentCount = 1,
				.pAttachments = &renderer->fb.view,
				.width = renderer->config.width,
				.height = renderer->config.height,
				.layers = 1,
			}, NULL, &renderer->fb.fb);
	renderer_vk(result, "failed to create framebuffer");
}

static void renderer_init_vk_pipeline(struct renderer *renderer)
{
	VkResult result = vkCreatePipelineLayout(renderer->dev,
			&(VkPipelineLayoutCreateInfo) {
				.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
				.setLayoutCount = 1,
				.pSetLayouts = &renderer->desc.layout,
			}, NULL, &renderer->pipeline.layout);
	renderer_vk(result, "failed to create pipeline layout");

	result = vkCreateShaderModule(renderer->dev,
			&(VkShaderModuleCreateInfo) {
				.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
				.codeSize = sizeof(renderer_vs_code),
				.pCode = renderer_vs_code,
			}, NULL, &renderer->pipeline.vs);
	renderer_vk(result, "failed to create vertex shader");

	result = vkCreateShaderModule(renderer->dev,
			&(VkShaderModuleCreateInfo) {
				.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
				.codeSize = sizeof(renderer_fs_code),
				.pCode = renderer_fs_code,
				}, NULL, &renderer->pipeline.fs);
	renderer_vk(result, "failed to create fragment shader");

	result = vkCreateGraphicsPipelines(renderer->dev, VK_NULL_HANDLE, 1,
			&(VkGraphicsPipelineCreateInfo) {
				.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
				.stageCount = 2,
				.pStages = (VkPipelineShaderStageCreateInfo[]) {
					{
						.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
						.stage = VK_SHADER_STAGE_VERTEX_BIT,
						.module = renderer->pipeline.vs,
						.pName = "main",
					},
					{
						.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
						.stage = VK_SHADER_STAGE_FRAGMENT_BIT,
						.module = renderer->pipeline.fs,
						.pName = "main",
					},
				},
				.pVertexInputState = &(VkPipelineVertexInputStateCreateInfo) {
					.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
					.vertexBindingDescriptionCount = 1,
					.pVertexBindingDescriptions = &(VkVertexInputBindingDescription) {
						.stride = sizeof(float[2]),
						.inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
					},
					.vertexAttributeDescriptionCount = 1,
					.pVertexAttributeDescriptions = &(VkVertexInputAttributeDescription) {
						.format = VK_FORMAT_R32G32_SFLOAT,
					},
				},
				.pInputAssemblyState = &(VkPipelineInputAssemblyStateCreateInfo) {
					.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
					.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
				},
				.pViewportState = &(VkPipelineViewportStateCreateInfo) {
					.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
					.viewportCount = 1,
					.pViewports = &(VkViewport) {
						.width = (float) renderer->config.width,
						.height = (float) renderer->config.height,
					},
					.scissorCount = 1,
					.pScissors = &(VkRect2D) {
						.extent = {
							.width = renderer->config.width,
							.height = renderer->config.height,
						},
					},
				},
				.pRasterizationState = &(VkPipelineRasterizationStateCreateInfo) {
					.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
					.polygonMode = VK_POLYGON_MODE_FILL,
					.cullMode = VK_CULL_MODE_NONE,
					.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
				},
				.pMultisampleState = &(VkPipelineMultisampleStateCreateInfo) {
					.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
					.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
				},
				.pColorBlendState = &(VkPipelineColorBlendStateCreateInfo) {
					.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
					.attachmentCount = 1,
					.pAttachments = &(VkPipelineColorBlendAttachmentState) {
						.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
							          VK_COLOR_COMPONENT_G_BIT |
							          VK_COLOR_COMPONENT_B_BIT |
							          VK_COLOR_COMPONENT_A_BIT,
					},
				},
				.layout = renderer->pipeline.layout,
				.renderPass = renderer->fb.pass,
				.subpass = 0,
			}, NULL, &renderer->pipeline.pipeline);
	renderer_vk(result, "failed to create pipeline");
}

static void renderer_build_command_buffer(struct renderer *renderer,
		VkCommandBuffer cmd, const struct buffer *output)
{
	VkResult result = vkBeginCommandBuffer(cmd,
			&(VkCommandBufferBeginInfo) {
				.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
			});
	renderer_vk(result, "failed to begin command buffer");

	vkCmdBindVertexBuffers(cmd, 0, 1, &renderer->vb.buf, &(VkDeviceSize) { 0 });

	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
			renderer->pipeline.layout, 0, 1, &renderer->desc.set, 0, NULL);

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
			renderer->pipeline.pipeline);

	/*
	 * We consider memfd/udmabuf plain host memory.  We don't access it
	 * with an external queue (a queue from another compatible Vulkan
	 * instance) nor a foreign queue (a queue from an alien device).  It
	 * is always accessed by this Vulkan instance or the host.  No
	 * queue/resource ownership transfer is required.
	 *
	 * However, whether mmaped accesses to memfd/udmabuf are coherent with
	 * the device is platform-defined.
	 */

	/* vkQueueSubmit implies a domain operation from the host domain to
	 * the device domain.  No explicit barrier on UBO is needed.
	 */

	vkCmdBeginRenderPass(cmd,
			&(VkRenderPassBeginInfo) {
				.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
				.renderPass = renderer->fb.pass,
				.framebuffer = renderer->fb.fb,
				.renderArea = {
					.extent = {
						.width = renderer->config.width,
						.height = renderer->config.height,
					},
				},
				.clearValueCount = 1,
				.pClearValues = &(VkClearValue) {
					.color = { .float32 = { 0.1f, 0.1f, 0.1f, 1.0f } },
				},
			}, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdDraw(cmd, 3, 1, 0, 0);
	vkCmdEndRenderPass(cmd);

	vkCmdCopyImageToBuffer(cmd, renderer->fb.img,
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, output->buf, 1,
			&(VkBufferImageCopy) {
				.imageSubresource = {
					.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
					.layerCount = 1,
				},
				.imageExtent = {
					.width = renderer->config.width,
					.height = renderer->config.height,
					.depth = 1,
				},
			});

	/* Explicit barrier to make sure the transfer is available to the host
	 * domain.
	 */
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1,
			&(VkBufferMemoryBarrier) {
				.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
				.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
				.dstAccessMask = VK_ACCESS_HOST_READ_BIT,
				.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.buffer = output->buf,
				.size = VK_WHOLE_SIZE,
			}, 0, NULL);

	result = vkEndCommandBuffer(cmd);
	renderer_vk(result, "failed to end command buffer");
}

static void renderer_init_vk_cmd(struct renderer *renderer)
{
	VkResult result = vkCreateCommandPool(renderer->dev,
			&(VkCommandPoolCreateInfo) {
				.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
				.queueFamilyIndex = 0,
			}, NULL, &renderer->cmd.pool);
	renderer_vk(result, "failed to create command pool");

	renderer->cmd.bufs = malloc(sizeof(renderer->cmd.bufs[0]) *
			renderer->config.output_count);
	if (!renderer->cmd.bufs)
		renderer_vk(result, "failed to create command buffer array");

	result = vkAllocateCommandBuffers(renderer->dev,
			&(VkCommandBufferAllocateInfo) {
				.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
				.commandPool = renderer->cmd.pool,
				.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
				.commandBufferCount = renderer->config.output_count,
			}, renderer->cmd.bufs);
	renderer_vk(result, "failed to allocate command buffer");

	for (int i = 0; i < renderer->config.output_count; i++) {
		renderer_build_command_buffer(renderer, renderer->cmd.bufs[i],
				&renderer->outputs[i]);
	}
}

static uint32_t renderer_recv(const struct renderer *renderer)
{
	uint32_t val;
	if (read(renderer->ctrl.in, &val, sizeof(val)) != sizeof(val))
		renderer_fatal("failed to receive a value");

	return val;
}

static void renderer_send(const struct renderer *renderer, uint32_t val)
{
	if (write(renderer->ctrl.out, &val, sizeof(val)) != sizeof(val))
		renderer_fatal("failed to send a value");
}

static void renderer_render(const struct renderer *renderer, int output)
{
	VkResult result = vkQueueSubmit(renderer->queue, 1,
			&(VkSubmitInfo) {
				.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
				.commandBufferCount = 1,
				.pCommandBuffers = &renderer->cmd.bufs[output],
			}, VK_NULL_HANDLE);
	renderer_vk(result, "failed to submit command buffer");

	result = vkQueueWaitIdle(renderer->queue);
	renderer_vk(result, "failed to wait queue");
}

static void renderer_mainloop(const struct renderer *renderer)
{
	while (true) {
		const int output = renderer_recv(renderer);
		renderer_render(renderer, output);
		renderer_send(renderer, output);
	}
}

int renderer(int width, int height, int output_count, int ctrl_in,
		int ctrl_out, int memfd, bool use_udmabuf)
{
	struct renderer renderer = {
		.config = {
			.width = width,
			.height = height,
			.output_count = output_count,
			.use_udmabuf = use_udmabuf,
		},
		.ctrl = {
			.in = ctrl_in,
			.out = ctrl_out,
		},
	};

	renderer_init_heap(&renderer, memfd);
	renderer_init_vk_instance(&renderer);
	renderer_init_vk_physical_device(&renderer);
	renderer_init_vk_device(&renderer);
	renderer_init_heap_layout(&renderer);

	/* send the heap layout */
	renderer_send(&renderer, renderer.heap_layout.base_skip);
	renderer_send(&renderer, renderer.heap_layout.ubo_size);
	renderer_send(&renderer, renderer.heap_layout.output_size);

	renderer_init_heap_buffers(&renderer);
	renderer_init_vk_vertex_buffer(&renderer);
	renderer_init_vk_descriptor_set(&renderer);
	renderer_init_vk_framebuffer(&renderer);
	renderer_init_vk_pipeline(&renderer);
	renderer_init_vk_cmd(&renderer);

	renderer_mainloop(&renderer);

	return 0;
}
