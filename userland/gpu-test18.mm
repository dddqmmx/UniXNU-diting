/* SPDX-License-Identifier: MIT
 * Real A730 Vulkan/Metal workloads with independent CPU reference pixels.
 * Only the fixed supervisor argv is accepted. Every GPU wait is bounded.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurfaceRef.h>
#import <objc/runtime.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <mach/mach_time.h>
#include <mach/mach.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "gpu-test-oracle.h"
#include "gpu-test-clock.h"
#include "GpuTestShaders.h"
#include "../drivers/DitingAdreno/submit/NativeGpuTestProtocol.h"
#include "../drivers/DitingAdreno/DitingScanoutWire.h"

namespace {
using DitingGpuOracle::Color;
static FILE *results;
static uint32_t failures, case_count, job_id;
static uint64_t deadline_ns;
static volatile sig_atomic_t cancelled;
static void cancel_worker(int number) { (void)number; cancelled = 1; }
static uint64_t now_ns() {
    static mach_timebase_info_data_t scale;
    if (!scale.denom) mach_timebase_info(&scale);
    const uint64_t time = mach_absolute_time();
    return uint64_t((__uint128_t(time)*scale.numer)/scale.denom);
}
static uint64_t absolute_ticks(uint64_t ns) {
    mach_timebase_info_data_t scale{};
    if (mach_timebase_info(&scale) || !scale.numer || !scale.denom) _Exit(74);
    return uint64_t((__uint128_t(ns) * scale.denom + scale.numer - 1) / scale.numer);
}
struct Failure { std::string stage; int code; };
static void emit(NSDictionary *object) {
    NSError *error = nil;
    NSData *encoded = [NSJSONSerialization dataWithJSONObject:object options:0 error:&error];
    if (!encoded || fwrite(encoded.bytes,1,encoded.length,results) != encoded.length ||
        fputc('\n',results) == EOF || fflush(results)) _Exit(74);
}
static void check_time() {
    if (cancelled) throw Failure{"job-cancelled",ECANCELED};
    if (now_ns() >= deadline_ns) throw Failure{"job-deadline",ETIMEDOUT};
}
static void wait_until_ns(uint64_t deadline) {
    for (;;) {
        check_time();
        if (now_ns() >= deadline) return;
        const kern_return_t result = mach_wait_until(absolute_ticks(deadline));
        if (result && result != KERN_ABORTED) throw Failure{"ui-monotonic-wait",int(result)};
    }
}
// Only the automation worker's UI input and observer threads use this policy.
// XNU's real-time user timers have zero coalescing leeway; ordinary background
// timers may be deferred by 100 ms. No application/system policy is changed.
struct UITimingPolicy {
    thread_t thread = MACH_PORT_NULL;
    thread_extended_policy_data_t previous{};
    thread_time_constraint_policy_data_t requested{};
    bool active = false;
    void acquire() {
        thread = mach_thread_self();
        mach_msg_type_number_t count = THREAD_EXTENDED_POLICY_COUNT;
        boolean_t defaults = FALSE;
        kern_return_t result = thread_policy_get(thread,THREAD_EXTENDED_POLICY,
            reinterpret_cast<thread_policy_t>(&previous),&count,&defaults);
        if (result || count != THREAD_EXTENDED_POLICY_COUNT) throw Failure{"ui-timing-policy-read",int(result)};
        requested = {uint32_t(absolute_ticks(DitingGpuPacing::PeriodNs)),
            uint32_t(absolute_ticks(500000)),uint32_t(absolute_ticks(1000000)),TRUE};
        result = thread_policy_set(thread,THREAD_TIME_CONSTRAINT_POLICY,
            reinterpret_cast<thread_policy_t>(&requested),THREAD_TIME_CONSTRAINT_POLICY_COUNT);
        if (result) throw Failure{"ui-timing-policy-set",int(result)};
        active = true;
        verify();
    }
    void verify() const {
        thread_time_constraint_policy_data_t actual{};
        mach_msg_type_number_t count = THREAD_TIME_CONSTRAINT_POLICY_COUNT;
        boolean_t defaults = FALSE;
        const kern_return_t result = thread_policy_get(thread,THREAD_TIME_CONSTRAINT_POLICY,
            reinterpret_cast<thread_policy_t>(&actual),&count,&defaults);
        if (result || defaults || count != THREAD_TIME_CONSTRAINT_POLICY_COUNT ||
            actual.period != requested.period || actual.computation != requested.computation ||
            actual.constraint != requested.constraint || actual.preemptible != TRUE)
            throw Failure{"ui-timing-policy-readback",int(result)};
    }
    ~UITimingPolicy() {
        if (active) thread_policy_set(thread,THREAD_EXTENDED_POLICY,
            reinterpret_cast<thread_policy_t>(&previous),THREAD_EXTENDED_POLICY_COUNT);
        if (thread) mach_port_deallocate(mach_task_self(),thread);
    }
};
static void vkcheck(VkResult result, const char *stage) {
    if (result == VK_SUCCESS) return;
    if (result == VK_TIMEOUT || result == VK_ERROR_DEVICE_LOST) {
        // Leave in-flight resources to the existing driver quarantine on
        // process exit. Never free backing on an unconfirmed fence.
        emit(@{@"kind":@"fatal",@"stage":@(stage),@"code":@(int(result)),@"job":@(job_id)});
        _Exit(71);
    }
    throw Failure{stage,int(result)};
}
template <class Work> static void testcase(uint32_t id, const char *name, const char *category, Work work) {
    check_time(); const uint64_t start = now_ns(); ++case_count;
    fprintf(results,"{\"kind\":\"begin\",\"id\":%u,\"start_ns\":%llu}\n",id,(unsigned long long)start); fflush(results);
    @autoreleasepool {
        NSMutableDictionary *record = [@{@"kind":@"case",@"id":@(id),@"name":@(name),
            @"category":@(category),@"required":@YES,@"start_ns":@(start)} mutableCopy];
        @try {
            try {
                [record addEntriesFromDictionary:work()];
                if (!record[@"pass"]) record[@"pass"] = @YES;
            } catch (const Failure &error) {
                if (error.code == ECANCELED) throw;
                record[@"pass"] = @NO; record[@"stage"] = @(error.stage.c_str()); record[@"code"] = @(error.code);
            } catch (const std::exception &error) {
                record[@"pass"] = @NO; record[@"stage"] = @"worker-exception"; record[@"detail"] = @(error.what());
            }
        } @catch (NSException *error) {
            record[@"pass"] = @NO; record[@"stage"] = @"objc-exception";
            record[@"detail"] = error.reason ?: error.name;
        }
        const uint64_t finish = now_ns();
        if (![record[@"pass"] boolValue]) ++failures;
        record[@"end_ns"] = @(finish); record[@"duration_us"] = @((finish-start)/1000);
        fprintf(results,"{\"kind\":\"end\",\"id\":%u,\"start_ns\":%llu,\"end_ns\":%llu}\n",
            id,(unsigned long long)start,(unsigned long long)finish);
        emit(record);
    }
}
#define GPU_VK_FUNCTIONS(X) \
    X(CreateInstance) X(DestroyInstance) X(EnumeratePhysicalDevices) X(GetPhysicalDeviceProperties) \
    X(GetPhysicalDeviceMemoryProperties) X(GetPhysicalDeviceQueueFamilyProperties) X(GetPhysicalDeviceFormatProperties) \
    X(GetPhysicalDeviceProperties2) X(EnumerateDeviceExtensionProperties) \
    X(CreateDevice) X(DestroyDevice) X(GetDeviceQueue) X(CreateCommandPool) X(DestroyCommandPool) \
    X(AllocateCommandBuffers) X(ResetCommandBuffer) X(BeginCommandBuffer) X(EndCommandBuffer) \
    X(CreateFence) X(DestroyFence) X(ResetFences) X(WaitForFences) X(QueueSubmit) \
    X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) X(AllocateMemory) X(FreeMemory) \
    X(BindBufferMemory) X(MapMemory) X(UnmapMemory) X(FlushMappedMemoryRanges) X(InvalidateMappedMemoryRanges) \
    X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements) X(GetImageSubresourceLayout) X(BindImageMemory) X(CreateImageView) X(DestroyImageView) \
    X(CreateRenderPass) X(DestroyRenderPass) X(CreateFramebuffer) X(DestroyFramebuffer) \
    X(CreateShaderModule) X(DestroyShaderModule) X(CreatePipelineLayout) X(DestroyPipelineLayout) \
    X(CreateGraphicsPipelines) X(CreateComputePipelines) X(DestroyPipeline) \
    X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) X(CreateDescriptorPool) X(DestroyDescriptorPool) \
    X(AllocateDescriptorSets) X(UpdateDescriptorSets) X(CreateSampler) X(DestroySampler) \
    X(CreateQueryPool) X(DestroyQueryPool) X(GetQueryPoolResults) X(CmdResetQueryPool) X(CmdWriteTimestamp) \
    X(CmdPipelineBarrier) X(CmdBeginRenderPass) X(CmdNextSubpass) X(CmdEndRenderPass) X(CmdBindPipeline) \
    X(CmdBindDescriptorSets) X(CmdPushConstants) X(CmdSetViewport) X(CmdSetScissor) X(CmdDraw) X(CmdDispatch) \
    X(CmdCopyImageToBuffer) X(CmdCopyBufferToImage) X(CmdBlitImage) X(CmdFillBuffer)
struct Vulkan {
#define DECLARE(name) PFN_vk##name name = nullptr;
    GPU_VK_FUNCTIONS(DECLARE)
#undef DECLARE
    void *loader = nullptr;
    VkInstance instance = VK_NULL_HANDLE; VkPhysicalDevice physical = VK_NULL_HANDLE; VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE; VkCommandPool pool = VK_NULL_HANDLE; VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE; VkQueryPool query = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties{}; VkPhysicalDeviceMemoryProperties memory{};
    uint32_t queue_family = 0, timestamp_bits = 0;
    bool host_import = false;
    VkDeviceSize host_alignment = 0;
    PFN_vkGetMemoryHostPointerPropertiesEXT hostPointerProperties = nullptr;
    VkDescriptorSetLayout sampled_layout = VK_NULL_HANDLE, compute_layout = VK_NULL_HANDLE, input_layout = VK_NULL_HANDLE;
    VkPipelineLayout graphics_layout = VK_NULL_HANDLE, compute_pipeline_layout = VK_NULL_HANDLE, input_pipeline_layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptors = VK_NULL_HANDLE; VkSampler sampler = VK_NULL_HANDLE;
    VkShaderModule vertex = VK_NULL_HANDLE, fragment = VK_NULL_HANDLE, compute = VK_NULL_HANDLE, input_fragment = VK_NULL_HANDLE;
    uint64_t encode_start = 0;
    struct Timing { uint64_t encode_us, submit_us, wait_us, total_us; double gpu_us; };
    uint32_t type(uint32_t bits, VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred = 0) {
        for (unsigned pass = 0; pass < 2; ++pass)
            for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
                if ((bits & (1u<<i)) && (memory.memoryTypes[i].propertyFlags & required) == required &&
                    (pass || (memory.memoryTypes[i].propertyFlags & preferred) == preferred)) return i;
        throw Failure{"memory-type",int(required)};
    }
    struct Buffer {
        Vulkan &v; VkBuffer buffer = VK_NULL_HANDLE; VkDeviceMemory backing = VK_NULL_HANDLE;
        void *mapped = nullptr; size_t bytes; bool coherent = false;
        Buffer(Vulkan &vk, size_t amount) : v(vk), bytes(amount) {
            try {
            VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            info.size = amount; info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            vkcheck(v.CreateBuffer(v.device,&info,nullptr,&buffer),"create-buffer");
            VkMemoryRequirements needs; v.GetBufferMemoryRequirements(v.device,buffer,&needs);
            VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocation.allocationSize=needs.size; allocation.memoryTypeIndex=v.type(needs.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            coherent = bool(v.memory.memoryTypes[allocation.memoryTypeIndex].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            vkcheck(v.AllocateMemory(v.device,&allocation,nullptr,&backing),"allocate-buffer");
            vkcheck(v.BindBufferMemory(v.device,buffer,backing,0),"bind-buffer");
            vkcheck(v.MapMemory(v.device,backing,0,VK_WHOLE_SIZE,0,&mapped),"map-buffer");
            } catch (...) { release(); throw; }
        }
        void flush(bool acquire) {
            if (coherent) return;
            VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE}; range.memory=backing; range.size=VK_WHOLE_SIZE;
            vkcheck(acquire?v.InvalidateMappedMemoryRanges(v.device,1,&range):v.FlushMappedMemoryRanges(v.device,1,&range),acquire?"host-acquire":"host-publish");
        }
        void release() { if(mapped)v.UnmapMemory(v.device,backing); if(buffer)v.DestroyBuffer(v.device,buffer,nullptr); if(backing)v.FreeMemory(v.device,backing,nullptr); mapped=nullptr;buffer=VK_NULL_HANDLE;backing=VK_NULL_HANDLE; }
        ~Buffer() { release(); }
        Buffer(const Buffer &) = delete;
    };
    struct Image {
        Vulkan &v; VkImage image = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE; VkDeviceMemory backing = VK_NULL_HANDLE;
        VkFormat format; uint32_t width,height; VkSampleCountFlagBits samples; VkImageAspectFlags aspect; VkImageTiling tiling;
        VkDeviceSize allocation_bytes=0;uint32_t memory_type=0;void *host_storage=nullptr;
        Image(Vulkan &vk,VkFormat fmt,uint32_t w,uint32_t h,VkSampleCountFlagBits sample=VK_SAMPLE_COUNT_1_BIT,bool depth=false,
              VkImageTiling tile=VK_IMAGE_TILING_OPTIMAL,VkImageCreateFlags flags=0,bool cpu_visible=false,bool imported_host=false):
            v(vk),format(fmt),width(w),height(h),samples(sample),aspect(depth?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT),tiling(tile) {
            try {
            VkFormatProperties support; v.GetPhysicalDeviceFormatProperties(v.physical,fmt,&support);
            const VkFormatFeatureFlags need = depth?VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT:
                VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT|VK_FORMAT_FEATURE_TRANSFER_SRC_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
            const VkFormatFeatureFlags supported=tile==VK_IMAGE_TILING_LINEAR?support.linearTilingFeatures:support.optimalTilingFeatures;
            if ((supported & need) != need) throw Failure{"image-format-unsupported",int(fmt)};
            VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            info.imageType=VK_IMAGE_TYPE_2D; info.format=fmt; info.extent={w,h,1}; info.mipLevels=info.arrayLayers=1;
            info.samples=sample; info.tiling=tile; info.flags=flags;
            VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
            if(imported_host) {
                if(!v.host_import || !v.hostPointerProperties || tile!=VK_IMAGE_TILING_LINEAR || depth)
                    throw Failure{"external-host-linear-image-required",-1};
                external.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;info.pNext=&external;
            }
            info.usage=depth?VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT:
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
            vkcheck(v.CreateImage(v.device,&info,nullptr,&image),"create-image");
            VkMemoryRequirements needs; v.GetImageMemoryRequirements(v.device,image,&needs);
            VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocation.allocationSize=needs.size; allocation.memoryTypeIndex=v.type(needs.memoryTypeBits,
                cpu_visible?VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT:0,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            VkImportMemoryHostPointerInfoEXT import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
            if(imported_host) {
                const VkDeviceSize alignment=v.host_alignment;
                if(alignment<sizeof(void *) || alignment>1048576 || (alignment&(alignment-1)) ||
                   needs.size>16*1048576 || needs.size>UINT64_MAX-alignment)
                    throw Failure{"external-host-alignment-or-size",-1};
                allocation.allocationSize=(needs.size+alignment-1)&~(alignment-1);
                const int result=posix_memalign(&host_storage,size_t(alignment),size_t(allocation.allocationSize));
                if(result)throw Failure{"external-host-allocation",result};
                memset(host_storage,0xcd,size_t(allocation.allocationSize));
                VkMemoryHostPointerPropertiesEXT properties{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
                vkcheck(v.hostPointerProperties(v.device,VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                    host_storage,&properties),"external-host-memory-types");
                allocation.memoryTypeIndex=v.type(needs.memoryTypeBits&properties.memoryTypeBits,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
                import.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
                import.pHostPointer=host_storage;allocation.pNext=&import;
            }
            allocation_bytes=allocation.allocationSize;memory_type=allocation.memoryTypeIndex;
            vkcheck(v.AllocateMemory(v.device,&allocation,nullptr,&backing),"allocate-image");
            vkcheck(v.BindImageMemory(v.device,image,backing,0),"bind-image");
            VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            viewInfo.image=image; viewInfo.viewType=VK_IMAGE_VIEW_TYPE_2D; viewInfo.format=fmt;
            viewInfo.subresourceRange={aspect,0,1,0,1};
            vkcheck(v.CreateImageView(v.device,&viewInfo,nullptr,&view),"image-view");
            } catch (...) { release(); throw; }
        }
        void release() { if(view)v.DestroyImageView(v.device,view,nullptr); if(image)v.DestroyImage(v.device,image,nullptr); if(backing)v.FreeMemory(v.device,backing,nullptr); free(host_storage);host_storage=nullptr;view=VK_NULL_HANDLE;image=VK_NULL_HANDLE;backing=VK_NULL_HANDLE; }
        ~Image() { release(); }
        Image(const Image &) = delete;
    };
    std::unique_ptr<Image> dummy;
    VkDescriptorSet dummy_set = VK_NULL_HANDLE;
    VkShaderModule module(const uint32_t *words,size_t bytes) {
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; info.codeSize=bytes; info.pCode=words;
        VkShaderModule result; vkcheck(CreateShaderModule(device,&info,nullptr,&result),"shader-module"); return result;
    }
    Vulkan() { try { initialize(); } catch (...) { release(); throw; } }
    void initialize() {
        loader=dlopen("/System/Library/Extensions/DitingAdrenoMetal.bundle/libvulkan_freedreno.dylib",RTLD_NOW|RTLD_LOCAL);
        if(!loader)throw Failure{"turnip-load",-1};
        auto get=reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(loader,"vkGetInstanceProcAddr"));
        if(!get)throw Failure{"turnip-entrypoint",-1};
        CreateInstance=reinterpret_cast<PFN_vkCreateInstance>(get(nullptr,"vkCreateInstance"));
        if(!CreateInstance)throw Failure{"vulkan-create-instance-entrypoint",-1};
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.pApplicationName="Diting GPU automation"; app.apiVersion=VK_API_VERSION_1_1;
        VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; create.pApplicationInfo=&app;
        vkcheck(CreateInstance(&create,nullptr,&instance),"create-instance");
#define LOAD(name) name=reinterpret_cast<PFN_vk##name>(get(instance,"vk" #name)); if(!name)throw Failure{"entrypoint-vk" #name,-1};
        GPU_VK_FUNCTIONS(LOAD)
#undef LOAD
        uint32_t count=0; vkcheck(EnumeratePhysicalDevices(instance,&count,nullptr),"enumerate-device");
        if(!count || count>16)throw Failure{"device-count",int(count)};
        std::vector<VkPhysicalDevice> list(count); vkcheck(EnumeratePhysicalDevices(instance,&count,list.data()),"enumerate-device");
        for(auto candidate:list) {
            VkPhysicalDeviceProperties facts; GetPhysicalDeviceProperties(candidate,&facts);
            if(facts.vendorID==0x5143 && facts.deviceID==0x07030002 &&
                facts.deviceType!=VK_PHYSICAL_DEVICE_TYPE_CPU) { physical=candidate;properties=facts;break; }
        }
        if(!physical)throw Failure{"actual-a730v3-required",-1};
        GetPhysicalDeviceMemoryProperties(physical,&memory);
        uint32_t families=0; GetPhysicalDeviceQueueFamilyProperties(physical,&families,nullptr);
        if(!families || families>64)throw Failure{"queue-family-count",int(families)};
        std::vector<VkQueueFamilyProperties> queues(families); GetPhysicalDeviceQueueFamilyProperties(physical,&families,queues.data());
        bool found=false;
        for(uint32_t i=0;i<families;++i)if((queues[i].queueFlags&(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT))==(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT)) {
            queue_family=i;timestamp_bits=queues[i].timestampValidBits;found=true;break;
        }
        if(!found)throw Failure{"graphics-compute-queue-required",-1};
        float priority=1; VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex=queue_family;queueInfo.queueCount=1;queueInfo.pQueuePriorities=&priority;
        VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};deviceInfo.queueCreateInfoCount=1;deviceInfo.pQueueCreateInfos=&queueInfo;
        uint32_t extensionCount=0;
        vkcheck(EnumerateDeviceExtensionProperties(physical,nullptr,&extensionCount,nullptr),"device-extension-count");
        if(extensionCount>1024)throw Failure{"device-extension-bound",int(extensionCount)};
        std::vector<VkExtensionProperties> extensions(extensionCount);
        vkcheck(EnumerateDeviceExtensionProperties(physical,nullptr,&extensionCount,extensions.data()),"device-extensions");
        for(const auto &extension:extensions)if(!strcmp(extension.extensionName,VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME))host_import=true;
        const char *hostExtension=VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME;
        if(host_import) {
            VkPhysicalDeviceExternalMemoryHostPropertiesEXT hostProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
            VkPhysicalDeviceProperties2 physicalProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};physicalProperties.pNext=&hostProperties;
            GetPhysicalDeviceProperties2(physical,&physicalProperties);host_alignment=hostProperties.minImportedHostPointerAlignment;
            deviceInfo.enabledExtensionCount=1;deviceInfo.ppEnabledExtensionNames=&hostExtension;
        }
        vkcheck(CreateDevice(physical,&deviceInfo,nullptr,&device),"create-device");GetDeviceQueue(device,queue_family,0,&queue);
        if(host_import)hostPointerProperties=reinterpret_cast<PFN_vkGetMemoryHostPointerPropertiesEXT>(get(instance,"vkGetMemoryHostPointerPropertiesEXT"));
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};poolInfo.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;poolInfo.queueFamilyIndex=queue_family;
        vkcheck(CreateCommandPool(device,&poolInfo,nullptr,&pool),"command-pool");
        VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};allocation.commandPool=pool;allocation.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;allocation.commandBufferCount=1;
        vkcheck(AllocateCommandBuffers(device,&allocation,&command),"command-buffer");
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};vkcheck(CreateFence(device,&fenceInfo,nullptr,&fence),"fence");
        if(timestamp_bits && std::isfinite(properties.limits.timestampPeriod) && properties.limits.timestampPeriod>0) {
            VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};info.queryType=VK_QUERY_TYPE_TIMESTAMP;info.queryCount=2;
            vkcheck(CreateQueryPool(device,&info,nullptr,&query),"timestamp-query");
        }
        VkDescriptorSetLayoutBinding sampled{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};setInfo.bindingCount=1;setInfo.pBindings=&sampled;
        vkcheck(CreateDescriptorSetLayout(device,&setInfo,nullptr,&sampled_layout),"sample-layout");
        sampled.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;sampled.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;
        vkcheck(CreateDescriptorSetLayout(device,&setInfo,nullptr,&compute_layout),"compute-layout");
        sampled.descriptorType=VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;sampled.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;
        vkcheck(CreateDescriptorSetLayout(device,&setInfo,nullptr,&input_layout),"input-layout");
        VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,32};
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.setLayoutCount=1;layoutInfo.pSetLayouts=&sampled_layout;layoutInfo.pushConstantRangeCount=1;layoutInfo.pPushConstantRanges=&push;
        vkcheck(CreatePipelineLayout(device,&layoutInfo,nullptr,&graphics_layout),"graphics-layout");
        layoutInfo.pSetLayouts=&input_layout;
        vkcheck(CreatePipelineLayout(device,&layoutInfo,nullptr,&input_pipeline_layout),"input-pipeline-layout");
        layoutInfo.pSetLayouts=&compute_layout;layoutInfo.pushConstantRangeCount=0;layoutInfo.pPushConstantRanges=nullptr;
        vkcheck(CreatePipelineLayout(device,&layoutInfo,nullptr,&compute_pipeline_layout),"compute-pipeline-layout");
        VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,64},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,16},{VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,16}};
        VkDescriptorPoolCreateInfo descriptorsInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};descriptorsInfo.maxSets=96;descriptorsInfo.poolSizeCount=3;descriptorsInfo.pPoolSizes=sizes;
        vkcheck(CreateDescriptorPool(device,&descriptorsInfo,nullptr,&descriptors),"descriptor-pool");
        VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        samplerInfo.magFilter=samplerInfo.minFilter=VK_FILTER_NEAREST;samplerInfo.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerInfo.addressModeU=samplerInfo.addressModeV=samplerInfo.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        vkcheck(CreateSampler(device,&samplerInfo,nullptr,&sampler),"sampler");
        vertex=module(diting_test_vert,sizeof(diting_test_vert));fragment=module(diting_test_frag,sizeof(diting_test_frag));compute=module(diting_test_comp,sizeof(diting_test_comp));
        input_fragment=module(diting_test_input_frag,sizeof(diting_test_input_frag));
        dummy=std::make_unique<Image>(*this,VK_FORMAT_R8G8B8A8_UNORM,1,1);
        Buffer white(*this,4096);memset(white.mapped,255,4096);white.flush(false);
        begin();transition(*dummy,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={1,1,1};
        CmdCopyBufferToImage(command,white.buffer,dummy->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
        transition(*dummy,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_SHADER_READ_BIT);
        finish();dummy_set=sampledSet(*dummy);
        emit(@{@"kind":@"device",@"api":@"vulkan",@"name":@(properties.deviceName),
            @"vendor":@(properties.vendorID),@"chip_id":@(properties.deviceID),@"driver":@(properties.driverVersion),
            @"timestamp_bits":@(timestamp_bits),@"timestamp_period_ns":@(properties.limits.timestampPeriod),
            @"tu_debug":@(getenv("TU_DEBUG") ?: ""),@"external_host_import":@(host_import),
            @"host_pointer_alignment":@(host_alignment),@"software_renderer":@NO});
    }
    void begin() {
        check_time();encode_start=now_ns();vkcheck(ResetFences(device,1,&fence),"reset-fence");
        vkcheck(ResetCommandBuffer(command,0),"reset-command");
        VkCommandBufferBeginInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};info.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkcheck(BeginCommandBuffer(command,&info),"begin-command");
        if(query){CmdResetQueryPool(command,query,0,2);CmdWriteTimestamp(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,query,0);}
    }
    Timing finish() {
        if(query)CmdWriteTimestamp(command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,query,1);
        vkcheck(EndCommandBuffer(command),"end-command");const uint64_t ready=now_ns();
        VkSubmitInfo submission{VK_STRUCTURE_TYPE_SUBMIT_INFO};submission.commandBufferCount=1;submission.pCommandBuffers=&command;
        vkcheck(QueueSubmit(queue,1,&submission,fence),"queue-submit");const uint64_t submitted=now_ns();
        vkcheck(WaitForFences(device,1,&fence,VK_TRUE,5000000000ull),"fence-timeout");
        const uint64_t retired=now_ns();double gpu=NAN;
        if(query) {
            uint64_t ticks[2];vkcheck(GetQueryPoolResults(device,query,0,2,sizeof(ticks),ticks,8,VK_QUERY_RESULT_64_BIT),"timestamp-results");
            const uint64_t mask=timestamp_bits>=64?UINT64_MAX:(uint64_t(1)<<timestamp_bits)-1;
            gpu=double((ticks[1]-ticks[0])&mask)*properties.limits.timestampPeriod/1000.0;
        }
        return {(ready-encode_start)/1000,(submitted-ready)/1000,(retired-submitted)/1000,(retired-encode_start)/1000,gpu};
    }
    void transition(Image &image,VkImageLayout from,VkImageLayout to,VkAccessFlags access) {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask=from==VK_IMAGE_LAYOUT_UNDEFINED?0:VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_MEMORY_READ_BIT;
        barrier.dstAccessMask=access;barrier.oldLayout=from;barrier.newLayout=to;
        barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        barrier.image=image.image;barrier.subresourceRange={image.aspect,0,1,0,1};
        CmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&barrier);
    }
    VkDescriptorSet sampledSet(Image &image) {
        VkDescriptorSetAllocateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        info.descriptorPool=descriptors;info.descriptorSetCount=1;info.pSetLayouts=&sampled_layout;
        VkDescriptorSet set;vkcheck(AllocateDescriptorSets(device,&info,&set),"sample-descriptor");
        VkDescriptorImageInfo imageInfo{sampler,image.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=set;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;write.pImageInfo=&imageInfo;
        UpdateDescriptorSets(device,1,&write,0,nullptr);return set;
    }
    void readback(Image &image,Buffer &buffer,unsigned pixelBytes) {
        VkBufferImageCopy copy{};copy.bufferOffset=64;copy.bufferRowLength=image.width+13;copy.bufferImageHeight=image.height;
        copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={image.width,image.height,1};
        CmdCopyImageToBuffer(command,image.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer.buffer,1,&copy);
        VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.buffer=buffer.buffer;barrier.size=VK_WHOLE_SIZE;
        CmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&barrier,0,nullptr);
        (void)pixelBytes;
    }
    void release() {
        dummy.reset();
        if(device) {
            if(vertex)DestroyShaderModule(device,vertex,nullptr);if(fragment)DestroyShaderModule(device,fragment,nullptr);if(compute)DestroyShaderModule(device,compute,nullptr);
            if(input_fragment)DestroyShaderModule(device,input_fragment,nullptr);
            if(sampler)DestroySampler(device,sampler,nullptr);if(descriptors)DestroyDescriptorPool(device,descriptors,nullptr);
            if(graphics_layout)DestroyPipelineLayout(device,graphics_layout,nullptr);if(compute_pipeline_layout)DestroyPipelineLayout(device,compute_pipeline_layout,nullptr);
            if(input_pipeline_layout)DestroyPipelineLayout(device,input_pipeline_layout,nullptr);
            if(sampled_layout)DestroyDescriptorSetLayout(device,sampled_layout,nullptr);if(compute_layout)DestroyDescriptorSetLayout(device,compute_layout,nullptr);
            if(input_layout)DestroyDescriptorSetLayout(device,input_layout,nullptr);
            if(query)DestroyQueryPool(device,query,nullptr);if(fence)DestroyFence(device,fence,nullptr);if(pool)DestroyCommandPool(device,pool,nullptr);
            DestroyDevice(device,nullptr);
        }
        if(instance&&DestroyInstance)DestroyInstance(instance,nullptr);if(loader)dlclose(loader);
        device=VK_NULL_HANDLE;instance=VK_NULL_HANDLE;loader=nullptr;
    }
    ~Vulkan() { release(); }
};
struct Pass {
    Vulkan &v; Vulkan::Image &image; std::unique_ptr<Vulkan::Image> multisample,depth;
    VkRenderPass pass=VK_NULL_HANDLE;VkFramebuffer framebuffer=VK_NULL_HANDLE;VkPipeline pipeline=VK_NULL_HANDLE,blend=VK_NULL_HANDLE;
    uint64_t pipeline_us=0;
    Pass(Vulkan &vk,Vulkan::Image &target,bool load=false,bool msaa=false,bool withDepth=false):v(vk),image(target) {
        try {
        VkAttachmentDescription attachments[3]{};VkImageView views[3]{};unsigned count=1;
        VkAttachmentReference color{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},resolve{1,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},depthRef{};
        attachments[0].format=image.format;attachments[0].samples=msaa?VK_SAMPLE_COUNT_4_BIT:VK_SAMPLE_COUNT_1_BIT;
        attachments[0].loadOp=load?VK_ATTACHMENT_LOAD_OP_LOAD:VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[0].storeOp=VK_ATTACHMENT_STORE_OP_STORE;attachments[0].stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;attachments[0].stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[0].initialLayout=load?VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[0].finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;views[0]=image.view;
        if(msaa) {
            if(!(v.properties.limits.framebufferColorSampleCounts&VK_SAMPLE_COUNT_4_BIT))throw Failure{"msaa4-required",-1};
            multisample=std::make_unique<Vulkan::Image>(v,image.format,image.width,image.height,VK_SAMPLE_COUNT_4_BIT);
            views[0]=multisample->view;views[1]=image.view;
            attachments[1]=attachments[0];attachments[1].samples=VK_SAMPLE_COUNT_1_BIT;attachments[1].loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachments[0].finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;attachments[0].storeOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;count=2;
        }
        if(withDepth) {
            depth=std::make_unique<Vulkan::Image>(v,VK_FORMAT_D32_SFLOAT,image.width,image.height,VK_SAMPLE_COUNT_1_BIT,true);
            views[count]=depth->view;depthRef={count,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
            attachments[count].format=VK_FORMAT_D32_SFLOAT;attachments[count].samples=VK_SAMPLE_COUNT_1_BIT;
            attachments[count].loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;attachments[count].storeOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachments[count].initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;attachments[count].finalLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;++count;
        }
        VkSubpassDescription subpass{};subpass.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;subpass.colorAttachmentCount=1;subpass.pColorAttachments=&color;
        subpass.pResolveAttachments=msaa?&resolve:nullptr;subpass.pDepthStencilAttachment=withDepth?&depthRef:nullptr;
        VkSubpassDependency dependencies[2]{};
        dependencies[0].srcSubpass=VK_SUBPASS_EXTERNAL;dependencies[0].dstSubpass=0;
        dependencies[0].srcStageMask=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;dependencies[0].dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependencies[0].srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;dependencies[0].dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependencies[1].srcSubpass=0;dependencies[1].dstSubpass=VK_SUBPASS_EXTERNAL;
        dependencies[1].srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;dependencies[1].dstStageMask=VK_PIPELINE_STAGE_TRANSFER_BIT;
        dependencies[1].srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;dependencies[1].dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};info.attachmentCount=count;info.pAttachments=attachments;info.subpassCount=1;info.pSubpasses=&subpass;info.dependencyCount=2;info.pDependencies=dependencies;
        vkcheck(v.CreateRenderPass(v.device,&info,nullptr,&pass),"render-pass");
        VkFramebufferCreateInfo frame{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};frame.renderPass=pass;frame.attachmentCount=count;frame.pAttachments=views;frame.width=image.width;frame.height=image.height;frame.layers=1;
        vkcheck(v.CreateFramebuffer(v.device,&frame,nullptr,&framebuffer),"framebuffer");
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType=stages[1].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=v.vertex;stages[0].pName="main";
        stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=v.fragment;stages[1].pName="main";
        VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};assembly.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};viewport.viewportCount=viewport.scissorCount=1;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};raster.polygonMode=VK_POLYGON_MODE_FILL;raster.lineWidth=1;raster.cullMode=VK_CULL_MODE_NONE;
        VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};samples.rasterizationSamples=msaa?VK_SAMPLE_COUNT_4_BIT:VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo depthState{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};depthState.depthTestEnable=depthState.depthWriteEnable=withDepth;depthState.depthCompareOp=VK_COMPARE_OP_LESS;
        VkPipelineColorBlendAttachmentState attachment{};attachment.colorWriteMask=15;
        VkPipelineColorBlendStateCreateInfo blendInfo{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};blendInfo.attachmentCount=1;blendInfo.pAttachments=&attachment;
        VkDynamicState states[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};dynamic.dynamicStateCount=2;dynamic.pDynamicStates=states;
        VkGraphicsPipelineCreateInfo graphics{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        graphics.stageCount=2;graphics.pStages=stages;graphics.pVertexInputState=&vertex;graphics.pInputAssemblyState=&assembly;
        graphics.pViewportState=&viewport;graphics.pRasterizationState=&raster;graphics.pMultisampleState=&samples;graphics.pDepthStencilState=&depthState;
        graphics.pColorBlendState=&blendInfo;graphics.pDynamicState=&dynamic;graphics.layout=v.graphics_layout;graphics.renderPass=pass;
        const uint64_t start=now_ns();vkcheck(v.CreateGraphicsPipelines(v.device,VK_NULL_HANDLE,1,&graphics,nullptr,&pipeline),"graphics-pipeline");
        attachment.blendEnable=VK_TRUE;attachment.srcColorBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA;attachment.dstColorBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        attachment.colorBlendOp=attachment.alphaBlendOp=VK_BLEND_OP_ADD;attachment.srcAlphaBlendFactor=VK_BLEND_FACTOR_ONE;attachment.dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        vkcheck(v.CreateGraphicsPipelines(v.device,VK_NULL_HANDLE,1,&graphics,nullptr,&blend),"blend-pipeline");pipeline_us=(now_ns()-start)/1000;
        } catch (...) { release(); throw; }
    }
    void begin(Color clear,const VkRect2D *area=nullptr) {
        VkClearValue values[3]{};values[0].color={{clear.r,clear.g,clear.b,clear.a}};
        if(depth)values[multisample?2:1].depthStencil={1,0};
        VkRenderPassBeginInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};info.renderPass=pass;info.framebuffer=framebuffer;
        info.renderArea=area?*area:VkRect2D{{0,0},{image.width,image.height}};
        info.clearValueCount=depth?2:multisample?2:1;info.pClearValues=values;
        v.CmdBeginRenderPass(v.command,&info,VK_SUBPASS_CONTENTS_INLINE);
        VkViewport viewport{0,0,float(image.width),float(image.height),0,1};v.CmdSetViewport(v.command,0,1,&viewport);
        VkRect2D scissor=info.renderArea;v.CmdSetScissor(v.command,0,1,&scissor);
    }
    void draw(Color color,unsigned mode=0,bool blending=false,VkDescriptorSet descriptor=VK_NULL_HANDLE,float z=0.5f) {
        v.CmdBindPipeline(v.command,VK_PIPELINE_BIND_POINT_GRAPHICS,blending?blend:pipeline);
        const VkDescriptorSet set=descriptor?descriptor:v.dummy_set;
        v.CmdBindDescriptorSets(v.command,VK_PIPELINE_BIND_POINT_GRAPHICS,v.graphics_layout,0,1,&set,0,nullptr);
        const float params[]={color.r,color.g,color.b,color.a,z,float(mode),float(image.width),float(image.height)};
        v.CmdPushConstants(v.command,v.graphics_layout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(params),params);
        v.CmdDraw(v.command,3,1,0,0);
    }
    void end(){v.CmdEndRenderPass(v.command);}
    void release(){if(pipeline)v.DestroyPipeline(v.device,pipeline,nullptr);if(blend)v.DestroyPipeline(v.device,blend,nullptr);if(framebuffer)v.DestroyFramebuffer(v.device,framebuffer,nullptr);if(pass)v.DestroyRenderPass(v.device,pass,nullptr);pipeline=blend=VK_NULL_HANDLE;framebuffer=VK_NULL_HANDLE;pass=VK_NULL_HANDLE;}
    ~Pass(){release();}
};
// Unlike a sampled-image copy, this reads the preceding subpass's attachment
// directly. It exercises the GMEM input descriptor used by framebuffer fetch.
struct FetchPass {
    Vulkan &v; Vulkan::Image &source,&output;
    VkRenderPass pass=VK_NULL_HANDLE;VkFramebuffer framebuffer=VK_NULL_HANDLE;
    VkPipeline pipelines[2]{};VkDescriptorSet input=VK_NULL_HANDLE;
    FetchPass(Vulkan &vk,Vulkan::Image &src,Vulkan::Image &dst):v(vk),source(src),output(dst) {
        try {
        VkAttachmentDescription attachments[2]{};
        for(unsigned i=0;i<2;++i) {
            attachments[i].format=i?output.format:source.format;attachments[i].samples=VK_SAMPLE_COUNT_1_BIT;
            attachments[i].loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;attachments[i].storeOp=VK_ATTACHMENT_STORE_OP_STORE;
            attachments[i].stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;attachments[i].stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachments[i].initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;attachments[i].finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        }
        VkAttachmentReference srcColor{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},dstColor{1,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference srcInput{0,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkSubpassDescription subpasses[2]{};
        for(unsigned i=0;i<2;++i){subpasses[i].pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;subpasses[i].colorAttachmentCount=1;}
        subpasses[0].pColorAttachments=&srcColor;subpasses[1].pColorAttachments=&dstColor;
        subpasses[1].inputAttachmentCount=1;subpasses[1].pInputAttachments=&srcInput;
        VkSubpassDependency dependencies[3]{};
        dependencies[0].srcSubpass=VK_SUBPASS_EXTERNAL;dependencies[0].dstSubpass=0;
        dependencies[0].srcStageMask=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;dependencies[0].dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[0].srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;dependencies[0].dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependencies[1].srcSubpass=0;dependencies[1].dstSubpass=1;
        dependencies[1].srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;dependencies[1].dstStageMask=VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dependencies[1].srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;dependencies[1].dstAccessMask=VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
        dependencies[1].dependencyFlags=VK_DEPENDENCY_BY_REGION_BIT;
        dependencies[2].srcSubpass=1;dependencies[2].dstSubpass=VK_SUBPASS_EXTERNAL;
        dependencies[2].srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;dependencies[2].dstStageMask=VK_PIPELINE_STAGE_TRANSFER_BIT;
        dependencies[2].srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;dependencies[2].dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};info.attachmentCount=2;info.pAttachments=attachments;
        info.subpassCount=2;info.pSubpasses=subpasses;info.dependencyCount=3;info.pDependencies=dependencies;
        vkcheck(v.CreateRenderPass(v.device,&info,nullptr,&pass),"fetch-render-pass");
        VkImageView views[]={source.view,output.view};
        VkFramebufferCreateInfo frame{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};frame.renderPass=pass;frame.attachmentCount=2;frame.pAttachments=views;
        frame.width=output.width;frame.height=output.height;frame.layers=1;
        vkcheck(v.CreateFramebuffer(v.device,&frame,nullptr,&framebuffer),"fetch-framebuffer");
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};allocate.descriptorPool=v.descriptors;
        allocate.descriptorSetCount=1;allocate.pSetLayouts=&v.input_layout;
        vkcheck(v.AllocateDescriptorSets(v.device,&allocate,&input),"fetch-descriptor");
        VkDescriptorImageInfo imageInfo{VK_NULL_HANDLE,source.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=input;write.descriptorCount=1;
        write.descriptorType=VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;write.pImageInfo=&imageInfo;v.UpdateDescriptorSets(v.device,1,&write,0,nullptr);
        VkPipelineShaderStageCreateInfo stages[2]{};stages[0].sType=stages[1].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=v.vertex;stages[0].pName="main";
        stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].pName="main";
        VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};assembly.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};viewport.viewportCount=viewport.scissorCount=1;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};raster.polygonMode=VK_POLYGON_MODE_FILL;raster.lineWidth=1;
        VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};samples.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState attachment{};attachment.colorWriteMask=15;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};blend.attachmentCount=1;blend.pAttachments=&attachment;
        VkDynamicState states[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};dynamic.dynamicStateCount=2;dynamic.pDynamicStates=states;
        VkGraphicsPipelineCreateInfo graphics{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};graphics.stageCount=2;graphics.pStages=stages;
        graphics.pVertexInputState=&vertex;graphics.pInputAssemblyState=&assembly;graphics.pViewportState=&viewport;
        graphics.pRasterizationState=&raster;graphics.pMultisampleState=&samples;graphics.pColorBlendState=&blend;graphics.pDynamicState=&dynamic;graphics.renderPass=pass;
        for(unsigned i=0;i<2;++i) {
            stages[1].module=i?v.input_fragment:v.fragment;graphics.layout=i?v.input_pipeline_layout:v.graphics_layout;graphics.subpass=i;
            vkcheck(v.CreateGraphicsPipelines(v.device,VK_NULL_HANDLE,1,&graphics,nullptr,&pipelines[i]),"fetch-pipeline");
        }
        } catch (...) {release();throw;}
    }
    void encode() {
        VkClearValue values[2]{};
        VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};begin.renderPass=pass;begin.framebuffer=framebuffer;
        begin.renderArea.extent={output.width,output.height};begin.clearValueCount=2;begin.pClearValues=values;
        v.CmdBeginRenderPass(v.command,&begin,VK_SUBPASS_CONTENTS_INLINE);
        VkViewport viewport{0,0,float(output.width),float(output.height),0,1};v.CmdSetViewport(v.command,0,1,&viewport);
        VkRect2D scissor{{0,0},{output.width,output.height}};v.CmdSetScissor(v.command,0,1,&scissor);
        const float params[]={0,0,0,0,0.5f,1,float(output.width),float(output.height)};
        for(unsigned i=0;i<2;++i) {
            if(i)v.CmdNextSubpass(v.command,VK_SUBPASS_CONTENTS_INLINE);
            const VkPipelineLayout layout=i?v.input_pipeline_layout:v.graphics_layout;const VkDescriptorSet descriptor=i?input:v.dummy_set;
            v.CmdBindPipeline(v.command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipelines[i]);
            v.CmdBindDescriptorSets(v.command,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,1,&descriptor,0,nullptr);
            v.CmdPushConstants(v.command,layout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(params),params);
            v.CmdDraw(v.command,3,1,0,0);
        }
        v.CmdEndRenderPass(v.command);
    }
    void release(){for(auto pipeline:pipelines)if(pipeline)v.DestroyPipeline(v.device,pipeline,nullptr);
        if(framebuffer)v.DestroyFramebuffer(v.device,framebuffer,nullptr);if(pass)v.DestroyRenderPass(v.device,pass,nullptr);
        pipelines[0]=pipelines[1]=VK_NULL_HANDLE;framebuffer=VK_NULL_HANDLE;pass=VK_NULL_HANDLE;}
    ~FetchPass(){release();}
};
struct RenderEvidence {
    using Read=void (*)(uint64_t *);
    void *library=nullptr;Read modes=nullptr,sides=nullptr,events=nullptr,destinations=nullptr;
    uint64_t before[8]{},sideBefore[16]{},eventBefore[8]{},destinationBefore[12]{};
    RenderEvidence() {
        library=dlopen("/System/Library/Extensions/DitingAdrenoMetal.bundle/libvulkan_freedreno.dylib",RTLD_NOW|RTLD_LOCAL);
        if(library){modes=reinterpret_cast<Read>(dlsym(library,"tu_render_mode_stats"));sides=reinterpret_cast<Read>(dlsym(library,"tu_gmem_side_stats"));
            events=reinterpret_cast<Read>(dlsym(library,"tu_gmem_event_stats"));
            destinations=reinterpret_cast<Read>(dlsym(library,"tu_gmem_destination_stats"));}
        if(!modes||!sides||!events||!destinations){if(library)dlclose(library);library=nullptr;throw Failure{"render-path-counters-required",-1};}
        modes(before);sides(sideBefore);events(eventBefore);destinations(destinationBefore);
    }
    NSDictionary *finish(NSDictionary *pixels,bool linear,bool fetch,unsigned minimumPasses,bool surface=false) {
        uint64_t after[8]{},sideAfter[16]{},eventAfter[8]{},destinationAfter[12]{};
        modes(after);sides(sideAfter);events(eventAfter);destinations(destinationAfter);
        NSMutableArray *modeDelta=[NSMutableArray new],*sideDelta=[NSMutableArray new],*eventDelta=[NSMutableArray new],*destinationDelta=[NSMutableArray new];bool valid=true;
        for(unsigned i=0;i<8;++i){if(after[i]<before[i])valid=false;after[i]-=before[i];[modeDelta addObject:@(after[i])];}
        for(unsigned i=0;i<16;++i){if(sideAfter[i]<sideBefore[i])valid=false;sideAfter[i]-=sideBefore[i];[sideDelta addObject:@(sideAfter[i])];}
        for(unsigned i=0;i<8;++i){if(eventAfter[i]<eventBefore[i])valid=false;eventAfter[i]-=eventBefore[i];[eventDelta addObject:@(eventAfter[i])];}
        for(unsigned i=0;i<12;++i){if(destinationAfter[i]<destinationBefore[i])valid=false;destinationAfter[i]-=destinationBefore[i];[destinationDelta addObject:@(destinationAfter[i])];}
        const bool linearObserved=sideAfter[3]+sideAfter[7]+sideAfter[11]+sideAfter[15]+eventAfter[3]+eventAfter[7]>0;
        const char *policy=getenv("DITING_GPU_TEST_POLICY") ?: "auto";
        valid=valid&&after[0]+after[2]+after[4]>=minimumPasses;
        if(!strcmp(policy,"gmem")) {
            valid=valid&&after[0]>=minimumPasses;
            if(linear)valid=valid&&linearObserved;
            if(fetch)valid=valid&&sideAfter[12]>0;
            if(surface)valid=valid&&sideAfter[0]+sideAfter[4]+sideAfter[8]+eventAfter[0]+eventAfter[4]>0;
        } else if(!strcmp(policy,"sysmem"))valid=valid&&!after[0]&&after[2]+after[4]>=minimumPasses;
        NSMutableDictionary *record=[pixels mutableCopy];record[@"path_verified"]=@(valid);
        record[@"pass"]=@([pixels[@"pass"] boolValue]&&valid);record[@"render_mode_delta"]=modeDelta;record[@"gmem_side_delta"]=sideDelta;
        record[@"gmem_event_delta"]=eventDelta;record[@"linear_transfer_observed"]=@(linearObserved);
        record[@"gmem_destination_delta"]=destinationDelta;
        // IOSurface import may use a shadow image. CPU pixel checks establish
        // the import contract; counters establish GMEM use without inventing
        // the tiling of the runtime's actual render target.
        record[@"linear_tiling"]=surface?[NSNull null]:@(linear);
        record[@"render_target_kind"]=surface?@"metal-iosurface":linear?@"vulkan-linear-image":@"vulkan-optimal-image";
        record[@"input_fetch"]=@(fetch);record[@"minimum_render_passes"]=@(minimumPasses);
        return record;
    }
    ~RenderEvidence(){if(library)dlclose(library);}
};
enum class PixelFormat { RGBA, BGRA, SRGBA, SBGRA, Half };
static unsigned pixel_bytes(PixelFormat format){return format==PixelFormat::Half?8:4;}
static VkFormat vkformat(PixelFormat format) {
    return format==PixelFormat::RGBA?VK_FORMAT_R8G8B8A8_UNORM:format==PixelFormat::BGRA?VK_FORMAT_B8G8R8A8_UNORM:
        format==PixelFormat::SRGBA?VK_FORMAT_R8G8B8A8_SRGB:format==PixelFormat::SBGRA?VK_FORMAT_B8G8R8A8_SRGB:VK_FORMAT_R16G16B16A16_SFLOAT;
}
static NSDictionary *compare(const uint8_t *data,size_t bytes,unsigned width,unsigned height,unsigned stride,unsigned offset,
    PixelFormat format,std::function<Color(unsigned,unsigned)> reference,bool guards=true) {
    const unsigned bpp=pixel_bytes(format);unsigned mismatches=0,guard_errors=0,nonfinite=0;double maximum=0;
    if(size_t(offset)+size_t(stride)*height>bytes)throw Failure{"readback-bounds",-1};
    NSMutableArray *first=[NSMutableArray new];
    for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x) {
        Color color=reference(x,y);const uint8_t *pixel=data+offset+size_t(y)*stride+x*bpp;bool wrong=false;
        NSMutableArray *actual=nil,*expected=nil;
        if(format==PixelFormat::Half) {
            float values[]={color.r,color.g,color.b,color.a};double errors[4];
            for(unsigned c=0;c<4;++c){uint16_t raw;memcpy(&raw,pixel+2*c,2);errors[c]=std::abs(double(DitingGpuOracle::half(raw))-values[c]);
                if(!std::isfinite(errors[c])){wrong=true;++nonfinite;}
                else {if(errors[c]>0.002)wrong=true;maximum=std::max(maximum,errors[c]);}}
            if(wrong && first.count<8){actual=[NSMutableArray new];expected=[NSMutableArray new];
                for(unsigned c=0;c<4;++c){uint16_t raw;memcpy(&raw,pixel+2*c,2);float value=DitingGpuOracle::half(raw);[actual addObject:std::isfinite(value)?@(value):@"nonfinite"];[expected addObject:@(values[c])];}}
        } else {
            uint8_t oracle[4];DitingGpuOracle::bytes(color,oracle,format==PixelFormat::BGRA||format==PixelFormat::SBGRA,format==PixelFormat::SRGBA||format==PixelFormat::SBGRA);
            for(unsigned c=0;c<4;++c){int error=abs(int(pixel[c])-int(oracle[c]));maximum=std::max(maximum,double(error));if(error>2)wrong=true;}
            if(wrong && first.count<8){actual=[NSMutableArray new];expected=[NSMutableArray new];for(unsigned c=0;c<4;++c){[actual addObject:@(pixel[c])];[expected addObject:@(oracle[c])];}}
        }
        if(wrong){++mismatches;if(actual)[first addObject:@{@"x":@(x),@"y":@(y),@"actual":actual,@"expected":expected}];}
    }
    if(guards) {
        for(unsigned i=0;i<offset;++i)if(data[i]!=0xcd)++guard_errors;
        for(unsigned y=0;y<height;++y)for(unsigned i=width*bpp;i<stride;++i)if(data[offset+size_t(y)*stride+i]!=0xcd)++guard_errors;
        for(size_t i=size_t(offset)+size_t(stride)*height;i<bytes;++i)if(data[i]!=0xcd)++guard_errors;
    }
    return @{@"pass":@(!mismatches&&!guard_errors),@"pixels":@(uint64_t(width)*height),@"mismatches":@(mismatches),
        @"guard_errors":@(guard_errors),@"nonfinite_channels":@(nonfinite),
        @"max_channel_error":nonfinite?[NSNull null]:@(maximum),@"first_mismatches":first,
        @"reference":@"independent CPU RGBA/sRGB/FP16 and alpha-over",@"row_bytes":@(stride),@"offset":@(offset)};
}
static NSDictionary *read_image(Vulkan &v,Vulkan::Image &image,PixelFormat format,
    std::function<void()> encode,std::function<Color(unsigned,unsigned)> reference) {
    const unsigned stride=(image.width+13)*pixel_bytes(format);
    Vulkan::Buffer buffer(v,64+size_t(stride)*image.height+64);memset(buffer.mapped,0xcd,buffer.bytes);buffer.flush(false);
    v.begin();encode();v.readback(image,buffer,pixel_bytes(format));const auto timing=v.finish();buffer.flush(true);
    NSMutableDictionary *result=[compare(static_cast<const uint8_t *>(buffer.mapped),buffer.bytes,image.width,image.height,stride,64,format,reference) mutableCopy];
    result[@"gpu_us"]=std::isfinite(timing.gpu_us)?@(timing.gpu_us):[NSNull null];result[@"cpu_total_us"]=@(timing.total_us);
    return result;
}
// Check linear storage itself. A GPU image-to-buffer blit can otherwise hide
// an attachment swap by applying the inverse swap on the readback path.
static NSDictionary *read_linear_storage(Vulkan &v,Vulkan::Image &image,PixelFormat format,
    std::function<Color(unsigned,unsigned)> reference) {
    if(image.tiling!=VK_IMAGE_TILING_LINEAR ||
       !(v.memory.memoryTypes[image.memory_type].propertyFlags&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
        throw Failure{"linear-storage-not-host-visible",-1};
    VkImageSubresource subresource{VK_IMAGE_ASPECT_COLOR_BIT,0,0};VkSubresourceLayout layout{};
    v.GetImageSubresourceLayout(v.device,image.image,&subresource,&layout);
    if(layout.rowPitch>UINT32_MAX || layout.offset>UINT32_MAX || image.allocation_bytes>SIZE_MAX)
        throw Failure{"linear-storage-layout",-1};
    v.begin();v.transition(image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_HOST_READ_BIT);v.finish();
    void *mapped=nullptr;vkcheck(v.MapMemory(v.device,image.backing,0,VK_WHOLE_SIZE,0,&mapped),"map-linear-storage");
    struct Unmap {Vulkan &v;VkDeviceMemory memory;~Unmap(){v.UnmapMemory(v.device,memory);}};
    NSDictionary *pixels=nil;
    {
        Unmap guard{v,image.backing};
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};range.memory=image.backing;range.size=VK_WHOLE_SIZE;
        vkcheck(v.InvalidateMappedMemoryRanges(v.device,1,&range),"acquire-linear-storage");
        // Imported images are checked through the owner's original pointer.
        // A driver-created shadow mapping cannot replace this CPU oracle.
        const void *storage=image.host_storage?image.host_storage:mapped;
        pixels=compare(static_cast<const uint8_t *>(storage),size_t(image.allocation_bytes),image.width,image.height,
            unsigned(layout.rowPitch),unsigned(layout.offset),format,reference,false);
    }
    v.begin();v.transition(image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_READ_BIT);v.finish();
    return pixels;
}
static void vulkan_qualification(Vulkan &v) {
    const Color base{0.25,0.5,0.75,0.625};
    const PixelFormat formats[]={PixelFormat::RGBA,PixelFormat::BGRA,PixelFormat::SRGBA,PixelFormat::SBGRA,PixelFormat::Half};
    const char *names[]={"vk-clear-rgba8","vk-clear-bgra8","vk-clear-rgba8-srgb","vk-clear-bgra8-srgb","vk-clear-fp16"};
    for(unsigned i=0;i<5;++i)testcase(1+i,names[i],"colour",[&]{
        Vulkan::Image image(v,vkformat(formats[i]),257,129);Pass pass(v,image);
        return read_image(v,image,formats[i],[&]{pass.begin(base);pass.end();},[&](unsigned,unsigned){return base;});
    });
    testcase(6,"vk-bgra-pattern-and-stride","colour",[&]{
        Vulkan::Image image(v,VK_FORMAT_B8G8R8A8_UNORM,257,129);Pass pass(v,image);
        return read_image(v,image,PixelFormat::BGRA,[&]{pass.begin({0,0,0,0});pass.draw({},1);pass.end();},DitingGpuOracle::bars);
    });
    testcase(7,"vk-alpha-over","colour",[&]{
        const Color dest{0.25,0.5,0.75,1},source{1,0,0,0.25};Vulkan::Image image(v,VK_FORMAT_B8G8R8A8_UNORM,257,129);Pass pass(v,image);
        return read_image(v,image,PixelFormat::BGRA,[&]{pass.begin(dest);pass.draw(source,0,true);pass.end();},[&](unsigned,unsigned){return DitingGpuOracle::over(source,dest);});
    });
    testcase(8,"vk-load-store-scissor","colour",[&]{
        const Color dest{0,0,1,1},source{1,0,0,1};Vulkan::Image image(v,VK_FORMAT_B8G8R8A8_UNORM,257,129);Pass first(v,image),second(v,image,true);
        return read_image(v,image,PixelFormat::BGRA,[&]{
            first.begin(dest);first.end();second.begin({});VkRect2D scissor{{13,7},{121,63}};
            v.CmdSetScissor(v.command,0,1,&scissor);second.draw(source);second.end();
        },[&](unsigned x,unsigned y){return x>=13&&x<134&&y>=7&&y<70?source:dest;});
    });
    testcase(9,"vk-bgra-sampled-to-rgba","colour",[&]{
        Vulkan::Image source(v,VK_FORMAT_B8G8R8A8_UNORM,257,129),dest(v,VK_FORMAT_R8G8B8A8_UNORM,257,129);Pass sourcePass(v,source),destPass(v,dest);
        v.begin();sourcePass.begin({});sourcePass.draw({},1);sourcePass.end();
        v.transition(source,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_SHADER_READ_BIT);v.finish();
        VkDescriptorSet texture=v.sampledSet(source);
        return read_image(v,dest,PixelFormat::RGBA,[&]{destPass.begin({});destPass.draw({},2,false,texture);destPass.end();},DitingGpuOracle::bars);
    });
    testcase(10,"vk-msaa4-bgra-resolve","colour",[&]{
        Vulkan::Image image(v,VK_FORMAT_B8G8R8A8_UNORM,257,129);Pass pass(v,image,false,true);
        return read_image(v,image,PixelFormat::BGRA,[&]{pass.begin({});pass.draw({},1);pass.end();},DitingGpuOracle::bars);
    });
    testcase(11,"vk-depth-order","colour",[&]{
        const Color front{0,0,1,1};Vulkan::Image image(v,VK_FORMAT_R8G8B8A8_UNORM,257,129);Pass pass(v,image,false,false,true);
        return read_image(v,image,PixelFormat::RGBA,[&]{pass.begin({});pass.draw(front,0,false,VK_NULL_HANDLE,0.25);pass.draw({1,0,0,1},0,false,VK_NULL_HANDLE,0.75);pass.end();},[&](unsigned,unsigned){return front;});
    });
    testcase(12,"vk-compute-and-host-acquire","synchronization",[&]{
        Vulkan::Buffer output(v,4096*4);memset(output.mapped,0xcd,output.bytes);output.flush(false);
        VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};allocation.descriptorPool=v.descriptors;allocation.descriptorSetCount=1;allocation.pSetLayouts=&v.compute_layout;
        VkDescriptorSet set;vkcheck(v.AllocateDescriptorSets(v.device,&allocation,&set),"compute-descriptor");
        VkDescriptorBufferInfo buffer{output.buffer,0,output.bytes};VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet=set;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;write.pBufferInfo=&buffer;v.UpdateDescriptorSets(v.device,1,&write,0,nullptr);
        VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};info.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;info.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;info.stage.module=v.compute;info.stage.pName="main";info.layout=v.compute_pipeline_layout;
        VkPipeline pipeline;vkcheck(v.CreateComputePipelines(v.device,VK_NULL_HANDLE,1,&info,nullptr,&pipeline),"compute-pipeline");
        v.begin();v.CmdBindPipeline(v.command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);v.CmdBindDescriptorSets(v.command,VK_PIPELINE_BIND_POINT_COMPUTE,v.compute_pipeline_layout,0,1,&set,0,nullptr);
        v.CmdDispatch(v.command,64,1,1);
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        v.CmdPipelineBarrier(v.command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);v.finish();output.flush(true);
        unsigned errors=0;for(unsigned i=0;i<4096;++i)if(static_cast<uint32_t *>(output.mapped)[i]!=DitingGpuOracle::compute(i))++errors;
        v.DestroyPipeline(v.device,pipeline,nullptr);return @{@"pass":@(!errors),@"words":@4096,@"mismatches":@(errors)};
    });
    testcase(13,"vk-bo-map-retire-release-64","lifetime",[&]{
        unsigned errors=0;const uint64_t start=now_ns();
        for(unsigned cycle=0;cycle<64;++cycle) {
            Vulkan::Buffer output(v,4096);const uint32_t value=DitingGpuOracle::compute(cycle);
            v.begin();v.CmdFillBuffer(v.command,output.buffer,0,VK_WHOLE_SIZE,value);
            VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
            v.CmdPipelineBarrier(v.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);v.finish();output.flush(true);
            for(unsigned i=0;i<1024;++i)if(static_cast<uint32_t *>(output.mapped)[i]!=value)++errors;
        }
        return @{@"pass":@(!errors),@"cycles":@64,@"mismatches":@(errors),@"elapsed_us":@((now_ns()-start)/1000)};
    });
    const PixelFormat linearFormats[]={PixelFormat::BGRA,PixelFormat::SBGRA,PixelFormat::BGRA};
    const uint32_t linearIds[]={15,16,19};
    const char *linearNames[]={"vk-linear-bgra-load-store-draw","vk-linear-bgra-srgb-load-store-draw","vk-mutable-linear-bgra-load-store-draw"};
    for(unsigned i=0;i<3;++i)testcase(linearIds[i],linearNames[i],"colour",[&]{
        Vulkan::Image image(v,vkformat(linearFormats[i]),257,129,VK_SAMPLE_COUNT_1_BIT,false,VK_IMAGE_TILING_LINEAR,
                            i==2?VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT:0);Pass first(v,image),loaded(v,image,true);RenderEvidence evidence;
        NSDictionary *seed=read_image(v,image,linearFormats[i],[&]{first.begin({});first.draw({},1);first.end();},DitingGpuOracle::bars);
        const Color overlay{0,0,1,0.5};
        NSDictionary *record=read_image(v,image,linearFormats[i],[&]{
            loaded.begin({});VkRect2D scissor{{13,7},{121,63}};v.CmdSetScissor(v.command,0,1,&scissor);
            loaded.draw(overlay,0,true);loaded.end();
        },[&](unsigned x,unsigned y){const Color original=DitingGpuOracle::bars(x,y);
            return x>=13&&x<134&&y>=7&&y<70?DitingGpuOracle::over(overlay,original):original;});
        NSMutableDictionary *combined=[record mutableCopy];combined[@"seed_pixels"]=seed;
        combined[@"pass"]=@([seed[@"pass"] boolValue]&&[record[@"pass"] boolValue]);
        return evidence.finish(combined,true,false,2);
    });
    const PixelFormat partialFormats[]={PixelFormat::BGRA,PixelFormat::SBGRA,PixelFormat::BGRA,PixelFormat::BGRA,
        PixelFormat::BGRA,PixelFormat::SBGRA,PixelFormat::BGRA};
    const unsigned partialIds[]={34,35,36,37,46,47,48};
    const char *partialNames[]={"vk-linear-bgra-partial-render-area","vk-linear-sbgra-partial-render-area",
        "vk-mutable-linear-bgra-partial-render-area","vk-optimal-bgra-partial-render-area",
        "vk-host-linear-bgra-partial-render-area","vk-host-linear-sbgra-partial-render-area",
        "vk-host-mutable-linear-bgra-partial-render-area"};
    for(unsigned i=0;i<7;++i)testcase(partialIds[i],partialNames[i],"colour",[&]{
        constexpr unsigned width=257,height=129,cyclesCount=6;
        const bool linear=i!=3,imported=i>=4;
        Vulkan::Image image(v,vkformat(partialFormats[i]),width,height,VK_SAMPLE_COUNT_1_BIT,false,
            linear?VK_IMAGE_TILING_LINEAR:VK_IMAGE_TILING_OPTIMAL,
            i==2||i==6?VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT:0,linear,imported);
        Pass first(v,image),loaded(v,image,true);RenderEvidence evidence;
        std::vector<Color> oracle(width*height);
        for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x)oracle[y*width+x]=DitingGpuOracle::bars(x,y);
        auto reference=[&](unsigned x,unsigned y){return oracle[y*width+x];};
        NSDictionary *seed=read_image(v,image,partialFormats[i],[&]{first.begin({});first.draw({},1);first.end();},reference);
        NSMutableArray *cycles=[NSMutableArray new],*rawCycles=[NSMutableArray new];bool passed=[seed[@"pass"] boolValue];
        NSDictionary *last=seed;
        if(linear){auto raw=read_linear_storage(v,image,partialFormats[i],reference);[rawCycles addObject:raw];passed=passed&&[raw[@"pass"] boolValue];}
        for(unsigned cycle=0;cycle<cyclesCount;++cycle) {
            // Unaligned renderArea forces draw/2D GMEM stores. Imported
            // linear images with odd dimensions also require draw loads.
            VkRect2D area{{int32_t(13+cycle*7),int32_t(7+cycle*5)},{121,63}};
            const Color overlay=cycle%2?Color{0.25,0.5,0.75,1}:Color{1,0.125,0.5,1};
            for(unsigned y=unsigned(area.offset.y);y<unsigned(area.offset.y)+area.extent.height;++y)
                for(unsigned x=unsigned(area.offset.x);x<unsigned(area.offset.x)+area.extent.width;++x)oracle[y*width+x]=overlay;
            last=read_image(v,image,partialFormats[i],[&]{loaded.begin({},&area);loaded.draw(overlay);loaded.end();},reference);
            passed=passed&&[last[@"pass"] boolValue];[cycles addObject:last];
            if(linear){auto raw=read_linear_storage(v,image,partialFormats[i],reference);[rawCycles addObject:raw];passed=passed&&[raw[@"pass"] boolValue];}
        }
        NSMutableDictionary *record=[last mutableCopy];record[@"pass"]=@(passed);record[@"reuse_cycles"]=@(cyclesCount);
        record[@"cycle_pixels"]=cycles;record[@"linear_storage_pixels"]=rawCycles;record[@"partial_render_area"]=@YES;record[@"seed_pixels"]=seed;
        record[@"readback_source"]=linear?@"raw mapped linear image plus independent transfer-buffer oracle":@"Vulkan transfer buffer";
        if(imported)record[@"readback_source"]=@"original imported host pointer plus independent transfer-buffer oracle";
        record[@"imported_host_pointer"]=@(imported);record[@"draw_load_required"]=@(imported);
        if(imported) {
            record[@"external_handle_type"]=@(VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT);
            record[@"host_pointer_alignment"]=@(v.host_alignment);record[@"host_allocation_bytes"]=@(image.allocation_bytes);
        }
        record=[evidence.finish(record,linear,false,cyclesCount+1) mutableCopy];
        NSArray *delta=record[@"gmem_destination_delta"];
        const bool drawStore=[delta[0] unsignedLongLongValue]+[delta[4] unsignedLongLongValue]>0;
        const bool drawLoad=[delta[8] unsignedLongLongValue]>0;
        const bool verified=strcmp(getenv("DITING_GPU_TEST_POLICY") ?: "auto","gmem") || (drawStore&&(!imported||drawLoad));
        record[@"draw_transfer_verified"]=@(verified);record[@"pass"]=@([record[@"pass"] boolValue]&&verified);
        return record;
    });
    for(unsigned i=0;i<2;++i)testcase(17+i,i?"vk-optimal-bgra-input-fetch":"vk-linear-bgra-input-fetch","colour",[&]{
        Vulkan::Image source(v,VK_FORMAT_B8G8R8A8_UNORM,257,129,VK_SAMPLE_COUNT_1_BIT,false,
                             i?VK_IMAGE_TILING_OPTIMAL:VK_IMAGE_TILING_LINEAR);
        Vulkan::Image output(v,VK_FORMAT_R8G8B8A8_UNORM,257,129);FetchPass pass(v,source,output);RenderEvidence evidence;
        NSDictionary *record=read_image(v,output,PixelFormat::RGBA,[&]{pass.encode();},DitingGpuOracle::bars);
        return evidence.finish(record,!i,true,1);
    });
}
static double percentile(std::vector<double> list,double p) {
    if(list.empty())return NAN;std::sort(list.begin(),list.end());return list[size_t(std::ceil(p*list.size()))-1];
}
static void vulkan_performance(Vulkan &v,unsigned width,unsigned height,unsigned frames,unsigned warmup) {
    const unsigned layers[]={0,8,32};const char *names[]={"vk-panel-clear","vk-panel-alpha-8","vk-panel-alpha-32"};
    for(unsigned scene=0;scene<3;++scene)testcase(40+scene,names[scene],"performance",[&]{
        Vulkan::Image image(v,VK_FORMAT_B8G8R8A8_UNORM,width,height);Pass pass(v,image);
        std::vector<double> total,gpu;NSMutableArray *samples=[NSMutableArray new];uint64_t measured_start=0,finish=0;
        for(unsigned frame=0;frame<frames+warmup;++frame) {
            if(frame==warmup)measured_start=now_ns();
            v.begin();pass.begin({0.25,0.5,0.75,1});
            for(unsigned layer=0;layer<layers[scene];++layer)pass.draw({float((frame+layer)%17)/16,float(layer%7)/6,0.25,0.125},0,true);
            pass.end();const auto time=v.finish();finish=now_ns();
            if(frame>=warmup) {
                total.push_back(double(time.total_us));if(std::isfinite(time.gpu_us))gpu.push_back(time.gpu_us);
                [samples addObject:@[@(time.encode_us),@(time.submit_us),@(time.wait_us),@(time.total_us),std::isfinite(time.gpu_us)?@(uint64_t(std::llround(time.gpu_us))):[NSNull null]]];
            }
        }
        const double budget=1000000.0/120,tail=percentile(total,0.95),gpu_tail=percentile(gpu,0.95);
        return @{@"pass":@(tail<=budget && gpu.size()==frames && gpu_tail<=budget),@"width":@(width),@"height":@(height),
            @"layers":@(layers[scene]),@"frames":@(frames),@"warmup":@(warmup),@"target_fps":@120,@"frame_budget_us":@(budget),
            @"p50_us":@(percentile(total,0.5)),@"p95_us":@(tail),@"p99_us":@(percentile(total,0.99)),@"max_us":@(*std::max_element(total.begin(),total.end())),
            @"gpu_p95_us":std::isfinite(gpu_tail)?@(gpu_tail):[NSNull null],@"pipeline_setup_us":@(pass.pipeline_us),
            @"throughput_fps":@(double(frames)*1e9/double(finish-measured_start)),@"sample_columns":@[@"encode_us",@"submit_us",@"wait_us",@"total_us",@"gpu_us"],
            @"samples":samples,@"scope":@"offscreen full-panel GPU work; no physical presentation"};
    });
}
static void metal_wait(id<MTLCommandBuffer> command) {
    if(!command)throw Failure{"metal-command-buffer",-1};
    // status can become Completed before the Diting host-import acquire.
    // The completion callback follows that acquire and is the readback gate.
    dispatch_semaphore_t completed=dispatch_semaphore_create(0);
    [command addCompletedHandler:^(id<MTLCommandBuffer>){dispatch_semaphore_signal(completed);}];
    [command commit];const uint64_t now=now_ns();
    const uint64_t left=deadline_ns>now?std::min<uint64_t>(5000000000ull,deadline_ns-now):0;
    if(!left || dispatch_semaphore_wait(completed,dispatch_time(DISPATCH_TIME_NOW,int64_t(left)))) {
        emit(@{@"kind":@"fatal",@"stage":@"metal-fence-timeout",@"job":@(job_id)});_Exit(71);
    }
    if(command.status!=MTLCommandBufferStatusCompleted) {
        emit(@{@"kind":@"fatal",@"stage":@"metal-command-error",@"code":@(command.error.code),@"job":@(job_id)});_Exit(71);
    }
    check_time();
}
static MTLPixelFormat metalformat(PixelFormat format) {
    return format==PixelFormat::RGBA?MTLPixelFormatRGBA8Unorm:format==PixelFormat::BGRA?MTLPixelFormatBGRA8Unorm:
        format==PixelFormat::SRGBA?MTLPixelFormatRGBA8Unorm_sRGB:format==PixelFormat::SBGRA?MTLPixelFormatBGRA8Unorm_sRGB:MTLPixelFormatRGBA16Float;
}
struct Metal {
    id<MTLDevice> device;id<MTLCommandQueue> queue;id<MTLLibrary> library;
    Metal() {
        device=MTLCreateSystemDefaultDevice();
        if(!device)throw Failure{"metal-discovery",-1};
        // Qualify the actually loaded Diting implementation, even if a profile
        // supplies an Apple-compatible device name.
        Method method=class_getInstanceMethod([device class],@selector(newCommandQueue));
        Dl_info origin{};
        if(!method || !dladdr(reinterpret_cast<const void *>(method_getImplementation(method)),&origin) ||
            !origin.dli_fname || !strstr(origin.dli_fname,"DitingMetalBridge"))throw Failure{"diting-metal-implementation-required",-1};
        queue=[device newCommandQueue];if(!queue)throw Failure{"metal-queue",-1};
        NSError *error=nil;
        dispatch_data_t data=dispatch_data_create(diting_test_metallib,sizeof(diting_test_metallib),nullptr,DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        library=[device newLibraryWithData:data error:&error];
        if(!library)throw Failure{"metal-owned-air-library",int(error.code)};
        emit(@{@"kind":@"device",@"api":@"metal",@"name":device.name ?: @"",
            @"class":NSStringFromClass([device class]),@"implementation":@(origin.dli_fname),
            @"tu_debug":@(getenv("TU_DEBUG") ?: ""),@"single_sample_override":@(getenv("NATIVE_METAL_SINGLE_SAMPLE") ?: ""),
            @"shader_source":@"owned AIR assembled locally"});
    }
    id<MTLTexture> texture(PixelFormat format,unsigned width,unsigned height,unsigned samples=1) {
        MTLTextureDescriptor *desc=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:metalformat(format) width:width height:height mipmapped:NO];
        desc.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;desc.storageMode=MTLStorageModePrivate;
        if(samples>1){desc.textureType=MTLTextureType2DMultisample;desc.sampleCount=samples;}
        id<MTLTexture> result=[device newTextureWithDescriptor:desc];if(!result)throw Failure{"metal-texture",int(metalformat(format))};
        if(result.sampleCount!=samples)throw Failure{"metal-actual-sample-count",int(result.sampleCount)};
        return result;
    }
    id<MTLRenderPipelineState> pipeline(PixelFormat format,bool blend=false,unsigned samples=1) {
        MTLRenderPipelineDescriptor *info=[MTLRenderPipelineDescriptor new];
        info.vertexFunction=[library newFunctionWithName:@"diting_test_vertex"];
        info.fragmentFunction=[library newFunctionWithName:@"diting_test_fragment"];
        info.rasterSampleCount=samples;info.colorAttachments[0].pixelFormat=metalformat(format);
        if(!info.vertexFunction||!info.fragmentFunction)throw Failure{"metal-owned-air-functions",-1};
        auto attachment=info.colorAttachments[0];attachment.blendingEnabled=blend;
        attachment.sourceRGBBlendFactor=MTLBlendFactorSourceAlpha;attachment.destinationRGBBlendFactor=MTLBlendFactorOneMinusSourceAlpha;
        attachment.sourceAlphaBlendFactor=MTLBlendFactorOne;attachment.destinationAlphaBlendFactor=MTLBlendFactorOneMinusSourceAlpha;
        NSError *error=nil;id<MTLRenderPipelineState> result=[device newRenderPipelineStateWithDescriptor:info error:&error];
        if(!result)throw Failure{"metal-air-pipeline",int(error.code)};return result;
    }
    id<MTLRenderCommandEncoder> begin(id<MTLCommandBuffer> command,id<MTLTexture> target,Color clear,id<MTLTexture> resolve=nil,bool load=false) {
        MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture=target;pass.colorAttachments[0].clearColor=MTLClearColorMake(clear.r,clear.g,clear.b,clear.a);
        pass.colorAttachments[0].loadAction=load?MTLLoadActionLoad:MTLLoadActionClear;
        pass.colorAttachments[0].storeAction=resolve?MTLStoreActionMultisampleResolve:MTLStoreActionStore;
        pass.colorAttachments[0].resolveTexture=resolve;
        id<MTLRenderCommandEncoder> result=[command renderCommandEncoderWithDescriptor:pass];
        if(!result)throw Failure{"metal-render-encoder",-1};return result;
    }
    void draw(id<MTLRenderCommandEncoder> encoder,id<MTLRenderPipelineState> pipeline,Color color) {
        [encoder setRenderPipelineState:pipeline];[encoder setFragmentBytes:&color length:sizeof(color) atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    }
    NSDictionary *readback(id<MTLTexture> target,PixelFormat format,
        std::function<void(id<MTLCommandBuffer>)> encode,std::function<Color(unsigned,unsigned)> reference) {
        const unsigned width=unsigned(target.width),height=unsigned(target.height),stride=(width*pixel_bytes(format)+255)&~255u;
        const size_t bytes=256+size_t(stride)*height+256;
        id<MTLBuffer> buffer=[device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
        if(!buffer||!buffer.contents)throw Failure{"metal-host-buffer",-1};
        memset(buffer.contents,0xcd,bytes);
        id<MTLCommandBuffer> command=[queue commandBuffer];if(!command)throw Failure{"metal-command-buffer",-1};
        encode(command);id<MTLBlitCommandEncoder> blit=[command blitCommandEncoder];if(!blit)throw Failure{"metal-blit-encoder",-1};
        [blit copyFromTexture:target sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(width,height,1)
            toBuffer:buffer destinationOffset:256 destinationBytesPerRow:stride destinationBytesPerImage:size_t(stride)*height];
        [blit endEncoding];metal_wait(command);
        return compare(static_cast<const uint8_t *>(buffer.contents),bytes,width,height,stride,256,format,reference);
    }
};
static void metal_qualification(Metal &m) {
    const Color base{0.25,0.5,0.75,0.625};
    const PixelFormat formats[]={PixelFormat::RGBA,PixelFormat::BGRA,PixelFormat::SRGBA,PixelFormat::SBGRA,PixelFormat::Half};
    const char *names[]={"metal-clear-rgba8","metal-clear-bgra8","metal-clear-rgba8-srgb","metal-clear-bgra8-srgb","metal-clear-fp16"};
    for(unsigned i=0;i<5;++i)testcase(20+i,names[i],"colour",[&]{
        auto target=m.texture(formats[i],257,129);
        return m.readback(target,formats[i],[&](id<MTLCommandBuffer> command){auto encoder=m.begin(command,target,base);[encoder endEncoding];},[&](unsigned,unsigned){return base;});
    });
    testcase(25,"metal-air-bgra-draw","colour",[&]{
        const Color color{1,0.125,0.5,1};auto target=m.texture(PixelFormat::BGRA,257,129);auto pipeline=m.pipeline(PixelFormat::BGRA);
        return m.readback(target,PixelFormat::BGRA,[&](id<MTLCommandBuffer> command){auto encoder=m.begin(command,target,{});m.draw(encoder,pipeline,color);[encoder endEncoding];},[&](unsigned,unsigned){return color;});
    });
    testcase(26,"metal-air-alpha-over","colour",[&]{
        const Color source{1,0,0,0.25},dest{0.25,0.5,0.75,1};auto target=m.texture(PixelFormat::BGRA,257,129);auto pipeline=m.pipeline(PixelFormat::BGRA,true);
        return m.readback(target,PixelFormat::BGRA,[&](id<MTLCommandBuffer> command){auto encoder=m.begin(command,target,dest);m.draw(encoder,pipeline,source);[encoder endEncoding];},[&](unsigned,unsigned){return DitingGpuOracle::over(source,dest);});
    });
    testcase(27,"metal-iosurface-bgra-import","colour",[&]{
        const unsigned width=257,height=129,stride=(width*4+255)&~255u;
        NSDictionary *properties=@{(__bridge id)kIOSurfaceWidth:@(width),(__bridge id)kIOSurfaceHeight:@(height),(__bridge id)kIOSurfaceBytesPerElement:@4,
            (__bridge id)kIOSurfaceBytesPerRow:@(stride),(__bridge id)kIOSurfaceAllocSize:@(stride*height),(__bridge id)kIOSurfacePixelFormat:@(0x42475241u)};
        IOSurfaceRef surface=IOSurfaceCreate((__bridge CFDictionaryRef)properties);
        if(!surface)throw Failure{"iosurface-create",-1};
        const IOReturn locked=IOSurfaceLock(surface,IOSurfaceLockOptions(0),nullptr);
        if(locked){CFRelease(surface);throw Failure{"iosurface-lock",int(locked)};}
        void *base=IOSurfaceGetBaseAddress(surface);const size_t actualStride=IOSurfaceGetBytesPerRow(surface);
        if(!base||actualStride<width*4){IOSurfaceUnlock(surface,IOSurfaceLockOptions(0),nullptr);CFRelease(surface);throw Failure{"iosurface-layout",-1};}
        for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x)DitingGpuOracle::bytes(DitingGpuOracle::bars(x,y),static_cast<uint8_t *>(base)+y*actualStride+x*4,true,false);
        const IOReturn unlocked=IOSurfaceUnlock(surface,IOSurfaceLockOptions(0),nullptr);
        if(unlocked){CFRelease(surface);throw Failure{"iosurface-unlock",int(unlocked)};}
        MTLTextureDescriptor *desc=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:width height:height mipmapped:NO];
        desc.storageMode=MTLStorageModeShared;desc.usage=MTLTextureUsageShaderRead;
        auto target=[m.device newTextureWithDescriptor:desc iosurface:surface plane:0];
        if(!target){CFRelease(surface);throw Failure{"metal-iosurface-import",-1};}
        NSDictionary *record=m.readback(target,PixelFormat::BGRA,[](id<MTLCommandBuffer>){},DitingGpuOracle::bars);
        target=nil;CFRelease(surface);return record;
    });
    testcase(28,"metal-air-compute-host-acquire","synchronization",[&]{
        auto function=[m.library newFunctionWithName:@"diting_test_compute"];NSError *error=nil;
        auto pipeline=[m.device newComputePipelineStateWithFunction:function error:&error];
        if(!pipeline)throw Failure{"metal-air-compute-pipeline",int(error.code)};
        auto output=[m.device newBufferWithLength:4096*4 options:MTLResourceStorageModeShared];
        if(!output||!output.contents)throw Failure{"metal-compute-buffer",-1};memset(output.contents,0xcd,4096*4);
        auto command=[m.queue commandBuffer];auto encoder=[command computeCommandEncoder];
        if(!encoder)throw Failure{"metal-compute-encoder",-1};
        [encoder setComputePipelineState:pipeline];[encoder setBuffer:output offset:0 atIndex:0];
        [encoder dispatchThreadgroups:MTLSizeMake(64,1,1) threadsPerThreadgroup:MTLSizeMake(64,1,1)];[encoder endEncoding];metal_wait(command);
        unsigned errors=0;for(unsigned i=0;i<4096;++i)if(static_cast<uint32_t *>(output.contents)[i]!=DitingGpuOracle::compute(i))++errors;
        return @{@"pass":@(!errors),@"words":@4096,@"mismatches":@(errors)};
    });
    testcase(29,"metal-msaa4-bgra-resolve","colour",[&]{
        const Color color{1,0.125,0.5,1};auto target=m.texture(PixelFormat::BGRA,257,129),msaa=m.texture(PixelFormat::BGRA,257,129,4);
        auto pipeline=m.pipeline(PixelFormat::BGRA,false,4);
        return m.readback(target,PixelFormat::BGRA,[&](id<MTLCommandBuffer> command){auto encoder=m.begin(command,msaa,{},target);m.draw(encoder,pipeline,color);[encoder endEncoding];},[&](unsigned,unsigned){return color;});
    });
    const PixelFormat surfaceFormats[]={PixelFormat::BGRA,PixelFormat::SBGRA,PixelFormat::Half};
    const char *surfaceNames[]={"metal-iosurface-bgra-render-reuse","metal-iosurface-bgra-srgb-render-reuse","metal-iosurface-fp16-render-reuse"};
    for(unsigned i=0;i<3;++i)testcase(31+i,surfaceNames[i],"colour",[&]{
        const unsigned width=257,height=129,bpp=pixel_bytes(surfaceFormats[i]),stride=(width*bpp+255)&~255u;
        NSDictionary *properties=@{(__bridge id)kIOSurfaceWidth:@(width),(__bridge id)kIOSurfaceHeight:@(height),
            (__bridge id)kIOSurfaceBytesPerElement:@(bpp),(__bridge id)kIOSurfaceBytesPerRow:@(stride),
            (__bridge id)kIOSurfaceAllocSize:@(stride*height),(__bridge id)kIOSurfacePixelFormat:@(i==2?0x52476841u:0x42475241u)};
        struct OwnedSurface {
            IOSurfaceRef value;bool locked=false;
            OwnedSurface(CFDictionaryRef properties):value(IOSurfaceCreate(properties)){if(!value)throw Failure{"iosurface-create",-1};}
            void lock(){const IOReturn result=IOSurfaceLock(value,IOSurfaceLockOptions(0),nullptr);
                if(result)throw Failure{"iosurface-lock",int(result)};locked=true;}
            void unlock(){const IOReturn result=IOSurfaceUnlock(value,IOSurfaceLockOptions(0),nullptr);
                if(result)throw Failure{"iosurface-unlock",int(result)};locked=false;}
            ~OwnedSurface(){if(locked)IOSurfaceUnlock(value,IOSurfaceLockOptions(0),nullptr);CFRelease(value);}
        } surface((__bridge CFDictionaryRef)properties);
        surface.lock();auto base=static_cast<uint8_t *>(IOSurfaceGetBaseAddress(surface.value));
        const size_t row=IOSurfaceGetBytesPerRow(surface.value),bytes=IOSurfaceGetAllocSize(surface.value);
        if(!base||row<width*bpp||row>UINT32_MAX||bytes<row*height)throw Failure{"iosurface-layout",-1};
        memset(base,0xcd,bytes);
        const Color background{0,0,1,1};std::vector<Color> oracle(width*height,background);
        for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x) {
            uint8_t *pixel=base+y*row+x*bpp;
            if(i==2){const uint16_t blue[]={0,0,0x3c00,0x3c00};memcpy(pixel,blue,sizeof(blue));}
            else DitingGpuOracle::bytes(background,pixel,true,i==1);
        }
        surface.unlock();
        MTLTextureDescriptor *desc=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:metalformat(surfaceFormats[i]) width:width height:height mipmapped:NO];
        desc.storageMode=MTLStorageModeShared;desc.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;
        auto target=[m.device newTextureWithDescriptor:desc iosurface:surface.value plane:0];
        if(!target||target.iosurface!=surface.value)throw Failure{"metal-iosurface-render-import",-1};
        auto pipeline=m.pipeline(surfaceFormats[i]);RenderEvidence evidence;
        NSMutableArray *cycles=[NSMutableArray new];NSDictionary *last=nil;bool passed=true;
        for(unsigned cycle=0;cycle<4;++cycle) {
            const unsigned left=13+cycle*23,top=7+cycle*11;const Color overlay=cycle%2?Color{0,1,0,1}:Color{1,0,0,1};
            auto command=[m.queue commandBuffer];auto encoder=m.begin(command,target,{},nil,true);
            [encoder setScissorRect:MTLScissorRect{left,top,121,63}];m.draw(encoder,pipeline,overlay);[encoder endEncoding];metal_wait(command);
            for(unsigned y=top;y<top+63;++y)for(unsigned x=left;x<left+121;++x)oracle[y*width+x]=overlay;
            surface.lock();last=compare(base,bytes,width,height,unsigned(row),0,surfaceFormats[i],
                [&](unsigned x,unsigned y){return oracle[y*width+x];});surface.unlock();
            passed=passed&&[last[@"pass"] boolValue];[cycles addObject:last];
        }
        NSMutableDictionary *record=[last mutableCopy];record[@"pass"]=@(passed);record[@"reuse_cycles"]=@4;record[@"cycle_pixels"]=cycles;
        record[@"readback_source"]=@"IOSurface CPU mapping after GPU completion and IOSurfaceLock";
        return evidence.finish(record,false,false,4,true);
    });
}
static void metal_performance(Metal &m,unsigned width,unsigned height,unsigned frames,unsigned warmup) {
    const unsigned layers[]={0,8,32};const char *names[]={"metal-panel-clear","metal-panel-alpha-8","metal-panel-alpha-32"};
    for(unsigned scene=0;scene<3;++scene)testcase(43+scene,names[scene],"performance",[&]{
        auto target=m.texture(PixelFormat::BGRA,width,height);const uint64_t pipeline_start=now_ns();auto pipeline=m.pipeline(PixelFormat::BGRA,true);
        const uint64_t pipeline_us=(now_ns()-pipeline_start)/1000;
        NSMutableArray *samples=[NSMutableArray new];std::vector<double> total;uint64_t measured_start=0,finish=0;
        for(unsigned frame=0;frame<frames+warmup;++frame) {
            @autoreleasepool {
                check_time();const uint64_t start=now_ns();if(frame==warmup)measured_start=start;
                auto command=[m.queue commandBuffer];auto encoder=m.begin(command,target,{0.25,0.5,0.75,1});
                for(unsigned layer=0;layer<layers[scene];++layer)m.draw(encoder,pipeline,{float((frame+layer)%17)/16,float(layer%7)/6,0.25,0.125});
                [encoder endEncoding];const uint64_t encoded=now_ns();metal_wait(command);finish=now_ns();
                if(frame>=warmup){total.push_back(double(finish-start)/1000);[samples addObject:@[@((encoded-start)/1000),@((finish-encoded)/1000),@((finish-start)/1000)]];}
            }
        }
        const double budget=1000000.0/120,tail=percentile(total,0.95);
        return @{@"pass":@(tail<=budget),@"width":@(width),@"height":@(height),@"layers":@(layers[scene]),@"frames":@(frames),
            @"warmup":@(warmup),@"target_fps":@120,@"frame_budget_us":@(budget),@"pipeline_setup_us":@(pipeline_us),
            @"p50_us":@(percentile(total,0.5)),@"p95_us":@(tail),@"p99_us":@(percentile(total,0.99)),@"max_us":@(*std::max_element(total.begin(),total.end())),
            @"throughput_fps":@(double(frames)*1e9/double(finish-measured_start)),
            @"sample_columns":@[@"encode_us",@"commit_and_complete_us",@"total_us"],@"samples":samples,
            @"scope":@"actual Metal AIR pipeline offscreen; no physical presentation"};
    });
}
// The private HID ABI is shared with the installed 22H374 touch bridge. The
// virtual services identify themselves as automation and never read/write the
// physical touchscreen.
using HidEvent=CFTypeRef;using HidClient=CFTypeRef;using HidService=CFTypeRef;
struct HidCallbacks {
    uint64_t version;void (*notification)(void *,void *,HidService,uint32_t,CFDictionaryRef);
    Boolean (*setProperty)(void *,void *,HidService,CFStringRef,CFTypeRef);
    CFTypeRef (*copyProperty)(void *,void *,HidService,CFStringRef);
    HidEvent (*copyEvent)(void *,void *,HidService,uint32_t,HidEvent,uint32_t);
    int (*setOutputEvent)(void *,void *,HidService,HidEvent);
    CFTypeRef (*copyMatchingEvent)(void *,void *,HidService,CFDictionaryRef);
};
struct UI {
    struct Frame { uint32_t elapsed_us, read_us; uint64_t hash; };
    static_assert(sizeof(Frame)==16,"UI observation wire ABI");
    std::vector<Frame> frames;
    std::vector<uint32_t> inputs;
    NSMutableArray *motions=nil;
    UITimingPolicy timing;
    unsigned observer_policies=0;
    uint64_t frame_start_ns=0;
    HidClient client=nullptr;HidService touch=nullptr,keys=nullptr;dispatch_queue_t queue=nil;
    void *hid=nullptr,*sbs=nullptr;io_connect_t connection=0;mach_vm_address_t framebuffer=0;mach_vm_size_t framebuffer_bytes=0;
    unsigned width,height,stride,accepted=0,refused=0;bool touching=false,key_down=false;
    struct Properties {CFDictionaryRef values=nullptr;uint32_t opened=0;};
    Properties touch_properties{},key_properties{};
    HidClient (*create)(CFAllocatorRef,int,CFDictionaryRef)=nullptr;
    void (*schedule)(HidClient,dispatch_queue_t)=nullptr;
    void (*unschedule)(HidClient,dispatch_queue_t)=nullptr;
    HidService (*createService)(HidClient,CFDictionaryRef,const HidCallbacks *,void *,void *)=nullptr;
    Boolean (*remove)(HidService)=nullptr;Boolean (*send)(HidService,HidEvent)=nullptr;
    HidEvent (*hand)(CFAllocatorRef,uint64_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,double,double,double,double,double,double,Boolean,Boolean,uint32_t)=nullptr;
    HidEvent (*finger)(CFAllocatorRef,uint64_t,uint32_t,uint32_t,uint32_t,double,double,double,double,double,Boolean,Boolean,uint32_t)=nullptr;
    HidEvent (*keyboard)(CFAllocatorRef,uint64_t,uint32_t,uint32_t,Boolean,uint32_t)=nullptr;
    void (*append)(HidEvent,HidEvent,uint32_t)=nullptr;void (*integer)(HidEvent,uint32_t,CFIndex)=nullptr;
    int (*launch)(CFStringRef,Boolean)=nullptr;CFStringRef (*frontmost)()=nullptr;
    static Boolean set(void *,void *context,HidService,CFStringRef key,CFTypeRef value) {
        auto *properties=static_cast<Properties *>(context);
        if(key&&value&&CFEqual(key,CFSTR("DeviceOpenedByEventSystem"))&&CFGetTypeID(value)==CFBooleanGetTypeID())
            __atomic_store_n(&properties->opened,CFBooleanGetValue(static_cast<CFBooleanRef>(value)),__ATOMIC_RELEASE);
        return true;
    }
    static void notification(void *,void *,HidService,uint32_t,CFDictionaryRef) {}
    static CFTypeRef copy(void *,void *context,HidService,CFStringRef key) {
        auto *properties=static_cast<Properties *>(context);CFTypeRef value=key?CFDictionaryGetValue(properties->values,key):nullptr;
        return value?CFRetain(value):nullptr;
    }
    HidService service(Properties &properties,bool screen) {
        NSDictionary *pair=@{@"DeviceUsagePage":@(screen?13:12),@"DeviceUsage":@(screen?4:1)};
        NSDictionary *values=@{@"PrimaryUsagePage":@(screen?13:12),@"PrimaryUsage":@(screen?4:1),
            @"DeviceUsagePage":@(screen?13:12),@"DeviceUsage":@(screen?4:1),@"DeviceUsagePairs":@[pair],
            @"Built-In":@YES,@"DisplayIntegrated":@(screen),@"Transport":@"Virtual",@"Manufacturer":@"Diting",
            @"Product":screen?@"Diting GPU automation digitizer":@"Diting GPU automation menu",@"VendorID":@65535,@"ProductID":@197};
        properties.values=(CFDictionaryRef)CFRetain((__bridge CFDictionaryRef)values);
        static const HidCallbacks callbacks{2,&notification,&set,&copy,nullptr,nullptr,nullptr};
        HidService result=createService(client,properties.values,&callbacks,nullptr,&properties);
        if(!result)throw Failure{"hid-virtual-service",-1};
        const uint64_t start=now_ns();while(!__atomic_load_n(&properties.opened,__ATOMIC_ACQUIRE)&&now_ns()-start<3000000000ull)usleep(100000);
        if(!__atomic_load_n(&properties.opened,__ATOMIC_ACQUIRE)){remove(result);CFRelease(result);throw Failure{"hid-event-server-did-not-open-service",-1};}
        return result;
    }
    UI(unsigned w,unsigned h):width(w),height(h),stride(w*4) {
        @try { try { initialize(); } catch (...) { release(); throw; } }
        @catch(NSException *error) { release(); @throw error; }
    }
    void initialize() {
        timing.acquire();
        hid=dlopen("/System/Library/Frameworks/IOKit.framework/IOKit",RTLD_NOW|RTLD_LOCAL);
        if(!hid)throw Failure{"hid-library",-1};
#define HID_LOAD(member,symbol) member=reinterpret_cast<decltype(member)>(dlsym(hid,symbol));if(!member)throw Failure{symbol,-1};
        HID_LOAD(create,"IOHIDEventSystemClientCreateWithType") HID_LOAD(schedule,"IOHIDEventSystemClientScheduleWithDispatchQueue")
        HID_LOAD(createService,"IOHIDVirtualServiceClientCreateWithCallbacks") HID_LOAD(remove,"IOHIDVirtualServiceClientRemove")
        HID_LOAD(send,"IOHIDVirtualServiceClientDispatchEvent") HID_LOAD(hand,"IOHIDEventCreateDigitizerEvent")
        HID_LOAD(finger,"IOHIDEventCreateDigitizerFingerEvent") HID_LOAD(keyboard,"IOHIDEventCreateKeyboardEvent")
        HID_LOAD(append,"IOHIDEventAppendEvent") HID_LOAD(integer,"IOHIDEventSetIntegerValue")
#undef HID_LOAD
        unschedule=reinterpret_cast<decltype(unschedule)>(dlsym(hid,"IOHIDEventSystemClientUnscheduleFromDispatchQueue"));
        queue=dispatch_queue_create("diting.gpu.automation.hid",DISPATCH_QUEUE_SERIAL);
        client=create(nullptr,4,nullptr);if(!client)client=create(nullptr,3,nullptr);
        if(!client)throw Failure{"hid-client",-1};schedule(client,queue);touch=service(touch_properties,true);keys=service(key_properties,false);
        io_service_t gpu=IOServiceGetMatchingService(kIOMainPortDefault,IOServiceMatching("NativeAdrenoGpuPublished"));
        if(!gpu)throw Failure{"ui-gpu-service",-1};
        kern_return_t error=IOServiceOpen(gpu,mach_task_self(),DitingGpuTestOpenType,&connection);IOObjectRelease(gpu);
        if(error)throw Failure{"ui-readonly-display-open",int(error)};
        DitingScanoutModeInfo mode{};size_t mode_bytes=sizeof(mode);
        error=IOConnectCallStructMethod(connection,DitingGpuTestReadMode,nullptr,0,&mode,&mode_bytes);
        if(error||mode_bytes!=sizeof(mode)||mode.version!=DitingScanoutVersion||mode.format!=DitingScanoutBgra||
            mode.width!=width||mode.height!=height||mode.stride<uint64_t(width)*4||mode.stride%4||
            mode.stride>UINT32_MAX||mode.bytes<uint64_t(mode.stride)*height||mode.bytes>64*1024*1024)
            throw Failure{"ui-actual-display-mode",int(error)};
        stride=uint32_t(mode.stride);
        error=IOConnectMapMemory64(connection,0x5343,mach_task_self(),&framebuffer,&framebuffer_bytes,kIOMapAnywhere|kIOMapReadOnly);
        if(error||!framebuffer||framebuffer_bytes!=mode.bytes)throw Failure{"ui-readonly-framebuffer",int(error)};
        sbs=dlopen("/System/Library/PrivateFrameworks/SpringBoardServices.framework/SpringBoardServices",RTLD_NOW|RTLD_LOCAL);
        if(sbs){launch=reinterpret_cast<decltype(launch)>(dlsym(sbs,"SBSLaunchApplicationWithIdentifier"));frontmost=reinterpret_cast<decltype(frontmost)>(dlsym(sbs,"SBSCopyFrontmostApplicationDisplayIdentifier"));}
    }
    uint64_t snapshot() {
        // A thumbnail of the actual pre-DPU framebuffer. It detects changed
        // content; it does not claim an atomic capture or physical panel latch.
        uint64_t hash=1469598103934665603ull;const volatile uint32_t *pixels=reinterpret_cast<const volatile uint32_t *>(framebuffer);
        for(unsigned y=0;y<32;++y)for(unsigned x=0;x<32;++x) {
            const uint32_t pixel=pixels[size_t((y*(height-1))/31)*(stride/4)+(x*(width-1))/31];
            for(unsigned c=0;c<4;++c){hash^=(pixel>>(c*8))&255;hash*=1099511628211ull;}
        }
        return hash;
    }
    uint64_t observe() {
        check_time();
        const uint64_t before=now_ns(),hash=snapshot(),after=now_ns();
        if(frame_start_ns) {
            if(frames.size()>=1536 || (before-frame_start_ns)/1000>UINT32_MAX || (after-before)/1000>UINT32_MAX)
                throw Failure{"ui-observation-bound",EOVERFLOW};
            frames.push_back({uint32_t((before-frame_start_ns)/1000),uint32_t((after-before)/1000),hash});
        }
        return hash;
    }
    void begin_timeline() {
        frames.clear();inputs.clear();observer_policies=0;
        frames.reserve(1536);inputs.reserve(8*43);
        motions=[NSMutableArray new];frame_start_ns=now_ns();observe();
    }
    void wait(unsigned duration_us) {
        const uint64_t finish=now_ns()+uint64_t(duration_us)*1000;
        for(;;) {
            check_time();const uint64_t now=now_ns();
            if(now>=finish)break;
            const uint64_t next=std::min(finish,now+8333333);
            wait_until_ns(next);
            if(frame_start_ns)observe();
        }
    }
    void dispatch(HidService service,HidEvent event) {
        if(!event)throw Failure{"hid-event-create",-1};
        integer(event,4,1);const bool ok=send(service,event);CFRelease(event);
        if(ok)++accepted;else ++refused;
    }
    void home(bool twice=false) {
        for(unsigned press=0;press<(twice?2u:1u);++press) {
            key_down=true;
            dispatch(keys,keyboard(nullptr,mach_absolute_time(),12,0x40,true,0));wait(60000);
            dispatch(keys,keyboard(nullptr,mach_absolute_time(),12,0x40,false,0));if(twice)wait(80000);
            key_down=false;
        }
    }
    void contact(double x,double y,bool down,bool moving,bool edge) {
        const uint64_t time=mach_absolute_time();const uint32_t change=moving?4:3;
        HidEvent parent=hand(nullptr,time,3,0,0,change&~1u,0,x,y,0,0,0,0,down,down,0);
        HidEvent child=finger(nullptr,time,1,2,change|(edge?0x800:0),x,y,0,down?1:0,0,down,down,0);
        if(!parent||!child){if(parent)CFRelease(parent);if(child)CFRelease(child);throw Failure{"hid-digitizer-event",-1};}
        integer(parent,0xb0019,1);integer(child,0xb0019,1);append(parent,child,0);CFRelease(child);touching=down;dispatch(touch,parent);
    }
    void swipe(double x0,double y0,double x1,double y1,NSMutableArray *hashes) {
        struct Observer {
            UI *ui;
            std::array<Frame,43> samples{};
            uint32_t ready=0,stop=0;
            unsigned count=0;
            int error=0;
            bool policy_pass=false;
            static void *run(void *context) {
                auto &self=*static_cast<Observer *>(context);
                try {
                    UITimingPolicy policy;policy.acquire();
                    const uint64_t origin=now_ns();uint64_t previous=origin;
                    for(unsigned i=0;i<43&&!__atomic_load_n(&self.stop,__ATOMIC_ACQUIRE);++i) {
                        if(i)wait_until_ns(DitingGpuPacing::next(origin,i,previous));
                        const uint64_t before=now_ns(),hash=self.ui->snapshot(),after=now_ns();
                        if((before-self.ui->frame_start_ns)/1000>UINT32_MAX||(after-before)/1000>UINT32_MAX)
                            throw Failure{"ui-observation-bound",EOVERFLOW};
                        self.samples[self.count++]={uint32_t((before-self.ui->frame_start_ns)/1000),uint32_t((after-before)/1000),hash};
                        previous=before;
                        if(!i)__atomic_store_n(&self.ready,1,__ATOMIC_RELEASE);
                    }
                    policy.verify();self.policy_pass=true;
                } catch(const Failure &error) {self.error=error.code?error.code:EIO;}
                  catch(...) {self.error=EIO;}
                __atomic_store_n(&self.ready,1,__ATOMIC_RELEASE);
                return nullptr;
            }
        } observer{this};
        pthread_t thread{};
        const int created=pthread_create(&thread,nullptr,Observer::run,&observer);
        if(created)throw Failure{"ui-observer-thread",created};
        try {
            const uint64_t ready_deadline=now_ns()+1000000000ull;
            while(!__atomic_load_n(&observer.ready,__ATOMIC_ACQUIRE)) {
                if(now_ns()>=ready_deadline)throw Failure{"ui-observer-readiness",ETIMEDOUT};
                wait_until_ns(now_ns()+1000000);
            }
            const bool edge=x0<0.05||x0>0.95||y0<0.04||y0>0.95;
            const uint64_t start=now_ns();uint64_t previous=start;
            inputs.push_back(uint32_t((start-frame_start_ns)/1000));contact(x0,y0,true,false,edge);
            for(unsigned i=1;i<=42;++i) {
                wait_until_ns(DitingGpuPacing::next(start,i,previous));
                previous=now_ns();inputs.push_back(uint32_t((previous-frame_start_ns)/1000));
                const double t=double(i)/42;contact(x0+(x1-x0)*t,y0+(y1-y0)*t,true,true,edge);
            }
            contact(x1,y1,false,false,edge);
        } catch(...) {
            __atomic_store_n(&observer.stop,1,__ATOMIC_RELEASE);pthread_join(thread,nullptr);throw;
        }
        const int joined=pthread_join(thread,nullptr);
        if(joined||observer.error||observer.count!=43||!observer.policy_pass)
            throw Failure{"ui-independent-observer",joined?joined:observer.error?observer.error:EIO};
        timing.verify();++observer_policies;
        const unsigned first=unsigned(frames.size());
        if(first+observer.count>1536)throw Failure{"ui-observation-bound",EOVERFLOW};
        frames.insert(frames.end(),observer.samples.begin(),observer.samples.end());
        for(unsigned i=7;i<=42;i+=7)[hashes addObject:@(observer.samples[i].hash)];
        [motions addObject:@[@(first),@(frames.size()-1)]];
        wait(350000);[hashes addObject:@(snapshot())];
    }
    NSString *foreground() {
        if(!frontmost)throw Failure{"sbs-foreground-query-unavailable",-1};
        CFStringRef value=frontmost();if(!value)return @"";
        NSString *result=[(__bridge NSString *)value copy];CFRelease(value);return result;
    }
    void release() {
        if(touching&&touch) { @try { try{contact(0.5,0.5,false,false,false);}catch(...){} } @catch(NSException *) {} }
        if(key_down&&keys) { @try { try{dispatch(keys,keyboard(nullptr,mach_absolute_time(),12,0x40,false,0));}catch(...){} } @catch(NSException *) {} }
        if(framebuffer)IOConnectUnmapMemory64(connection,0x5343,mach_task_self(),framebuffer);
        if(connection)IOServiceClose(connection);
        if(touch){remove(touch);CFRelease(touch);}if(keys){remove(keys);CFRelease(keys);}
        if(client&&queue&&unschedule)unschedule(client,queue);
        if(client)CFRelease(client);if(queue)dispatch_sync(queue,^{});
        if(touch_properties.values)CFRelease(touch_properties.values);if(key_properties.values)CFRelease(key_properties.values);
        // IOKit/SBS code must remain loaded while queued service callbacks drain.
        framebuffer=0;connection=0;client=touch=keys=nullptr;queue=nil;
        touch_properties.values=key_properties.values=nullptr;
    }
    ~UI() { release(); }
};
static NSDictionary *ui_result(UI &ui,NSMutableArray *hashes,unsigned before_accepted,unsigned before_refused,unsigned minimum) {
    NSSet *distinct=[NSSet setWithArray:hashes];
    NSData *frames=[NSData dataWithBytes:ui.frames.data() length:ui.frames.size()*sizeof(UI::Frame)];
    return @{@"pass":@(ui.refused==before_refused && distinct.count>=minimum && ui.accepted>before_accepted),
        @"accepted_events":@(ui.accepted-before_accepted),@"refused_events":@(ui.refused-before_refused),
        @"content_hashes":hashes,@"distinct_content":@(distinct.count),@"minimum_distinct_content":@(minimum),
        @"frame_sample_start_ns":@(ui.frame_start_ns),@"frame_sample_hz":@120,
        @"frame_sample_format":@"le-u32-elapsed-us,u32-read-us,u64-hash",@"frame_sample_count":@(ui.frames.size()),
        @"frame_samples_base64":[frames base64EncodedStringWithOptions:0],@"motion_sample_ranges":ui.motions,
        @"observation_schema":@2,@"observer_independent":@YES,@"observer_policy_passes":@(ui.observer_policies),
        @"input_pacing":@"absolute-monotonic-no-catchup",@"input_sample_count":@(ui.inputs.size()),
        @"input_samples_base64":[[NSData dataWithBytes:ui.inputs.data() length:ui.inputs.size()*sizeof(uint32_t)] base64EncodedStringWithOptions:0],
        @"motion_steps":@42,@"motion_warmup_steps":@6,
        @"input":@"acknowledged virtual HID",@"observation":@"32x32 readonly actual framebuffer; pre-DPU, non-atomic"};
}
static void ui_qualification(UI &ui) {
    testcase(60,"ui-home-continuous-swipes","ui",[&]{
        ui.begin_timeline();unsigned accepted=ui.accepted,refused=ui.refused;ui.home();ui.wait(1000000);
        NSMutableArray *hashes=[NSMutableArray arrayWithObject:@(ui.snapshot())];
        for(unsigned i=0;i<8;++i)ui.swipe(i%2?0.15:0.85,0.55,i%2?0.85:0.15,0.55,hashes);
        return ui_result(ui,hashes,accepted,refused,3);
    });
    testcase(61,"ui-preferences-open-scroll-close","ui",[&]{
        if(!ui.launch||!ui.frontmost)throw Failure{"springboard-application-control-unavailable",-1};
        ui.begin_timeline();
        unsigned accepted=ui.accepted,refused=ui.refused;NSMutableArray *hashes=[NSMutableArray arrayWithObject:@(ui.snapshot())];
        NSMutableArray *launches=[NSMutableArray new];bool verified=true;
        for(unsigned i=0;i<2;++i) {
            const int result=ui.launch(CFSTR("com.apple.Preferences"),false);[launches addObject:@(result)];
            const uint64_t start=now_ns();NSString *front=ui.foreground();
            while(![front isEqualToString:@"com.apple.Preferences"]&&now_ns()-start<3000000000ull){ui.wait(100000);front=ui.foreground();}
            if(result||![front isEqualToString:@"com.apple.Preferences"])
                throw Failure{"preferences-foreground-prerequisite",result?result:ENXIO};
            verified=verified&&!result&&[front isEqualToString:@"com.apple.Preferences"];
            ui.wait(500000);
            [hashes addObject:@(ui.snapshot())];ui.swipe(0.65,0.8,0.65,0.3,hashes);ui.home();ui.wait(700000);
            verified=verified&&![ui.foreground() isEqualToString:@"com.apple.Preferences"];[hashes addObject:@(ui.snapshot())];
        }
        NSMutableDictionary *record=[ui_result(ui,hashes,accepted,refused,3) mutableCopy];record[@"launch_results"]=launches;
        record[@"foreground_verified"]=@(verified);record[@"pass"]=@([record[@"pass"] boolValue]&&verified);return record;
    });
    testcase(62,"ui-app-switcher","ui",[&]{
        ui.begin_timeline();
        unsigned accepted=ui.accepted,refused=ui.refused;NSMutableArray *hashes=[NSMutableArray arrayWithObject:@(ui.snapshot())];
        ui.home(true);for(unsigned i=0;i<10;++i){ui.wait(100000);[hashes addObject:@(ui.snapshot())];}
        ui.swipe(0.85,0.5,0.15,0.5,hashes);ui.home();ui.wait(500000);[hashes addObject:@(ui.snapshot())];
        return ui_result(ui,hashes,accepted,refused,2);
    });
}
} // namespace

extern "C" int diting_gpu_test_worker(int argc,char **argv) {
    if(argc!=10||geteuid())return 64;
    uint32_t values[8]{};
    for(unsigned i=0;i<8;++i) {
        char *end=nullptr;errno=0;unsigned long value=strtoul(argv[i+2],&end,10);
        if(errno||!argv[i+2][0]||*end||value>UINT32_MAX)return 64;values[i]=uint32_t(value);
    }
    const uint32_t profile=values[0],policy=values[1],job=values[2],width=values[3],height=values[4],
        frames=values[5],warmup=values[6],timeout=values[7];
    if(profile<1||profile>4||policy>2||!job||width!=1220||height!=2712||frames!=240||warmup!=24||timeout!=120000)return 64;
    // Keep diagnostics NDJSON separate from stdout emitted by native loaders.
    int output=dup(STDOUT_FILENO);if(output<0)return 74;
    results=fdopen(output,"w");if(!results)return 74;dup2(STDERR_FILENO,STDOUT_FILENO);
    job_id=job;deadline_ns=now_ns()+uint64_t(timeout)*1000000;alarm(timeout/1000);
    signal(SIGTERM,cancel_worker);signal(SIGINT,cancel_worker);
    const char *names[]={"auto","sysmem","gmem"},*debug[]={"nocb","sysmem,nocb","gmem,nocb"};
    setenv("TU_DEBUG",debug[policy],1);setenv("DITING_GPU_TEST_POLICY",names[policy],1);
    setenv("TU_DARWIN_SERVICE","NativeAdrenoGpuPublished",1);setenv("TU_DARWIN_OPEN_TYPE","0x4147",1);
    setenv("TU_DARWIN_BO_CACHE_MIB","0",1);setenv("METAL2VULKAN_SPARE_IMAGE_MIB","0",1);
    setenv("NATIVE_METAL_SINGLE_SAMPLE","0",1);
    @autoreleasepool {
        const uint64_t start=now_ns();
        emit(@{@"kind":@"manifest",@"schema":@1,@"job":@(job),@"profile":@(profile),@"policy":@(policy),
            @"target_fps":@120,@"width":@(width),@"height":@(height),@"frames":@(frames),@"warmup":@(warmup),
            @"start_ns":@(start),@"pid":@(getpid()),@"host_supplied_shader":@NO});
        @try {
            try {
                if(profile!=DitingGpuTestUI) {
                    {
                        std::unique_ptr<Vulkan> vk;
                        testcase(14,"vk-native-device","prerequisite",[&]{vk=std::make_unique<Vulkan>();return @{@"pass":@YES};});
                        if(vk){if(profile!=DitingGpuTestPerformance)vulkan_qualification(*vk);
                            if(profile!=DitingGpuTestQualification)vulkan_performance(*vk,width,height,frames,warmup);}
                    }
                    {
                        std::unique_ptr<Metal> metal;
                        testcase(30,"metal-native-device","prerequisite",[&]{metal=std::make_unique<Metal>();return @{@"pass":@YES};});
                        if(metal){if(profile!=DitingGpuTestPerformance)metal_qualification(*metal);
                            if(profile!=DitingGpuTestQualification)metal_performance(*metal,width,height,frames,warmup);}
                    }
                }
                if(profile==DitingGpuTestUI || (profile==DitingGpuTestAll && policy==DitingGpuTestAuto)) {
                    std::unique_ptr<UI> ui;
                    testcase(63,"ui-automation-ready","prerequisite",[&]{
                        ui=std::make_unique<UI>(width,height);
                        return @{@"pass":@YES,@"width":@(ui->width),@"height":@(ui->height),
                            @"stride":@(ui->stride),@"framebuffer_bytes":@(ui->framebuffer_bytes),@"readonly":@YES};
                    });
                    if(ui)ui_qualification(*ui);
                }
            } catch(const Failure &error) {
                ++failures;emit(@{@"kind":@"fatal",@"stage":@(error.stage.c_str()),@"code":@(error.code),@"job":@(job)});
            } catch(const std::exception &error) {
                ++failures;emit(@{@"kind":@"fatal",@"stage":@"worker-exception",@"detail":@(error.what()),@"job":@(job)});
            }
        } @catch(NSException *error) {
            ++failures;emit(@{@"kind":@"fatal",@"stage":@"objc-exception",@"detail":error.reason?:error.name,@"job":@(job)});
        }
        emit(@{@"kind":@"summary",@"schema":@1,@"job":@(job),@"cases":@(case_count),@"failures":@(failures),
            @"pass":@(failures==0&&case_count>0),@"end_ns":@(now_ns()),@"hardware_performance_pass":@NO,
            @"physical_present":@"UNQUALIFIED",@"scope":@"automated GPU pixels/workloads and UI content; no physical composed-frame proof"});
    }
    fclose(results);return failures||!case_count?1:0;
}
