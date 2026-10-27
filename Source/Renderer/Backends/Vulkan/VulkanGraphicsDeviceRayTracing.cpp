// VulkanGraphicsDeviceRayTracing.cpp
// Vulkan implementation of RHI ray tracing device methods.
// Requires VK_KHR_acceleration_structure + VK_KHR_buffer_device_address +
// VK_EXT_descriptor_indexing + VK_KHR_deferred_host_operations +
// VK_KHR_ray_tracing_pipeline (all detected and enabled at device creation;
// see CreateDevice() in VulkanGraphicsDevice.cpp).
//
// The RHI ray-tracing pipeline is fed SPIR-V bytecode via
// RhiRayTracingPipelineDesc::spirvBytecode (the DX12 backend uses the parallel
// DXIL field). CreateRhiRayTracingPipeline builds a VkPipeline with an internal
// fixed descriptor set layout (binding 0 = acceleration structure, binding 1 =
// storage image), CreateRhiShaderBindingTable packs the SBT, and DispatchRays
// records vkCmdTraceRaysKHR. RunRayTracingSmokeTest exercises the full path.
//
// NOTE: DxrRayTracer (the DX12 render-graph ray tracer) does not run on Vulkan
// (supportsFeatureRenderPasses=false); wiring these primitives into a Vulkan
// render pass — including a bindless resource model matching the DXR shader — is
// a separate, larger effort.

#include "Renderer/Backends/Vulkan/VulkanGraphicsDevice.h"
#include "Renderer/Backends/Vulkan/VulkanRayTracingUtils.h"

#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "Foundation/Tools/DebugOutput.h"

#if RHI_VULKAN
#include <filesystem>

#if defined(_WIN32)
#include <Windows.h>
#include <wrl/client.h>
#include <dxcapi.h>
#endif

#include "Renderer/Resources/ShaderCompilationService.h"
#endif

namespace SasamiEngine::Render
{
#if RHI_VULKAN

    // -------------------------------------------------------------------------
    // CreateRhiRayTracingPipeline
    // Builds a VkPipeline from the SPIR-V library (RhiRayTracingPipelineDesc::
    // spirvBytecode). Shader stages are derived per export from the SPIR-V entry
    // points; an internal fixed descriptor set layout (binding 0 = acceleration
    // structure, binding 1 = storage image) provides the pipeline layout.
    // -------------------------------------------------------------------------
    RhiRayTracingPipelineHandle VulkanGraphicsDevice::CreateRhiRayTracingPipeline(
        const RhiRayTracingPipelineDesc& desc)
    {
        if (!m_hasVkKhrRayTracingPipeline || !m_pfnCreateRtPipeline) {
            DebugLog("VulkanGraphicsDevice::CreateRhiRayTracingPipeline: "
                     "VK_KHR_ray_tracing_pipeline not available.\n");
            return {};
        }
        if (!desc.spirvBytecode || desc.spirvBytecodeSizeInBytes == 0 ||
            (desc.spirvBytecodeSizeInBytes % 4) != 0) {
            DebugLog("VulkanGraphicsDevice::CreateRhiRayTracingPipeline: "
                     "valid SPIR-V bytecode required (set spirvBytecode/size).\n");
            return {};
        }
        if (!desc.shaderGroups || desc.shaderGroupCount == 0) {
            DebugLog("VulkanGraphicsDevice::CreateRhiRayTracingPipeline: no shader groups.\n");
            return {};
        }

        VkShaderModuleCreateInfo smi{};
        smi.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smi.codeSize = desc.spirvBytecodeSizeInBytes;
        smi.pCode    = reinterpret_cast<const uint32_t*>(desc.spirvBytecode);
        VkShaderModule shaderModule = VK_NULL_HANDLE;
        if (vkCreateShaderModule(m_device, &smi, nullptr, &shaderModule) != VK_SUCCESS) {
            DebugLog("VulkanGraphicsDevice::CreateRhiRayTracingPipeline: vkCreateShaderModule failed.\n");
            return {};
        }

        std::unordered_map<std::string, VkShaderStageFlagBits> entryStages;
        CollectSpirvEntryStages(smi.pCode, desc.spirvBytecodeSizeInBytes / 4, entryStages);

        std::vector<VkPipelineShaderStageCreateInfo>       stages;
        std::vector<VkRayTracingShaderGroupCreateInfoKHR>  groups;
        std::unordered_map<std::string, uint32_t>          stageIndexByName;
        std::vector<std::string>                           stageNames;  // stable storage for pName
        stageNames.reserve(desc.shaderGroupCount + 1);
        stages.reserve(desc.shaderGroupCount + 1);
        groups.reserve(desc.shaderGroupCount);

        bool resolveOk = true;
        auto addStage = [&](const char* exportName) -> uint32_t {
            if (!exportName) return VK_SHADER_UNUSED_KHR;
            const std::string name(exportName);
            const auto existing = stageIndexByName.find(name);
            if (existing != stageIndexByName.end()) return existing->second;
            const auto se = entryStages.find(name);
            if (se == entryStages.end()) {
                DebugLog("VulkanGraphicsDevice::CreateRhiRayTracingPipeline: "
                         "shader export not found among SPIR-V entry points: ");
                DebugLog(name.c_str());
                DebugLog("\n");
                resolveOk = false;
                return VK_SHADER_UNUSED_KHR;
            }
            const uint32_t idx = static_cast<uint32_t>(stages.size());
            stageNames.push_back(name);
            VkPipelineShaderStageCreateInfo st{};
            st.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            st.stage  = se->second;
            st.module = shaderModule;
            st.pName  = stageNames.back().c_str();
            stages.push_back(st);
            stageIndexByName[name] = idx;
            return idx;
        };

        for (uint32_t g = 0; g < desc.shaderGroupCount; ++g) {
            const RhiRayTracingShaderGroupDesc& gd = desc.shaderGroups[g];
            VkRayTracingShaderGroupCreateInfoKHR grp{};
            grp.sType              = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
            grp.generalShader      = VK_SHADER_UNUSED_KHR;
            grp.closestHitShader   = VK_SHADER_UNUSED_KHR;
            grp.anyHitShader       = VK_SHADER_UNUSED_KHR;
            grp.intersectionShader = VK_SHADER_UNUSED_KHR;

            if (gd.type == RhiRayTracingShaderGroupDesc::Type::General) {
                grp.type          = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
                grp.generalShader = addStage(gd.exportName);
            } else {
                grp.type            = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
                grp.closestHitShader = addStage(gd.closestHitExport);
            }
            groups.push_back(grp);
        }

        if (!resolveOk || stages.empty()) {
            vkDestroyShaderModule(m_device, shaderModule, nullptr);
            DebugLog("VulkanGraphicsDevice::CreateRhiRayTracingPipeline: failed to resolve stages.\n");
            return {};
        }

        // Internal fixed descriptor set layout: AS (0) + storage image (1).
        VkDescriptorSetLayoutBinding binds[2]{};
        binds[0].binding         = 0;
        binds[0].descriptorType  = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        binds[0].descriptorCount = 1;
        binds[0].stageFlags      = VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                                   VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                                   VK_SHADER_STAGE_MISS_BIT_KHR;
        binds[1].binding         = 1;
        binds[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        binds[1].descriptorCount = 1;
        binds[1].stageFlags      = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

        VkDescriptorSetLayoutCreateInfo dsli{};
        dsli.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dsli.bindingCount = 2;
        dsli.pBindings    = binds;
        VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
        if (vkCreateDescriptorSetLayout(m_device, &dsli, nullptr, &dsl) != VK_SUCCESS) {
            vkDestroyShaderModule(m_device, shaderModule, nullptr);
            return {};
        }

        VkPipelineLayoutCreateInfo pli{};
        pli.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount = 1;
        pli.pSetLayouts    = &dsl;
        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
        if (vkCreatePipelineLayout(m_device, &pli, nullptr, &pipelineLayout) != VK_SUCCESS) {
            vkDestroyDescriptorSetLayout(m_device, dsl, nullptr);
            vkDestroyShaderModule(m_device, shaderModule, nullptr);
            return {};
        }

        uint32_t recursion = desc.maxRecursionDepth;
        if (m_rtMaxRayRecursionDepth != 0 && recursion > m_rtMaxRayRecursionDepth)
            recursion = m_rtMaxRayRecursionDepth;

        VkRayTracingPipelineCreateInfoKHR rtci{};
        rtci.sType                        = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR;
        rtci.stageCount                   = static_cast<uint32_t>(stages.size());
        rtci.pStages                      = stages.data();
        rtci.groupCount                   = static_cast<uint32_t>(groups.size());
        rtci.pGroups                      = groups.data();
        rtci.maxPipelineRayRecursionDepth = recursion;
        rtci.layout                       = pipelineLayout;

        VkPipeline pipeline = VK_NULL_HANDLE;
        const VkResult pr = m_pfnCreateRtPipeline(m_device, VK_NULL_HANDLE, VK_NULL_HANDLE,
                                                  1, &rtci, nullptr, &pipeline);
        if (pr != VK_SUCCESS || pipeline == VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(m_device, pipelineLayout, nullptr);
            vkDestroyDescriptorSetLayout(m_device, dsl, nullptr);
            vkDestroyShaderModule(m_device, shaderModule, nullptr);
            DebugLog("VulkanGraphicsDevice::CreateRhiRayTracingPipeline: vkCreateRayTracingPipelinesKHR failed.\n");
            return {};
        }

        VulkanRtPipeline rec{};
        rec.pipeline       = pipeline;
        rec.pipelineLayout = pipelineLayout;
        rec.descSetLayout  = dsl;
        rec.module         = shaderModule;
        rec.groupCount     = static_cast<uint32_t>(groups.size());

        const uint64_t id = m_nextRhiPipelineHandle++;
        m_rtPipelines.emplace(id, rec);
        return RhiRayTracingPipelineHandle{ id };
    }

    // -------------------------------------------------------------------------
    // CreateRhiShaderBindingTable
    // Fetches shader group handles and packs raygen / miss / hit regions into a
    // host-visible SBT buffer, honoring the device's handle size and alignments.
    // -------------------------------------------------------------------------
    RhiShaderBindingTableHandle VulkanGraphicsDevice::CreateRhiShaderBindingTable(
        const RhiShaderBindingTableDesc& desc)
    {
        if (!m_pfnGetRtHandles || m_rtShaderGroupHandleSize == 0) {
            DebugLog("VulkanGraphicsDevice::CreateRhiShaderBindingTable: RT pipeline properties unavailable.\n");
            return {};
        }
        const auto pit = m_rtPipelines.find(desc.pipeline.id);
        if (pit == m_rtPipelines.end()) {
            DebugLog("VulkanGraphicsDevice::CreateRhiShaderBindingTable: pipeline not found.\n");
            return {};
        }
        const VulkanRtPipeline& pipe = pit->second;
        const uint32_t     groupCount   = pipe.groupCount;
        const uint32_t     handleSize   = m_rtShaderGroupHandleSize;
        const VkDeviceSize handleAligned = AlignUp(handleSize, m_rtShaderGroupHandleAlignment);
        const VkDeviceSize baseAlign     = m_rtShaderGroupBaseAlignment;

        std::vector<uint8_t> handles(static_cast<size_t>(groupCount) * handleSize);
        if (m_pfnGetRtHandles(m_device, pipe.pipeline, 0, groupCount,
                              handles.size(), handles.data()) != VK_SUCCESS) {
            DebugLog("VulkanGraphicsDevice::CreateRhiShaderBindingTable: vkGetRayTracingShaderGroupHandlesKHR failed.\n");
            return {};
        }
        auto handlePtr = [&](uint32_t groupIndex) -> const uint8_t* {
            if (groupIndex >= groupCount) return nullptr;
            return handles.data() + static_cast<size_t>(groupIndex) * handleSize;
        };

        const uint32_t missCount = desc.missGroupCount;
        const uint32_t hitCount  = desc.hitGroupCount;

        const VkDeviceSize raygenSize = AlignUp(handleAligned, baseAlign);
        const VkDeviceSize missSize   = missCount ? AlignUp(missCount * handleAligned, baseAlign) : 0;
        const VkDeviceSize hitSize    = hitCount  ? AlignUp(hitCount  * handleAligned, baseAlign) : 0;

        const VkDeviceSize raygenOffset = 0;
        const VkDeviceSize missOffset   = raygenSize;
        const VkDeviceSize hitOffset    = missOffset + missSize;
        const VkDeviceSize sbtSize      = hitOffset + hitSize;

        VkRtBuffer sbtBuf = AllocRtBufferHostVisible(m_device, m_physicalDevice, sbtSize,
                                                     VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR |
                                                     VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        if (!sbtBuf.buffer) {
            DebugLog("VulkanGraphicsDevice::CreateRhiShaderBindingTable: SBT buffer allocation failed.\n");
            return {};
        }

        void* mapped = nullptr;
        if (vkMapMemory(m_device, sbtBuf.memory, 0, sbtSize, 0, &mapped) != VK_SUCCESS || !mapped) {
            FreeRtBuffer(m_device, sbtBuf);
            return {};
        }
        uint8_t* dst = static_cast<uint8_t*>(mapped);
        std::memset(dst, 0, static_cast<size_t>(sbtSize));
        if (const uint8_t* h = handlePtr(desc.rayGenGroupIndex))
            std::memcpy(dst + raygenOffset, h, handleSize);
        for (uint32_t i = 0; i < missCount; ++i) {
            const uint32_t gi = desc.missGroupIndices ? desc.missGroupIndices[i] : 0;
            if (const uint8_t* h = handlePtr(gi))
                std::memcpy(dst + missOffset + i * handleAligned, h, handleSize);
        }
        for (uint32_t i = 0; i < hitCount; ++i) {
            const uint32_t gi = desc.hitGroupIndices ? desc.hitGroupIndices[i] : 0;
            if (const uint8_t* h = handlePtr(gi))
                std::memcpy(dst + hitOffset + i * handleAligned, h, handleSize);
        }
        vkUnmapMemory(m_device, sbtBuf.memory);

        const VkDeviceAddress sbtAddr = GetBufAddr(m_device, m_pfnGetBufAddr, sbtBuf.buffer);

        VulkanShaderBindingTable rec{};
        rec.buffer     = sbtBuf.buffer;
        rec.memory     = sbtBuf.memory;
        rec.pipelineId = desc.pipeline.id;
        rec.raygen.deviceAddress = sbtAddr + raygenOffset;
        rec.raygen.stride        = raygenSize;   // raygen: stride must equal size
        rec.raygen.size          = raygenSize;
        rec.miss.deviceAddress   = missCount ? sbtAddr + missOffset : 0;
        rec.miss.stride          = missCount ? handleAligned : 0;
        rec.miss.size            = missSize;
        rec.hit.deviceAddress    = hitCount ? sbtAddr + hitOffset : 0;
        rec.hit.stride           = hitCount ? handleAligned : 0;
        rec.hit.size             = hitSize;
        // callable region left zeroed.

        const uint64_t id = m_nextRhiResourceHandle++;
        m_shaderBindingTables.emplace(id, rec);
        return RhiShaderBindingTableHandle{ id };
    }

    // -------------------------------------------------------------------------
    // RecordTraceRays — shared trace-rays recording helper.
    // -------------------------------------------------------------------------
    void VulkanGraphicsDevice::RecordTraceRays(
        VkCommandBuffer cmd, VkPipeline pipeline, VkPipelineLayout layout,
        VkDescriptorSet descriptorSet,
        const VkStridedDeviceAddressRegionKHR& raygen,
        const VkStridedDeviceAddressRegionKHR& miss,
        const VkStridedDeviceAddressRegionKHR& hit,
        const VkStridedDeviceAddressRegionKHR& callable,
        uint32_t width, uint32_t height, uint32_t depth)
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pipeline);
        if (descriptorSet != VK_NULL_HANDLE) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                                    layout, 0, 1, &descriptorSet, 0, nullptr);
        }
        m_pfnCmdTraceRays(cmd, &raygen, &miss, &hit, &callable, width, height, depth);
    }

    // -------------------------------------------------------------------------
    // RunRayTracingSmokeTest
    // End-to-end validation of the Vulkan HW ray-tracing RHI primitives: builds a
    // one-triangle BLAS/TLAS, compiles the smoke-test shader to SPIR-V, creates
    // an RT pipeline + SBT, traces one ray per pixel into a 64×64 storage image,
    // and verifies the centre pixel is red (hit) and a corner is blue (miss).
    // -------------------------------------------------------------------------
    bool VulkanGraphicsDevice::RunRayTracingSmokeTest(std::string* outMessage)
    {
        auto fail = [&](const char* msg) -> bool {
            if (outMessage) *outMessage = msg;
            DebugLog("Vulkan RT smoke test FAIL: ");
            DebugLog(msg);
            DebugLog("\n");
            return false;
        };

        if (!m_hasVkKhrRayTracingPipeline || !m_pfnCreateRtPipeline ||
            !m_pfnGetRtHandles || !m_pfnCmdTraceRays)
            return fail("VK_KHR_ray_tracing_pipeline not available on this device.");
        if (!m_hasVkKhrAccelerationStructure)
            return fail("VK_KHR_acceleration_structure not available on this device.");

#if !defined(_WIN32)
        return fail("smoke test requires DXC (Windows) to compile the SPIR-V library.");
#else
        constexpr uint32_t W = 64, H = 64;

        // Resources tracked for cleanup on every exit path.
        VkRtBuffer vtxBuf{}, readBuf{};
        RhiAccelerationStructureHandle blas{}, tlas{};
        VkImage        image     = VK_NULL_HANDLE;
        VkDeviceMemory imageMem  = VK_NULL_HANDLE;
        VkImageView    imageView = VK_NULL_HANDLE;
        VkDescriptorPool pool    = VK_NULL_HANDLE;
        RhiRayTracingPipelineHandle pipeH{};
        RhiShaderBindingTableHandle sbtH{};

        auto cleanup = [&]() {
            if (readBuf.buffer)  FreeRtBuffer(m_device, readBuf);
            if (pool)            vkDestroyDescriptorPool(m_device, pool, nullptr);
            if (imageView)       vkDestroyImageView(m_device, imageView, nullptr);
            if (image)           vkDestroyImage(m_device, image, nullptr);
            if (imageMem)        vkFreeMemory(m_device, imageMem, nullptr);
            if (sbtH.id) {
                auto it = m_shaderBindingTables.find(sbtH.id);
                if (it != m_shaderBindingTables.end()) {
                    VkRtBuffer b{ it->second.buffer, it->second.memory };
                    FreeRtBuffer(m_device, b);
                    m_shaderBindingTables.erase(it);
                }
            }
            if (pipeH.id) {
                auto it = m_rtPipelines.find(pipeH.id);
                if (it != m_rtPipelines.end()) {
                    if (it->second.pipeline)       vkDestroyPipeline(m_device, it->second.pipeline, nullptr);
                    if (it->second.pipelineLayout) vkDestroyPipelineLayout(m_device, it->second.pipelineLayout, nullptr);
                    if (it->second.descSetLayout)  vkDestroyDescriptorSetLayout(m_device, it->second.descSetLayout, nullptr);
                    if (it->second.module)         vkDestroyShaderModule(m_device, it->second.module, nullptr);
                    m_rtPipelines.erase(it);
                }
            }
            if (tlas.id) DestroyRhiAccelerationStructure(tlas);
            if (blas.id) DestroyRhiAccelerationStructure(blas);
            if (vtxBuf.buffer) FreeRtBuffer(m_device, vtxBuf);
        };
        auto failClean = [&](const char* msg) -> bool { cleanup(); return fail(msg); };

        // 1. Compile the smoke-test shader to SPIR-V via DXC.
        std::vector<uint8_t> spirv;
        {
            namespace WRL = Microsoft::WRL;
            static HMODULE dxc = LoadLibraryW(L"dxcompiler.dll");
            auto createInst = dxc
                ? reinterpret_cast<HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*)>(
                      GetProcAddress(dxc, "DxcCreateInstance"))
                : nullptr;
            if (!createInst) return fail("dxcompiler.dll / DxcCreateInstance not found.");

            WRL::ComPtr<IDxcUtils>    utils;
            WRL::ComPtr<IDxcCompiler3> compiler;
            if (FAILED(createInst(CLSID_DxcUtils, IID_PPV_ARGS(&utils))) ||
                FAILED(createInst(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler))))
                return fail("failed to create DXC instances.");

            const std::filesystem::path shaderRoot = ShaderCompilationService::GetShaderSourceRoot();
            const std::filesystem::path shaderPath =
                shaderRoot / L"RayTracing" / L"DXR" / L"VulkanRtSmokeTest.hlsl";

            WRL::ComPtr<IDxcBlobEncoding> source;
            if (FAILED(utils->LoadFile(shaderPath.c_str(), nullptr, &source)) || !source)
                return fail("failed to load VulkanRtSmokeTest.hlsl.");

            WRL::ComPtr<IDxcIncludeHandler> includeHandler;
            utils->CreateDefaultIncludeHandler(&includeHandler);

            const std::wstring srcName = shaderPath.native();
            const std::wstring incDir  = shaderRoot.native();
            std::vector<LPCWSTR> args{
                srcName.c_str(),
                L"-T", L"lib_6_6",
                L"-I", incDir.c_str(),
                L"-spirv",
                L"-fspv-target-env=vulkan1.2",
                L"-HV", L"2021",
            };

            DxcBuffer buf{ source->GetBufferPointer(), source->GetBufferSize(), DXC_CP_ACP };
            WRL::ComPtr<IDxcResult> result;
            if (FAILED(compiler->Compile(&buf, args.data(), static_cast<UINT32>(args.size()),
                                         includeHandler.Get(), IID_PPV_ARGS(&result))) || !result)
                return fail("DXC compile invocation failed.");

            WRL::ComPtr<IDxcBlobUtf8> errors;
            if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr)) &&
                errors && errors->GetStringLength() > 0) {
                DebugLog(errors->GetStringPointer());
                DebugLog("\n");
            }
            HRESULT status = S_OK;
            if (FAILED(result->GetStatus(&status)) || FAILED(status))
                return fail("SPIR-V compilation failed (see DXC errors above).");

            WRL::ComPtr<IDxcBlob> object;
            if (FAILED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr)) || !object)
                return fail("missing SPIR-V object output.");
            spirv.resize(object->GetBufferSize());
            std::memcpy(spirv.data(), object->GetBufferPointer(), spirv.size());
        }

        // 2. Single triangle in the z = 1 plane → BLAS → TLAS.
        const float verts[9] = {
            -0.5f, -0.5f, 1.0f,
             0.5f, -0.5f, 1.0f,
             0.0f,  0.5f, 1.0f,
        };
        vtxBuf = AllocRtBufferHostVisible(
            m_device, m_physicalDevice, sizeof(verts),
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true);
        if (!vtxBuf.buffer) return failClean("vertex buffer allocation failed.");
        {
            void* m = nullptr;
            if (vkMapMemory(m_device, vtxBuf.memory, 0, sizeof(verts), 0, &m) != VK_SUCCESS || !m)
                return failClean("vertex buffer map failed.");
            std::memcpy(m, verts, sizeof(verts));
            vkUnmapMemory(m_device, vtxBuf.memory);
        }
        const VkDeviceAddress vtxAddr = GetBufAddr(m_device, m_pfnGetBufAddr, vtxBuf.buffer);

        RhiRayTracingGeometryDesc geo{};
        geo.vertexBufferAddress = static_cast<RhiGpuAddress>(vtxAddr);
        geo.vertexFormat        = RhiFormat::R32G32B32Float;
        geo.vertexCount         = 3;
        geo.vertexStrideInBytes = 12;
        geo.opaque              = true;

        RhiBlasDesc blasDesc{};
        blasDesc.geometries     = &geo;
        blasDesc.geometryCount  = 1;
        blasDesc.preferFastTrace = true;
        if (!BuildRhiBlases(&blasDesc, 1, &blas) || blas.id == 0)
            return failClean("BLAS build failed.");

        RhiTlasInstanceDesc inst{};
        inst.blasHandle    = blas;
        inst.transform[0]  = 1.0f;  // identity 3×4 (row-major)
        inst.transform[5]  = 1.0f;
        inst.transform[10] = 1.0f;
        inst.instanceMask  = 0xFF;
        inst.forceOpaque   = true;

        RhiTlasDesc tlasDesc{};
        tlasDesc.instances      = &inst;
        tlasDesc.instanceCount  = 1;
        tlasDesc.preferFastTrace = true;
        tlas = BuildRhiTlas(tlasDesc);
        if (tlas.id == 0) return failClean("TLAS build failed.");

        const auto tlasIt = m_accelStructures.find(tlas.id);
        if (tlasIt == m_accelStructures.end()) return failClean("TLAS record missing.");
        const VkAccelerationStructureKHR tlasVk = tlasIt->second.as;

        // 3. Storage image (RGBA8) for the trace output.
        {
            VkImageCreateInfo ii{};
            ii.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            ii.imageType     = VK_IMAGE_TYPE_2D;
            ii.format        = VK_FORMAT_R8G8B8A8_UNORM;
            ii.extent        = { W, H, 1 };
            ii.mipLevels     = 1;
            ii.arrayLayers   = 1;
            ii.samples       = VK_SAMPLE_COUNT_1_BIT;
            ii.tiling        = VK_IMAGE_TILING_OPTIMAL;
            ii.usage         = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (vkCreateImage(m_device, &ii, nullptr, &image) != VK_SUCCESS)
                return failClean("storage image creation failed.");

            VkMemoryRequirements mr{};
            vkGetImageMemoryRequirements(m_device, image, &mr);
            VkPhysicalDeviceMemoryProperties mp{};
            vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &mp);
            uint32_t mi = UINT32_MAX;
            for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
                if ((mr.memoryTypeBits & (1u << i)) &&
                    (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                    mi = i;
                    break;
                }
            }
            VkMemoryAllocateInfo mai{};
            mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            mai.allocationSize  = mr.size;
            mai.memoryTypeIndex = mi;
            if (mi == UINT32_MAX ||
                vkAllocateMemory(m_device, &mai, nullptr, &imageMem) != VK_SUCCESS)
                return failClean("storage image memory allocation failed.");
            vkBindImageMemory(m_device, image, imageMem, 0);

            VkImageViewCreateInfo vi{};
            vi.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            vi.image            = image;
            vi.viewType         = VK_IMAGE_VIEW_TYPE_2D;
            vi.format           = VK_FORMAT_R8G8B8A8_UNORM;
            vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            if (vkCreateImageView(m_device, &vi, nullptr, &imageView) != VK_SUCCESS)
                return failClean("storage image view creation failed.");
        }

        // Transition UNDEFINED → GENERAL for storage-image writes.
        {
            ScopedCmd sc(m_device, m_commandPool, m_graphicsQueue);
            if (!sc.ok) return failClean("command buffer allocation failed (layout).");
            VkImageMemoryBarrier b{};
            b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image               = image;
            b.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            b.srcAccessMask       = 0;
            b.dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(sc.cmd,
                                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &b);
            if (!sc.Submit()) return failClean("layout transition submit failed.");
        }

        // 4. Ray-tracing pipeline (raygen, miss, closest-hit).
        RhiRayTracingShaderGroupDesc groups[3]{};
        groups[0].type       = RhiRayTracingShaderGroupDesc::Type::General;
        groups[0].exportName = "RayGen";
        groups[1].type       = RhiRayTracingShaderGroupDesc::Type::General;
        groups[1].exportName = "Miss";
        groups[2].type            = RhiRayTracingShaderGroupDesc::Type::TrianglesHit;
        groups[2].hitGroupExport  = "HitGroup";
        groups[2].closestHitExport = "ClosestHit";

        RhiRayTracingPipelineDesc pd{};
        pd.spirvBytecode            = spirv.data();
        pd.spirvBytecodeSizeInBytes = spirv.size();
        pd.shaderGroups             = groups;
        pd.shaderGroupCount         = 3;
        pd.maxRecursionDepth        = 1;
        pd.maxPayloadSizeBytes      = 16;
        pd.maxAttributeSizeBytes    = 8;
        pipeH = CreateRhiRayTracingPipeline(pd);
        if (pipeH.id == 0) return failClean("RT pipeline creation failed.");
        auto pipeIt = m_rtPipelines.find(pipeH.id);
        if (pipeIt == m_rtPipelines.end()) return failClean("RT pipeline record missing.");

        // 5. Descriptor set: AS (binding 0) + storage image (binding 1).
        {
            VkDescriptorPoolSize ps[2]{};
            ps[0].type            = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
            ps[0].descriptorCount = 1;
            ps[1].type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            ps[1].descriptorCount = 1;
            VkDescriptorPoolCreateInfo dpi{};
            dpi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            dpi.maxSets       = 1;
            dpi.poolSizeCount = 2;
            dpi.pPoolSizes    = ps;
            if (vkCreateDescriptorPool(m_device, &dpi, nullptr, &pool) != VK_SUCCESS)
                return failClean("descriptor pool creation failed.");

            VkDescriptorSetAllocateInfo dsa{};
            dsa.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            dsa.descriptorPool     = pool;
            dsa.descriptorSetCount = 1;
            dsa.pSetLayouts        = &pipeIt->second.descSetLayout;
            VkDescriptorSet set = VK_NULL_HANDLE;
            if (vkAllocateDescriptorSets(m_device, &dsa, &set) != VK_SUCCESS)
                return failClean("descriptor set allocation failed.");

            VkWriteDescriptorSetAccelerationStructureKHR asWrite{};
            asWrite.sType                      = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
            asWrite.accelerationStructureCount = 1;
            asWrite.pAccelerationStructures    = &tlasVk;

            VkDescriptorImageInfo imgInfo{};
            imgInfo.imageView   = imageView;
            imgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

            VkWriteDescriptorSet writes[2]{};
            writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].pNext           = &asWrite;
            writes[0].dstSet          = set;
            writes[0].dstBinding      = 0;
            writes[0].descriptorCount = 1;
            writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
            writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[1].dstSet          = set;
            writes[1].dstBinding      = 1;
            writes[1].descriptorCount = 1;
            writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[1].pImageInfo      = &imgInfo;
            vkUpdateDescriptorSets(m_device, 2, writes, 0, nullptr);

            // DispatchRays reads the descriptor set from the pipeline record.
            pipeIt->second.boundSet = set;
        }

        // 6. Shader binding table.
        const uint32_t missIdx[1] = { 1 };
        const uint32_t hitIdx[1]  = { 2 };
        RhiShaderBindingTableDesc sd{};
        sd.pipeline         = pipeH;
        sd.rayGenGroupIndex = 0;
        sd.missGroupIndices = missIdx;
        sd.missGroupCount   = 1;
        sd.hitGroupIndices  = hitIdx;
        sd.hitGroupCount    = 1;
        sbtH = CreateRhiShaderBindingTable(sd);
        if (sbtH.id == 0) return failClean("shader binding table creation failed.");
        const auto sbtIt = m_shaderBindingTables.find(sbtH.id);
        if (sbtIt == m_shaderBindingTables.end()) return failClean("SBT record missing.");

        // 7. Trace one ray per pixel. Uses the shared RecordTraceRays helper — the
        //    same code path the command encoder's DispatchRays records.
        {
            ScopedCmd sc(m_device, m_commandPool, m_graphicsQueue);
            if (!sc.ok) return failClean("command buffer allocation failed (dispatch).");
            RecordTraceRays(sc.cmd, pipeIt->second.pipeline, pipeIt->second.pipelineLayout,
                            pipeIt->second.boundSet,
                            sbtIt->second.raygen, sbtIt->second.miss,
                            sbtIt->second.hit, sbtIt->second.callable, W, H, 1);
            if (!sc.Submit()) return failClean("trace rays submit failed.");
        }

        // 8. Copy the image to a host-visible buffer and read it back.
        readBuf = AllocRtBufferHostVisible(m_device, m_physicalDevice,
                                           static_cast<VkDeviceSize>(W) * H * 4,
                                           VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
        if (!readBuf.buffer) return failClean("readback buffer allocation failed.");
        {
            ScopedCmd sc(m_device, m_commandPool, m_graphicsQueue);
            if (!sc.ok) return failClean("command buffer allocation failed (readback).");
            VkImageMemoryBarrier b{};
            b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
            b.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image               = image;
            b.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            b.srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
            b.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(sc.cmd,
                                 VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &b);
            VkBufferImageCopy copy{};
            copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            copy.imageExtent      = { W, H, 1 };
            vkCmdCopyImageToBuffer(sc.cmd, image, VK_IMAGE_LAYOUT_GENERAL,
                                   readBuf.buffer, 1, &copy);
            if (!sc.Submit()) return failClean("readback submit failed.");
        }

        bool centerRed = false, cornerBlue = false;
        {
            void* rd = nullptr;
            if (vkMapMemory(m_device, readBuf.memory, 0,
                            static_cast<VkDeviceSize>(W) * H * 4, 0, &rd) != VK_SUCCESS || !rd)
                return failClean("readback map failed.");
            const uint8_t* px = static_cast<const uint8_t*>(rd);
            auto at = [&](uint32_t x, uint32_t y) -> const uint8_t* {
                return px + (static_cast<size_t>(y) * W + x) * 4;
            };
            const uint8_t* c = at(W / 2, H / 2);
            const uint8_t* k = at(0, 0);
            centerRed  = (c[0] > 200 && c[2] < 60);
            cornerBlue = (k[2] > 200 && k[0] < 60);
            vkUnmapMemory(m_device, readBuf.memory);
        }

        const bool pass = centerRed && cornerBlue;
        cleanup();

        if (pass) {
            if (outMessage) *outMessage = "PASS (hit=red, miss=blue)";
            DebugLog("Vulkan RT smoke test: PASS (hit=red, miss=blue)\n");
            return true;
        }
        return fail(centerRed ? "corner pixel was not blue (miss shader)."
                              : "centre pixel was not red (hit shader).");
#endif // _WIN32
    }

#endif // RHI_VULKAN
} // namespace SasamiEngine::Render
