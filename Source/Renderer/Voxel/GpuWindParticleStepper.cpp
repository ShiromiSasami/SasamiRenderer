#include "Renderer/Voxel/GpuWindParticleStepper.h"

#include "Renderer/Resources/ShaderCompilationService.h"
#include "Renderer/Voxel/GpuVoxelBuffers.h"
#include "Renderer/Voxel/SignedDistanceGrid.h"
#include "Renderer/Voxel/StaggeredVelocityGrid.h"

#include <memory>
#include <string>
#include <vector>

namespace SasamiEngine::Render::Voxel
{
    namespace
    {
        using namespace GpuVoxelBuffers;

        constexpr uint32_t kGroupSize = 64;
        constexpr uint32_t kBindingCount = 6;
        constexpr uint32_t kFieldBindingCount = 4;
        constexpr uint32_t kParamsBinding = 4;
        constexpr uint32_t kParticleBinding = 5;

        std::string MakeError(const char* message)
        {
            return std::string("GpuWindParticleStepper: ") + message;
        }
    }

    GpuWindParticleStepper::~GpuWindParticleStepper()
    {
        ReleaseField();
    }

    bool GpuWindParticleStepper::Initialize(IRhiDevice& device, std::string& outError)
    {
        if (m_device != nullptr) {
            return true;
        }
        if (device.GetRhiBackendApi() != RhiBackendApi::DirectX12) {
            outError = MakeError("only the D3D12 backend is supported");
            return false;
        }

        std::vector<uint8_t> bytecode;
        if (!ShaderCompilationService::GetOrCompileShaderBytecodeDxc("Voxel", L"Compute/Voxel/WindParticleStep_CS.hlsl",
                                                                     "CSMain", "cs_6_0", bytecode)
            || bytecode.empty()) {
            outError = MakeError("failed to compile WindParticleStep_CS.hlsl");
            return false;
        }

        RhiShaderModuleDesc shaderDesc{};
        shaderDesc.bytecode = bytecode.data();
        shaderDesc.bytecodeSize = bytecode.size();
        shaderDesc.entryPoint = "CSMain";
        shaderDesc.stage = RhiShaderStageFlags::Compute;
        if (!device.CreateRhiShaderModule(shaderDesc).IsValid()) {
            outError = MakeError("failed to create the shader module");
            return false;
        }

        RhiBindingRangeDesc bindings[kBindingCount]{};
        for (uint32_t i = 0; i < kBindingCount; ++i) {
            bindings[i].type = (i < kParticleBinding) ? RhiBindingType::ShaderResource : RhiBindingType::UnorderedAccess;
            bindings[i].visibility = RhiShaderStageFlags::Compute;
            bindings[i].baseRegister = (i < kParticleBinding) ? i : 0;
            bindings[i].registerSpace = 0;
            bindings[i].descriptorCount = 1;
            bindings[i].inlineRootDescriptor = false;
            bindings[i].bufferResource = true;
        }

        RhiPipelineLayoutDesc layoutDesc{};
        layoutDesc.bindings = bindings;
        layoutDesc.bindingCount = kBindingCount;
        layoutDesc.allowInputAssembler = false;
        const RhiPipelineLayoutHandle layout = device.CreateRhiPipelineLayout(layoutDesc);
        if (!layout.IsValid()) {
            outError = MakeError("failed to create the pipeline layout");
            return false;
        }

        RhiComputePipelineDesc pipelineDesc{};
        pipelineDesc.layout = layout;
        pipelineDesc.shader = shaderDesc;
        const RhiPipelineHandle pipeline = device.CreateRhiComputePipeline(pipelineDesc);
        if (!pipeline.IsValid()) {
            outError = MakeError("failed to create the compute pipeline");
            return false;
        }

        const RhiPipelineLayoutHandle bindableLayout = device.GetBindablePipelineLayoutHandle(layout);
        const RhiPipelineHandle bindablePipeline = device.GetBindablePipelineHandle(pipeline);
        if (!bindableLayout.IsValid() || !bindablePipeline.IsValid()) {
            outError = MakeError("failed to resolve the bindable pipeline");
            return false;
        }

        const RhiDescriptorAllocation descriptors =
            device.AllocateRhiDescriptors(RhiDescriptorHeapType::CbvSrvUav, kBindingCount, true);
        if (!descriptors.IsValid() || descriptors.heap.id == 0) {
            outError = MakeError("failed to allocate descriptors");
            return false;
        }

        RhiBindingRangeDesc inlineBindings[kBindingCount]{};
        for (uint32_t i = 0; i < kBindingCount; ++i) {
            inlineBindings[i] = bindings[i];
            inlineBindings[i].inlineRootDescriptor = true;
        }
        RhiPipelineLayoutDesc inlineLayoutDesc{};
        inlineLayoutDesc.bindings = inlineBindings;
        inlineLayoutDesc.bindingCount = kBindingCount;
        inlineLayoutDesc.allowInputAssembler = false;
        const RhiPipelineLayoutHandle inlineLayout = device.CreateRhiPipelineLayout(inlineLayoutDesc);
        if (!inlineLayout.IsValid()) {
            outError = MakeError("failed to create the inline pipeline layout");
            return false;
        }
        RhiComputePipelineDesc inlinePipelineDesc{};
        inlinePipelineDesc.layout = inlineLayout;
        inlinePipelineDesc.shader = shaderDesc;
        const RhiPipelineHandle inlinePipeline = device.CreateRhiComputePipeline(inlinePipelineDesc);
        if (!inlinePipeline.IsValid()) {
            outError = MakeError("failed to create the inline compute pipeline");
            return false;
        }
        const RhiPipelineLayoutHandle bindableInlineLayout = device.GetBindablePipelineLayoutHandle(inlineLayout);
        const RhiPipelineHandle bindableInlinePipeline = device.GetBindablePipelineHandle(inlinePipeline);
        if (!bindableInlineLayout.IsValid() || !bindableInlinePipeline.IsValid()) {
            outError = MakeError("failed to resolve the bindable inline pipeline");
            return false;
        }

        m_bindableLayout = bindableLayout;
        m_bindablePipeline = bindablePipeline;
        m_inlineLayout = bindableInlineLayout;
        m_inlinePipeline = bindableInlinePipeline;
        m_descriptors = descriptors;
        m_device = &device;
        return true;
    }

    bool GpuWindParticleStepper::UploadField(const SignedDistanceGrid& sdf, const StaggeredVelocityGrid& velocity,
                                             const VoxelGridDesc& desc, std::string& outError)
    {
        if (m_device == nullptr) {
            outError = MakeError("not initialized");
            return false;
        }
        if (desc.dimX == 0 || desc.dimY == 0 || desc.dimZ == 0 || !(desc.voxelSize > 0.0f)) {
            outError = MakeError("empty grid description");
            return false;
        }

        const uint64_t dimX = desc.dimX;
        const uint64_t dimY = desc.dimY;
        const uint64_t dimZ = desc.dimZ;
        const std::vector<float>& sdfValues = sdf.Values();
        const std::vector<float>& uValues = velocity.UValues();
        const std::vector<float>& vValues = velocity.VValues();
        const std::vector<float>& wValues = velocity.WValues();
        if (sdfValues.size() != dimX * dimY * dimZ || uValues.size() != (dimX + 1) * dimY * dimZ ||
            vValues.size() != dimX * (dimY + 1) * dimZ || wValues.size() != dimX * dimY * (dimZ + 1)) {
            outError = MakeError("field sizes do not match the grid description");
            return false;
        }

        ReleaseField();

        const std::vector<float>* sources[kFieldBindingCount] = { &sdfValues, &uValues, &vValues, &wValues };
        RhiBufferHandle created[kFieldBindingCount]{};
        for (uint32_t i = 0; i < kFieldBindingCount; ++i) {
            const uint64_t bytes = static_cast<uint64_t>(sources[i]->size()) * sizeof(float);
            created[i] = m_device->CreateRhiBuffer(
                MakeBufferDesc(bytes, sizeof(float), RhiBufferUsageFlags::ShaderResource | RhiBufferUsageFlags::Structured,
                               RhiMemoryUsage::GpuOnly, RhiResourceState::ShaderResource),
                sources[i]->data());
            if (!created[i].IsValid()) {
                for (uint32_t j = 0; j < i; ++j) {
                    m_device->DestroyRhiResource(created[j]);
                }
                outError = MakeError("failed to create a field buffer");
                return false;
            }
        }

        m_sdfBuffer = created[0];
        m_uBuffer = created[1];
        m_vBuffer = created[2];
        m_wBuffer = created[3];
        m_sdfCount = sdfValues.size();
        m_uCount = uValues.size();
        m_vCount = vValues.size();
        m_wCount = wValues.size();
        m_desc = desc;
        return true;
    }

    bool GpuWindParticleStepper::Run(std::vector<WindParticleState>& states, const WindParticleStepParams& params,
                                     uint32_t steps, std::string& outError)
    {
        if (m_device == nullptr) {
            outError = MakeError("not initialized");
            return false;
        }
        if (!HasField()) {
            outError = MakeError("no field uploaded");
            return false;
        }
        if (states.empty() || steps == 0) {
            outError = MakeError("nothing to run");
            return false;
        }

        const uint64_t particleCount = states.size();
        const uint64_t particleBytes = particleCount * sizeof(WindParticleState);

        const WindParticleStepParamsGpu gpuParams = MakeGpuParams(params, static_cast<uint32_t>(particleCount));

        BufferGuard guard(*m_device);
        const RhiBufferHandle paramsBuffer = guard.Create(
            MakeBufferDesc(sizeof(gpuParams), sizeof(gpuParams),
                           RhiBufferUsageFlags::ShaderResource | RhiBufferUsageFlags::Structured,
                           RhiMemoryUsage::GpuOnly, RhiResourceState::ShaderResource),
            &gpuParams);
        const RhiBufferHandle particleBuffer = guard.Create(
            MakeBufferDesc(particleBytes, sizeof(WindParticleState),
                           RhiBufferUsageFlags::UnorderedAccess | RhiBufferUsageFlags::CopySource |
                               RhiBufferUsageFlags::Structured,
                           RhiMemoryUsage::GpuOnly, RhiResourceState::UnorderedAccess),
            states.data());
        const RhiBufferHandle readbackBuffer = guard.Create(
            MakeBufferDesc(particleBytes, 0, RhiBufferUsageFlags::CopyDest, RhiMemoryUsage::GpuToCpu,
                           RhiResourceState::CopyDest),
            nullptr);
        if (!paramsBuffer.IsValid() || !particleBuffer.IsValid() || !readbackBuffer.IsValid()) {
            outError = MakeError("failed to create the working buffers");
            return false;
        }

        const uint64_t cpuBase = m_descriptors.cpu.ptr;
        const uint64_t increment = m_descriptors.increment;
        const RhiBufferHandle fieldBuffers[kFieldBindingCount] = { m_sdfBuffer, m_uBuffer, m_vBuffer, m_wBuffer };
        const uint64_t fieldCounts[kFieldBindingCount] = { m_sdfCount, m_uCount, m_vCount, m_wCount };
        bool viewsOk = true;
        for (uint32_t i = 0; i < kFieldBindingCount; ++i) {
            viewsOk = viewsOk
                && m_device->CreateRhiBufferShaderResourceView(
                    fieldBuffers[i], MakeStructuredView(fieldCounts[i] * sizeof(float), sizeof(float)),
                    { cpuBase + i * increment });
        }
        viewsOk = viewsOk
            && m_device->CreateRhiBufferShaderResourceView(
                paramsBuffer, MakeStructuredView(sizeof(gpuParams), sizeof(gpuParams)),
                { cpuBase + kParamsBinding * increment })
            && m_device->CreateRhiBufferUnorderedAccessView(
                particleBuffer, MakeStructuredView(particleBytes, sizeof(WindParticleState)),
                { cpuBase + kParticleBinding * increment });
        if (!viewsOk) {
            outError = MakeError("failed to create views");
            return false;
        }

        std::unique_ptr<IRhiCommandEncoder> encoder = m_device->CreateCommandEncoder(RhiQueueType::Graphics);
        if (!encoder) {
            outError = MakeError("failed to create a command encoder");
            return false;
        }
        encoder->BeginDebugLabel("Voxel.WindParticleStep");
        encoder->SetDescriptorHeap(m_descriptors.heap);
        encoder->SetComputePipelineLayout(m_bindableLayout);
        encoder->SetComputePipeline(m_bindablePipeline);
        for (uint32_t i = 0; i < kBindingCount; ++i) {
            encoder->SetComputeDescriptorTable(i, RhiGpuDescriptorHandle{ m_descriptors.gpu.ptr + i * increment });
        }

        RhiDispatchDesc dispatch{};
        dispatch.groupCountX = static_cast<uint32_t>((particleCount + kGroupSize - 1) / kGroupSize);
        dispatch.groupCountY = 1;
        dispatch.groupCountZ = 1;
        for (uint32_t step = 0; step < steps; ++step) {
            encoder->Dispatch(dispatch);
            encoder->UavBarrier(&particleBuffer, 1);
        }

        RhiResourceTransitionDesc transition{};
        transition.resource = particleBuffer;
        transition.before = RhiResourceState::UnorderedAccess;
        transition.after = RhiResourceState::CopySource;
        encoder->TransitionResources(&transition, 1);

        RhiBufferCopyDesc copy{};
        copy.source = particleBuffer;
        copy.destination = readbackBuffer;
        copy.sizeInBytes = particleBytes;
        encoder->CopyBuffer(copy);
        encoder->EndDebugLabel();

        if (!m_device->SubmitCommandEncoder(*encoder, RhiQueueType::Graphics)) {
            outError = MakeError("failed to submit the command encoder");
            return false;
        }
        if (!m_device->WaitForRhiIdle()) {
            outError = MakeError("waiting for the GPU failed");
            return false;
        }

        if (!m_device->ReadRhiBuffer(readbackBuffer, 0, states.data(), particleBytes)) {
            outError = MakeError("failed to read the particles back");
            return false;
        }
        return true;
    }

    WindParticleStepParamsGpu GpuWindParticleStepper::MakeGpuParams(const WindParticleStepParams& params,
                                                                     uint32_t particleCount) const
    {
        WindParticleStepParamsGpu gpuParams{};
        for (int c = 0; c < 3; ++c) {
            gpuParams.origin[c] = m_desc.origin[c];
            gpuParams.acceleration[c] = params.acceleration[c];
        }
        gpuParams.voxelSize = m_desc.voxelSize;
        gpuParams.invVoxelSize = 1.0f / m_desc.voxelSize;
        gpuParams.dimX = m_desc.dimX;
        gpuParams.dimY = m_desc.dimY;
        gpuParams.dimZ = m_desc.dimZ;
        gpuParams.dt = params.dt;
        gpuParams.tau = params.tau;
        gpuParams.decay = params.decay;
        gpuParams.buoyancy = params.buoyancy;
        gpuParams.flutterAcceleration = params.flutterAcceleration;
        gpuParams.flutterFrequency = params.flutterFrequency;
        gpuParams.wallFriction = params.wallFriction;
        gpuParams.flutter = params.flutter;
        gpuParams.particleCount = particleCount;
        return gpuParams;
    }

    bool GpuWindParticleStepper::RecordStep(IRhiCommandEncoder& encoder, RhiGpuAddress paramsAddress,
                                            RhiBufferHandle particles, uint32_t particleCount,
                                            std::string& outError) const
    {
        if (m_device == nullptr) {
            outError = MakeError("not initialized");
            return false;
        }
        if (!HasField()) {
            outError = MakeError("no field uploaded");
            return false;
        }
        if (paramsAddress == 0 || !particles.IsValid() || particleCount == 0) {
            outError = MakeError("nothing to record");
            return false;
        }
        const RhiBufferHandle fieldBuffers[kFieldBindingCount] = { m_sdfBuffer, m_uBuffer, m_vBuffer, m_wBuffer };
        RhiGpuAddress fieldAddresses[kFieldBindingCount]{};
        for (uint32_t i = 0; i < kFieldBindingCount; ++i) {
            fieldAddresses[i] = m_device->GetRhiBufferGpuAddress(fieldBuffers[i]);
            if (fieldAddresses[i] == 0) {
                outError = MakeError("a field buffer has no GPU address");
                return false;
            }
        }
        const RhiGpuAddress particleAddress = m_device->GetRhiBufferGpuAddress(particles);
        if (particleAddress == 0) {
            outError = MakeError("the particle buffer has no GPU address");
            return false;
        }

        encoder.BeginDebugLabel("Voxel.WindParticleStep");
        encoder.SetComputePipelineLayout(m_inlineLayout);
        encoder.SetComputePipeline(m_inlinePipeline);
        for (uint32_t i = 0; i < kFieldBindingCount; ++i) {
            encoder.SetComputeShaderResourceView(i, fieldAddresses[i]);
        }
        encoder.SetComputeShaderResourceView(kParamsBinding, paramsAddress);
        encoder.SetComputeUnorderedAccessView(kParticleBinding, particleAddress);

        RhiDispatchDesc dispatch{};
        dispatch.groupCountX = (particleCount + kGroupSize - 1) / kGroupSize;
        dispatch.groupCountY = 1;
        dispatch.groupCountZ = 1;
        encoder.Dispatch(dispatch);
        encoder.UavBarrier(&particles, 1);
        encoder.EndDebugLabel();
        return true;
    }

    void GpuWindParticleStepper::ReleaseField()
    {
        if (m_device != nullptr) {
            RhiBufferHandle* buffers[] = { &m_sdfBuffer, &m_uBuffer, &m_vBuffer, &m_wBuffer };
            for (RhiBufferHandle* buffer : buffers) {
                if (buffer->IsValid()) {
                    m_device->DestroyRhiResource(*buffer);
                }
            }
        }
        m_sdfBuffer = {};
        m_uBuffer = {};
        m_vBuffer = {};
        m_wBuffer = {};
        m_sdfCount = 0;
        m_uCount = 0;
        m_vCount = 0;
        m_wCount = 0;
    }
}
