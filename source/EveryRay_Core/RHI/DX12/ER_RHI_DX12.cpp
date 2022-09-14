#include "ER_RHI_DX12.h"
#include "ER_RHI_DX12_GPUBuffer.h"
#include "ER_RHI_DX12_GPUTexture.h"
#include "ER_RHI_DX12_GPUShader.h"
#include "ER_RHI_DX12_GPUPipelineStateObject.h"
#include "ER_RHI_DX12_GPURootSignature.h"
#include "ER_RHI_DX12_GPUDescriptorHeapManager.h"

#include "..\..\ER_CoreException.h"
#include "..\..\ER_Utility.h"

namespace EveryRay_Core
{
	static ER_RHI_DX12_DescriptorHandle sNullSRV2DHandle;
	static ER_RHI_DX12_DescriptorHandle sNullSRV3DHandle;

	ER_RHI_DX12::ER_RHI_DX12()
	{
	}

	ER_RHI_DX12::~ER_RHI_DX12()
	{
		DeleteObject(mDescriptorHeapManager);
	}

	bool ER_RHI_DX12::Initialize(HWND windowHandle, UINT width, UINT height, bool isFullscreen)
	{
		mAPI = ER_GRAPHICS_API::DX12;
		assert(width > 0 && height > 0);
		HRESULT hr;

#if defined(_DEBUG) || defined (DEBUG)
		{
			ComPtr<ID3D12Debug> debugController;
			if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(debugController.GetAddressOf()))))
				debugController->EnableDebugLayer();
			else
				ER_OUTPUT_LOG(L"[ER Logger] ER_RHI_DX12: Direct3D Debug Device is not available\n");

			ComPtr<IDXGIInfoQueue> dxgiInfoQueue;
			if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(dxgiInfoQueue.GetAddressOf()))))
			{
				mDXGIFactoryFlags = DXGI_CREATE_FACTORY_DEBUG;

				dxgiInfoQueue->SetBreakOnSeverity(DXGI_DEBUG_ALL, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_ERROR, true);
				dxgiInfoQueue->SetBreakOnSeverity(DXGI_DEBUG_ALL, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_CORRUPTION, true);

				DXGI_INFO_QUEUE_MESSAGE_ID hide[] = { 80, };
				DXGI_INFO_QUEUE_FILTER filter = {};
				filter.DenyList.NumIDs = _countof(hide);
				filter.DenyList.pIDList = hide;
				dxgiInfoQueue->AddStorageFilterEntries(DXGI_DEBUG_DXGI, &filter);
			}
		}
#endif

		if (FAILED(hr = CreateDXGIFactory2(mDXGIFactoryFlags, IID_PPV_ARGS(mDXGIFactory.ReleaseAndGetAddressOf()))))
			throw ER_CoreException("ER_RHI_DX12: CreateDXGIFactory2() failed", hr);

		ComPtr<IDXGIAdapter1> adapter;
		mDXGIFactory->EnumAdapters1(0, adapter.ReleaseAndGetAddressOf());

		// Create the DX12 API device object.
		if (FAILED(hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(mDevice.ReleaseAndGetAddressOf()))))
			throw ER_CoreException("ER_RHI_DX12: D3D12CreateDevice() failed", hr);

		static const D3D_FEATURE_LEVEL s_featureLevels[] =
		{
			D3D_FEATURE_LEVEL_12_1,
			D3D_FEATURE_LEVEL_12_0,
			D3D_FEATURE_LEVEL_11_1,
			D3D_FEATURE_LEVEL_11_0,
		};

		D3D12_FEATURE_DATA_FEATURE_LEVELS featLevels =
		{
			_countof(s_featureLevels), s_featureLevels, D3D_FEATURE_LEVEL_11_0
		};

		hr = mDevice->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &featLevels, sizeof(featLevels));
		if (SUCCEEDED(hr))
			mFeatureLevel = featLevels.MaxSupportedFeatureLevel;
		else
			mFeatureLevel = D3D_FEATURE_LEVEL_11_0;

		D3D12_FEATURE_DATA_D3D12_OPTIONS5 featureSupportData = {};
		HRESULT hr2 = mDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &featureSupportData, sizeof(featureSupportData));
		if (SUCCEEDED(hr2) && featureSupportData.RaytracingTier != D3D12_RAYTRACING_TIER_NOT_SUPPORTED)
			mIsRaytracingTierAvailable = true;
		else
			mIsRaytracingTierAvailable = false;

		// Create graphics command queue data
		{
			D3D12_COMMAND_QUEUE_DESC queueDesc = {};
			queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
			queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

			if (FAILED(mDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(mCommandQueueGraphics.ReleaseAndGetAddressOf()))))
				throw ER_CoreException("ER_RHI_DX12: Could not create graphics command queue");

			// Create descriptor heaps for render target views and depth stencil views.
			//mDescriptorHeapManager = new DXRS::DescriptorHeapManager(mDevice.Get());

			D3D12_DESCRIPTOR_HEAP_DESC rtvDescriptorHeapDesc = {};
			rtvDescriptorHeapDesc.NumDescriptors = 1;
			rtvDescriptorHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;

			if (FAILED(mDevice->CreateDescriptorHeap(&rtvDescriptorHeapDesc, IID_PPV_ARGS(mRTVDescriptorHeap.ReleaseAndGetAddressOf()))))
				throw ER_CoreException("ER_RHI_DX12: Could not create descriptor heap for main RTV");

			mRTVDescriptorSize = mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

			D3D12_DESCRIPTOR_HEAP_DESC dsvDescriptorHeapDesc = {};
			dsvDescriptorHeapDesc.NumDescriptors = 1;
			dsvDescriptorHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;

			if (FAILED(mDevice->CreateDescriptorHeap(&dsvDescriptorHeapDesc, IID_PPV_ARGS(mDSVDescriptorHeap.ReleaseAndGetAddressOf()))))
				throw ER_CoreException("ER_RHI_DX12: Could not create descriptor heap for main DSV");

			for (int i = 0; i < ER_RHI_MAX_GRAPHICS_COMMAND_LISTS; i++)
			{
				// Create a command allocator for each back buffer that will be rendered to.
				if (FAILED(mDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(mCommandAllocatorsGraphics[i].ReleaseAndGetAddressOf()))))
				{
					std::string message = "ER_RHI_DX12: Could not create graphics command allocator " + std::to_string(i);
					throw ER_CoreException(message.c_str());
				}
				// Create a command list for recording graphics commands.
				if (FAILED(mDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, mCommandAllocatorsGraphics[i].Get(), nullptr, IID_PPV_ARGS(mCommandListGraphics[i].ReleaseAndGetAddressOf()))))
				{
					std::string message = "ER_RHI_DX12: Could not create graphics command list " + std::to_string(i);
					throw ER_CoreException(message.c_str());
				}
				mCommandListGraphics[i]->Close();
			}
		}

		//TODO create compute queue data 
		{
			//...
		}

		// Create swapchain, main rtv, main dsv
		{
			// swapchain
			{
				// Create a descriptor for the swap chain.
				DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
				swapChainDesc.Width = width;
				swapChainDesc.Height = height;
				swapChainDesc.Format = mMainRTBufferFormat;
				swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
				swapChainDesc.BufferCount = 2;
				swapChainDesc.SampleDesc.Count = 1;
				swapChainDesc.SampleDesc.Quality = 0;
				swapChainDesc.Scaling = DXGI_SCALING_STRETCH;
				swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
				swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

				DXGI_SWAP_CHAIN_FULLSCREEN_DESC fsSwapChainDesc = {};
				fsSwapChainDesc.Windowed = TRUE;

				ComPtr<IDXGISwapChain1> swapChain;
				// Create a swap chain for the window.
				if (FAILED(mDXGIFactory->CreateSwapChainForHwnd(mCommandQueueGraphics.Get(), windowHandle, &swapChainDesc, &fsSwapChainDesc, nullptr, swapChain.GetAddressOf())))
					throw ER_CoreException("ER_RHI_DX12: Could not create swapchain");
				swapChain.As(&mSwapChain);
				//ThrowIfFailed(mDXGIFactory->MakeWindowAssociation(mAppWindow, DXGI_MWA_NO_ALT_ENTER));
			}

			// main RTV
			{
				mSwapChain->GetBuffer(0, IID_PPV_ARGS(mMainRenderTarget.GetAddressOf()));
				mMainRenderTarget->SetName(L"Main Render target");

				D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
				rtvDesc.Format = mMainRTBufferFormat;
				rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;

				mMainRTVDescriptorHandle = CD3DX12_CPU_DESCRIPTOR_HANDLE(mRTVDescriptorHeap->GetCPUDescriptorHandleForHeapStart(), 0, mRTVDescriptorSize);
				mDevice->CreateRenderTargetView(mMainRenderTarget.Get(), &rtvDesc, mMainRTVDescriptorHandle);

				// Reset the index to the current back buffer.
				//mBackBufferIndex = mSwapChain->GetCurrentBackBufferIndex();
			}

			// main DSV
			{
				CD3DX12_HEAP_PROPERTIES depthHeapProperties(D3D12_HEAP_TYPE_DEFAULT);

				D3D12_RESOURCE_DESC depthStencilDesc = CD3DX12_RESOURCE_DESC::Tex2D(mMainDepthBufferFormat, width, height, 1, 1);
				depthStencilDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

				D3D12_CLEAR_VALUE depthOptimizedClearValue = {};
				depthOptimizedClearValue.Format = mMainDepthBufferFormat;
				depthOptimizedClearValue.DepthStencil.Depth = 1.0f;
				depthOptimizedClearValue.DepthStencil.Stencil = 0;

				if (FAILED(mDevice->CreateCommittedResource(&depthHeapProperties, D3D12_HEAP_FLAG_NONE, &depthStencilDesc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthOptimizedClearValue, IID_PPV_ARGS(mMainDepthStencilTarget.ReleaseAndGetAddressOf()))))
					throw ER_CoreException("ER_RHI_DX12: Could not create comitted resource for main depth-stencil target");

				mMainDepthStencilTarget->SetName(L"Main Depth-Stencil target");

				mMainDSVDescriptorHandle = CD3DX12_CPU_DESCRIPTOR_HANDLE(mDSVDescriptorHeap->GetCPUDescriptorHandleForHeapStart());

				D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
				dsvDesc.Format = mMainDepthBufferFormat;
				dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;

				mDevice->CreateDepthStencilView(mMainDepthStencilTarget.Get(), &dsvDesc, mDSVDescriptorHeap->GetCPUDescriptorHandleForHeapStart());
			}
		}

		mMainViewport.TopLeftX = 0.0f;
		mMainViewport.TopLeftY = 0.0f;
		mMainViewport.Width = static_cast<float>(width);
		mMainViewport.Height = static_cast<float>(height);
		mMainViewport.MinDepth = 0.0f;
		mMainViewport.MaxDepth = 1.0f;

		D3D12_VIEWPORT viewport;
		viewport.TopLeftX = mMainViewport.TopLeftX;
		viewport.TopLeftY = mMainViewport.TopLeftY;
		viewport.Width = mMainViewport.Width;
		viewport.Height = mMainViewport.Height;
		viewport.MinDepth = 0.0f;
		viewport.MaxDepth = 1.0f;

		CreateSamplerStates();
		CreateRasterizerStates();
		CreateDepthStencilStates();
		CreateBlendStates();

		mDescriptorHeapManager = new ER_RHI_DX12_GPUDescriptorHeapManager(mDevice.Get());

		// null SRVs descriptor handles
		{
			sNullSRV2DHandle = mDescriptorHeapManager->CreateCPUHandle(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			sNullSRV3DHandle = mDescriptorHeapManager->CreateCPUHandle(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

			D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
			srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MipLevels = 1;
			mDevice->CreateShaderResourceView(nullptr, &srvDesc, sNullSRV2DHandle.GetCPUHandle());

			srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
			srvDesc.Texture3D.MipLevels = 1;
			mDevice->CreateShaderResourceView(nullptr, &srvDesc, sNullSRV3DHandle.GetCPUHandle());
		}

		return true;
	}

	void ER_RHI_DX12::BeginGraphicsCommandList(int index)
	{
		assert(index < ER_RHI_MAX_GRAPHICS_COMMAND_LISTS);

		mCurrentGraphicsCommandListIndex = index;

		HRESULT hr;
		if (FAILED(hr = mCommandAllocatorsGraphics[index]->Reset()))
		{
			std::string message = "ER_RHI_DX12:: Could not Reset() command allocator (graphics) " + std::to_string(index);
			throw ER_CoreException(message.c_str());
		}

		if (FAILED(hr = mCommandListGraphics[index]->Reset(mCommandAllocatorsGraphics[index].Get(), nullptr)))
		{
			std::string message = "ER_RHI_DX12:: Could not Reset() command list (graphics) " + std::to_string(index);
			throw ER_CoreException(message.c_str());
		}
	}

	void ER_RHI_DX12::EndGraphicsCommandList(int index)
	{
		assert(index < ER_RHI_MAX_GRAPHICS_COMMAND_LISTS);
		mCurrentGraphicsCommandListIndex = -1;

		HRESULT hr;
		if (FAILED(hr = mCommandListGraphics[index]->Close()))
		{
			std::string message = "ER_RHI_DX12:: Could not close command list (graphics) " + std::to_string(index);
			throw ER_CoreException(message.c_str());
		}
	}

	void ER_RHI_DX12::ClearMainRenderTarget(float colors[4])
	{
		mCommandListGraphics[0]->ClearRenderTargetView(mMainRTVDescriptorHandle, colors, 0, nullptr);
	}

	void ER_RHI_DX12::ClearMainDepthStencilTarget(float depth, UINT stencil /*= 0*/)
	{
		mCommandListGraphics[0]->ClearDepthStencilView(mMainDSVDescriptorHandle, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, depth, stencil, 0, nullptr);
	}

	void ER_RHI_DX12::ClearRenderTarget(ER_RHI_GPUTexture* aRenderTarget, float colors[4], int rtvArrayIndex)
	{
		assert(aRenderTarget);

		if (rtvArrayIndex > 0)
		{
			ER_RHI_DX12_DescriptorHandle& handle = static_cast<ER_RHI_DX12_GPUTexture*>(aRenderTarget)->GetRTVHandle(rtvArrayIndex);
			mCommandListGraphics[0]->ClearRenderTargetView(handle.GetCPUHandle(), colors, 0, nullptr);
		}
		else
		{
			ER_RHI_DX12_DescriptorHandle& handle = static_cast<ER_RHI_DX12_GPUTexture*>(aRenderTarget)->GetRTVHandle();
			mCommandListGraphics[0]->ClearRenderTargetView(handle.GetCPUHandle(), colors, 0, nullptr);
		}
	}

	void ER_RHI_DX12::ClearDepthStencilTarget(ER_RHI_GPUTexture* aDepthTarget, float depth, UINT stencil /*= 0*/)
	{
		assert(aDepthTarget);
		ER_RHI_DX12_GPUTexture* dtDX12 = static_cast<ER_RHI_DX12_GPUTexture*>(aDepthTarget);
		assert(dtDX12);
		mCommandListGraphics[0]->ClearDepthStencilView(dtDX12->GetDSVHandle().GetCPUHandle(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, depth, stencil, 0, nullptr);
	}

	void ER_RHI_DX12::ClearUAV(ER_RHI_GPUResource* aRenderTarget, float colors[4])
	{
		assert(aRenderTarget);
		ER_RHI_DX12_GPUTexture* uavDX12 = static_cast<ER_RHI_DX12_GPUTexture*>(aRenderTarget);
		assert(uavDX12);
		mCommandListGraphics[0]->ClearUnorderedAccessViewFloat(uavDX12->GetUAVHandle().GetGPUHandle(), uavDX12->GetUAVHandle().GetCPUHandle(), 
			static_cast<ID3D12Resource*>(uavDX12->GetResource()), colors, 0, nullptr);
	}

	ER_RHI_InputLayout* ER_RHI_DX12::CreateInputLayout(ER_RHI_INPUT_ELEMENT_DESC* inputElementDescriptions, UINT inputElementDescriptionCount)
	{
		return new ER_RHI_InputLayout(inputElementDescriptions, inputElementDescriptionCount);
	}

	ER_RHI_GPUShader* ER_RHI_DX12::CreateGPUShader()
	{
		return new ER_RHI_DX12_GPUShader();
	}

	ER_RHI_GPUBuffer* ER_RHI_DX12::CreateGPUBuffer()
	{
		return new ER_RHI_DX12_GPUBuffer();
	}

	ER_RHI_GPUTexture* ER_RHI_DX12::CreateGPUTexture()
	{
		return new ER_RHI_DX12_GPUTexture();
	}

	ER_RHI_GPURootSignature* ER_RHI_DX12::CreateRootSignature(UINT NumRootParams /*= 0*/, UINT NumStaticSamplers /*= 0*/)
	{
		return new ER_RHI_DX12_GPURootSignature(NumRootParams, NumStaticSamplers);
	}

	void ER_RHI_DX12::CreateTexture(ER_RHI_GPUTexture* aOutTexture, UINT width, UINT height, UINT samples, ER_RHI_FORMAT format, ER_RHI_BIND_FLAG bindFlags /*= ER_BIND_SHADER_RESOURCE | ER_BIND_RENDER_TARGET*/, int mip /*= 1*/, int depth /*= -1*/, int arraySize /*= 1*/, bool isCubemap /*= false*/, int cubemapArraySize /*= -1*/)
	{
		assert(aOutTexture);
		aOutTexture->CreateGPUTextureResource(this, width, height, samples, format, bindFlags, mip, depth, arraySize, isCubemap, cubemapArraySize);
	}

	void ER_RHI_DX12::CreateTexture(ER_RHI_GPUTexture* aOutTexture, const std::string& aPath, bool isFullPath /*= false*/)
	{
		assert(aOutTexture);
		aOutTexture->CreateGPUTextureResource(this, aPath, isFullPath);
	}

	void ER_RHI_DX12::CreateTexture(ER_RHI_GPUTexture* aOutTexture, const std::wstring& aPath, bool isFullPath /*= false*/)
	{
		assert(aOutTexture);
		aOutTexture->CreateGPUTextureResource(this, aPath, isFullPath);
	}

	void ER_RHI_DX12::CreateBuffer(ER_RHI_GPUBuffer* aOutBuffer, void* aData, UINT objectsCount, UINT byteStride, bool isDynamic /*= false*/, ER_RHI_BIND_FLAG bindFlags /*= 0*/, UINT cpuAccessFlags /*= 0*/, ER_RHI_RESOURCE_MISC_FLAG miscFlags /*= 0*/, ER_RHI_FORMAT format /*= ER_FORMAT_UNKNOWN*/)
	{
		assert(aOutBuffer);
		aOutBuffer->CreateGPUBufferResource(this, aData, objectsCount, byteStride, isDynamic, bindFlags, cpuAccessFlags, miscFlags, format);
	}

	void ER_RHI_DX12::CopyBuffer(ER_RHI_GPUBuffer* aDestBuffer, ER_RHI_GPUBuffer* aSrcBuffer)
	{
		assert(aDestBuffer);
		assert(aSrcBuffer);

		ER_RHI_DX12_GPUBuffer* dstResource = static_cast<ER_RHI_DX12_GPUBuffer*>(aDestBuffer);
		ER_RHI_DX12_GPUBuffer* srcResource = static_cast<ER_RHI_DX12_GPUBuffer*>(aSrcBuffer);
		
		assert(dstResource);
		assert(srcResource);

		mCommandListGraphics[0]->CopyResource(static_cast<ID3D12Resource*>(dstResource->GetResource()), static_cast<ID3D12Resource*>(srcResource->GetResource()));
	}

	void ER_RHI_DX12::BeginBufferRead(ER_RHI_GPUBuffer* aBuffer, void** output)
	{
		assert(aBuffer);
		assert(!mIsContextReadingBuffer);

		ER_RHI_DX12_GPUBuffer* buffer = static_cast<ER_RHI_DX12_GPUBuffer*>(aBuffer);
		assert(buffer);

		buffer->Map(this, output);
		mIsContextReadingBuffer = true;
	}

	void ER_RHI_DX12::EndBufferRead(ER_RHI_GPUBuffer* aBuffer)
	{
		ER_RHI_DX12_GPUBuffer* buffer = static_cast<ER_RHI_DX12_GPUBuffer*>(aBuffer);
		assert(buffer);

		buffer->Unmap(this);
		mIsContextReadingBuffer = false;
	}

	void ER_RHI_DX12::CopyGPUTextureSubresourceRegion(ER_RHI_GPUResource* aDestBuffer, UINT DstSubresource, UINT DstX, UINT DstY, UINT DstZ, ER_RHI_GPUResource* aSrcBuffer, UINT SrcSubresource)
	{
		assert(aDestBuffer);
		assert(aSrcBuffer);

		ER_RHI_DX12_GPUTexture* dstbuffer = static_cast<ER_RHI_DX12_GPUTexture*>(aDestBuffer);
		assert(dstbuffer);
		ER_RHI_DX12_GPUTexture* srcbuffer = static_cast<ER_RHI_DX12_GPUTexture*>(aSrcBuffer);
		assert(srcbuffer);

		D3D12_TEXTURE_COPY_LOCATION dstLocation;
		dstLocation.pResource = static_cast<ID3D12Resource*>(dstbuffer->GetResource());
		dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		dstLocation.SubresourceIndex = DstSubresource;

		D3D12_TEXTURE_COPY_LOCATION srcLocation;
		srcLocation.pResource = static_cast<ID3D12Resource*>(srcbuffer->GetResource());
		srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		srcLocation.SubresourceIndex = SrcSubresource;
		
		mCommandListGraphics[0]->CopyTextureRegion(&dstLocation, DstX, DstY, DstZ, &srcLocation, NULL);
		//else if (dstbuffer->GetTexture3D() && srcbuffer->GetTexture3D())
		//else
		//	throw ER_CoreException("ER_RHI_DX12:: One of the resources is NULL during CopyGPUTextureSubresourceRegion()");
	}

	void ER_RHI_DX12::Draw(UINT VertexCount)
	{
		assert(VertexCount > 0);
		mCommandListGraphics[0]->DrawInstanced(VertexCount, 1, 0, 0);
	}

	void ER_RHI_DX12::DrawIndexed(UINT IndexCount)
	{
		assert(IndexCount > 0);
		mCommandListGraphics[0]->DrawIndexedInstanced(IndexCount, 1, 0, 0, 0);
	}

	void ER_RHI_DX12::DrawInstanced(UINT VertexCountPerInstance, UINT InstanceCount, UINT StartVertexLocation, UINT StartInstanceLocation)
	{
		assert(VertexCountPerInstance > 0);
		assert(InstanceCount > 0);

		mCommandListGraphics[0]->DrawInstanced(VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation);
	}

	void ER_RHI_DX12::DrawIndexedInstanced(UINT IndexCountPerInstance, UINT InstanceCount, UINT StartIndexLocation, INT BaseVertexLocation, UINT StartInstanceLocation)
	{
		assert(IndexCountPerInstance > 0);
		assert(InstanceCount > 0);

		mCommandListGraphics[0]->DrawIndexedInstanced(IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
	}

	void ER_RHI_DX12::Dispatch(UINT ThreadGroupCountX, UINT ThreadGroupCountY, UINT ThreadGroupCountZ)
	{
		mCommandListGraphics[0]->Dispatch(ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);
	}

	void ER_RHI_DX12::ExecuteCommandLists(int commandListIndex /*= 0*/, bool isCompute /*= false*/)
	{
		if (!isCompute)
		{
			ID3D12CommandList* ppCommandLists[] = { mCommandListGraphics[commandListIndex].Get() };
			mCommandQueueGraphics->ExecuteCommandLists(1, ppCommandLists);
		}
	}

	void ER_RHI_DX12::GenerateMips(ER_RHI_GPUTexture* aTexture)
	{
		//TODO
	}

	void ER_RHI_DX12::PresentGraphics()
	{
		HRESULT hr = mSwapChain->Present(1, 0);

		if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
		{
#if defined (_DEBUG) || (DEBUG)
			char buff[64] = {};
			sprintf_s(buff, "ER_RHI_DX12: Device Lost on Present: Reason code 0x%08X\n", (hr == DXGI_ERROR_DEVICE_REMOVED) ? mDevice->GetDeviceRemovedReason() : hr);
			OutputDebugStringA(buff);
#endif
			throw ER_CoreException("ER_RHI_DX12: Could not call Present() in the swapchain...", hr);
		}
	}

	void ER_RHI_DX12::PresentCompute()
	{
		//mCommandListCompute->Close();
		//
		//ID3D12CommandList* ppCommandLists[] = { mCommandListCompute };
		//mCommandQueueCompute->ExecuteCommandLists(1, ppCommandLists);

		//UINT64 fenceValue = mFenceValuesCompute;
		//mCommandQueueCompute->Signal(mFenceCompute.Get(), fenceValue);
		//mFenceValuesCompute++;
	}

	bool ER_RHI_DX12::ProjectCubemapToSH(ER_RHI_GPUTexture* aTexture, UINT order, float* resultR, float* resultG, float* resultB)
	{
		//TODO
		return false;
	}

	void ER_RHI_DX12::SaveGPUTextureToFile(ER_RHI_GPUTexture* aTexture, const std::wstring& aPathName)
	{
		//TODO
	}

	void ER_RHI_DX12::SetMainRenderTargets()
	{
		mCommandListGraphics[0]->OMSetRenderTargets(1, &mMainRTVDescriptorHandle, false, &mMainDSVDescriptorHandle);
	}

	void ER_RHI_DX12::SetRenderTargets(const std::vector<ER_RHI_GPUTexture*>& aRenderTargets, ER_RHI_GPUTexture* aDepthTarget /*= nullptr*/, ER_RHI_GPUTexture* aUAV /*= nullptr*/, int rtvArrayIndex)
	{
		if (!aUAV)
		{
			if (rtvArrayIndex > 0)
				assert(aRenderTargets.size() == 1);

			assert(aRenderTargets.size() > 0);
			assert(aRenderTargets.size() <= DX12_MAX_BOUND_RENDER_TARGETS_VIEWS);

			D3D12_CPU_DESCRIPTOR_HANDLE rtvHandles[DX12_MAX_BOUND_RENDER_TARGETS_VIEWS] = {};
			UINT rtCount = static_cast<UINT>(aRenderTargets.size());
			for (UINT i = 0; i < rtCount; i++)
			{
				assert(aRenderTargets[i]);
				if (rtvArrayIndex > 0)
					rtvHandles[i] = static_cast<ER_RHI_DX12_GPUTexture*>(aRenderTargets[i])->GetRTVHandle(rtvArrayIndex).GetCPUHandle();
				else
					rtvHandles[i] = static_cast<ER_RHI_DX12_GPUTexture*>(aRenderTargets[i])->GetRTVHandle().GetCPUHandle();
			}
			TransitionResources(aRenderTargets, ER_RHI_RESOURCE_STATE::ER_RESOURCE_STATE_RENDER_TARGET);
			if (aDepthTarget)
			{
				D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = static_cast<ER_RHI_DX12_GPUTexture*>(aDepthTarget)->GetDSVHandle().GetCPUHandle();
				TransitionResources({ static_cast<ER_RHI_GPUResource*>(aDepthTarget) }, ER_RHI_RESOURCE_STATE::ER_RESOURCE_STATE_DEPTH_WRITE);
				mCommandListGraphics[0]->OMSetRenderTargets(rtCount, rtvHandles, FALSE, &dsvHandle);
			}
			else
				mCommandListGraphics[0]->OMSetRenderTargets(rtCount, rtvHandles, FALSE, NULL);

		}
		else
		{
			assert(0);
			//TODO: OMSetRenderTargetsAndUnorderedAccessViews() is not available on DX12
		}
	}

	void ER_RHI_DX12::SetDepthTarget(ER_RHI_GPUTexture* aDepthTarget)
	{
		assert(aDepthTarget);
		D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = static_cast<ER_RHI_DX12_GPUTexture*>(aDepthTarget)->GetDSVHandle().GetCPUHandle();
		TransitionResources({ static_cast<ER_RHI_GPUResource*>(aDepthTarget) }, ER_RHI_RESOURCE_STATE::ER_RESOURCE_STATE_DEPTH_WRITE);

		mCommandListGraphics[0]->OMSetRenderTargets(1, nullptr, FALSE, &dsvHandle);
	}

	void ER_RHI_DX12::SetRenderTargetFormats(const std::vector<ER_RHI_GPUTexture*>& aRenderTargets, ER_RHI_GPUTexture* aDepthTarget /*= nullptr*/)
	{
		if (mCurrentPSOState == ER_RHI_DX12_PSO_STATE::COMPUTE)
			return;

		assert(mCurrentPSOState == ER_RHI_DX12_PSO_STATE::GRAPHICS);

		ER_RHI_DX12_GraphicsPSO& pso = mGraphicsPSONames.at(mCurrentGraphicsPSOName);
		int rtCount = static_cast<int>(aRenderTargets.size());
		assert(rtCount <= 8);

		DXGI_FORMAT formats[8] = { DXGI_FORMAT_UNKNOWN };
		for (int i = 0; i < rtCount; i++)
			formats[i] = static_cast<ER_RHI_DX12_GPUTexture*>(aRenderTargets[i])->GetFormat();
		pso.SetRenderTargetFormats(rtCount, rtCount > 0 ? formats : nullptr, aDepthTarget ? static_cast<ER_RHI_DX12_GPUTexture*>(aDepthTarget)->GetFormat() : DXGI_FORMAT_UNKNOWN);
	}

	void ER_RHI_DX12::SetMainRenderTargetFormats()
	{
		assert(mCurrentPSOState == ER_RHI_DX12_PSO_STATE::GRAPHICS);

		DXGI_FORMAT formats[8] = { DXGI_FORMAT_UNKNOWN };
		formats[0] = mMainRTBufferFormat;

		ER_RHI_DX12_GraphicsPSO& pso = mGraphicsPSONames.at(mCurrentGraphicsPSOName);
		pso.SetRenderTargetFormats(1, formats, mMainDepthBufferFormat);
	}

	void ER_RHI_DX12::SetDepthStencilState(ER_RHI_DEPTH_STENCIL_STATE aDS, UINT stencilRef)
	{
		if (mCurrentPSOState == ER_RHI_DX12_PSO_STATE::UNSET)
			return;

		assert(mCurrentPSOState == ER_RHI_DX12_PSO_STATE::GRAPHICS);

		auto it = mDepthStates.find(aDS);
		if (it != mDepthStates.end())
		{
			mCurrentDS = aDS;
			ER_RHI_DX12_GraphicsPSO& pso = mGraphicsPSONames.at(mCurrentGraphicsPSOName);
			pso.SetDepthStencilState(it->second);
		}
		else
			throw ER_CoreException("ER_RHI_DX11: DepthStencil state is not found.");
	}

	void ER_RHI_DX12::SetBlendState(ER_RHI_BLEND_STATE aBS, const float BlendFactor[4], UINT SampleMask)
	{
		if (mCurrentPSOState == ER_RHI_DX12_PSO_STATE::UNSET)
			return;

		assert(mCurrentPSOState == ER_RHI_DX12_PSO_STATE::GRAPHICS);

		auto it = mBlendStates.find(aBS);
		if (it != mBlendStates.end())
		{
			mCurrentBS = aBS;
			ER_RHI_DX12_GraphicsPSO& pso = mGraphicsPSONames.at(mCurrentGraphicsPSOName);
			pso.SetBlendState(it->second);
		}
		else
			throw ER_CoreException("ER_RHI_DX11: Blend state is not found.");
	}

	void ER_RHI_DX12::SetRasterizerState(ER_RHI_RASTERIZER_STATE aRS)
	{
		if (mCurrentPSOState == ER_RHI_DX12_PSO_STATE::UNSET)
			return;

		assert(mCurrentPSOState == ER_RHI_DX12_PSO_STATE::GRAPHICS);

		auto it = mRasterizerStates.find(aRS);
		if (it != mRasterizerStates.end())
		{
			mCurrentRS = aRS;
			ER_RHI_DX12_GraphicsPSO& pso = mGraphicsPSONames.at(mCurrentGraphicsPSOName);
			pso.SetRasterizerState(it->second);
		}
		else
			throw ER_CoreException("ER_RHI_DX11: Rasterizer state is not found.");
	}

	void ER_RHI_DX12::SetViewport(const ER_RHI_Viewport& aViewport)
	{
		D3D12_VIEWPORT viewport;
		viewport.TopLeftX = aViewport.TopLeftX;
		viewport.TopLeftY = aViewport.TopLeftY;
		viewport.Width = aViewport.Width;
		viewport.Height = aViewport.Height;
		viewport.MinDepth = aViewport.MinDepth;
		viewport.MaxDepth = aViewport.MaxDepth;

		mCurrentViewport = aViewport;

		mCommandListGraphics[0]->RSSetViewports(1, &viewport);
	}

	void ER_RHI_DX12::SetRect(const ER_RHI_Rect& rect)
	{
		D3D12_RECT currentRect = { rect.left, rect.top, rect.right, rect.bottom };
		mCommandListGraphics[0]->RSSetScissorRects(1, &currentRect);
	}

	void ER_RHI_DX12::SetShader(ER_RHI_GPUShader* aShader)
	{
		assert(aShader);

		assert(mCurrentPSOState != ER_RHI_DX12_PSO_STATE::UNSET);

		ER_RHI_DX12_GPUShader* aDX12_Shader = static_cast<ER_RHI_DX12_GPUShader*>(aShader);
		assert(aDX12_Shader);

		ID3DBlob* blob = static_cast<ID3DBlob*>(aDX12_Shader->GetShaderObject());
		assert(blob);

		if (mCurrentPSOState == ER_RHI_DX12_PSO_STATE::GRAPHICS)
		{
			ER_RHI_DX12_GraphicsPSO& pso = mGraphicsPSONames.at(mCurrentGraphicsPSOName);

			switch (aShader->mShaderType)
			{
			case ER_RHI_SHADER_TYPE::ER_VERTEX:
				pso.SetVertexShader(blob->GetBufferPointer(), blob->GetBufferSize());
				break;
			case ER_RHI_SHADER_TYPE::ER_GEOMETRY:
				pso.SetGeometryShader(blob->GetBufferPointer(), blob->GetBufferSize());
				break;
			case ER_RHI_SHADER_TYPE::ER_TESSELLATION_HULL:
				pso.SetHullShader(blob->GetBufferPointer(), blob->GetBufferSize());
				break;
			case ER_RHI_SHADER_TYPE::ER_TESSELLATION_DOMAIN:
				pso.SetDomainShader(blob->GetBufferPointer(), blob->GetBufferSize());
				break;
			case ER_RHI_SHADER_TYPE::ER_PIXEL:
				pso.SetPixelShader(blob->GetBufferPointer(), blob->GetBufferSize());
				break;
			}
		}
		else
		{
			ER_RHI_DX12_ComputePSO& pso = mComputePSONames.at(mCurrentComputePSOName);
			pso.SetComputeShader(blob->GetBufferPointer(), blob->GetBufferSize());
		}
	}

	void ER_RHI_DX12::SetShaderResources(ER_RHI_SHADER_TYPE aShaderType, const std::vector<ER_RHI_GPUResource*>& aSRVs, UINT startSlot /*= 0*/,
		ER_RHI_GPURootSignature* rs, int rootParamIndex, bool isComputeRS)
	{
		int srvCount = static_cast<int>(aSRVs.size());
		assert(rs);
		assert(rootParamIndex >= 0 && rootParamIndex < rs->GetRootParameterCount());
		assert(srvCount > 0 && srvCount <= DX12_MAX_BOUND_SHADER_RESOURCE_VIEWS);
		//assert(srvCount <= rs->GetRootParameterSRVCount(rootParamIndex));
		assert(mDescriptorHeapManager);

		ER_RHI_DX12_GPUDescriptorHeap* gpuDescriptorHeap = mDescriptorHeapManager->GetGPUHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		ER_RHI_DX12_DescriptorHandle& srvHandle = gpuDescriptorHeap->GetHandleBlock(srvCount);
		for (int i = 0; i < srvCount; i++)
		{
			if (aSRVs[i])
				gpuDescriptorHeap->AddToHandle(mDevice.Get(), srvHandle, static_cast<ER_RHI_DX12_GPUTexture*>(aSRVs[i])->GetSRVHandle());
			else
				gpuDescriptorHeap->AddToHandle(mDevice.Get(), srvHandle, sNullSRV2DHandle);

		}

		TransitionResources(aSRVs, aShaderType == ER_RHI_SHADER_TYPE::ER_PIXEL ? ER_RHI_RESOURCE_STATE::ER_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : ER_RHI_RESOURCE_STATE::ER_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

		if (!isComputeRS)
			mCommandListGraphics[0]->SetGraphicsRootDescriptorTable(rootParamIndex, srvHandle.GetGPUHandle());
		else
			mCommandListGraphics[0]->SetComputeRootDescriptorTable(rootParamIndex, srvHandle.GetGPUHandle());

		//TODO compute queue
	}

	void ER_RHI_DX12::SetUnorderedAccessResources(ER_RHI_SHADER_TYPE aShaderType, const std::vector<ER_RHI_GPUResource*>& aUAVs, UINT startSlot /*= 0*/,
		ER_RHI_GPURootSignature* rs, int rootParamIndex, bool isComputeRS)
	{
		int uavCount = static_cast<int>(aUAVs.size());
		assert(rs);
		assert(rootParamIndex >= 0 && rootParamIndex < rs->GetRootParameterCount());
		assert(uavCount > 0 && uavCount <= DX12_MAX_BOUND_UNORDERED_ACCESS_VIEWS);
		//assert(uavCount <= rs->GetRootParameterUAVCount(rootParamIndex));
		assert(mDescriptorHeapManager);

		ER_RHI_DX12_GPUDescriptorHeap* gpuDescriptorHeap = mDescriptorHeapManager->GetGPUHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		ER_RHI_DX12_DescriptorHandle& uavHandle = gpuDescriptorHeap->GetHandleBlock(uavCount);
		for (int i = 0; i < uavCount; i++)
		{
			assert(aUAVs[i]);
			if (aUAVs[i]->IsBuffer())
				gpuDescriptorHeap->AddToHandle(mDevice.Get(), uavHandle, static_cast<ER_RHI_DX12_GPUBuffer*>(aUAVs[i])->GetUAVDescriptorHandle());
			else
				gpuDescriptorHeap->AddToHandle(mDevice.Get(), uavHandle, static_cast<ER_RHI_DX12_GPUTexture*>(aUAVs[i])->GetUAVHandle());
		}

		TransitionResources(aUAVs, ER_RHI_RESOURCE_STATE::ER_RESOURCE_STATE_UNORDERED_ACCESS);

		if (!isComputeRS)
			mCommandListGraphics[0]->SetGraphicsRootDescriptorTable(rootParamIndex, uavHandle.GetGPUHandle());
		else
			mCommandListGraphics[0]->SetComputeRootDescriptorTable(rootParamIndex, uavHandle.GetGPUHandle());

		//TODO compute queue
	}

	void ER_RHI_DX12::SetConstantBuffers(ER_RHI_SHADER_TYPE aShaderType, const std::vector<ER_RHI_GPUBuffer*>& aCBs, UINT startSlot /*= 0*/,
		ER_RHI_GPURootSignature* rs, int rootParamIndex, bool isComputeRS)
	{
		int cbvCount = static_cast<int>(aCBs.size());
		assert(rs);
		assert(rootParamIndex >= 0 && rootParamIndex < rs->GetRootParameterCount());
		assert(cbvCount > 0 && cbvCount <= DX12_MAX_BOUND_CONSTANT_BUFFERS);
		//assert(cbvCount <= rs->GetRootParameterCBVCount(rootParamIndex));
		assert(mDescriptorHeapManager);

		ER_RHI_DX12_GPUDescriptorHeap* gpuDescriptorHeap = mDescriptorHeapManager->GetGPUHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		ER_RHI_DX12_DescriptorHandle& cbvHandle = gpuDescriptorHeap->GetHandleBlock(cbvCount);
		for (int i = 0; i < cbvCount; i++)
		{
			assert(aCBs[i]);
			gpuDescriptorHeap->AddToHandle(mDevice.Get(), cbvHandle, static_cast<ER_RHI_DX12_GPUBuffer*>(aCBs[i])->GetCBVDescriptorHandle());
		}

		if (!isComputeRS)
			mCommandListGraphics[0]->SetGraphicsRootDescriptorTable(rootParamIndex, cbvHandle.GetGPUHandle());
		else
			mCommandListGraphics[0]->SetComputeRootDescriptorTable(rootParamIndex, cbvHandle.GetGPUHandle());

		//TODO compute queue
	}

	void ER_RHI_DX12::SetSamplers(ER_RHI_SHADER_TYPE aShaderType, const std::vector<ER_RHI_SAMPLER_STATE>& aSamplers, UINT startSlot /*= 0*/, ER_RHI_GPURootSignature* rs)
	{
		assert(rs);
		assert(rs->GetStaticSamplersCount() == aSamplers.size()); // we can do better checks (compare samplers), but thats ok for now

		//nothing here, because we bind static samplers in root signatures
	}

	void ER_RHI_DX12::SetInputLayout(ER_RHI_InputLayout* aIL)
	{
		assert(mCurrentPSOState == ER_RHI_DX12_PSO_STATE::GRAPHICS);
		assert(aIL);

		ER_RHI_DX12_GraphicsPSO& pso = mGraphicsPSONames.at(mCurrentGraphicsPSOName);
		pso.SetInputLayout(this, aIL->mInputElementDescriptionCount, aIL->mInputElementDescriptions);
	}

	void ER_RHI_DX12::SetEmptyInputLayout()
	{
		//is it possible on DX12 without PSO?
	}

	void ER_RHI_DX12::SetIndexBuffer(ER_RHI_GPUBuffer* aBuffer, UINT offset /*= 0*/)
	{
		assert(aBuffer);
		ER_RHI_DX12_GPUBuffer* buf = static_cast<ER_RHI_DX12_GPUBuffer*>(aBuffer);
		assert(buf);

		D3D12_INDEX_BUFFER_VIEW view = buf->GetIndexBufferView();
		mCommandListGraphics[0]->IASetIndexBuffer(&view);
	}

	void ER_RHI_DX12::SetVertexBuffers(const std::vector<ER_RHI_GPUBuffer*>& aVertexBuffers)
	{
		assert(aVertexBuffers.size() > 0 && aVertexBuffers.size() <= ER_RHI_MAX_BOUND_VERTEX_BUFFERS);
		if (aVertexBuffers.size() == 1)
		{
			assert(aVertexBuffers[0]);

			ER_RHI_DX12_GPUBuffer* buffer = static_cast<ER_RHI_DX12_GPUBuffer*>(aVertexBuffers[0]);
			assert(buffer);

			D3D12_VERTEX_BUFFER_VIEW view = buffer->GetVertexBufferView();
			mCommandListGraphics[0]->IASetVertexBuffers(0, 1, &view);
		}
		else //+ instance buffer
		{
			UINT strides[2] = { aVertexBuffers[0]->GetStride(), aVertexBuffers[1]->GetStride() };
			UINT offsets[2] = { 0, 0 };

			assert(aVertexBuffers[0]);
			assert(aVertexBuffers[1]);
			ER_RHI_DX12_GPUBuffer* vertexBuffer = static_cast<ER_RHI_DX12_GPUBuffer*>(aVertexBuffers[0]);
			ER_RHI_DX12_GPUBuffer* instanceBuffer = static_cast<ER_RHI_DX12_GPUBuffer*>(aVertexBuffers[1]);
			assert(vertexBuffer);
			assert(instanceBuffer);

			D3D12_VERTEX_BUFFER_VIEW views[2] = { vertexBuffer->GetVertexBufferView(), instanceBuffer->GetVertexBufferView() };
			mCommandListGraphics[0]->IASetVertexBuffers(0, 2, views);
		}
	}

	void ER_RHI_DX12::SetTopologyType(ER_RHI_PRIMITIVE_TYPE aType)
	{
		mCommandListGraphics[0]->IASetPrimitiveTopology(GetTopology(aType));
	}

	void ER_RHI_DX12::SetRootSignature(ER_RHI_GPURootSignature* rs, bool isCompute)
	{
		assert(rs);
		if (!isCompute)
			mCommandListGraphics[0]->SetGraphicsRootSignature(static_cast<ER_RHI_DX12_GPURootSignature*>(rs)->GetSignature());
		else
			mCommandListGraphics[0]->SetComputeRootSignature(static_cast<ER_RHI_DX12_GPURootSignature*>(rs)->GetSignature());

		//TODO compute queue
	}

	void ER_RHI_DX12::SetTopologyTypeToPSO(const std::string& aName, ER_RHI_PRIMITIVE_TYPE aType)
	{
		if (mCurrentPSOState == ER_RHI_DX12_PSO_STATE::UNSET)
			return;

		assert(mCurrentPSOState == ER_RHI_DX12_PSO_STATE::GRAPHICS);
		assert(mCurrentGraphicsPSOName == aName);
		mGraphicsPSONames.at(aName).SetPrimitiveTopologyType(GetTopologyType(aType));
	}

	ER_RHI_PRIMITIVE_TYPE ER_RHI_DX12::GetCurrentTopologyType()
	{
		//TODO
		return ER_RHI_PRIMITIVE_TYPE::ER_PRIMITIVE_TOPOLOGY_LINELIST;
	}

	void ER_RHI_DX12::SetGPUDescriptorHeap(ER_RHI_DESCRIPTOR_HEAP_TYPE aType, bool aReset)
	{
		assert(mDescriptorHeapManager);

		ER_RHI_DX12_GPUDescriptorHeap* gpuDescriptorHeap = mDescriptorHeapManager->GetGPUHeap(GetHeapType(aType));
		if (aReset)
			gpuDescriptorHeap->Reset();

		ID3D12DescriptorHeap* ppHeaps[] = { gpuDescriptorHeap->GetHeap() };
		mCommandListGraphics[0]->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);
	}

	void ER_RHI_DX12::SetGPUDescriptorHeapImGui()
	{
		ID3D12DescriptorHeap* ppHeaps[] = { mImGuiDescriptorHeap.Get() };
		mCommandListGraphics[0]->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);
	}

	bool ER_RHI_DX12::IsPSOReady(const std::string& aName, bool isCompute)
	{
		if (!isCompute)
		{
			auto it = mGraphicsPSONames.find(aName);
			if (it != mGraphicsPSONames.end())
				return true;
			else
				return false;
		}
		else
		{
			auto it2 = mComputePSONames.find(aName);
			if (it2 != mComputePSONames.end())
				return true;
			else
				return false;
		}
	}

	void ER_RHI_DX12::InitializePSO(const std::string& aName, bool isCompute)
	{
		if (isCompute)
		{
			mComputePSONames.insert(std::make_pair(aName, ER_RHI_DX12_ComputePSO(aName)));
			mCurrentComputePSOName = aName;
			mCurrentPSOState = ER_RHI_DX12_PSO_STATE::COMPUTE;
		}
		else
		{
			mGraphicsPSONames.insert(std::make_pair(aName, ER_RHI_DX12_GraphicsPSO(aName)));
			mCurrentGraphicsPSOName = aName;
			mCurrentPSOState = ER_RHI_DX12_PSO_STATE::GRAPHICS;
		}
	}

	void ER_RHI_DX12::SetRootSignatureToPSO(const std::string& aName, ER_RHI_GPURootSignature* rs, bool isCompute /*= false*/)
	{
		assert(rs);
		ER_RHI_DX12_GPURootSignature* rsDX12 = static_cast<ER_RHI_DX12_GPURootSignature*>(rs);
		assert(rsDX12);

		if (!isCompute)
		{
			assert(mCurrentGraphicsPSOName == aName);
			mGraphicsPSONames.at(aName).SetRootSignature(*rsDX12);
		}
		else
		{
			assert(mCurrentComputePSOName == aName);
			mComputePSONames.at(aName).SetRootSignature(*rsDX12);
		}
	}

	void ER_RHI_DX12::FinalizePSO(const std::string& aName, bool isCompute /*= false*/)
	{
		if (!isCompute)
		{
			assert(mCurrentGraphicsPSOName == aName);
			mGraphicsPSONames.at(aName).Finalize(mDevice.Get());
		}
		else
		{
			assert(mCurrentComputePSOName == aName);
			mComputePSONames.at(aName).Finalize(mDevice.Get());
		}
	}

	void ER_RHI_DX12::SetPSO(const std::string& aName, bool isCompute)
	{
		auto resetPSO = [&](const std::string& name, bool comp)
		{
			std::wstring msg = L"[ER Logger] ER_RHI_DX12: Could not find PSO to set, adding it now and trying to reset: " + ER_Utility::ToWideString(aName) + L'\n';
			ER_OUTPUT_LOG(msg.c_str());
			InitializePSO(name, comp);
			SetPSO(name, comp);
		};

		if (!isCompute)
		{
			auto it = mGraphicsPSONames.find(aName);
			if (it != mGraphicsPSONames.end())
			{
				mCommandListGraphics[0]->SetPipelineState(it->second.GetPipelineStateObject());
				mCurrentGraphicsPSOName = it->first;
				mCurrentPSOState = ER_RHI_DX12_PSO_STATE::GRAPHICS;
			}
			else
				resetPSO(aName, isCompute);
		}
		else
		{
			auto it = mComputePSONames.find(aName);
			if (it != mComputePSONames.end())
			{
				mCommandListGraphics[0]->SetPipelineState(it->second.GetPipelineStateObject());
				mCurrentComputePSOName = it->first;
				mCurrentPSOState = ER_RHI_DX12_PSO_STATE::COMPUTE;
			}
			else
				resetPSO(aName, isCompute);
		}
	}

	void ER_RHI_DX12::UnsetPSO()
	{
		mCurrentPSOState = ER_RHI_DX12_PSO_STATE::UNSET;
		//TODO maybe set null PSO? (not sure its needed/possible in DX12)
	}

	void ER_RHI_DX12::TransitionResources(const std::vector<ER_RHI_GPUResource*>& aResources, const std::vector<ER_RHI_RESOURCE_STATE>& aStates, int cmdListIndex)
	{
		int size = static_cast<int>(aResources.size());
		assert(size > 0 && size == aStates.size());
		std::vector<CD3DX12_RESOURCE_BARRIER> barriers;

		for (int i = 0; i < size; i++)
		{
			if (aResources[i] && aResources[i]->GetCurrentState() != aStates[i])
				barriers.emplace_back(CD3DX12_RESOURCE_BARRIER::Transition(static_cast<ID3D12Resource*>(aResources[i]->GetResource()), GetState(aResources[i]->GetCurrentState()), GetState(aStates[i])));
		}

		if (barriers.size() > 0)
			mCommandListGraphics[cmdListIndex]->ResourceBarrier(size, barriers.data());
	}

	void ER_RHI_DX12::TransitionResources(const std::vector<ER_RHI_GPUResource*>& aResources, ER_RHI_RESOURCE_STATE aState, int cmdListIndex /*= 0*/)
	{
		int size = static_cast<int>(aResources.size());
		std::vector<CD3DX12_RESOURCE_BARRIER> barriers;

		for (int i = 0; i < size; i++)
		{
			if (aResources[i] && aResources[i]->GetCurrentState() != aState)
				barriers.emplace_back(CD3DX12_RESOURCE_BARRIER::Transition(static_cast<ID3D12Resource*>(aResources[i]->GetResource()), GetState(aResources[i]->GetCurrentState()), GetState(aState)));
		}

		if (barriers.size() > 0)
			mCommandListGraphics[cmdListIndex]->ResourceBarrier(size, barriers.data());
	}

	void ER_RHI_DX12::TransitionResources(const std::vector<ER_RHI_GPUTexture*>& aResources, const std::vector<ER_RHI_RESOURCE_STATE>& aStates, int cmdListIndex /*= 0*/)
	{
		int size = static_cast<int>(aResources.size());
		assert(size > 0 && size == aStates.size());

		std::vector<CD3DX12_RESOURCE_BARRIER> barriers;

		for (int i = 0; i < size; i++)
		{
			if (aResources[i] && aResources[i]->GetCurrentState() != aStates[i])
				barriers.emplace_back(CD3DX12_RESOURCE_BARRIER::Transition(static_cast<ID3D12Resource*>(aResources[i]->GetResource()), GetState(aResources[i]->GetCurrentState()), GetState(aStates[i])));
		}
		if (barriers.size() > 0)
			mCommandListGraphics[cmdListIndex]->ResourceBarrier(size, barriers.data());
	}

	void ER_RHI_DX12::TransitionResources(const std::vector<ER_RHI_GPUTexture*>& aResources, ER_RHI_RESOURCE_STATE aState, int cmdListIndex /*= 0*/)
	{
		int size = static_cast<int>(aResources.size());
		std::vector<CD3DX12_RESOURCE_BARRIER> barriers;

		for (int i = 0; i < size; i++)
		{
			if (aResources[i] && aResources[i]->GetCurrentState() != aState)
				barriers.emplace_back(CD3DX12_RESOURCE_BARRIER::Transition(static_cast<ID3D12Resource*>(aResources[i]->GetResource()), GetState(aResources[i]->GetCurrentState()), GetState(aState)));
		}
		if (barriers.size() > 0)
			mCommandListGraphics[cmdListIndex]->ResourceBarrier(size, barriers.data());
	}

	void ER_RHI_DX12::TransitionResources(const std::vector<ER_RHI_GPUBuffer*>& aResources, const std::vector<ER_RHI_RESOURCE_STATE>& aStates, int cmdListIndex /*= 0*/)
	{
		int size = static_cast<int>(aResources.size());
		assert(size > 0 && size == aStates.size());

		std::vector<CD3DX12_RESOURCE_BARRIER> barriers;

		for (int i = 0; i < size; i++)
		{
			if (aResources[i] && aResources[i]->GetCurrentState() != aStates[i])
				barriers.emplace_back(CD3DX12_RESOURCE_BARRIER::Transition(static_cast<ID3D12Resource*>(aResources[i]->GetResource()), GetState(aResources[i]->GetCurrentState()), GetState(aStates[i])));
		}
		if (barriers.size() > 0)
			mCommandListGraphics[cmdListIndex]->ResourceBarrier(size, barriers.data());
	}

	void ER_RHI_DX12::TransitionResources(const std::vector<ER_RHI_GPUBuffer*>& aResources, ER_RHI_RESOURCE_STATE aState, int cmdListIndex /*= 0*/)
	{
		int size = static_cast<int>(aResources.size());
		std::vector<CD3DX12_RESOURCE_BARRIER> barriers;
	
		for (int i = 0; i < size; i++)
		{
			if (aResources[i] && aResources[i]->GetCurrentState() != aState)
				barriers.emplace_back(CD3DX12_RESOURCE_BARRIER::Transition(static_cast<ID3D12Resource*>(aResources[i]->GetResource()), GetState(aResources[i]->GetCurrentState()), GetState(aState)));
		}
		if (barriers.size() > 0)
			mCommandListGraphics[cmdListIndex]->ResourceBarrier(size, barriers.data());
	}

	void ER_RHI_DX12::UnbindRenderTargets()
	{
		mCommandListGraphics[0]->OMSetRenderTargets(0, nullptr, false, nullptr);
	}

	void ER_RHI_DX12::UpdateBuffer(ER_RHI_GPUBuffer* aBuffer, void* aData, int dataSize)
	{
		assert(aBuffer->GetSize() >= dataSize);

		ER_RHI_DX12_GPUBuffer* buffer = static_cast<ER_RHI_DX12_GPUBuffer*>(aBuffer);
		assert(buffer);

		//unsigned int* aOutData;
		//buffer->Map(this, reinterpret_cast<void**>(&aOutData));
		//memcpy(aOutData, aData, dataSize);
		//buffer->Unmap(this);
	}

	void ER_RHI_DX12::InitImGui()
	{
		D3D12_DESCRIPTOR_HEAP_DESC desc = {};
		desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		desc.NumDescriptors = 1;
		desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

		if (FAILED(mDevice->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&mImGuiDescriptorHeap))))
			throw ER_CoreException("ER_RHI_DX12: Could not create a descriptor heap for ImGui");

		ImGui_ImplDX12_Init(mDevice.Get(), 1, mMainRTBufferFormat, mImGuiDescriptorHeap.Get(), mImGuiDescriptorHeap->GetCPUDescriptorHandleForHeapStart(), mImGuiDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
	}

	void ER_RHI_DX12::StartNewImGuiFrame()
	{
		ImGui_ImplDX12_NewFrame();
	}

	void ER_RHI_DX12::RenderDrawDataImGui()
	{
		ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), mCommandListGraphics[0].Get());
	}

	void ER_RHI_DX12::ShutdownImGui()
	{
		ImGui_ImplDX12_Shutdown();
	}

	const D3D12_SAMPLER_DESC& ER_RHI_DX12::FindSamplerState(ER_RHI_SAMPLER_STATE aState)
	{
		auto it = mSamplerStates.find(aState);
		if (it != mSamplerStates.end())
		{
			return it->second;
		}
		else
		{
			return mEmptySampler;
			throw ER_CoreException("ER_RHI_DX12: Sampler state is not found.");
		}
	}

	DXGI_FORMAT ER_RHI_DX12::GetFormat(ER_RHI_FORMAT aFormat)
	{
		switch (aFormat)
		{
		case ER_RHI_FORMAT::ER_FORMAT_R32G32B32A32_TYPELESS:
			return DXGI_FORMAT_R32G32B32A32_TYPELESS;
		case ER_RHI_FORMAT::ER_FORMAT_R32G32B32A32_FLOAT:
			return DXGI_FORMAT_R32G32B32A32_FLOAT;
		case ER_RHI_FORMAT::ER_FORMAT_R32G32B32A32_UINT:
			return DXGI_FORMAT_R32G32B32A32_UINT;
		case ER_RHI_FORMAT::ER_FORMAT_R32G32B32_TYPELESS:
			return DXGI_FORMAT_R32G32B32_TYPELESS;
		case ER_RHI_FORMAT::ER_FORMAT_R32G32B32_FLOAT:
			return DXGI_FORMAT_R32G32B32_FLOAT;
		case ER_RHI_FORMAT::ER_FORMAT_R32G32B32_UINT:
			return DXGI_FORMAT_R32G32B32_UINT;
		case ER_RHI_FORMAT::ER_FORMAT_R16G16B16A16_TYPELESS:
			return DXGI_FORMAT_R16G16B16A16_TYPELESS;
		case ER_RHI_FORMAT::ER_FORMAT_R16G16B16A16_FLOAT:
			return DXGI_FORMAT_R16G16B16A16_FLOAT;
		case ER_RHI_FORMAT::ER_FORMAT_R16G16B16A16_UNORM:
			return DXGI_FORMAT_R16G16B16A16_UNORM;
		case ER_RHI_FORMAT::ER_FORMAT_R16G16B16A16_UINT:
			return DXGI_FORMAT_R16G16B16A16_UINT;
		case ER_RHI_FORMAT::ER_FORMAT_R32G32_TYPELESS:
			return DXGI_FORMAT_R32G32_TYPELESS;
		case ER_RHI_FORMAT::ER_FORMAT_R32G32_FLOAT:
			return DXGI_FORMAT_R32G32_FLOAT;
		case ER_RHI_FORMAT::ER_FORMAT_R32G32_UINT:
			return DXGI_FORMAT_R32G32_UINT;
		case ER_RHI_FORMAT::ER_FORMAT_R10G10B10A2_TYPELESS:
			return DXGI_FORMAT_R10G10B10A2_TYPELESS;
		case ER_RHI_FORMAT::ER_FORMAT_R10G10B10A2_UNORM:
			return DXGI_FORMAT_R10G10B10A2_UNORM;
		case ER_RHI_FORMAT::ER_FORMAT_R10G10B10A2_UINT:
			return DXGI_FORMAT_R10G10B10A2_UINT;
		case ER_RHI_FORMAT::ER_FORMAT_R11G11B10_FLOAT:
			return DXGI_FORMAT_R11G11B10_FLOAT;
		case ER_RHI_FORMAT::ER_FORMAT_R8G8B8A8_TYPELESS:
			return DXGI_FORMAT_R8G8B8A8_TYPELESS;
		case ER_RHI_FORMAT::ER_FORMAT_R8G8B8A8_UNORM:
			return DXGI_FORMAT_R8G8B8A8_UNORM;
		case ER_RHI_FORMAT::ER_FORMAT_R8G8B8A8_UINT:
			return DXGI_FORMAT_R8G8B8A8_UINT;
		case ER_RHI_FORMAT::ER_FORMAT_R16G16_TYPELESS:
			return DXGI_FORMAT_R16G16_TYPELESS;
		case ER_RHI_FORMAT::ER_FORMAT_R16G16_FLOAT:
			return DXGI_FORMAT_R16G16_FLOAT;
		case ER_RHI_FORMAT::ER_FORMAT_R16G16_UNORM:
			return DXGI_FORMAT_R16G16_UNORM;
		case ER_RHI_FORMAT::ER_FORMAT_R16G16_UINT:
			return DXGI_FORMAT_R16G16_UINT;
		case ER_RHI_FORMAT::ER_FORMAT_R32_TYPELESS:
			return DXGI_FORMAT_R32_TYPELESS;
		case ER_RHI_FORMAT::ER_FORMAT_D32_FLOAT:
			return DXGI_FORMAT_D32_FLOAT;
		case ER_RHI_FORMAT::ER_FORMAT_R32_FLOAT:
			return DXGI_FORMAT_R32_FLOAT;
		case ER_RHI_FORMAT::ER_FORMAT_R32_UINT:
			return DXGI_FORMAT_R32_UINT;
		case ER_RHI_FORMAT::ER_FORMAT_D24_UNORM_S8_UINT:
			return DXGI_FORMAT_D24_UNORM_S8_UINT;
		case ER_RHI_FORMAT::ER_FORMAT_R8G8_TYPELESS:
			return DXGI_FORMAT_R8G8_TYPELESS;
		case ER_RHI_FORMAT::ER_FORMAT_R8G8_UNORM:
			return DXGI_FORMAT_R8G8_UNORM;
		case ER_RHI_FORMAT::ER_FORMAT_R8G8_UINT:
			return DXGI_FORMAT_R8G8_UINT;
		case ER_RHI_FORMAT::ER_FORMAT_R16_TYPELESS:
			return DXGI_FORMAT_R16_TYPELESS;
		case ER_RHI_FORMAT::ER_FORMAT_R16_FLOAT:
			return DXGI_FORMAT_R16_FLOAT;
		case ER_RHI_FORMAT::ER_FORMAT_R16_UNORM:
			return DXGI_FORMAT_R16_UNORM;
		case ER_RHI_FORMAT::ER_FORMAT_R16_UINT:
			return DXGI_FORMAT_R16_UINT;
		case ER_RHI_FORMAT::ER_FORMAT_R8_TYPELESS:
			return DXGI_FORMAT_R8_TYPELESS;
		case ER_RHI_FORMAT::ER_FORMAT_R8_UNORM:
			return DXGI_FORMAT_R8_UNORM;
		case ER_RHI_FORMAT::ER_FORMAT_R8_UINT:
			return DXGI_FORMAT_R8_UINT;
		default:
			return DXGI_FORMAT_UNKNOWN;
		}
	}

	ER_RHI_RESOURCE_STATE ER_RHI_DX12::GetState(D3D12_RESOURCE_STATES aState)
	{
		switch (aState)
		{
		//case D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_COMMON:
		//	return ER_RHI_RESOURCE_STATE::ER_RESOURCE_STATE_COMMON;
		case D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER:
			return ER_RHI_RESOURCE_STATE::ER_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
		case D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_INDEX_BUFFER:
			return ER_RHI_RESOURCE_STATE::ER_RESOURCE_STATE_INDEX_BUFFER;
		case D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_RENDER_TARGET:
			return ER_RHI_RESOURCE_STATE::ER_RESOURCE_STATE_RENDER_TARGET;
		case D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_UNORDERED_ACCESS:
			return ER_RHI_RESOURCE_STATE::ER_RESOURCE_STATE_UNORDERED_ACCESS;
		case D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_DEPTH_WRITE:
			return ER_RHI_RESOURCE_STATE::ER_RESOURCE_STATE_DEPTH_WRITE;
		case D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_DEPTH_READ:
			return ER_RHI_RESOURCE_STATE::ER_RESOURCE_STATE_DEPTH_READ;
		case D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE:
			return ER_RHI_RESOURCE_STATE::ER_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		case D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE:
			return ER_RHI_RESOURCE_STATE::ER_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		case D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT:
			return ER_RHI_RESOURCE_STATE::ER_RESOURCE_STATE_INDIRECT_ARGUMENT;
		case D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_COPY_DEST:
			return ER_RHI_RESOURCE_STATE::ER_RESOURCE_STATE_COPY_DEST;
		case D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_COPY_SOURCE:
			return ER_RHI_RESOURCE_STATE::ER_RESOURCE_STATE_COPY_SOURCE;
		case D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_GENERIC_READ:
			return ER_RHI_RESOURCE_STATE::ER_RESOURCE_STATE_GENERIC_READ;
		case D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_PRESENT:
			return ER_RHI_RESOURCE_STATE::ER_RESOURCE_STATE_PRESENT;
		default:
			//throw ER_CoreException("ER_RHI_DX12: Could not convert the resource state!");
			return ER_RHI_RESOURCE_STATE::ER_RESOURCE_STATE_COMMON;
		}
	}

	D3D12_RESOURCE_STATES ER_RHI_DX12::GetState(ER_RHI_RESOURCE_STATE aState)
	{
		switch (aState)
		{
		case ER_RHI_RESOURCE_STATE::ER_RESOURCE_STATE_COMMON:
			return D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_COMMON;
		case ER_RHI_RESOURCE_STATE::ER_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER:
			return D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
		case ER_RHI_RESOURCE_STATE::ER_RESOURCE_STATE_INDEX_BUFFER:
			return D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_INDEX_BUFFER;
		case ER_RHI_RESOURCE_STATE::ER_RESOURCE_STATE_RENDER_TARGET:
			return D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_RENDER_TARGET;
		case ER_RHI_RESOURCE_STATE::ER_RESOURCE_STATE_UNORDERED_ACCESS:
			return D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		case ER_RHI_RESOURCE_STATE::ER_RESOURCE_STATE_DEPTH_WRITE:
			return D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_DEPTH_WRITE;
		case ER_RHI_RESOURCE_STATE::ER_RESOURCE_STATE_DEPTH_READ:
			return D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_DEPTH_READ;
		case ER_RHI_RESOURCE_STATE::ER_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE:
			return D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		case ER_RHI_RESOURCE_STATE::ER_RESOURCE_STATE_PIXEL_SHADER_RESOURCE:
			return D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		case ER_RHI_RESOURCE_STATE::ER_RESOURCE_STATE_INDIRECT_ARGUMENT:
			return D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
		case ER_RHI_RESOURCE_STATE::ER_RESOURCE_STATE_COPY_DEST:
			return D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_COPY_DEST;
		case ER_RHI_RESOURCE_STATE::ER_RESOURCE_STATE_COPY_SOURCE:
			return D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_COPY_SOURCE;
		case ER_RHI_RESOURCE_STATE::ER_RESOURCE_STATE_GENERIC_READ:
			return D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_GENERIC_READ;
		case ER_RHI_RESOURCE_STATE::ER_RESOURCE_STATE_PRESENT:
			return D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_PRESENT;
		default:
			throw ER_CoreException("ER_RHI_DX12: Could not convert the resource state!");
			return D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_COMMON;
		}
	}

	D3D12_SHADER_VISIBILITY ER_RHI_DX12::GetShaderVisibility(ER_RHI_SHADER_VISIBILITY aVis)
	{
		switch (aVis)
		{
		case ER_RHI_SHADER_VISIBILITY::ER_RHI_SHADER_VISIBILITY_ALL:
			return D3D12_SHADER_VISIBILITY::D3D12_SHADER_VISIBILITY_ALL;
		case ER_RHI_SHADER_VISIBILITY::ER_RHI_SHADER_VISIBILITY_VERTEX:
			return D3D12_SHADER_VISIBILITY::D3D12_SHADER_VISIBILITY_VERTEX;
		case ER_RHI_SHADER_VISIBILITY::ER_RHI_SHADER_VISIBILITY_HULL:
			return D3D12_SHADER_VISIBILITY::D3D12_SHADER_VISIBILITY_HULL;
		case ER_RHI_SHADER_VISIBILITY::ER_RHI_SHADER_VISIBILITY_DOMAIN:
			return D3D12_SHADER_VISIBILITY::D3D12_SHADER_VISIBILITY_DOMAIN;
		case ER_RHI_SHADER_VISIBILITY::ER_RHI_SHADER_VISIBILITY_GEOMETRY:
			return D3D12_SHADER_VISIBILITY::D3D12_SHADER_VISIBILITY_GEOMETRY;
		case ER_RHI_SHADER_VISIBILITY::ER_RHI_SHADER_VISIBILITY_PIXEL:
			return D3D12_SHADER_VISIBILITY::D3D12_SHADER_VISIBILITY_PIXEL;
		default:
		{
			assert(0);
			return D3D12_SHADER_VISIBILITY::D3D12_SHADER_VISIBILITY_ALL;
		}
		}
	}

	D3D12_DESCRIPTOR_RANGE_TYPE ER_RHI_DX12::GetDescriptorRangeType(ER_RHI_DESCRIPTOR_RANGE_TYPE aDesc)
	{
		switch (aDesc)
		{
		case ER_RHI_DESCRIPTOR_RANGE_TYPE::ER_RHI_DESCRIPTOR_RANGE_TYPE_SRV:
			return D3D12_DESCRIPTOR_RANGE_TYPE::D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		case ER_RHI_DESCRIPTOR_RANGE_TYPE::ER_RHI_DESCRIPTOR_RANGE_TYPE_UAV:
			return D3D12_DESCRIPTOR_RANGE_TYPE::D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
		case ER_RHI_DESCRIPTOR_RANGE_TYPE::ER_RHI_DESCRIPTOR_RANGE_TYPE_CBV:
			return D3D12_DESCRIPTOR_RANGE_TYPE::D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
		default:
		{
			assert(0);
			return D3D12_DESCRIPTOR_RANGE_TYPE::D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		}
		}
	}

	D3D12_PRIMITIVE_TOPOLOGY_TYPE ER_RHI_DX12::GetTopologyType(ER_RHI_PRIMITIVE_TYPE aType)
	{
		switch (aType)
		{
		case ER_RHI_PRIMITIVE_TYPE::ER_PRIMITIVE_TOPOLOGY_POINTLIST:
			return D3D12_PRIMITIVE_TOPOLOGY_TYPE::D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
		case ER_RHI_PRIMITIVE_TYPE::ER_PRIMITIVE_TOPOLOGY_LINELIST:
			return D3D12_PRIMITIVE_TOPOLOGY_TYPE::D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
		case ER_RHI_PRIMITIVE_TYPE::ER_PRIMITIVE_TOPOLOGY_TRIANGLELIST:
			return D3D12_PRIMITIVE_TOPOLOGY_TYPE::D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		case ER_RHI_PRIMITIVE_TYPE::ER_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:
			return D3D12_PRIMITIVE_TOPOLOGY_TYPE::D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		case ER_RHI_PRIMITIVE_TYPE::ER_PRIMITIVE_TOPOLOGY_CONTROL_POINT_PATCHLIST:
			return D3D12_PRIMITIVE_TOPOLOGY_TYPE::D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
		default:
		{
			assert(0);
			return D3D12_PRIMITIVE_TOPOLOGY_TYPE::D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED;
		}
		}
	}

	ER_RHI_PRIMITIVE_TYPE ER_RHI_DX12::GetTopologyType(D3D_PRIMITIVE_TOPOLOGY aType)
	{
		switch (aType)
		{
		case D3D12_PRIMITIVE_TOPOLOGY::D3D_PRIMITIVE_TOPOLOGY_POINTLIST:
			return ER_RHI_PRIMITIVE_TYPE::ER_PRIMITIVE_TOPOLOGY_POINTLIST;
		case D3D12_PRIMITIVE_TOPOLOGY::D3D_PRIMITIVE_TOPOLOGY_LINELIST:
			return ER_RHI_PRIMITIVE_TYPE::ER_PRIMITIVE_TOPOLOGY_LINELIST;
		case D3D12_PRIMITIVE_TOPOLOGY::D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST:
			return ER_RHI_PRIMITIVE_TYPE::ER_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		case D3D12_PRIMITIVE_TOPOLOGY::D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:
			return ER_RHI_PRIMITIVE_TYPE::ER_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
		case D3D12_PRIMITIVE_TOPOLOGY::D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST:
			return ER_RHI_PRIMITIVE_TYPE::ER_PRIMITIVE_TOPOLOGY_CONTROL_POINT_PATCHLIST;
		default:
		{
			assert(0);
			return ER_RHI_PRIMITIVE_TYPE::ER_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		}
		}
	}

	D3D12_PRIMITIVE_TOPOLOGY ER_RHI_DX12::GetTopology(ER_RHI_PRIMITIVE_TYPE aType)
	{
		switch (aType)
		{
		case ER_RHI_PRIMITIVE_TYPE::ER_PRIMITIVE_TOPOLOGY_POINTLIST:
			return D3D12_PRIMITIVE_TOPOLOGY::D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
		case ER_RHI_PRIMITIVE_TYPE::ER_PRIMITIVE_TOPOLOGY_LINELIST:
			return D3D12_PRIMITIVE_TOPOLOGY::D3D_PRIMITIVE_TOPOLOGY_LINELIST;
		case ER_RHI_PRIMITIVE_TYPE::ER_PRIMITIVE_TOPOLOGY_TRIANGLELIST:
			return D3D12_PRIMITIVE_TOPOLOGY::D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		case ER_RHI_PRIMITIVE_TYPE::ER_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:
			return D3D12_PRIMITIVE_TOPOLOGY::D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
		case ER_RHI_PRIMITIVE_TYPE::ER_PRIMITIVE_TOPOLOGY_CONTROL_POINT_PATCHLIST:
			return D3D12_PRIMITIVE_TOPOLOGY::D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST;
		default:
		{
			assert(0);
			return D3D12_PRIMITIVE_TOPOLOGY::D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
		}
		}
	}

	D3D12_DESCRIPTOR_HEAP_TYPE ER_RHI_DX12::GetHeapType(ER_RHI_DESCRIPTOR_HEAP_TYPE aType)
	{
		switch (aType)
		{
		case ER_RHI_DESCRIPTOR_HEAP_TYPE::ER_RHI_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV:
			return D3D12_DESCRIPTOR_HEAP_TYPE::D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		case ER_RHI_DESCRIPTOR_HEAP_TYPE::ER_RHI_DESCRIPTOR_HEAP_TYPE_SAMPLER:
			return D3D12_DESCRIPTOR_HEAP_TYPE::D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
		case ER_RHI_DESCRIPTOR_HEAP_TYPE::ER_RHI_DESCRIPTOR_HEAP_TYPE_RTV:
			return D3D12_DESCRIPTOR_HEAP_TYPE::D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		case ER_RHI_DESCRIPTOR_HEAP_TYPE::ER_RHI_DESCRIPTOR_HEAP_TYPE_DSV:
			return D3D12_DESCRIPTOR_HEAP_TYPE::D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
		default:
		{
			assert(0);
			return D3D12_DESCRIPTOR_HEAP_TYPE::D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		}
		}
	}

	void ER_RHI_DX12::CreateSamplerStates()
	{
		float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
		float black[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

		// trilinear samplers
		D3D12_SAMPLER_DESC samplerStateDesc;
		ZeroMemory(&samplerStateDesc, sizeof(samplerStateDesc));
		samplerStateDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
		samplerStateDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		samplerStateDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		samplerStateDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		samplerStateDesc.MipLODBias = 0;
		samplerStateDesc.MaxAnisotropy = 1;
		samplerStateDesc.MinLOD = -1000.0f;
		samplerStateDesc.MaxLOD = 1000.0f;
		mSamplerStates.insert(std::make_pair(ER_RHI_SAMPLER_STATE::ER_TRILINEAR_WRAP, samplerStateDesc));

		ZeroMemory(&samplerStateDesc, sizeof(samplerStateDesc));
		samplerStateDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
		samplerStateDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
		samplerStateDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
		samplerStateDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
		mSamplerStates.insert(std::make_pair(ER_RHI_SAMPLER_STATE::ER_TRILINEAR_MIRROR, samplerStateDesc));

		ZeroMemory(&samplerStateDesc, sizeof(samplerStateDesc));
		samplerStateDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
		samplerStateDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		samplerStateDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		samplerStateDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		mSamplerStates.insert(std::make_pair(ER_RHI_SAMPLER_STATE::ER_TRILINEAR_CLAMP, samplerStateDesc));

		ZeroMemory(&samplerStateDesc, sizeof(samplerStateDesc));
		samplerStateDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
		samplerStateDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
		samplerStateDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
		samplerStateDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
		memcpy(samplerStateDesc.BorderColor, reinterpret_cast<FLOAT*>(&black), sizeof(FLOAT) * 4);
		mSamplerStates.insert(std::make_pair(ER_RHI_SAMPLER_STATE::ER_TRILINEAR_BORDER, samplerStateDesc));

		// bilinear samplers
		ZeroMemory(&samplerStateDesc, sizeof(samplerStateDesc));
		samplerStateDesc.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
		samplerStateDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		samplerStateDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		samplerStateDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		mSamplerStates.insert(std::make_pair(ER_RHI_SAMPLER_STATE::ER_BILINEAR_WRAP, samplerStateDesc));

		ZeroMemory(&samplerStateDesc, sizeof(samplerStateDesc));
		samplerStateDesc.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
		samplerStateDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
		samplerStateDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
		samplerStateDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
		mSamplerStates.insert(std::make_pair(ER_RHI_SAMPLER_STATE::ER_BILINEAR_MIRROR, samplerStateDesc));

		ZeroMemory(&samplerStateDesc, sizeof(samplerStateDesc));
		samplerStateDesc.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
		samplerStateDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		samplerStateDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		samplerStateDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		mSamplerStates.insert(std::make_pair(ER_RHI_SAMPLER_STATE::ER_BILINEAR_CLAMP, samplerStateDesc));

		ZeroMemory(&samplerStateDesc, sizeof(samplerStateDesc));
		samplerStateDesc.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
		samplerStateDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
		samplerStateDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
		samplerStateDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
		memcpy(samplerStateDesc.BorderColor, reinterpret_cast<FLOAT*>(&black), sizeof(FLOAT) * 4);
		mSamplerStates.insert(std::make_pair(ER_RHI_SAMPLER_STATE::ER_BILINEAR_BORDER, samplerStateDesc));

		// anisotropic samplers
		ZeroMemory(&samplerStateDesc, sizeof(samplerStateDesc));
		samplerStateDesc.Filter = D3D12_FILTER_ANISOTROPIC;
		samplerStateDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		samplerStateDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		samplerStateDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		mSamplerStates.insert(std::make_pair(ER_RHI_SAMPLER_STATE::ER_ANISOTROPIC_WRAP, samplerStateDesc));

		ZeroMemory(&samplerStateDesc, sizeof(samplerStateDesc));
		samplerStateDesc.Filter = D3D12_FILTER_ANISOTROPIC;
		samplerStateDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
		samplerStateDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
		samplerStateDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
		mSamplerStates.insert(std::make_pair(ER_RHI_SAMPLER_STATE::ER_ANISOTROPIC_MIRROR, samplerStateDesc));

		ZeroMemory(&samplerStateDesc, sizeof(samplerStateDesc));
		samplerStateDesc.Filter = D3D12_FILTER_ANISOTROPIC;
		samplerStateDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		samplerStateDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		samplerStateDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		mSamplerStates.insert(std::make_pair(ER_RHI_SAMPLER_STATE::ER_ANISOTROPIC_CLAMP, samplerStateDesc));

		ZeroMemory(&samplerStateDesc, sizeof(samplerStateDesc));
		samplerStateDesc.Filter = D3D12_FILTER_ANISOTROPIC;
		samplerStateDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
		samplerStateDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
		samplerStateDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
		memcpy(samplerStateDesc.BorderColor, reinterpret_cast<FLOAT*>(&black), sizeof(FLOAT) * 4);
		mSamplerStates.insert(std::make_pair(ER_RHI_SAMPLER_STATE::ER_ANISOTROPIC_BORDER, samplerStateDesc));

		// shadow sampler state
		samplerStateDesc.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
		samplerStateDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
		samplerStateDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
		samplerStateDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
		memcpy(samplerStateDesc.BorderColor, reinterpret_cast<FLOAT*>(&white), sizeof(FLOAT) * 4);
		samplerStateDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
		mSamplerStates.insert(std::make_pair(ER_RHI_SAMPLER_STATE::ER_SHADOW_SS, samplerStateDesc));
	}

	void ER_RHI_DX12::CreateBlendStates()
	{
		D3D12_BLEND_DESC blendStateDescription;
		ZeroMemory(&blendStateDescription, sizeof(D3D12_BLEND_DESC));

		blendStateDescription.AlphaToCoverageEnable = TRUE;
		blendStateDescription.RenderTarget[0].BlendEnable = TRUE;
		blendStateDescription.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
		blendStateDescription.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
		blendStateDescription.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
		blendStateDescription.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
		blendStateDescription.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
		blendStateDescription.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
		blendStateDescription.RenderTarget[0].RenderTargetWriteMask = 0x0f;
		mBlendStates.insert(std::make_pair(ER_RHI_BLEND_STATE::ER_ALPHA_TO_COVERAGE, blendStateDescription));

		blendStateDescription.RenderTarget[0].BlendEnable = FALSE;
		blendStateDescription.AlphaToCoverageEnable = FALSE;
		mBlendStates.insert(std::make_pair(ER_RHI_BLEND_STATE::ER_NO_BLEND, blendStateDescription));
	}

	void ER_RHI_DX12::CreateRasterizerStates()
	{
		D3D12_RASTERIZER_DESC rasterizerStateDesc;
		ZeroMemory(&rasterizerStateDesc, sizeof(rasterizerStateDesc));
		rasterizerStateDesc.FillMode = D3D12_FILL_MODE_SOLID;
		rasterizerStateDesc.CullMode = D3D12_CULL_MODE_BACK;
		rasterizerStateDesc.DepthClipEnable = true;
		mRasterizerStates.insert(std::make_pair(ER_RHI_RASTERIZER_STATE::ER_BACK_CULLING, rasterizerStateDesc));

		ZeroMemory(&rasterizerStateDesc, sizeof(rasterizerStateDesc));
		rasterizerStateDesc.FillMode = D3D12_FILL_MODE_SOLID;
		rasterizerStateDesc.CullMode = D3D12_CULL_MODE_BACK;
		rasterizerStateDesc.FrontCounterClockwise = true;
		rasterizerStateDesc.DepthClipEnable = true;
		mRasterizerStates.insert(std::make_pair(ER_RHI_RASTERIZER_STATE::ER_FRONT_CULLING, rasterizerStateDesc));

		ZeroMemory(&rasterizerStateDesc, sizeof(rasterizerStateDesc));
		rasterizerStateDesc.FillMode = D3D12_FILL_MODE_SOLID;
		rasterizerStateDesc.CullMode = D3D12_CULL_MODE_NONE;
		rasterizerStateDesc.DepthClipEnable = true;
		mRasterizerStates.insert(std::make_pair(ER_RHI_RASTERIZER_STATE::ER_NO_CULLING, rasterizerStateDesc));

		ZeroMemory(&rasterizerStateDesc, sizeof(rasterizerStateDesc));
		rasterizerStateDesc.FillMode = D3D12_FILL_MODE_WIREFRAME;
		rasterizerStateDesc.CullMode = D3D12_CULL_MODE_NONE;
		rasterizerStateDesc.DepthClipEnable = true;
		mRasterizerStates.insert(std::make_pair(ER_RHI_RASTERIZER_STATE::ER_WIREFRAME, rasterizerStateDesc));

		ZeroMemory(&rasterizerStateDesc, sizeof(rasterizerStateDesc));
		rasterizerStateDesc.FillMode = D3D12_FILL_MODE_SOLID;
		rasterizerStateDesc.CullMode = D3D12_CULL_MODE_NONE;
		rasterizerStateDesc.DepthClipEnable = false;
		//rasterizerStateDesc.ScissorEnable = true; TODO!
		mRasterizerStates.insert(std::make_pair(ER_RHI_RASTERIZER_STATE::ER_NO_CULLING_NO_DEPTH_SCISSOR_ENABLED, rasterizerStateDesc));

		ZeroMemory(&rasterizerStateDesc, sizeof(rasterizerStateDesc));
		rasterizerStateDesc.FillMode = D3D12_FILL_MODE_SOLID;
		rasterizerStateDesc.CullMode = D3D12_CULL_MODE_BACK;
		rasterizerStateDesc.DepthClipEnable = false;
		rasterizerStateDesc.DepthBias = 0; //0.05f
		rasterizerStateDesc.SlopeScaledDepthBias = 3.0f;
		rasterizerStateDesc.FrontCounterClockwise = false;
		mRasterizerStates.insert(std::make_pair(ER_RHI_RASTERIZER_STATE::ER_SHADOW_RS, rasterizerStateDesc));
	}

	void ER_RHI_DX12::CreateDepthStencilStates()
	{
		D3D12_DEPTH_STENCIL_DESC depthStencilStateDesc;
		depthStencilStateDesc.DepthEnable = TRUE;
		depthStencilStateDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		depthStencilStateDesc.DepthFunc = D3D12_COMPARISON_FUNC_NEVER;
		depthStencilStateDesc.StencilEnable = FALSE;
		mDepthStates.insert(std::make_pair(ER_RHI_DEPTH_STENCIL_STATE::ER_DEPTH_ONLY_READ_COMPARISON_NEVER, depthStencilStateDesc));

		// read only 
		depthStencilStateDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
		mDepthStates.insert(std::make_pair(ER_RHI_DEPTH_STENCIL_STATE::ER_DEPTH_ONLY_READ_COMPARISON_LESS, depthStencilStateDesc));

		depthStencilStateDesc.DepthFunc = D3D12_COMPARISON_FUNC_EQUAL;
		mDepthStates.insert(std::make_pair(ER_RHI_DEPTH_STENCIL_STATE::ER_DEPTH_ONLY_READ_COMPARISON_EQUAL, depthStencilStateDesc));

		depthStencilStateDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
		mDepthStates.insert(std::make_pair(ER_RHI_DEPTH_STENCIL_STATE::ER_DEPTH_ONLY_READ_COMPARISON_LESS_EQUAL, depthStencilStateDesc));

		depthStencilStateDesc.DepthFunc = D3D12_COMPARISON_FUNC_GREATER;
		mDepthStates.insert(std::make_pair(ER_RHI_DEPTH_STENCIL_STATE::ER_DEPTH_ONLY_READ_COMPARISON_GREATER, depthStencilStateDesc));

		depthStencilStateDesc.DepthFunc = D3D12_COMPARISON_FUNC_NOT_EQUAL;
		mDepthStates.insert(std::make_pair(ER_RHI_DEPTH_STENCIL_STATE::ER_DEPTH_ONLY_READ_COMPARISON_NOT_EQUAL, depthStencilStateDesc));

		depthStencilStateDesc.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
		mDepthStates.insert(std::make_pair(ER_RHI_DEPTH_STENCIL_STATE::ER_DEPTH_ONLY_READ_COMPARISON_GREATER_EQUAL, depthStencilStateDesc));

		depthStencilStateDesc.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		mDepthStates.insert(std::make_pair(ER_RHI_DEPTH_STENCIL_STATE::ER_DEPTH_ONLY_READ_COMPARISON_ALWAYS, depthStencilStateDesc));

		//write
		depthStencilStateDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
		depthStencilStateDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
		mDepthStates.insert(std::make_pair(ER_RHI_DEPTH_STENCIL_STATE::ER_DEPTH_ONLY_WRITE_COMPARISON_LESS, depthStencilStateDesc));

		depthStencilStateDesc.DepthFunc = D3D12_COMPARISON_FUNC_EQUAL;
		mDepthStates.insert(std::make_pair(ER_RHI_DEPTH_STENCIL_STATE::ER_DEPTH_ONLY_WRITE_COMPARISON_EQUAL, depthStencilStateDesc));

		depthStencilStateDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
		mDepthStates.insert(std::make_pair(ER_RHI_DEPTH_STENCIL_STATE::ER_DEPTH_ONLY_WRITE_COMPARISON_LESS_EQUAL, depthStencilStateDesc));

		depthStencilStateDesc.DepthFunc = D3D12_COMPARISON_FUNC_GREATER;
		mDepthStates.insert(std::make_pair(ER_RHI_DEPTH_STENCIL_STATE::ER_DEPTH_ONLY_WRITE_COMPARISON_GREATER, depthStencilStateDesc));

		depthStencilStateDesc.DepthFunc = D3D12_COMPARISON_FUNC_NOT_EQUAL;
		mDepthStates.insert(std::make_pair(ER_RHI_DEPTH_STENCIL_STATE::ER_DEPTH_ONLY_WRITE_COMPARISON_NOT_EQUAL, depthStencilStateDesc));

		depthStencilStateDesc.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
		mDepthStates.insert(std::make_pair(ER_RHI_DEPTH_STENCIL_STATE::ER_DEPTH_ONLY_WRITE_COMPARISON_GREATER_EQUAL, depthStencilStateDesc));

		depthStencilStateDesc.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		mDepthStates.insert(std::make_pair(ER_RHI_DEPTH_STENCIL_STATE::ER_DEPTH_ONLY_WRITE_COMPARISON_ALWAYS, depthStencilStateDesc));
	}

}