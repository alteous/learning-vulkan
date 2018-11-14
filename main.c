/*
 * Aim:
 *
 * 1. Create a render pass with a colour attachment.
 * 2. Clear the colour attachment to a solid yellow colour.
 * 3. Copy the contents of the colour attachment to a host-visible image.
 * 4. Confirm the the host-visible image is now yellow.
 */

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <vulkan/vulkan.h>

#define COUNT(x) (sizeof(x) / sizeof(*(x)))

void check(int condition, const char *description)
{
    if (!condition) {
	printf("check condition failed: %s\n", description);
	exit(-1);
    }
}

const char *instance_layers[] = {
    "VK_LAYER_LUNARG_standard_validation",
    "VK_LAYER_LUNARG_parameter_validation",
    "VK_LAYER_LUNARG_core_validation",
};

/* Instance of the Vulkan API */
VkInstance instance;
/* User allocator callbacks */
VkAllocationCallbacks *allocator = NULL;
/* Identifier for a GPU device on the system */
VkPhysicalDevice physical_device;
/* Memory properties of the physical device */
VkPhysicalDeviceMemoryProperties memory_properties;
/* Connection between a VkInstance and a VkPhysicalDevice */
VkDevice device;
/* Serializer for commands to be sent to the GPU */
VkQueue queue;
/* Index of the queue family that queue belongs to */
uint32_t queue_family_index;

/* Abstract handle to an image plus an immutable bind to GPU memory */
VkImage read_image, write_image;
/* The region of memory we will be reading from the image */
VkImageView read_image_view, write_image_view;
/* GPU memory bound to the image */
VkDeviceMemory read_image_memory, write_image_memory;

/* A graphics pipeline */
VkRenderPass render_pass;
/* Collection of memory attachments that the render pass will use */
VkFramebuffer framebuffer;
/* Synchronization primitive to signal the completion of GPU work */
VkFence fence;

/* Command buffer to queue/prepare GPU commands for submission into a queue */
VkCommandBuffer command_buffer;
/* Pool for allocating memory for the command buffer */
VkCommandPool command_pool;

int find_appropriate_memory_type(VkMemoryRequirements reqs, VkMemoryPropertyFlags props, uint32_t *type)
{
    for (*type = 0; *type < memory_properties.memoryTypeCount; ++(*type)) {
	if ((1 << *type) & reqs.memoryTypeBits) {
	    if ((memory_properties.memoryTypes[*type].propertyFlags & props) == props) {
		return 0;
	    }
	}
    }
    return -1;
}

int main(int argc, char **argv)
{
    VkResult res;

    /* Query the instance layers offered by the system */
    {
	uint32_t count;
	vkEnumerateInstanceLayerProperties(&count, NULL);
	VkLayerProperties layers[count];
	vkEnumerateInstanceLayerProperties(&count, layers);
	puts("Available layers:");
	for (uint32_t i = 0; i < count; ++i) {
	    printf("%s\n", layers[i].layerName);
	}
	puts("");
    }

    /* Create an instance of the Vulkan API */
    {
	VkInstanceCreateInfo args = {
	    .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
	    .enabledLayerCount = COUNT(instance_layers),
	    .ppEnabledLayerNames = instance_layers,
	};
	res = vkCreateInstance(&args, allocator, &instance);
	check(res == VK_SUCCESS, "");
    }

    /* Find a GPU to render with and grab its properties */
    {
	uint32_t count = 1;
	res = vkEnumeratePhysicalDevices(instance, &count, &physical_device);
	check(res == VK_SUCCESS, "");

	VkPhysicalDeviceProperties dprops;
	vkGetPhysicalDeviceProperties(physical_device, &dprops);
	printf("API version: %d\n", (int)dprops.apiVersion);
	printf("Driver version: %d\n", (int)dprops.driverVersion);
	printf("Vendor ID: %d\n", (int)dprops.vendorID);
	printf("Device ID: %d\n", (int)dprops.deviceID);
	printf("Device name: %s\n", dprops.deviceName);
	puts("");

	vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);
	for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
	    printf("Memory type #%d\n", (int)i);
	    printf("Heap index: %d\n", (int)memory_properties.memoryTypes[i].heapIndex);
	    printf("DEVICE_LOCAL: %s\n", (memory_properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? "Y" : "N");
	    printf("HOST_VISIBLE: %s\n", (memory_properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ? "Y" : "N");
	    printf("HOST_COHERENT: %s\n", (memory_properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ? "Y" : "N");
	    printf("HOST_CACHED: %s\n", (memory_properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ? "Y" : "N");
	    printf("LAZILY_ALLOCATED: %s\n", (memory_properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) ? "Y" : "N");
	    puts("");
	}

	for (uint32_t i = 0; i < memory_properties.memoryHeapCount; ++i) {
	    printf("Memory heap #%d\n", (int)i);
	    printf("Size: %zu\n", (size_t)memory_properties.memoryHeaps[i].size);
	    printf("DEVICE_LOCAL: %s\n", (memory_properties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? "Y" : "N");
	    puts("");
	}

	vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &count, NULL);
	VkQueueFamilyProperties qprops[count];
  	vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &count, qprops);
	for (uint32_t i = 0; i < count; ++i) {
	    printf("Queue family #%d:\n", (int)i);
	    printf("GRAPHICS_BIT: %s\n", (qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) ? "Y" : "N");
	    printf("COMPUTE_BIT: %s\n", (qprops[i].queueFlags & VK_QUEUE_COMPUTE_BIT) ? "Y" : "N");
	    printf("TRANSFER_BIT: %s\n", (qprops[i].queueFlags & VK_QUEUE_TRANSFER_BIT) ? "Y" : "N");
	    puts("");
	}

	queue_family_index = 0;
	check(qprops[queue_family_index].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_TRANSFER_BIT), "");
    }

    /* Create a connection from the Vulkan API (VkInstance) to the GPU (VkPhysicalDevice) */
    {
	VkDeviceQueueCreateInfo queue_args = {
	    .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
	    .queueFamilyIndex = queue_family_index,
	    .queueCount = 1,
	    .pQueuePriorities = (float[]){ 1.0 },
	};
	VkDeviceCreateInfo args = {
	    .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
	    .queueCreateInfoCount = 1,
	    .pQueueCreateInfos = &queue_args,
	};
	res = vkCreateDevice(physical_device, &args, allocator, &device);
	check(res == VK_SUCCESS, "vkCreateDevice");
	vkGetDeviceQueue(device, queue_family_index, 0, &queue);
    }

    /* Create the write_image to render to */
    {
	VkImageCreateInfo args = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	    .format = VK_FORMAT_R8G8B8A8_UNORM,
	    .imageType = VK_IMAGE_TYPE_2D,
	    .extent = (VkExtent3D){ .width = 400, .height = 400, .depth = 1 },
	    .tiling = VK_IMAGE_TILING_OPTIMAL,
	    .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
	    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	    .mipLevels = 1,
	    .arrayLayers = 1,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	res = vkCreateImage(device, &args, allocator, &write_image);
	check(res == VK_SUCCESS, "vkCreateImage(write_image)");
    }

    /* Create and bind memory to the write_image */
    {
	VkMemoryRequirements reqs;
	vkGetImageMemoryRequirements(device, write_image, &reqs);
	uint32_t memory_type;
	VkMemoryPropertyFlags desired_flags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	res = find_appropriate_memory_type(reqs, desired_flags, &memory_type);
	check(res == 0, "find_appropriate_memory_type for write_image");
	printf("Chosen memory type for write_image: %d\n", (int)memory_type);
	puts("");
	VkMemoryAllocateInfo margs = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	    .allocationSize = reqs.size,
	    .memoryTypeIndex = memory_type,
	};
	res = vkAllocateMemory(device, &margs, allocator, &write_image_memory);
	check(res == VK_SUCCESS, "vkAllocateMemory(write_image)");
	res = vkBindImageMemory(device, write_image, write_image_memory, 0);
	check(res == VK_SUCCESS, "vkBindImageMemory(write_image)");
	VkImageViewCreateInfo vargs = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
	    .image = write_image,
	    .viewType = VK_IMAGE_VIEW_TYPE_2D,
	    .format = VK_FORMAT_R8G8B8A8_UNORM,
	    .subresourceRange = {
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.layerCount = 1,
		.levelCount = VK_REMAINING_MIP_LEVELS,
	    },
	};
	res = vkCreateImageView(device, &vargs, allocator, &write_image_view);
	check(res == VK_SUCCESS, "vkCreateImageView(write_image)");
    }

    /* Create the read_image to copy the render result to */
    {
	VkImageCreateInfo args = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	    .format = VK_FORMAT_R8G8B8A8_UNORM,
	    .imageType = VK_IMAGE_TYPE_2D,
	    .extent = (VkExtent3D){ .width = 400, .height = 400, .depth = 1 },
	    .tiling = VK_IMAGE_TILING_LINEAR,
	    .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
	    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	    .mipLevels = 1,
	    .arrayLayers = 1,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	res = vkCreateImage(device, &args, allocator, &read_image);
	check(res == VK_SUCCESS, "vkCreateImage(read_image)");
    }

    /* Create and bind memory to the read_image */
    {
	VkMemoryRequirements reqs;
	vkGetImageMemoryRequirements(device, read_image, &reqs);
	uint32_t memory_type;
	VkMemoryPropertyFlags desired_flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
	res = find_appropriate_memory_type(reqs, desired_flags, &memory_type);
	check(res == 0, "find_appropriate_memory_type for read_image");
	printf("Chosen memory type for write_image: %d\n", (int)memory_type);
	puts("");
	VkMemoryAllocateInfo margs = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	    .allocationSize = reqs.size,
	    .memoryTypeIndex = memory_type,
	};
	res = vkAllocateMemory(device, &margs, allocator, &read_image_memory);
	check(res == VK_SUCCESS, "vkAllocateMemory(read_image)");
	res = vkBindImageMemory(device, read_image, read_image_memory, 0);
	check(res == VK_SUCCESS, "vkBindImageMemory(read_image)");
	VkImageViewCreateInfo vargs = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
	    .image = read_image,
	    .viewType = VK_IMAGE_VIEW_TYPE_2D,
	    .format = VK_FORMAT_R8G8B8A8_UNORM,
	    .subresourceRange = {
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.layerCount = 1,
		.levelCount = VK_REMAINING_MIP_LEVELS,
	    },
	};
	res = vkCreateImageView(device, &vargs, allocator, &read_image_view);
	check(res == VK_SUCCESS, "vkCreateImageView(read_image)");
    }

    /* Create the graphics pipeline */
    {
	VkAttachmentDescription color_attachment = {
	    .format = VK_FORMAT_R8G8B8A8_UNORM,
	    .samples = 1,
	    .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
	    .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
	    .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	};
	VkAttachmentReference color_attachment_reference = {
	    .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	    .attachment = 0,
	};
	VkSubpassDescription subpass_description = {
	    .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
	    .colorAttachmentCount = 1,
	    .pColorAttachments = &color_attachment_reference,
	};
	VkRenderPassCreateInfo args = {
	    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
	    .attachmentCount = 1,
	    .pAttachments = &color_attachment,
	    .subpassCount = 1,
	    .pSubpasses = &subpass_description,
	};
	res = vkCreateRenderPass(device, &args, allocator, &render_pass);
	check(res == VK_SUCCESS, "vkCreateRenderPass");
    }

    /* Create a framebuffer to render to */
    {
	VkFramebufferCreateInfo args = {
	    .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
	    .width = 400,
	    .height = 400,
	    .layers = 1,
	    .attachmentCount = 1,
	    .pAttachments = &write_image_view,
	    .renderPass = render_pass,
	};
	res = vkCreateFramebuffer(device, &args, allocator, &framebuffer);
	check(res == VK_SUCCESS, "vkFramebufferCreateInfo");
    }

    /* Create a command pool in order to create a command buffer */
    {
	VkCommandPoolCreateInfo args = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
	    .queueFamilyIndex = queue_family_index,
	};
	res = vkCreateCommandPool(device, &args, allocator, &command_pool);
	check(res == VK_SUCCESS, "vkCommandPoolCreateInfo");
    }

    /* Create a command buffer from the command pool */
    {
	VkCommandBufferAllocateInfo args = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
	    .commandPool = command_pool,
	    .commandBufferCount = 1,
	};
	res = vkAllocateCommandBuffers(device, &args, &command_buffer);
	check(res == VK_SUCCESS, "vkAllocateCommandBuffers");
    }

    /* Commence recording commands */
    {
	VkCommandBufferBeginInfo args = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	res = vkBeginCommandBuffer(command_buffer, &args);
	check(res == VK_SUCCESS, "vkBeginCommandBuffer");
    }
    
    /* Begin recording rendering commands */
    {
	VkRenderPassBeginInfo args = {
	    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
	    .renderPass = render_pass,
	    .framebuffer = framebuffer,
	    .renderArea = { .extent.width = 400, .extent.height = 400 },
	};
	VkSubpassContents contents = VK_SUBPASS_CONTENTS_INLINE;
	vkCmdBeginRenderPass(command_buffer, &args, contents);
    }

    /* Clear the color attachment */
    {
	VkClearRect rect = {
	    .layerCount = 1,
	    .rect = {
		.extent = {
		    .width = 400,
		    .height = 400,
		},
	    },
	};
	VkClearAttachment args = {
	    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	    .colorAttachment = 0,
	    .clearValue.color.float32 = {1.0, 1.0, 0.0, 1.0},
	};
	vkCmdClearAttachments(command_buffer, 1, &args, 1, &rect);
	vkCmdEndRenderPass(command_buffer);
    }

    /* Use pipeline barrier to transition the image layouts */
    {
	VkPipelineStageFlags src_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	VkPipelineStageFlags dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	VkDependencyFlags dependencies = 0;
	uint32_t generic_memory_barrier_count = 0;
	VkMemoryBarrier *generic_memory_barriers = NULL;
	uint32_t buffer_memory_barrier_count = 0;
	VkBufferMemoryBarrier *buffer_memory_barriers = NULL;
	uint32_t image_memory_barrier_count = 2;
	VkImageMemoryBarrier image_memory_barriers[] = {
	    {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = 0,
		.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		.srcQueueFamilyIndex = queue_family_index,
		.dstQueueFamilyIndex = queue_family_index,
		.image = read_image,
		.subresourceRange = {
		    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		    .levelCount = VK_REMAINING_MIP_LEVELS,
		    .layerCount = VK_REMAINING_ARRAY_LAYERS,
		},
	    },
	    {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		.srcQueueFamilyIndex = queue_family_index,
		.dstQueueFamilyIndex = queue_family_index,
		.image = write_image,
		.subresourceRange = {
		    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		    .levelCount = VK_REMAINING_MIP_LEVELS,
		    .layerCount = VK_REMAINING_ARRAY_LAYERS,
		},
	    },
	};
	vkCmdPipelineBarrier(
            command_buffer,
	    src_stage,
	    dst_stage,
	    dependencies,
	    generic_memory_barrier_count,
	    generic_memory_barriers,
	    buffer_memory_barrier_count,
	    buffer_memory_barriers,
	    image_memory_barrier_count,
	    image_memory_barriers
        );
    }

    /* Finish the render pass and copy the results to our read_image */
    {	
	VkImageCopy region = {
	    .srcSubresource = {
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.layerCount = 1,
	    },
	    .dstSubresource = {
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.layerCount = 1,
	    },
	    .extent = {
		.width = 400,
		.height = 400,
		.depth = 1,
	    },
	};
	vkCmdCopyImage(
            command_buffer,
	    write_image,
	    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	    read_image,
	    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	    1,
	    &region
	);
    }    

    /* Use another pipeline barrier to transition the read_image for reading */
    {
	VkPipelineStageFlags src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	VkPipelineStageFlags dst_stage = VK_PIPELINE_STAGE_HOST_BIT;
	VkDependencyFlags dependencies = 0;
	uint32_t generic_memory_barrier_count = 0;
	VkMemoryBarrier *generic_memory_barriers = NULL;
	uint32_t buffer_memory_barrier_count = 0;
	VkBufferMemoryBarrier *buffer_memory_barriers = NULL;
	uint32_t image_memory_barrier_count = 1;
	VkImageMemoryBarrier image_memory_barriers[] = {
	    {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_HOST_READ_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		.newLayout = VK_IMAGE_LAYOUT_GENERAL,
		.srcQueueFamilyIndex = queue_family_index,
		.dstQueueFamilyIndex = queue_family_index,
		.image = read_image,
		.subresourceRange = {
		    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		    .levelCount = VK_REMAINING_MIP_LEVELS,
		    .layerCount = VK_REMAINING_ARRAY_LAYERS,
		},
	    },
	};
	vkCmdPipelineBarrier(
            command_buffer,
	    src_stage,
	    dst_stage,
	    dependencies,
	    generic_memory_barrier_count,
	    generic_memory_barriers,
	    buffer_memory_barrier_count,
	    buffer_memory_barriers,
	    image_memory_barrier_count,
	    image_memory_barriers
        );
    }

    /* Stop recording commands */
    {
	res = vkEndCommandBuffer(command_buffer);
	check(res == VK_SUCCESS, "vkEndCommandBuffer");
    }

    /* Create a fence to signal the completion of GPU work */
    {
	VkFenceCreateInfo args = {
	    .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
	};
	res = vkCreateFence(device, &args, allocator, &fence);
	check(res == VK_SUCCESS, "vkCreateFence");
    }
    
    /* Submit command buffer into the queue */
    {
	VkSubmitInfo args = {
	    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
	    .commandBufferCount = 1,
	    .pCommandBuffers = &command_buffer,
	};
	res = vkQueueSubmit(queue, 1, &args, fence);
	check(res == VK_SUCCESS, "vkQueueSubmit");
    }

    /* Wait for GPU work to complete */
    {
	res = vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
	check(res == VK_SUCCESS, "vkWaitForFence");
    }

    /* Read back the results */
    {
	void *data;
	VkDeviceSize offset = 0, size = VK_WHOLE_SIZE;
	res = vkMapMemory(device, read_image_memory, offset, size, 0, &data);
	check(res == VK_SUCCESS, "vkMapMemory");
	uint32_t pixel = *(uint32_t *)data;
	uint8_t red = pixel & 0xff;
	uint8_t green = (pixel >> 8) & 0xff;
	uint8_t blue = (pixel >> 16) & 0xff;
	uint8_t alpha = (pixel >> 24) & 0xff;
	printf("r = %#x, g = %#x, b = %#x, a = %#x\n", red, green, blue, alpha);
	check(red == 0xff, "red == 0xff");
	check(green == 0xff, "green == 0xff");
	check(blue == 0, "blue == 0");
	check(alpha == 0xff, "alpha == 0xff");
    }

    return 0;
}
