#include "Renderer/ParticleCharacter/GpuMolecularBodyStepper.h"

#include "Renderer/Resources/ShaderCompilationService.h"
#include "Renderer/Voxel/GpuVoxelBuffers.h"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace SasamiEngine::Render::ParticleCharacter
{
    namespace
    {
        using namespace Voxel::GpuVoxelBuffers;

        constexpr uint32_t kGroupSize = 64;
        constexpr uint32_t kBindingCount = 4;
        constexpr uint32_t kTargetsBinding = 0;
        constexpr uint32_t kNormalsBinding = 1;
        constexpr uint32_t kParamsBinding = 2;
        constexpr uint32_t kParticleBinding = 3;
        constexpr float kLn2 = 0.69314718056f;

        std::string MakeError(const char* message)
        {
            return std::string("GpuMolecularBodyStepper: ") + message;
        }
    }

    MolecularStepParamsGpu MakeMolecularStepParamsGpu(const MolecularBodySettings& settings, float dt, float bondGoal,
                                                      float sinceGoalChange, uint32_t count, bool resetReleased)
    {
        MolecularStepParamsGpu params{};
        params.halflife = settings.halflife;
        params.bondRate = settings.bondRate;
        params.bondDelaySpread = settings.bondDelaySpread;
        params.bondDelayPerHeight = settings.bondDelayPerHeight;
        params.force = static_cast<uint32_t>(settings.force);
        params.scatterSpeed = settings.scatterSpeed;
        params.swirlRate = settings.swirlRate;
        params.swirlRise = settings.swirlRise;
        params.gravity = settings.gravity;
        params.drag = settings.drag;
        params.floorY = settings.floorY;
        params.restitution = settings.restitution;
        for (int c = 0; c < 3; ++c) {
            params.centre[c] = settings.centre[c];
        }
        params.dt = dt;
        params.bondGoal = bondGoal;
        params.sinceGoalChange = sinceGoalChange;
        params.count = count;
        params.fourLn2 = 4.0f * kLn2;
        params.resetReleased = resetReleased ? 1u : 0u;
        return params;
    }

    GpuMolecularBodyStepper::~GpuMolecularBodyStepper() = default;

    bool GpuMolecularBodyStepper::Initialize(IRhiDevice& device, std::string& outError)
    {
        if (m_device != nullptr) {
            return true;
        }
        if (device.GetRhiBackendApi() != RhiBackendApi::DirectX12) {
            outError = MakeError("only the D3D12 backend is supported");
            return false;
        }

        std::vector<uint8_t> bytecode;
        if (!ShaderCompilationService::GetOrCompileShaderBytecodeDxc("ParticleCharacter",
                                                                     L"Compute/ParticleCharacter/MolecularBodyStep_CS.hlsl",
                                                                     "CSMain", "cs_6_0", bytecode)
            || bytecode.empty()) {
            outError = MakeError("failed to compile MolecularBodyStep_CS.hlsl");
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
            bindings[i].inlineRootDescriptor = true;
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

        m_layout = bindableLayout;
        m_pipeline = bindablePipeline;
        m_device = &device;
        return true;
    }

    bool GpuMolecularBodyStepper::Run(std::vector<MolecularParticleGpu>& particles, const float* targets,
                                      const float* normals, const MolecularStepParamsGpu* params, uint32_t steps,
                                      double& outGpuMs, std::string& outError)
    {
        if (m_device == nullptr) {
            outError = MakeError("not initialized");
            return false;
        }
        if (particles.empty() || steps == 0 || targets == nullptr || normals == nullptr || params == nullptr) {
            outError = MakeError("nothing to run");
            return false;
        }

        const uint64_t count = particles.size();
        for (uint32_t s = 0; s < steps; ++s) {
            if (params[s].count != count) {
                outError = MakeError("a params entry has a different particle count");
                return false;
            }
        }

        const uint64_t particleBytes = count * sizeof(MolecularParticleGpu);
        const uint64_t stepFloatBytes = count * 3 * sizeof(float);
        const uint64_t stepFloats = count * 3;
        const uint64_t floatBytes = stepFloatBytes * steps;
        const uint64_t paramsBytes = static_cast<uint64_t>(steps) * sizeof(MolecularStepParamsGpu);

        BufferGuard guard(*m_device);
        const RhiBufferUsageFlags readOnlyUsage = RhiBufferUsageFlags::ShaderResource | RhiBufferUsageFlags::Structured;
        const RhiBufferHandle targetsBuffer = guard.Create(
            MakeBufferDesc(floatBytes, sizeof(float), readOnlyUsage, RhiMemoryUsage::CpuToGpu,
                           RhiResourceState::ShaderResource),
            targets);
        const RhiBufferHandle normalsBuffer = guard.Create(
            MakeBufferDesc(floatBytes, sizeof(float), readOnlyUsage, RhiMemoryUsage::CpuToGpu,
                           RhiResourceState::ShaderResource),
            normals);
        const RhiBufferHandle paramsBuffer = guard.Create(
            MakeBufferDesc(paramsBytes, sizeof(MolecularStepParamsGpu), readOnlyUsage, RhiMemoryUsage::CpuToGpu,
                           RhiResourceState::ShaderResource),
            params);
        const RhiBufferHandle particleBuffer = guard.Create(
            MakeBufferDesc(particleBytes, sizeof(MolecularParticleGpu),
                           RhiBufferUsageFlags::UnorderedAccess | RhiBufferUsageFlags::CopySource |
                               RhiBufferUsageFlags::Structured,
                           RhiMemoryUsage::GpuOnly, RhiResourceState::UnorderedAccess),
            particles.data());
        const RhiBufferHandle readbackBuffer = guard.Create(
            MakeBufferDesc(particleBytes, 0, RhiBufferUsageFlags::CopyDest, RhiMemoryUsage::GpuToCpu,
                           RhiResourceState::CopyDest),
            nullptr);
        if (!targetsBuffer.IsValid() || !normalsBuffer.IsValid() || !paramsBuffer.IsValid()
            || !particleBuffer.IsValid() || !readbackBuffer.IsValid()) {
            outError = MakeError("failed to create the working buffers");
            return false;
        }

        const RhiGpuAddress targetsAddress = m_device->GetRhiBufferGpuAddress(targetsBuffer);
        const RhiGpuAddress normalsAddress = m_device->GetRhiBufferGpuAddress(normalsBuffer);
        const RhiGpuAddress paramsAddress = m_device->GetRhiBufferGpuAddress(paramsBuffer);
        const RhiGpuAddress particleAddress = m_device->GetRhiBufferGpuAddress(particleBuffer);
        if (targetsAddress == 0 || normalsAddress == 0 || paramsAddress == 0 || particleAddress == 0) {
            outError = MakeError("a buffer has no GPU address");
            return false;
        }

        std::unique_ptr<IRhiCommandEncoder> encoder = m_device->CreateCommandEncoder(RhiQueueType::Graphics);
        if (!encoder) {
            outError = MakeError("failed to create a command encoder");
            return false;
        }
        encoder->BeginDebugLabel("MolecularBodyStep");
        encoder->SetComputePipelineLayout(m_layout);
        encoder->SetComputePipeline(m_pipeline);

        RhiDispatchDesc dispatch{};
        dispatch.groupCountX = static_cast<uint32_t>((count + kGroupSize - 1) / kGroupSize);
        dispatch.groupCountY = 1;
        dispatch.groupCountZ = 1;
        for (uint32_t s = 0; s < steps; ++s) {
            const uint64_t floatOffset = static_cast<uint64_t>(s) * stepFloats * sizeof(float);
            encoder->SetComputeShaderResourceView(kTargetsBinding, targetsAddress + floatOffset);
            encoder->SetComputeShaderResourceView(kNormalsBinding, normalsAddress + floatOffset);
            encoder->SetComputeShaderResourceView(kParamsBinding,
                                                  paramsAddress + static_cast<uint64_t>(s) * sizeof(MolecularStepParamsGpu));
            encoder->SetComputeUnorderedAccessView(kParticleBinding, particleAddress);
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

        const auto start = std::chrono::steady_clock::now();
        if (!m_device->SubmitCommandEncoder(*encoder, RhiQueueType::Graphics)) {
            outError = MakeError("failed to submit the command encoder");
            return false;
        }
        if (!m_device->WaitForRhiIdle()) {
            outError = MakeError("waiting for the GPU failed");
            return false;
        }
        outGpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

        if (!m_device->ReadRhiBuffer(readbackBuffer, 0, particles.data(), particleBytes)) {
            outError = MakeError("failed to read the particles back");
            return false;
        }
        return true;
    }
}
