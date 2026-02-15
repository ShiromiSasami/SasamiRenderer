#include "Renderer/Core/RenderTargetResourceBinder.h"
#include "Foundation/Diagnostics/DebugOutput.h"

namespace SasamiRenderer
{
	bool RenderTargetResourceBinder::Initialize(GraphicsDevice& device, SwapChain& swapChain, UINT bufferCount)
	{
		m_renderTargets.clear();
		m_renderTargets.resize(bufferCount);

		// RTVヒープ作成
		D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
		rtvHeapDesc.NumDescriptors = bufferCount;
		rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		HRESULT hr = device.CreateDescriptorHeap(rtvHeapDesc, m_rtvHeap);
		if (FAILED(hr)) {
			return false;
		}

		// RTV作成
		m_rtvHandles.resize(bufferCount);
		UINT rtvDescriptorSize = device.GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
		D3D12_CPU_DESCRIPTOR_HANDLE handle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart());

		for (UINT i = 0; i < bufferCount; i++) {
			hr = swapChain.GetBuffer(i, IID_PPV_ARGS(m_renderTargets[i].GetAddressOf()));
			if (FAILED(hr)) {
				return false;
			}
			device.CreateRenderTargetView(m_renderTargets[i], nullptr, handle);
			m_rtvHandles[i] = handle;
			handle.ptr += rtvDescriptorSize;
		}

		return true;
	}

	void RenderTargetResourceBinder::Release()
	{
		for (auto& rt : m_renderTargets) rt.Reset();
		m_renderTargets.clear();
		m_rtvHeap.Reset();
		m_rtvHandles.clear();
	}
	const Resource* RenderTargetResourceBinder::GetBackBufferResource(int index) const
	{
		if (index < 0 || index >= static_cast<int>(m_renderTargets.size()))
		{
			DebugLog("Index out of bounds in GetBackBufferResource\n");
			return nullptr;
		}

		return &m_renderTargets[index];
	}
}
