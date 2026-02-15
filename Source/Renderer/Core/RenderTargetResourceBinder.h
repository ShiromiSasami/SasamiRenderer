#pragma once
#include <vector>
#include "Renderer/Core/GraphicsDevice.h"

namespace SasamiRenderer
{
	class RenderTargetResourceBinder {
	public:
		bool Initialize(GraphicsDevice& device, SwapChain& swapChain, UINT bufferCount);
		inline CpuDescriptorHandle GetRTV(int index) const { return m_rtvHandles[index]; }
		const Resource* GetBackBufferResource(int index) const;
		void Release();

	private:
		std::vector<Resource> m_renderTargets;
		DescriptorHeap m_rtvHeap;
		std::vector<CpuDescriptorHandle> m_rtvHandles;
	};
}
