// Copyright 2020 The SwiftShader Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "benchmark/benchmark.h"

#if !defined(USE_HEADLESS_SURFACE)
#	define USE_HEADLESS_SURFACE 0
#endif

#if !defined(_WIN32)
// @TODO: implement native Window support for current platform. For now, always use HeadlessSurface.
#	undef USE_HEADLESS_SURFACE
#	define USE_HEADLESS_SURFACE 1
#endif

#if defined(_WIN32)
#	define VK_USE_PLATFORM_WIN32_KHR
#endif
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#define VULKAN_HPP_NO_NODISCARD_WARNINGS
#include <vulkan/vulkan.hpp>
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

#include "SPIRV/GlslangToSpv.h"
#include "StandAlone/ResourceLimits.h"
#include "glslang/Public/ShaderLang.h"

#if defined(_WIN32)
#	define WIN32_LEAN_AND_MEAN
#	include <Windows.h>
#endif

#include <cassert>
#include <vector>

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof(arr[0]))

namespace {
uint32_t getMemoryTypeIndex(vk::PhysicalDevice physicalDevice, uint32_t typeBits, vk::MemoryPropertyFlags properties)
{
	vk::PhysicalDeviceMemoryProperties deviceMemoryProperties = physicalDevice.getMemoryProperties();
	for(uint32_t i = 0; i < deviceMemoryProperties.memoryTypeCount; i++)
	{
		if((typeBits & 1) == 1)
		{
			if((deviceMemoryProperties.memoryTypes[i].propertyFlags & properties) == properties)
			{
				return i;
			}
		}
		typeBits >>= 1;
	}

	assert(false);
	return -1;
}

vk::CommandBuffer beginSingleTimeCommands(vk::Device device, vk::CommandPool commandPool)
{
	vk::CommandBufferAllocateInfo allocInfo{};
	allocInfo.level = vk::CommandBufferLevel::ePrimary;
	allocInfo.commandPool = commandPool;
	allocInfo.commandBufferCount = 1;

	auto commandBuffer = device.allocateCommandBuffers(allocInfo);

	vk::CommandBufferBeginInfo beginInfo{};
	beginInfo.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;

	commandBuffer[0].begin(beginInfo);

	return commandBuffer[0];
}

void endSingleTimeCommands(vk::Device device, vk::CommandPool commandPool, vk::Queue queue, vk::CommandBuffer commandBuffer)
{
	commandBuffer.end();

	vk::SubmitInfo submitInfo{};
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &commandBuffer;

	vk::Fence fence = {};  // TODO: pass in fence?
	queue.submit(1, &submitInfo, fence);
	queue.waitIdle();

	device.freeCommandBuffers(commandPool, 1, &commandBuffer);
}

void transitionImageLayout(vk::Device device, vk::CommandPool commandPool, vk::Queue queue, vk::Image image, vk::Format format, vk::ImageLayout oldLayout, vk::ImageLayout newLayout)
{
	vk::CommandBuffer commandBuffer = beginSingleTimeCommands(device, commandPool);

	vk::ImageMemoryBarrier barrier{};
	barrier.oldLayout = oldLayout;
	barrier.newLayout = newLayout;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
	barrier.subresourceRange.baseMipLevel = 0;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.baseArrayLayer = 0;
	barrier.subresourceRange.layerCount = 1;

	vk::PipelineStageFlags sourceStage;
	vk::PipelineStageFlags destinationStage;

	if(oldLayout == vk::ImageLayout::eUndefined && newLayout == vk::ImageLayout::eTransferDstOptimal)
	{
		barrier.srcAccessMask = {};
		barrier.dstAccessMask = vk::AccessFlagBits::eTransferWrite;

		sourceStage = vk::PipelineStageFlagBits::eTopOfPipe;
		destinationStage = vk::PipelineStageFlagBits::eTransfer;
	}
	else if(oldLayout == vk::ImageLayout::eTransferDstOptimal && newLayout == vk::ImageLayout::eShaderReadOnlyOptimal)
	{
		barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
		barrier.dstAccessMask = vk::AccessFlagBits::eTransferRead;

		sourceStage = vk::PipelineStageFlagBits::eTransfer;
		destinationStage = vk::PipelineStageFlagBits::eFragmentShader;
	}
	else
	{
		assert(!"unsupported layout transition!");
	}

	commandBuffer.pipelineBarrier(sourceStage, destinationStage, vk::DependencyFlags{}, 0, nullptr, 0, nullptr, 1, &barrier);

	endSingleTimeCommands(device, commandPool, queue, commandBuffer);
}

void copyBufferToImage(vk::Device device, vk::CommandPool commandPool, vk::Queue queue, vk::Buffer buffer, vk::Image image, uint32_t width, uint32_t height)
{
	vk::CommandBuffer commandBuffer = beginSingleTimeCommands(device, commandPool);

	vk::BufferImageCopy region{};
	region.bufferOffset = 0;
	region.bufferRowLength = 0;
	region.bufferImageHeight = 0;
	region.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
	region.imageSubresource.mipLevel = 0;
	region.imageSubresource.baseArrayLayer = 0;
	region.imageSubresource.layerCount = 1;
	region.imageOffset = vk::Offset3D{ 0, 0, 0 };
	region.imageExtent = vk::Extent3D{ width, height, 1 };

	commandBuffer.copyBufferToImage(buffer, image, vk::ImageLayout::eTransferDstOptimal, 1, &region);

	endSingleTimeCommands(device, commandPool, queue, commandBuffer);
}

}  // namespace

class VulkanBenchmark
{
public:
	VulkanBenchmark()
	{
		// TODO(b/158231104): Other platforms
#if defined(_WIN32)
		dl = std::make_unique<vk::DynamicLoader>("./vk_swiftshader.dll");
#elif defined(__linux__)
		dl = std::make_unique<vk::DynamicLoader>("./libvk_swiftshader.so");
#else
#	error Unimplemented platform
#endif
		assert(dl->success());

		PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = dl->getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
		VULKAN_HPP_DEFAULT_DISPATCHER.init(vkGetInstanceProcAddr);

		instance = vk::createInstance({}, nullptr);
		VULKAN_HPP_DEFAULT_DISPATCHER.init(instance);

		std::vector<vk::PhysicalDevice> physicalDevices = instance.enumeratePhysicalDevices();
		assert(!physicalDevices.empty());
		physicalDevice = physicalDevices[0];

		const float defaultQueuePriority = 0.0f;
		vk::DeviceQueueCreateInfo queueCreatInfo;
		queueCreatInfo.queueFamilyIndex = queueFamilyIndex;
		queueCreatInfo.queueCount = 1;
		queueCreatInfo.pQueuePriorities = &defaultQueuePriority;

		vk::DeviceCreateInfo deviceCreateInfo;
		deviceCreateInfo.queueCreateInfoCount = 1;
		deviceCreateInfo.pQueueCreateInfos = &queueCreatInfo;

		device = physicalDevice.createDevice(deviceCreateInfo, nullptr);

		queue = device.getQueue(queueFamilyIndex, 0);
	}

	virtual ~VulkanBenchmark()
	{
		device.waitIdle();
		device.destroy(nullptr);
		instance.destroy(nullptr);
	}

private:
	std::unique_ptr<vk::DynamicLoader> dl;

protected:
	const uint32_t queueFamilyIndex = 0;

	vk::Instance instance;  // Owning handle
	vk::PhysicalDevice physicalDevice;
	vk::Device device;  // Owning handle
	vk::Queue queue;
};

class ClearImageBenchmark : public VulkanBenchmark
{
public:
	ClearImageBenchmark(vk::Format clearFormat, vk::ImageAspectFlagBits clearAspect)
	{
		vk::ImageCreateInfo imageInfo;
		imageInfo.imageType = vk::ImageType::e2D;
		imageInfo.format = clearFormat;
		imageInfo.tiling = vk::ImageTiling::eOptimal;
		imageInfo.initialLayout = vk::ImageLayout::eGeneral;
		imageInfo.usage = vk::ImageUsageFlagBits::eTransferDst;
		imageInfo.samples = vk::SampleCountFlagBits::e4;
		imageInfo.extent = vk::Extent3D(1024, 1024, 1);
		imageInfo.mipLevels = 1;
		imageInfo.arrayLayers = 1;

		image = device.createImage(imageInfo);

		vk::MemoryRequirements memoryRequirements = device.getImageMemoryRequirements(image);

		vk::MemoryAllocateInfo allocateInfo;
		allocateInfo.allocationSize = memoryRequirements.size;
		allocateInfo.memoryTypeIndex = 0;

		memory = device.allocateMemory(allocateInfo);

		device.bindImageMemory(image, memory, 0);

		vk::CommandPoolCreateInfo commandPoolCreateInfo;
		commandPoolCreateInfo.queueFamilyIndex = queueFamilyIndex;

		commandPool = device.createCommandPool(commandPoolCreateInfo);

		vk::CommandBufferAllocateInfo commandBufferAllocateInfo;
		commandBufferAllocateInfo.commandPool = commandPool;
		commandBufferAllocateInfo.commandBufferCount = 1;

		commandBuffer = device.allocateCommandBuffers(commandBufferAllocateInfo)[0];

		vk::CommandBufferBeginInfo commandBufferBeginInfo;
		commandBufferBeginInfo.flags = {};

		commandBuffer.begin(commandBufferBeginInfo);

		vk::ImageSubresourceRange range;
		range.aspectMask = clearAspect;
		range.baseMipLevel = 0;
		range.levelCount = 1;
		range.baseArrayLayer = 0;
		range.layerCount = 1;

		if(clearAspect == vk::ImageAspectFlagBits::eColor)
		{
			vk::ClearColorValue clearColorValue;
			clearColorValue.float32[0] = 0.0f;
			clearColorValue.float32[1] = 1.0f;
			clearColorValue.float32[2] = 0.0f;
			clearColorValue.float32[3] = 1.0f;

			commandBuffer.clearColorImage(image, vk::ImageLayout::eGeneral, &clearColorValue, 1, &range);
		}
		else if(clearAspect == vk::ImageAspectFlagBits::eDepth)
		{
			vk::ClearDepthStencilValue clearDepthStencilValue;
			clearDepthStencilValue.depth = 1.0f;
			clearDepthStencilValue.stencil = 0xFF;

			commandBuffer.clearDepthStencilImage(image, vk::ImageLayout::eGeneral, &clearDepthStencilValue, 1, &range);
		}
		else
			assert(false);

		commandBuffer.end();
	}

	~ClearImageBenchmark()
	{
		device.freeCommandBuffers(commandPool, 1, &commandBuffer);
		device.destroyCommandPool(commandPool, nullptr);
		device.freeMemory(memory, nullptr);
		device.destroyImage(image, nullptr);
	}

	void clear()
	{
		vk::SubmitInfo submitInfo;
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &commandBuffer;

		queue.submit(1, &submitInfo, nullptr);
		queue.waitIdle();
	}

private:
	vk::Image image;                  // Owning handle
	vk::DeviceMemory memory;          // Owning handle
	vk::CommandPool commandPool;      // Owning handle
	vk::CommandBuffer commandBuffer;  // Owning handle
};

static void ClearImage(benchmark::State &state, vk::Format clearFormat, vk::ImageAspectFlagBits clearAspect)
{
	ClearImageBenchmark benchmark(clearFormat, clearAspect);

	// Execute once to have the Reactor routine generated.
	benchmark.clear();

	for(auto _ : state)
	{
		benchmark.clear();
	}
}

#if USE_HEADLESS_SURFACE
class Window
{
public:
	Window(vk::Instance instance, vk::Extent2D windowSize)
	{
		vk::HeadlessSurfaceCreateInfoEXT surfaceCreateInfo;
		surface = instance.createHeadlessSurfaceEXT(surfaceCreateInfo);
		assert(surface);
	}

	~Window()
	{
		instance.destroySurfaceKHR(surface, nullptr);
	}

	vk::SurfaceKHR getSurface()
	{
		return surface;
	}

	void show()
	{
	}

private:
	const vk::Instance instance;
	vk::SurfaceKHR surface;
};
#elif defined(_WIN32)
class Window
{
public:
	Window(vk::Instance instance, vk::Extent2D windowSize)
	{
		windowClass.cbSize = sizeof(WNDCLASSEX);
		windowClass.style = CS_HREDRAW | CS_VREDRAW;
		windowClass.lpfnWndProc = DefWindowProc;
		windowClass.cbClsExtra = 0;
		windowClass.cbWndExtra = 0;
		windowClass.hInstance = moduleInstance;
		windowClass.hIcon = LoadIcon(NULL, IDI_APPLICATION);
		windowClass.hCursor = LoadCursor(NULL, IDC_ARROW);
		windowClass.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
		windowClass.lpszMenuName = NULL;
		windowClass.lpszClassName = "Window";
		windowClass.hIconSm = LoadIcon(NULL, IDI_WINLOGO);

		RegisterClassEx(&windowClass);

		DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
		DWORD extendedStyle = WS_EX_APPWINDOW | WS_EX_WINDOWEDGE;

		RECT windowRect;
		windowRect.left = 0L;
		windowRect.top = 0L;
		windowRect.right = (long)windowSize.width;
		windowRect.bottom = (long)windowSize.height;

		AdjustWindowRectEx(&windowRect, style, FALSE, extendedStyle);
		uint32_t x = (GetSystemMetrics(SM_CXSCREEN) - windowRect.right) / 2;
		uint32_t y = (GetSystemMetrics(SM_CYSCREEN) - windowRect.bottom) / 2;

		window = CreateWindowEx(extendedStyle, "Window", "Hello",
		                        style | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
		                        x, y,
		                        windowRect.right - windowRect.left,
		                        windowRect.bottom - windowRect.top,
		                        NULL, NULL, moduleInstance, NULL);

		SetForegroundWindow(window);
		SetFocus(window);

		// Create the Vulkan surface
		vk::Win32SurfaceCreateInfoKHR surfaceCreateInfo;
		surfaceCreateInfo.hinstance = moduleInstance;
		surfaceCreateInfo.hwnd = window;
		surface = instance.createWin32SurfaceKHR(surfaceCreateInfo);
		assert(surface);
	}

	~Window()
	{
		instance.destroySurfaceKHR(surface, nullptr);
		DestroyWindow(window);
		UnregisterClass("Window", moduleInstance);
	}

	vk::SurfaceKHR getSurface()
	{
		return surface;
	}

	void show()
	{
		ShowWindow(window, SW_SHOW);
	}

private:
	HWND window;
	HINSTANCE moduleInstance;
	WNDCLASSEX windowClass;
	const vk::Instance instance;
	vk::SurfaceKHR surface;
};
#else
#	error Window class unimplemented for this platform
#endif

class Swapchain
{
public:
	Swapchain(vk::PhysicalDevice physicalDevice, vk::Device device, Window &window)
	    : device(device)
	{
		vk::SurfaceKHR surface = window.getSurface();

		// Create the swapchain
		vk::SurfaceCapabilitiesKHR surfaceCapabilities = physicalDevice.getSurfaceCapabilitiesKHR(surface);
		extent = surfaceCapabilities.currentExtent;

		vk::SwapchainCreateInfoKHR swapchainCreateInfo;
		swapchainCreateInfo.surface = surface;
		swapchainCreateInfo.minImageCount = 2;  // double-buffered
		swapchainCreateInfo.imageFormat = colorFormat;
		swapchainCreateInfo.imageColorSpace = vk::ColorSpaceKHR::eSrgbNonlinear;
		swapchainCreateInfo.imageExtent = extent;
		swapchainCreateInfo.imageUsage = vk::ImageUsageFlagBits::eColorAttachment;
		swapchainCreateInfo.preTransform = vk::SurfaceTransformFlagBitsKHR::eIdentity;
		swapchainCreateInfo.imageArrayLayers = 1;
		swapchainCreateInfo.imageSharingMode = vk::SharingMode::eExclusive;
		swapchainCreateInfo.presentMode = vk::PresentModeKHR::eFifo;
		swapchainCreateInfo.clipped = VK_TRUE;
		swapchainCreateInfo.compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;

		swapchain = device.createSwapchainKHR(swapchainCreateInfo);

		// Obtain the images and create views for them
		images = device.getSwapchainImagesKHR(swapchain);

		imageViews.resize(images.size());
		for(size_t i = 0; i < imageViews.size(); i++)
		{
			vk::ImageViewCreateInfo colorAttachmentView;
			colorAttachmentView.image = images[i];
			colorAttachmentView.viewType = vk::ImageViewType::e2D;
			colorAttachmentView.format = colorFormat;
			colorAttachmentView.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
			colorAttachmentView.subresourceRange.baseMipLevel = 0;
			colorAttachmentView.subresourceRange.levelCount = 1;
			colorAttachmentView.subresourceRange.baseArrayLayer = 0;
			colorAttachmentView.subresourceRange.layerCount = 1;

			imageViews[i] = device.createImageView(colorAttachmentView);
		}
	}

	~Swapchain()
	{
		for(auto &imageView : imageViews)
		{
			device.destroyImageView(imageView, nullptr);
		}

		device.destroySwapchainKHR(swapchain, nullptr);
	}

	void acquireNextImage(VkSemaphore presentCompleteSemaphore, uint32_t &imageIndex)
	{
		auto result = device.acquireNextImageKHR(swapchain, UINT64_MAX, presentCompleteSemaphore, vk::Fence());
		imageIndex = result.value;
	}

	void queuePresent(vk::Queue queue, uint32_t imageIndex, vk::Semaphore waitSemaphore)
	{
		vk::PresentInfoKHR presentInfo;
		presentInfo.pWaitSemaphores = &waitSemaphore;
		presentInfo.waitSemaphoreCount = 1;
		presentInfo.swapchainCount = 1;
		presentInfo.pSwapchains = &swapchain;
		presentInfo.pImageIndices = &imageIndex;

		queue.presentKHR(presentInfo);
	}

	size_t imageCount() const
	{
		return images.size();
	}

	vk::ImageView getImageView(size_t i) const
	{
		return imageViews[i];
	}

	vk::Extent2D getExtent() const
	{
		return extent;
	}

	const vk::Format colorFormat = vk::Format::eB8G8R8A8Unorm;

private:
	const vk::Device device;

	vk::SwapchainKHR swapchain;  // Owning handle
	vk::Extent2D extent;

	std::vector<vk::Image> images;          // Weak pointers. Presentable images owned by swapchain object.
	std::vector<vk::ImageView> imageViews;  // Owning handles
};

class Buffer
{
public:
	Buffer(vk::Device device, vk::DeviceSize size, vk::BufferUsageFlags usage)
	    : device(device)
	    , size(size)
	{
		vk::BufferCreateInfo bufferInfo{};
		bufferInfo.size = size;
		bufferInfo.usage = usage;
		bufferInfo.sharingMode = vk::SharingMode::eExclusive;

		buffer = device.createBuffer(bufferInfo);

		auto memRequirements = device.getBufferMemoryRequirements(buffer);

		vk::MemoryAllocateInfo allocInfo{};
		allocInfo.allocationSize = memRequirements.size;
		allocInfo.memoryTypeIndex = 0;  //TODO: getMemoryTypeIndex(memRequirements.memoryTypeBits, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);

		bufferMemory = device.allocateMemory(allocInfo);
		device.bindBufferMemory(buffer, bufferMemory, 0);
	}

	~Buffer()
	{
		device.freeMemory(bufferMemory);
		device.destroyBuffer(buffer);
	}

	vk::Buffer getBuffer()
	{
		return buffer;
	}

	void *mapMemory()
	{
		return device.mapMemory(bufferMemory, 0, size);
	}

	void unmapMemory()
	{
		device.unmapMemory(bufferMemory);
	}

private:
	const vk::Device device;
	vk::DeviceSize size;
	vk::Buffer buffer;              // Owning handle
	vk::DeviceMemory bufferMemory;  // Owning handle
};

class Image
{
public:
	Image(vk::Device device, uint32_t width, uint32_t height, vk::Format format, vk::SampleCountFlagBits sampleCount = vk::SampleCountFlagBits::e1)
	    : device(device)
	{
		vk::ImageCreateInfo imageInfo;
		imageInfo.imageType = vk::ImageType::e2D;
		imageInfo.format = format;
		imageInfo.tiling = vk::ImageTiling::eOptimal;
		imageInfo.initialLayout = vk::ImageLayout::eGeneral;
		imageInfo.usage = vk::ImageUsageFlagBits::eColorAttachment;
		imageInfo.samples = sampleCount;
		imageInfo.extent = vk::Extent3D(width, height, 1);
		imageInfo.mipLevels = 1;
		imageInfo.arrayLayers = 1;

		image = device.createImage(imageInfo);

		vk::MemoryRequirements memoryRequirements = device.getImageMemoryRequirements(image);

		vk::MemoryAllocateInfo allocateInfo;
		allocateInfo.allocationSize = memoryRequirements.size;
		allocateInfo.memoryTypeIndex = 0;  //getMemoryTypeIndex(memoryRequirements.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);

		imageMemory = device.allocateMemory(allocateInfo);

		device.bindImageMemory(image, imageMemory, 0);

		vk::ImageViewCreateInfo imageViewInfo;
		imageViewInfo.image = image;
		imageViewInfo.viewType = vk::ImageViewType::e2D;
		imageViewInfo.format = format;
		imageViewInfo.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
		imageViewInfo.subresourceRange.baseMipLevel = 0;
		imageViewInfo.subresourceRange.levelCount = 1;
		imageViewInfo.subresourceRange.baseArrayLayer = 0;
		imageViewInfo.subresourceRange.layerCount = 1;

		imageView = device.createImageView(imageViewInfo);
	}

	~Image()
	{
		device.destroyImageView(imageView);
		device.freeMemory(imageMemory);
		device.destroyImage(image);
	}

	vk::Image getImage()
	{
		return image;
	}

	vk::ImageView getImageView()
	{
		return imageView;
	}

private:
	const vk::Device device;

	vk::Image image;               // Owning handle
	vk::DeviceMemory imageMemory;  // Owning handle
	vk::ImageView imageView;       // Owning handle
};

class Framebuffer
{
public:
	Framebuffer(vk::Device device, vk::ImageView attachment, vk::Format colorFormat, vk::RenderPass renderPass, vk::Extent2D extent, bool multisample)
	    : device(device)
	{
		std::vector<vk::ImageView> attachments(multisample ? 2 : 1);

		if(multisample)
		{
			multisampleImage.reset(new Image(device, extent.width, extent.height, colorFormat, vk::SampleCountFlagBits::e4));

			// We'll be rendering to attachment location 0
			attachments[0] = multisampleImage->getImageView();
			attachments[1] = attachment;  // Resolve attachment
		}
		else
		{
			attachments[0] = attachment;
		}

		vk::FramebufferCreateInfo framebufferCreateInfo;

		framebufferCreateInfo.renderPass = renderPass;
		framebufferCreateInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
		framebufferCreateInfo.pAttachments = attachments.data();
		framebufferCreateInfo.width = extent.width;
		framebufferCreateInfo.height = extent.height;
		framebufferCreateInfo.layers = 1;

		framebuffer = device.createFramebuffer(framebufferCreateInfo);
	}

	~Framebuffer()
	{
		multisampleImage.reset();
		device.destroyFramebuffer(framebuffer);
	}

	vk::Framebuffer getFramebuffer()
	{
		return framebuffer;
	}

private:
	const vk::Device device;
	vk::Framebuffer framebuffer;  // Owning handle
	std::unique_ptr<Image> multisampleImage;
};

static std::vector<uint32_t> compileGLSLtoSPIRV(const char *glslSource, EShLanguage glslLanguage)
{
	// glslang requires one-time initialization.
	const struct GlslangProcessInitialiser
	{
		GlslangProcessInitialiser() { glslang::InitializeProcess(); }
		~GlslangProcessInitialiser() { glslang::FinalizeProcess(); }
	} glslangInitialiser;

	std::unique_ptr<glslang::TShader> glslangShader = std::make_unique<glslang::TShader>(glslLanguage);

	glslangShader->setStrings(&glslSource, 1);
	glslangShader->setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
	glslangShader->setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);

	const int defaultVersion = 100;
	EShMessages messages = static_cast<EShMessages>(EShMessages::EShMsgDefault | EShMessages::EShMsgSpvRules | EShMessages::EShMsgVulkanRules);
	bool parseResult = glslangShader->parse(&glslang::DefaultTBuiltInResource, defaultVersion, false, messages);

	if(!parseResult)
	{
		std::string debugLog = glslangShader->getInfoDebugLog();
		std::string infoLog = glslangShader->getInfoLog();
		assert(false);
	}

	glslang::TIntermediate *intermediateRepresentation = glslangShader->getIntermediate();
	assert(intermediateRepresentation);

	std::vector<uint32_t> spirv;
	glslang::SpvOptions options;
	glslang::GlslangToSpv(*intermediateRepresentation, spirv, &options);
	assert(spirv.size() != 0);

	return spirv;
}

class TriangleBenchmark : public VulkanBenchmark
{
public:
	TriangleBenchmark(bool multisample)
	    : multisample(multisample)
	{
		window.reset(new Window(instance, windowSize));
		swapchain.reset(new Swapchain(physicalDevice, device, *window));

		renderPass = createRenderPass(swapchain->colorFormat);
		createFramebuffers(renderPass);

		prepareVertices();

		pipeline = createGraphicsPipeline(renderPass);

		createSynchronizationPrimitives();

		createCommandBuffers(renderPass);
	}

	~TriangleBenchmark()
	{
		device.freeCommandBuffers(commandPool, commandBuffers);

		device.destroyDescriptorPool(descriptorPool);
		device.destroySampler(sampler, nullptr);
		texture.reset();
		device.destroyCommandPool(commandPool, nullptr);

		for(auto &fence : waitFences)
		{
			device.destroyFence(fence, nullptr);
		}

		device.destroySemaphore(renderCompleteSemaphore, nullptr);
		device.destroySemaphore(presentCompleteSemaphore, nullptr);

		device.destroyPipeline(pipeline);
		device.destroyPipelineLayout(pipelineLayout, nullptr);
		device.destroyDescriptorSetLayout(descriptorSetLayout);

		device.freeMemory(vertices.memory, nullptr);
		device.destroyBuffer(vertices.buffer, nullptr);

		for(auto &framebuffer : framebuffers)
		{
			framebuffer.reset();
		}

		device.destroyRenderPass(renderPass, nullptr);

		swapchain.reset();
		window.reset();
	}

	void renderFrame()
	{
		swapchain->acquireNextImage(presentCompleteSemaphore, currentFrameBuffer);

		device.waitForFences(1, &waitFences[currentFrameBuffer], VK_TRUE, UINT64_MAX);
		device.resetFences(1, &waitFences[currentFrameBuffer]);

		vk::PipelineStageFlags waitStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput;

		vk::SubmitInfo submitInfo;
		submitInfo.pWaitDstStageMask = &waitStageMask;
		submitInfo.pWaitSemaphores = &presentCompleteSemaphore;
		submitInfo.waitSemaphoreCount = 1;
		submitInfo.pSignalSemaphores = &renderCompleteSemaphore;
		submitInfo.signalSemaphoreCount = 1;
		submitInfo.pCommandBuffers = &commandBuffers[currentFrameBuffer];
		submitInfo.commandBufferCount = 1;

		queue.submit(1, &submitInfo, waitFences[currentFrameBuffer]);

		swapchain->queuePresent(queue, currentFrameBuffer, renderCompleteSemaphore);
	}

	void show()
	{
		window->show();
	}

protected:
	void createSynchronizationPrimitives()
	{
		vk::SemaphoreCreateInfo semaphoreCreateInfo;
		presentCompleteSemaphore = device.createSemaphore(semaphoreCreateInfo);
		renderCompleteSemaphore = device.createSemaphore(semaphoreCreateInfo);

		vk::FenceCreateInfo fenceCreateInfo;
		fenceCreateInfo.flags = vk::FenceCreateFlagBits::eSignaled;
		waitFences.resize(swapchain->imageCount());
		for(auto &fence : waitFences)
		{
			fence = device.createFence(fenceCreateInfo);
		}
	}

	void createCommandBuffers(vk::RenderPass renderPass)
	{
		vk::CommandPoolCreateInfo commandPoolCreateInfo;
		commandPoolCreateInfo.queueFamilyIndex = queueFamilyIndex;
		commandPoolCreateInfo.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
		commandPool = device.createCommandPool(commandPoolCreateInfo);

		texture.reset(new Image(device, 16, 16, vk::Format::eR8G8B8A8Unorm));

		// Fill texture with white
		vk::DeviceSize bufferSize = 16 * 16 * 4;
		Buffer buffer(device, bufferSize, vk::BufferUsageFlagBits::eTransferSrc);
		void *data = buffer.mapMemory();
		memset(data, 255, bufferSize);
		buffer.unmapMemory();

		transitionImageLayout(device, commandPool, queue, texture->getImage(), vk::Format::eR8G8B8A8Unorm, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal);
		copyBufferToImage(device, commandPool, queue, buffer.getBuffer(), texture->getImage(), 16, 16);
		transitionImageLayout(device, commandPool, queue, texture->getImage(), vk::Format::eR8G8B8A8Unorm, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal);

		vk::SamplerCreateInfo samplerInfo;
		samplerInfo.magFilter = vk::Filter::eLinear;
		samplerInfo.minFilter = vk::Filter::eLinear;
		samplerInfo.addressModeU = vk::SamplerAddressMode::eRepeat;
		samplerInfo.addressModeV = vk::SamplerAddressMode::eRepeat;
		samplerInfo.addressModeW = vk::SamplerAddressMode::eRepeat;
		samplerInfo.anisotropyEnable = VK_FALSE;
		samplerInfo.unnormalizedCoordinates = VK_FALSE;
		samplerInfo.mipmapMode = vk::SamplerMipmapMode::eLinear;
		samplerInfo.mipLodBias = 0.0f;
		samplerInfo.minLod = 0.0f;
		samplerInfo.maxLod = 0.0f;

		sampler = device.createSampler(samplerInfo);

		std::array<vk::DescriptorPoolSize, 1> poolSizes = {};
		poolSizes[0].type = vk::DescriptorType::eCombinedImageSampler;
		poolSizes[0].descriptorCount = 1;

		vk::DescriptorPoolCreateInfo poolInfo;
		poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
		poolInfo.pPoolSizes = poolSizes.data();
		poolInfo.maxSets = 1;

		descriptorPool = device.createDescriptorPool(poolInfo);

		std::vector<vk::DescriptorSetLayout> layouts(1, descriptorSetLayout);
		vk::DescriptorSetAllocateInfo allocInfo;
		allocInfo.descriptorPool = descriptorPool;
		allocInfo.descriptorSetCount = 1;
		allocInfo.pSetLayouts = layouts.data();

		std::vector<vk::DescriptorSet> descriptorSets = device.allocateDescriptorSets(allocInfo);

		vk::DescriptorImageInfo imageInfo;
		imageInfo.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
		imageInfo.imageView = texture->getImageView();
		imageInfo.sampler = sampler;

		std::array<vk::WriteDescriptorSet, 1> descriptorWrites = {};

		descriptorWrites[0].dstSet = descriptorSets[0];
		descriptorWrites[0].dstBinding = 1;
		descriptorWrites[0].dstArrayElement = 0;
		descriptorWrites[0].descriptorType = vk::DescriptorType::eCombinedImageSampler;
		descriptorWrites[0].descriptorCount = 1;
		descriptorWrites[0].pImageInfo = &imageInfo;

		device.updateDescriptorSets(static_cast<uint32_t>(descriptorWrites.size()), descriptorWrites.data(), 0, nullptr);

		vk::CommandBufferAllocateInfo commandBufferAllocateInfo;
		commandBufferAllocateInfo.commandPool = commandPool;
		commandBufferAllocateInfo.commandBufferCount = static_cast<uint32_t>(swapchain->imageCount());
		commandBufferAllocateInfo.level = vk::CommandBufferLevel::ePrimary;

		commandBuffers = device.allocateCommandBuffers(commandBufferAllocateInfo);

		for(size_t i = 0; i < commandBuffers.size(); i++)
		{
			vk::CommandBufferBeginInfo commandBufferBeginInfo;
			commandBuffers[i].begin(commandBufferBeginInfo);

			vk::ClearValue clearValues[1];
			clearValues[0].color = vk::ClearColorValue(std::array<float, 4>{ 0.5f, 0.5f, 0.5f, 1.0f });

			vk::RenderPassBeginInfo renderPassBeginInfo;
			renderPassBeginInfo.framebuffer = framebuffers[i]->getFramebuffer();
			renderPassBeginInfo.renderPass = renderPass;
			renderPassBeginInfo.renderArea.offset.x = 0;
			renderPassBeginInfo.renderArea.offset.y = 0;
			renderPassBeginInfo.renderArea.extent = windowSize;
			renderPassBeginInfo.clearValueCount = ARRAY_SIZE(clearValues);
			renderPassBeginInfo.pClearValues = clearValues;
			commandBuffers[i].beginRenderPass(renderPassBeginInfo, vk::SubpassContents::eInline);

			// Set dynamic state
			vk::Viewport viewport(0.0f, 0.0f, static_cast<float>(windowSize.width), static_cast<float>(windowSize.height), 0.0f, 1.0f);
			commandBuffers[i].setViewport(0, 1, &viewport);

			vk::Rect2D scissor(vk::Offset2D(0, 0), windowSize);
			commandBuffers[i].setScissor(0, 1, &scissor);

			commandBuffers[i].bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipelineLayout, 0, 1, &descriptorSets[0], 0, nullptr);

			// Draw a triangle
			commandBuffers[i].bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
			VULKAN_HPP_NAMESPACE::DeviceSize offset = 0;
			commandBuffers[i].bindVertexBuffers(0, 1, &vertices.buffer, &offset);
			commandBuffers[i].draw(3, 1, 0, 0);

			commandBuffers[i].endRenderPass();
			commandBuffers[i].end();
		}
	}

	void prepareVertices()
	{
		struct Vertex
		{
			float position[3];
			float color[3];
			float texCoord[2];
		};

		Vertex vertexBufferData[] = {
			{ { 1.0f, 1.0f, 0.05f }, { 1.0f, 0.0f, 0.0f }, { 1.0f, 0.0f } },
			{ { -1.0f, 1.0f, 0.5f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 1.0f } },
			{ { 0.0f, -1.0f, 0.5f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f } }
		};

		vk::BufferCreateInfo vertexBufferInfo;
		vertexBufferInfo.size = sizeof(vertexBufferData);
		vertexBufferInfo.usage = vk::BufferUsageFlagBits::eVertexBuffer;
		vertices.buffer = device.createBuffer(vertexBufferInfo);

		vk::MemoryAllocateInfo memoryAllocateInfo;
		vk::MemoryRequirements memoryRequirements = device.getBufferMemoryRequirements(vertices.buffer);
		memoryAllocateInfo.allocationSize = memoryRequirements.size;
		memoryAllocateInfo.memoryTypeIndex = getMemoryTypeIndex(physicalDevice, memoryRequirements.memoryTypeBits, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
		vertices.memory = device.allocateMemory(memoryAllocateInfo);

		void *data = device.mapMemory(vertices.memory, 0, VK_WHOLE_SIZE);
		memcpy(data, vertexBufferData, sizeof(vertexBufferData));
		device.unmapMemory(vertices.memory);
		device.bindBufferMemory(vertices.buffer, vertices.memory, 0);

		vertices.inputBinding.binding = 0;
		vertices.inputBinding.stride = sizeof(Vertex);
		vertices.inputBinding.inputRate = vk::VertexInputRate::eVertex;

		vertices.inputAttributes.push_back(vk::VertexInputAttributeDescription(0, 0, vk::Format::eR32G32B32Sfloat, offsetof(Vertex, position)));
		vertices.inputAttributes.push_back(vk::VertexInputAttributeDescription(1, 0, vk::Format::eR32G32B32Sfloat, offsetof(Vertex, color)));
		vertices.inputAttributes.push_back(vk::VertexInputAttributeDescription(2, 0, vk::Format::eR32G32Sfloat, offsetof(Vertex, texCoord)));

		vertices.inputState.vertexBindingDescriptionCount = 1;
		vertices.inputState.pVertexBindingDescriptions = &vertices.inputBinding;
		vertices.inputState.vertexAttributeDescriptionCount = static_cast<uint32_t>(vertices.inputAttributes.size());
		vertices.inputState.pVertexAttributeDescriptions = vertices.inputAttributes.data();
	}

	void createFramebuffers(vk::RenderPass renderPass)
	{
		framebuffers.resize(swapchain->imageCount());

		for(size_t i = 0; i < framebuffers.size(); i++)
		{
			framebuffers[i].reset(new Framebuffer(device, swapchain->getImageView(i), swapchain->colorFormat, renderPass, swapchain->getExtent(), multisample));
		}
	}

	vk::RenderPass createRenderPass(vk::Format colorFormat)
	{
		std::vector<vk::AttachmentDescription> attachments(multisample ? 2 : 1);

		if(multisample)
		{
			// Color attachment
			attachments[0].format = colorFormat;
			attachments[0].samples = vk::SampleCountFlagBits::e4;
			attachments[0].loadOp = vk::AttachmentLoadOp::eClear;
			attachments[0].storeOp = vk::AttachmentStoreOp::eStore;
			attachments[0].stencilLoadOp = vk::AttachmentLoadOp::eDontCare;
			attachments[0].stencilStoreOp = vk::AttachmentStoreOp::eDontCare;
			attachments[0].initialLayout = vk::ImageLayout::eUndefined;
			attachments[0].finalLayout = vk::ImageLayout::eColorAttachmentOptimal;

			// Resolve attachment
			attachments[1].format = colorFormat;
			attachments[1].samples = vk::SampleCountFlagBits::e1;
			attachments[1].loadOp = vk::AttachmentLoadOp::eDontCare;
			attachments[1].storeOp = vk::AttachmentStoreOp::eStore;
			attachments[1].stencilLoadOp = vk::AttachmentLoadOp::eDontCare;
			attachments[1].stencilStoreOp = vk::AttachmentStoreOp::eDontCare;
			attachments[1].initialLayout = vk::ImageLayout::eUndefined;
			attachments[1].finalLayout = vk::ImageLayout::ePresentSrcKHR;
		}
		else
		{
			attachments[0].format = colorFormat;
			attachments[0].samples = vk::SampleCountFlagBits::e1;
			attachments[0].loadOp = vk::AttachmentLoadOp::eDontCare;
			attachments[0].storeOp = vk::AttachmentStoreOp::eStore;
			attachments[0].stencilLoadOp = vk::AttachmentLoadOp::eDontCare;
			attachments[0].stencilStoreOp = vk::AttachmentStoreOp::eDontCare;
			attachments[0].initialLayout = vk::ImageLayout::eUndefined;
			attachments[0].finalLayout = vk::ImageLayout::ePresentSrcKHR;
		}

		vk::AttachmentReference attachment0;
		attachment0.attachment = 0;
		attachment0.layout = vk::ImageLayout::eColorAttachmentOptimal;

		vk::AttachmentReference attachment1;
		attachment1.attachment = 1;
		attachment1.layout = vk::ImageLayout::eColorAttachmentOptimal;

		vk::SubpassDescription subpassDescription;
		subpassDescription.pipelineBindPoint = vk::PipelineBindPoint::eGraphics;
		subpassDescription.colorAttachmentCount = 1;
		subpassDescription.pResolveAttachments = multisample ? &attachment1 : nullptr;
		subpassDescription.pColorAttachments = &attachment0;

		std::array<vk::SubpassDependency, 2> dependencies;

		dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
		dependencies[0].dstSubpass = 0;
		dependencies[0].srcStageMask = vk::PipelineStageFlagBits::eBottomOfPipe;
		dependencies[0].dstStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput;
		dependencies[0].srcAccessMask = vk::AccessFlagBits::eMemoryRead;
		dependencies[0].dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
		dependencies[0].dependencyFlags = vk::DependencyFlagBits::eByRegion;

		dependencies[1].srcSubpass = 0;
		dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
		dependencies[1].srcStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput;
		dependencies[1].dstStageMask = vk::PipelineStageFlagBits::eBottomOfPipe;
		dependencies[1].srcAccessMask = vk::AccessFlagBits::eColorAttachmentRead | vk::AccessFlagBits::eColorAttachmentWrite;
		dependencies[1].dstAccessMask = vk::AccessFlagBits::eMemoryRead;
		dependencies[1].dependencyFlags = vk::DependencyFlagBits::eByRegion;

		vk::RenderPassCreateInfo renderPassInfo;
		renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
		renderPassInfo.pAttachments = attachments.data();
		renderPassInfo.subpassCount = 1;
		renderPassInfo.pSubpasses = &subpassDescription;
		renderPassInfo.dependencyCount = static_cast<uint32_t>(dependencies.size());
		renderPassInfo.pDependencies = dependencies.data();

		return device.createRenderPass(renderPassInfo);
	}

	vk::ShaderModule createShaderModule(const char *glslSource, EShLanguage glslLanguage)
	{
		auto spirv = compileGLSLtoSPIRV(glslSource, glslLanguage);

		vk::ShaderModuleCreateInfo moduleCreateInfo;
		moduleCreateInfo.codeSize = spirv.size() * sizeof(uint32_t);
		moduleCreateInfo.pCode = (uint32_t *)spirv.data();

		return device.createShaderModule(moduleCreateInfo);
	}

	vk::Pipeline createGraphicsPipeline(vk::RenderPass renderPass)
	{
		vk::DescriptorSetLayoutBinding samplerLayoutBinding;
		samplerLayoutBinding.binding = 1;
		samplerLayoutBinding.descriptorCount = 1;
		samplerLayoutBinding.descriptorType = vk::DescriptorType::eCombinedImageSampler;
		samplerLayoutBinding.pImmutableSamplers = nullptr;
		samplerLayoutBinding.stageFlags = vk::ShaderStageFlagBits::eFragment;

		std::array<vk::DescriptorSetLayoutBinding, 1> bindings = { samplerLayoutBinding };
		vk::DescriptorSetLayoutCreateInfo layoutInfo;
		layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
		layoutInfo.pBindings = bindings.data();

		descriptorSetLayout = device.createDescriptorSetLayout(layoutInfo);

		std::vector<vk::DescriptorSetLayout> setLayouts(1, descriptorSetLayout);

		vk::PipelineLayoutCreateInfo pipelineLayoutCreateInfo;
		pipelineLayoutCreateInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
		pipelineLayoutCreateInfo.pSetLayouts = setLayouts.data();
		pipelineLayout = device.createPipelineLayout(pipelineLayoutCreateInfo);

		vk::GraphicsPipelineCreateInfo pipelineCreateInfo;
		pipelineCreateInfo.layout = pipelineLayout;
		pipelineCreateInfo.renderPass = renderPass;

		vk::PipelineInputAssemblyStateCreateInfo inputAssemblyState;
		inputAssemblyState.topology = vk::PrimitiveTopology::eTriangleList;

		vk::PipelineRasterizationStateCreateInfo rasterizationState;
		rasterizationState.depthClampEnable = VK_FALSE;
		rasterizationState.rasterizerDiscardEnable = VK_FALSE;
		rasterizationState.polygonMode = vk::PolygonMode::eFill;
		rasterizationState.cullMode = vk::CullModeFlagBits::eNone;
		rasterizationState.frontFace = vk::FrontFace::eCounterClockwise;
		rasterizationState.depthBiasEnable = VK_FALSE;
		rasterizationState.lineWidth = 1.0f;

		vk::PipelineColorBlendAttachmentState blendAttachmentState;
		blendAttachmentState.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
		blendAttachmentState.blendEnable = VK_FALSE;
		vk::PipelineColorBlendStateCreateInfo colorBlendState;
		colorBlendState.attachmentCount = 1;
		colorBlendState.pAttachments = &blendAttachmentState;

		vk::PipelineViewportStateCreateInfo viewportState;
		viewportState.viewportCount = 1;
		viewportState.scissorCount = 1;

		std::vector<vk::DynamicState> dynamicStateEnables;
		dynamicStateEnables.push_back(vk::DynamicState::eViewport);
		dynamicStateEnables.push_back(vk::DynamicState::eScissor);
		vk::PipelineDynamicStateCreateInfo dynamicState = {};
		dynamicState.pDynamicStates = dynamicStateEnables.data();
		dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStateEnables.size());

		vk::PipelineDepthStencilStateCreateInfo depthStencilState;
		depthStencilState.depthTestEnable = VK_FALSE;
		depthStencilState.depthWriteEnable = VK_FALSE;
		depthStencilState.depthCompareOp = vk::CompareOp::eLessOrEqual;
		depthStencilState.depthBoundsTestEnable = VK_FALSE;
		depthStencilState.back.failOp = vk::StencilOp::eKeep;
		depthStencilState.back.passOp = vk::StencilOp::eKeep;
		depthStencilState.back.compareOp = vk::CompareOp::eAlways;
		depthStencilState.stencilTestEnable = VK_FALSE;
		depthStencilState.front = depthStencilState.back;

		vk::PipelineMultisampleStateCreateInfo multisampleState;
		multisampleState.rasterizationSamples = multisample ? vk::SampleCountFlagBits::e4 : vk::SampleCountFlagBits::e1;
		multisampleState.pSampleMask = nullptr;

		const char *vertexShader = R"(#version 310 es
			layout(location = 0) in vec3 inPos;
			layout(location = 1) in vec3 inColor;

			layout(location = 0) out vec3 outColor;
			layout(location = 1) out vec2 fragTexCoord;

			void main()
			{
				outColor = inColor;
				gl_Position = vec4(inPos.xyz, 1.0);
				fragTexCoord = inPos.xy;
			}
		)";

		const char *fragmentShader = R"(#version 310 es
			precision highp float;

			layout(location = 0) in vec3 inColor;
			layout(location = 1) in vec2 fragTexCoord;

			layout(location = 0) out vec4 outColor;

			layout(binding = 0) uniform sampler2D texSampler;

			void main()
			{
				outColor = texture(texSampler, fragTexCoord) * vec4(inColor, 1.0);
			}
		)";

		vk::ShaderModule vertexModule = createShaderModule(vertexShader, EShLanguage::EShLangVertex);
		vk::ShaderModule fragmentModule = createShaderModule(fragmentShader, EShLanguage::EShLangFragment);

		std::array<vk::PipelineShaderStageCreateInfo, 2> shaderStages;

		shaderStages[0].module = vertexModule;
		shaderStages[0].stage = vk::ShaderStageFlagBits::eVertex;
		shaderStages[0].pName = "main";

		shaderStages[1].module = fragmentModule;
		shaderStages[1].stage = vk::ShaderStageFlagBits::eFragment;
		shaderStages[1].pName = "main";

		pipelineCreateInfo.stageCount = static_cast<uint32_t>(shaderStages.size());
		pipelineCreateInfo.pStages = shaderStages.data();
		pipelineCreateInfo.pVertexInputState = &vertices.inputState;
		pipelineCreateInfo.pInputAssemblyState = &inputAssemblyState;
		pipelineCreateInfo.pRasterizationState = &rasterizationState;
		pipelineCreateInfo.pColorBlendState = &colorBlendState;
		pipelineCreateInfo.pMultisampleState = &multisampleState;
		pipelineCreateInfo.pViewportState = &viewportState;
		pipelineCreateInfo.pDepthStencilState = &depthStencilState;
		pipelineCreateInfo.renderPass = renderPass;
		pipelineCreateInfo.pDynamicState = &dynamicState;

		auto pipeline = device.createGraphicsPipeline(nullptr, pipelineCreateInfo).value;

		device.destroyShaderModule(fragmentModule);
		device.destroyShaderModule(vertexModule);

		return pipeline;
	}

	const vk::Extent2D windowSize = { 1280, 720 };
	const bool multisample;

	std::unique_ptr<Window> window;
	std::unique_ptr<Swapchain> swapchain;

	vk::RenderPass renderPass;  // Owning handle
	std::vector<std::unique_ptr<Framebuffer>> framebuffers;
	uint32_t currentFrameBuffer = 0;

	struct VertexBuffer
	{
		vk::Buffer buffer;        // Owning handle
		vk::DeviceMemory memory;  // Owning handle

		vk::VertexInputBindingDescription inputBinding;
		std::vector<vk::VertexInputAttributeDescription> inputAttributes;
		vk::PipelineVertexInputStateCreateInfo inputState;
	} vertices;

	vk::DescriptorSetLayout descriptorSetLayout;  // Owning handle
	vk::PipelineLayout pipelineLayout;            // Owning handle
	vk::Pipeline pipeline;                        // Owning handle

	vk::Semaphore presentCompleteSemaphore;  // Owning handle
	vk::Semaphore renderCompleteSemaphore;   // Owning handle
	std::vector<vk::Fence> waitFences;       // Owning handles

	vk::CommandPool commandPool;  // Owning handle
	std::unique_ptr<Image> texture;
	vk::Sampler sampler;                            // Owning handle
	vk::DescriptorPool descriptorPool;              // Owning handle
	std::vector<vk::CommandBuffer> commandBuffers;  // Owning handles
};

static void Triangle(benchmark::State &state, bool multisample)
{
	TriangleBenchmark benchmark(multisample);

	if(false) benchmark.show();  // Enable for visual verification.

	// Warmup
	benchmark.renderFrame();

	for(auto _ : state)
	{
		benchmark.renderFrame();
	}
}

BENCHMARK_CAPTURE(ClearImage, VK_FORMAT_R8G8B8A8_UNORM, vk::Format::eR8G8B8A8Unorm, vk::ImageAspectFlagBits::eColor)->Unit(benchmark::kMillisecond);
BENCHMARK_CAPTURE(ClearImage, VK_FORMAT_R32_SFLOAT, vk::Format::eR32Sfloat, vk::ImageAspectFlagBits::eColor)->Unit(benchmark::kMillisecond);
BENCHMARK_CAPTURE(ClearImage, VK_FORMAT_D32_SFLOAT, vk::Format::eD32Sfloat, vk::ImageAspectFlagBits::eDepth)->Unit(benchmark::kMillisecond);
BENCHMARK_CAPTURE(Triangle, Hello, false)->Unit(benchmark::kMillisecond);
BENCHMARK_CAPTURE(Triangle, Multisample, true)->Unit(benchmark::kMillisecond);
