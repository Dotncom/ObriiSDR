#include "gpufftbackend.h"

#include <QDebug>
#include <QLibrary>

#include <cstring>
#include <limits>
#include <vector>

#ifdef FOBOSAPP_HAS_VKFFT
#ifndef VKFFT_BACKEND
#define VKFFT_BACKEND 0
#endif
#ifndef VK_API_VERSION
#define VK_API_VERSION 11
#endif
#include <vulkan/vulkan.h>
#include "vkFFT.h"
#endif

struct GpuFftBackend::Impl {
#ifdef FOBOSAPP_HAS_VKFFT
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamilyIndex = 0;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    VkBuffer deviceBuffer = VK_NULL_HANDLE;
    VkDeviceMemory deviceMemory = VK_NULL_HANDLE;
    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    void *mappedStaging = nullptr;
    uint64_t bufferBytes = 0;

    VkFFTApplication application = {};
    bool applicationInitialized = false;
    int planLength = 0;
    QString gpuName;
    QString lastError;
    bool initializationFailed = false;

    void setError(const QString &message) {
        lastError = message;
    }

    void setVkError(const char *operation, VkResult result) {
        setError(QStringLiteral("%1 failed (Vulkan %2)")
                     .arg(QString::fromLatin1(operation))
                     .arg(static_cast<int>(result)));
    }

    void setVkFftError(const char *operation, VkFFTResult result) {
        setError(QStringLiteral("%1 failed (VkFFT %2: %3)")
                     .arg(QString::fromLatin1(operation))
                     .arg(static_cast<int>(result))
                     .arg(QString::fromLatin1(getVkFFTErrorString(result))));
    }

    bool findMemoryType(uint32_t typeBits,
                        VkMemoryPropertyFlags required,
                        uint32_t &typeIndex) const {
        VkPhysicalDeviceMemoryProperties properties = {};
        vkGetPhysicalDeviceMemoryProperties(physicalDevice, &properties);
        for (uint32_t index = 0; index < properties.memoryTypeCount; ++index) {
            if ((typeBits & (1U << index)) != 0U &&
                (properties.memoryTypes[index].propertyFlags & required) == required) {
                typeIndex = index;
                return true;
            }
        }
        return false;
    }

    bool allocateBuffer(uint64_t size,
                        VkBufferUsageFlags usage,
                        VkMemoryPropertyFlags properties,
                        VkBuffer &buffer,
                        VkDeviceMemory &memory) {
        VkBufferCreateInfo bufferInfo = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = size;
        bufferInfo.usage = usage;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkResult result = vkCreateBuffer(device, &bufferInfo, nullptr, &buffer);
        if (result != VK_SUCCESS) {
            setVkError("vkCreateBuffer", result);
            return false;
        }

        VkMemoryRequirements requirements = {};
        vkGetBufferMemoryRequirements(device, buffer, &requirements);
        uint32_t memoryType = 0;
        if (!findMemoryType(requirements.memoryTypeBits, properties, memoryType)) {
            setError(QStringLiteral("No compatible Vulkan memory type"));
            vkDestroyBuffer(device, buffer, nullptr);
            buffer = VK_NULL_HANDLE;
            return false;
        }

        VkMemoryAllocateInfo allocation = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memoryType;
        result = vkAllocateMemory(device, &allocation, nullptr, &memory);
        if (result != VK_SUCCESS) {
            setVkError("vkAllocateMemory", result);
            vkDestroyBuffer(device, buffer, nullptr);
            buffer = VK_NULL_HANDLE;
            return false;
        }
        result = vkBindBufferMemory(device, buffer, memory, 0);
        if (result != VK_SUCCESS) {
            setVkError("vkBindBufferMemory", result);
            vkFreeMemory(device, memory, nullptr);
            vkDestroyBuffer(device, buffer, nullptr);
            memory = VK_NULL_HANDLE;
            buffer = VK_NULL_HANDLE;
            return false;
        }
        return true;
    }

    bool initializeDevice() {
        if (device != VK_NULL_HANDLE) {
            return true;
        }
        if (initializationFailed) {
            return false;
        }

        VkApplicationInfo applicationInfo = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
        applicationInfo.pApplicationName = "Obrii SDR VkFFT";
        applicationInfo.applicationVersion = VK_MAKE_VERSION(4, 8, 0);
        applicationInfo.pEngineName = "VkFFT";
        applicationInfo.engineVersion = 1;
        applicationInfo.apiVersion = VK_API_VERSION_1_1;

        VkInstanceCreateInfo instanceInfo = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        instanceInfo.pApplicationInfo = &applicationInfo;
        VkResult result = vkCreateInstance(&instanceInfo, nullptr, &instance);
        if (result != VK_SUCCESS) {
            setVkError("vkCreateInstance", result);
            initializationFailed = true;
            return false;
        }

        uint32_t deviceCount = 0;
        result = vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
        if (result != VK_SUCCESS || deviceCount == 0) {
            setError(QStringLiteral("No Vulkan compute device found"));
            initializationFailed = true;
            return false;
        }
        std::vector<VkPhysicalDevice> devices(deviceCount);
        result = vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());
        if (result != VK_SUCCESS) {
            setVkError("vkEnumeratePhysicalDevices", result);
            initializationFailed = true;
            return false;
        }

        int bestScore = std::numeric_limits<int>::min();
        for (VkPhysicalDevice candidate : devices) {
            uint32_t familyCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
            std::vector<VkQueueFamilyProperties> families(familyCount);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());
            for (uint32_t family = 0; family < familyCount; ++family) {
                if (families[family].queueCount == 0 ||
                    (families[family].queueFlags & VK_QUEUE_COMPUTE_BIT) == 0) {
                    continue;
                }
                VkPhysicalDeviceProperties properties = {};
                vkGetPhysicalDeviceProperties(candidate, &properties);
                int score = static_cast<int>(properties.limits.maxComputeSharedMemorySize / 1024U);
                if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
                    score += 3000;
                } else if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
                    score += 2000;
                }
                if ((families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) {
                    score += 100;
                }
                if (score > bestScore) {
                    bestScore = score;
                    physicalDevice = candidate;
                    queueFamilyIndex = family;
                    gpuName = QString::fromUtf8(properties.deviceName);
                }
            }
        }
        if (physicalDevice == VK_NULL_HANDLE) {
            setError(QStringLiteral("No Vulkan compute queue found"));
            initializationFailed = true;
            return false;
        }

        const float queuePriority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = queueFamilyIndex;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &queuePriority;
        VkDeviceCreateInfo deviceInfo = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        result = vkCreateDevice(physicalDevice, &deviceInfo, nullptr, &device);
        if (result != VK_SUCCESS) {
            setVkError("vkCreateDevice", result);
            initializationFailed = true;
            return false;
        }
        vkGetDeviceQueue(device, queueFamilyIndex, 0, &queue);

        VkCommandPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = queueFamilyIndex;
        result = vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool);
        if (result != VK_SUCCESS) {
            setVkError("vkCreateCommandPool", result);
            initializationFailed = true;
            return false;
        }

        VkCommandBufferAllocateInfo commandInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        commandInfo.commandPool = commandPool;
        commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commandInfo.commandBufferCount = 1;
        result = vkAllocateCommandBuffers(device, &commandInfo, &commandBuffer);
        if (result != VK_SUCCESS) {
            setVkError("vkAllocateCommandBuffers", result);
            initializationFailed = true;
            return false;
        }

        VkFenceCreateInfo fenceInfo = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        result = vkCreateFence(device, &fenceInfo, nullptr, &fence);
        if (result != VK_SUCCESS) {
            setVkError("vkCreateFence", result);
            initializationFailed = true;
            return false;
        }
        return true;
    }

    void releasePlan() {
        if (device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device);
        }
        if (commandBuffer != VK_NULL_HANDLE) {
            vkResetCommandBuffer(commandBuffer, 0);
        }
        if (applicationInitialized) {
            deleteVkFFT(&application);
            application = {};
            applicationInitialized = false;
        }
        if (mappedStaging && stagingMemory != VK_NULL_HANDLE) {
            vkUnmapMemory(device, stagingMemory);
            mappedStaging = nullptr;
        }
        if (stagingBuffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(device, stagingBuffer, nullptr);
            stagingBuffer = VK_NULL_HANDLE;
        }
        if (stagingMemory != VK_NULL_HANDLE) {
            vkFreeMemory(device, stagingMemory, nullptr);
            stagingMemory = VK_NULL_HANDLE;
        }
        if (deviceBuffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(device, deviceBuffer, nullptr);
            deviceBuffer = VK_NULL_HANDLE;
        }
        if (deviceMemory != VK_NULL_HANDLE) {
            vkFreeMemory(device, deviceMemory, nullptr);
            deviceMemory = VK_NULL_HANDLE;
        }
        bufferBytes = 0;
        planLength = 0;
    }

    bool recordTransformCommands() {
        VkResult result = vkResetCommandBuffer(commandBuffer, 0);
        if (result != VK_SUCCESS) {
            setVkError("vkResetCommandBuffer", result);
            return false;
        }

        VkCommandBufferBeginInfo beginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        result = vkBeginCommandBuffer(commandBuffer, &beginInfo);
        if (result != VK_SUCCESS) {
            setVkError("vkBeginCommandBuffer", result);
            return false;
        }

        VkBufferCopy copyRegion = {};
        copyRegion.size = bufferBytes;
        vkCmdCopyBuffer(commandBuffer, stagingBuffer, deviceBuffer, 1, &copyRegion);

        VkBufferMemoryBarrier toCompute = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        toCompute.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toCompute.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        toCompute.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toCompute.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toCompute.buffer = deviceBuffer;
        toCompute.offset = 0;
        toCompute.size = bufferBytes;
        vkCmdPipelineBarrier(commandBuffer,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0,
                             0,
                             nullptr,
                             1,
                             &toCompute,
                             0,
                             nullptr);

        VkFFTLaunchParams launchParams = {};
        launchParams.commandBuffer = &commandBuffer;
        const VkFFTResult fftResult = VkFFTAppend(&application, -1, &launchParams);
        if (fftResult != VKFFT_SUCCESS) {
            setVkFftError("VkFFTAppend", fftResult);
            vkEndCommandBuffer(commandBuffer);
            vkResetCommandBuffer(commandBuffer, 0);
            return false;
        }

        VkBufferMemoryBarrier toTransfer = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        toTransfer.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.buffer = deviceBuffer;
        toTransfer.offset = 0;
        toTransfer.size = bufferBytes;
        vkCmdPipelineBarrier(commandBuffer,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0,
                             0,
                             nullptr,
                             1,
                             &toTransfer,
                             0,
                             nullptr);
        vkCmdCopyBuffer(commandBuffer, deviceBuffer, stagingBuffer, 1, &copyRegion);

        result = vkEndCommandBuffer(commandBuffer);
        if (result != VK_SUCCESS) {
            setVkError("vkEndCommandBuffer", result);
            vkResetCommandBuffer(commandBuffer, 0);
            return false;
        }
        return true;
    }

    bool ensurePlan(int length) {
        if (length <= 0 || length > std::numeric_limits<int>::max() / 2) {
            setError(QStringLiteral("Invalid GPU FFT length"));
            return false;
        }
        if (!initializeDevice()) {
            return false;
        }
        if (applicationInitialized && planLength == length && mappedStaging) {
            return true;
        }
        releasePlan();

        bufferBytes = static_cast<uint64_t>(length) * 2ULL * sizeof(float);
        const VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                         VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (!allocateBuffer(bufferBytes,
                            usage,
                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                            deviceBuffer,
                            deviceMemory) ||
            !allocateBuffer(bufferBytes,
                            usage,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                            stagingBuffer,
                            stagingMemory)) {
            releasePlan();
            return false;
        }

        VkResult result = vkMapMemory(device, stagingMemory, 0, bufferBytes, 0, &mappedStaging);
        if (result != VK_SUCCESS) {
            setVkError("vkMapMemory", result);
            releasePlan();
            return false;
        }

        VkFFTConfiguration configuration = {};
        configuration.FFTdim = 1;
        configuration.size[0] = static_cast<uint64_t>(length);
        configuration.physicalDevice = &physicalDevice;
        configuration.device = &device;
        configuration.queue = &queue;
        configuration.commandPool = &commandPool;
        configuration.fence = &fence;
        configuration.buffer = &deviceBuffer;
        configuration.bufferSize = &bufferBytes;

        const VkFFTResult fftResult = initializeVkFFT(&application, configuration);
        if (fftResult != VKFFT_SUCCESS) {
            setVkFftError("initializeVkFFT", fftResult);
            releasePlan();
            return false;
        }
        applicationInitialized = true;
        planLength = length;
        if (!recordTransformCommands()) {
            releasePlan();
            return false;
        }
        lastError.clear();
        return true;
    }

    bool execute(const float *input, float *output, int length) {
        if (!input || !output) {
            setError(QStringLiteral("GPU FFT input/output buffer is null"));
            return false;
        }
        if (!ensurePlan(length)) {
            return false;
        }

        std::memcpy(mappedStaging, input, static_cast<std::size_t>(bufferBytes));
        VkResult result = vkResetFences(device, 1, &fence);
        if (result != VK_SUCCESS) {
            setVkError("vkResetFences", result);
            return false;
        }
        VkSubmitInfo submitInfo = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer;
        result = vkQueueSubmit(queue, 1, &submitInfo, fence);
        if (result != VK_SUCCESS) {
            setVkError("vkQueueSubmit", result);
            return false;
        }
        result = vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
        if (result != VK_SUCCESS) {
            setVkError("vkWaitForFences", result);
            return false;
        }

        std::memcpy(output, mappedStaging, static_cast<std::size_t>(bufferBytes));
        lastError.clear();
        return true;
    }

    void shutdown() {
        releasePlan();
        if (device != VK_NULL_HANDLE && fence != VK_NULL_HANDLE) {
            vkDestroyFence(device, fence, nullptr);
            fence = VK_NULL_HANDLE;
        }
        if (device != VK_NULL_HANDLE && commandPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device, commandPool, nullptr);
            commandPool = VK_NULL_HANDLE;
            commandBuffer = VK_NULL_HANDLE;
        }
        if (device != VK_NULL_HANDLE) {
            vkDestroyDevice(device, nullptr);
            device = VK_NULL_HANDLE;
        }
        if (instance != VK_NULL_HANDLE) {
            vkDestroyInstance(instance, nullptr);
            instance = VK_NULL_HANDLE;
        }
    }
#else
    QString lastError = QStringLiteral("VkFFT support was not compiled into this build");
#endif
};

GpuFftBackend::GpuFftBackend()
    : impl(std::make_unique<Impl>()) {
}

GpuFftBackend::~GpuFftBackend() {
#ifdef FOBOSAPP_HAS_VKFFT
    impl->shutdown();
#endif
}

bool GpuFftBackend::isCompiled() {
#ifdef FOBOSAPP_HAS_VKFFT
#ifdef Q_OS_WIN
    static QLibrary vulkanLoader(QStringLiteral("vulkan-1"));
    static const bool available = []() {
        vulkanLoader.setLoadHints(QLibrary::PreventUnloadHint);
        return vulkanLoader.load();
    }();
    return available;
#else
    return true;
#endif
#else
    return false;
#endif
}

bool GpuFftBackend::execute(const float *interleavedInput,
                            float *interleavedOutput,
                            int complexLength,
                            QString *errorMessage) {
#ifdef FOBOSAPP_HAS_VKFFT
    const bool success = impl->execute(interleavedInput, interleavedOutput, complexLength);
#else
    Q_UNUSED(interleavedInput)
    Q_UNUSED(interleavedOutput)
    Q_UNUSED(complexLength)
    const bool success = false;
#endif
    if (errorMessage) {
        *errorMessage = success ? QString() : impl->lastError;
    }
    return success;
}

QString GpuFftBackend::deviceName() const {
#ifdef FOBOSAPP_HAS_VKFFT
    return impl->gpuName;
#else
    return QString();
#endif
}

void GpuFftBackend::releasePlan() {
#ifdef FOBOSAPP_HAS_VKFFT
    impl->releasePlan();
#endif
}
