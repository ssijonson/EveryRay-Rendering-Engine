#include "stdafx.h"
#include <stdio.h>

#include "Illumination.h"
#include "GameComponent.h"
#include "GameTime.h"
#include "Camera.h"
#include "DirectionalLight.h"
#include "FullScreenRenderTarget.h"
#include "GameException.h"
#include "Model.h"
#include "Mesh.h"
#include "Game.h"
#include "MatrixHelper.h"
#include "MaterialHelper.h"
#include "Utility.h"
#include "VertexDeclarations.h"
#include "RasterizerStates.h"
#include "ShaderCompiler.h"
#include "VoxelizationGIMaterial.h"
#include "ER_RenderToLightProbeMaterial.h"
#include "Scene.h"
#include "ER_GBuffer.h"
#include "ER_ShadowMapper.h"
#include "Foliage.h"
#include "RenderableAABB.h"
#include "ER_GPUBuffer.h"
#include "ER_LightProbe.h"
#include "ER_MaterialsCallbacks.h"

namespace Library {

	const float voxelCascadesSizes[NUM_VOXEL_GI_CASCADES] = { 256.0f, 256.0f };

	Illumination::Illumination(Game& game, Camera& camera, const DirectionalLight& light, const ER_ShadowMapper& shadowMapper, const Scene* scene)
		: 
		GameComponent(game),
		mCamera(camera),
		mDirectionalLight(light),
		mShadowMapper(shadowMapper)
	{
		Initialize(scene);
	}

	Illumination::~Illumination()
	{
		ReleaseObject(mVCTMainCS);
		ReleaseObject(mUpsampleBlurCS);
		ReleaseObject(mVCTVoxelizationDebugVS);
		ReleaseObject(mVCTVoxelizationDebugGS);
		ReleaseObject(mVCTVoxelizationDebugPS);
		ReleaseObject(mDeferredLightingCS);
		ReleaseObject(mForwardLightingVS);
		ReleaseObject(mForwardLightingVS_Instancing);
		ReleaseObject(mForwardLightingPS);
		ReleaseObject(mForwardLightingDiffuseProbesPS);
		ReleaseObject(mForwardLightingSpecularProbesPS);

		ReleaseObject(mDepthStencilStateRW);

		ReleaseObject(mForwardLightingRenderingObjectInputLayout);
		ReleaseObject(mForwardLightingRenderingObjectInputLayout_Instancing);

		DeletePointerCollection(mVCTVoxelCascades3DRTs);
		DeletePointerCollection(mDebugVoxelZonesGizmos);
		DeleteObject(mVCTVoxelizationDebugRT);
		DeleteObject(mVCTMainRT);
		DeleteObject(mVCTUpsampleAndBlurRT);
		DeleteObject(mDepthBuffer);

		mVoxelizationDebugConstantBuffer.Release();
		mVoxelConeTracingConstantBuffer.Release();
		mUpsampleBlurConstantBuffer.Release();
		mDeferredLightingConstantBuffer.Release();
		mForwardLightingConstantBuffer.Release();
		mLightProbesConstantBuffer.Release();
	}

	void Illumination::Initialize(const Scene* scene)
	{
		if (!scene)
			return;
	
		//shaders
		{
			ID3DBlob* blob = nullptr;
			if (FAILED(ShaderCompiler::CompileShader(Utility::GetFilePath(L"content\\shaders\\GI\\VoxelConeTracingVoxelizationDebug.hlsl").c_str(), "VSMain", "vs_5_0", &blob)))
				throw GameException("Failed to load VSMain from shader: VoxelConeTracingVoxelization.hlsl!");
			if (FAILED(mGame->Direct3DDevice()->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), NULL, &mVCTVoxelizationDebugVS)))
				throw GameException("Failed to create vertex shader from VoxelConeTracingVoxelization.hlsl!");
			blob->Release();
			
			if (FAILED(ShaderCompiler::CompileShader(Utility::GetFilePath(L"content\\shaders\\GI\\VoxelConeTracingVoxelizationDebug.hlsl").c_str(), "GSMain", "gs_5_0", &blob)))
				throw GameException("Failed to load GSMain from shader: VoxelConeTracingVoxelization.hlsl!");
			if (FAILED(mGame->Direct3DDevice()->CreateGeometryShader(blob->GetBufferPointer(), blob->GetBufferSize(), NULL, &mVCTVoxelizationDebugGS)))
				throw GameException("Failed to create geometry shader from VoxelConeTracingVoxelization.hlsl!");
			blob->Release();
			
			blob = nullptr;
			if (FAILED(ShaderCompiler::CompileShader(Utility::GetFilePath(L"content\\shaders\\GI\\VoxelConeTracingVoxelizationDebug.hlsl").c_str(), "PSMain", "ps_5_0", &blob)))
				throw GameException("Failed to load PSMain from shader: VoxelConeTracingVoxelization.hlsl!");
			if (FAILED(mGame->Direct3DDevice()->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), NULL, &mVCTVoxelizationDebugPS)))
				throw GameException("Failed to create pixel shader from VoxelConeTracingVoxelization.hlsl!");
			blob->Release();
			
			blob = nullptr;
			if (FAILED(ShaderCompiler::CompileShader(Utility::GetFilePath(L"content\\shaders\\GI\\VoxelConeTracingMain.hlsl").c_str(), "CSMain", "cs_5_0", &blob)))
				throw GameException("Failed to load CSMain from shader: VoxelConeTracingMain.hlsl!");
			if (FAILED(mGame->Direct3DDevice()->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), NULL, &mVCTMainCS)))
				throw GameException("Failed to create shader from VoxelConeTracingMain.hlsl!");
			blob->Release();

			blob = nullptr;
			if (FAILED(ShaderCompiler::CompileShader(Utility::GetFilePath(L"content\\shaders\\UpsampleBlur.hlsl").c_str(), "CSMain", "cs_5_0", &blob)))
				throw GameException("Failed to load CSMain from shader: UpsampleBlur.hlsl!");
			if (FAILED(mGame->Direct3DDevice()->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), NULL, &mUpsampleBlurCS)))
				throw GameException("Failed to create shader from UpsampleBlur.hlsl!");
			blob->Release();

			blob = nullptr;
			if (FAILED(ShaderCompiler::CompileShader(Utility::GetFilePath(L"content\\shaders\\DeferredLighting.hlsl").c_str(), "CSMain", "cs_5_0", &blob)))
				throw GameException("Failed to load CSMain from shader: DeferredLighting.hlsl!");
			if (FAILED(mGame->Direct3DDevice()->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), NULL, &mDeferredLightingCS)))
				throw GameException("Failed to create shader from DeferredLighting.hlsl!");
			blob->Release();

			blob = nullptr;
			if (FAILED(ShaderCompiler::CompileShader(Utility::GetFilePath(L"content\\shaders\\ForwardLighting.hlsl").c_str(), "VSMain", "vs_5_0", &blob)))
				throw GameException("Failed to load VSMain from shader: ForwardLighting.hlsl!");
			if (FAILED(mGame->Direct3DDevice()->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), NULL, &mForwardLightingVS)))
				throw GameException("Failed to create vertex shader from ForwardLighting.hlsl!");
			D3D11_INPUT_ELEMENT_DESC inputElementDescriptions[] =
			{
				{ "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
				{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
				{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
				{ "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 }
			};
			if (FAILED(GetGame()->Direct3DDevice()->CreateInputLayout(inputElementDescriptions, ARRAYSIZE(inputElementDescriptions), blob->GetBufferPointer(), blob->GetBufferSize(), &mForwardLightingRenderingObjectInputLayout)))
				throw GameException("ID3D11Device::CreateInputLayout() failed for Forward Lighting Input Layout.");

			blob = nullptr;
			if (FAILED(ShaderCompiler::CompileShader(Utility::GetFilePath(L"content\\shaders\\ForwardLighting.hlsl").c_str(), "VSMain_instancing", "vs_5_0", &blob)))
				throw GameException("Failed to load VSMain from shader: ForwardLighting.hlsl!");
			if (FAILED(mGame->Direct3DDevice()->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), NULL, &mForwardLightingVS_Instancing)))
				throw GameException("Failed to create vertex shader from ForwardLighting.hlsl!");
			D3D11_INPUT_ELEMENT_DESC inputElementDescriptionsInstancing[] =
			{
				{ "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
				{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
				{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
				{ "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
				{ "WORLD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
				{ "WORLD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
				{ "WORLD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
				{ "WORLD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 48, D3D11_INPUT_PER_INSTANCE_DATA, 1 }
			};
			if (FAILED(GetGame()->Direct3DDevice()->CreateInputLayout(inputElementDescriptionsInstancing, ARRAYSIZE(inputElementDescriptionsInstancing), blob->GetBufferPointer(), blob->GetBufferSize(), &mForwardLightingRenderingObjectInputLayout_Instancing)))
				throw GameException("ID3D11Device::CreateInputLayout() failed for Forward Lighting Input Layout (Instancing).");

			blob->Release();

			blob = nullptr;
			if (FAILED(ShaderCompiler::CompileShader(Utility::GetFilePath(L"content\\shaders\\ForwardLighting.hlsl").c_str(), "PSMain", "ps_5_0", &blob)))
				throw GameException("Failed to load PSMain from shader: ForwardLighting.hlsl!");
			if (FAILED(mGame->Direct3DDevice()->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), NULL, &mForwardLightingPS)))
				throw GameException("Failed to create main pixel shader from ForwardLighting.hlsl!");
			blob->Release();

			blob = nullptr;
			if (FAILED(ShaderCompiler::CompileShader(Utility::GetFilePath(L"content\\shaders\\ForwardLighting.hlsl").c_str(), "PSMain_DiffuseProbes", "ps_5_0", &blob)))
				throw GameException("Failed to load PSMain_DiffuseProbes from shader: ForwardLighting.hlsl!");
			if (FAILED(mGame->Direct3DDevice()->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), NULL, &mForwardLightingDiffuseProbesPS)))
				throw GameException("Failed to create diffuse probes pixel shader from ForwardLighting.hlsl!");
			blob->Release();	
			
			blob = nullptr;
			if (FAILED(ShaderCompiler::CompileShader(Utility::GetFilePath(L"content\\shaders\\ForwardLighting.hlsl").c_str(), "PSMain_SpecularProbes", "ps_5_0", &blob)))
				throw GameException("Failed to load PSMain_SpecularProbes from shader: ForwardLighting.hlsl!");
			if (FAILED(mGame->Direct3DDevice()->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), NULL, &mForwardLightingSpecularProbesPS)))
				throw GameException("Failed to create specular probes pixel shader from ForwardLighting.hlsl!");
			blob->Release();
		}
		
		//cbuffers
		{
			mVoxelizationDebugConstantBuffer.Initialize(mGame->Direct3DDevice());
			mVoxelConeTracingConstantBuffer.Initialize(mGame->Direct3DDevice());
			mUpsampleBlurConstantBuffer.Initialize(mGame->Direct3DDevice());
			mDeferredLightingConstantBuffer.Initialize(mGame->Direct3DDevice());
			mForwardLightingConstantBuffer.Initialize(mGame->Direct3DDevice());
			mLightProbesConstantBuffer.Initialize(mGame->Direct3DDevice());
		}

		//RTs and gizmos
		{
			for (int i = 0; i < NUM_VOXEL_GI_CASCADES; i++)
			{
				mVCTVoxelCascades3DRTs.push_back(new ER_GPUTexture(mGame->Direct3DDevice(), voxelCascadesSizes[i], voxelCascadesSizes[i], 1u, 
					DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS, 6, voxelCascadesSizes[i]));
				
				mVoxelCameraPositions[i] = XMFLOAT4(mCamera.Position().x, mCamera.Position().y, mCamera.Position().z, 1.0f);
				
				mDebugVoxelZonesGizmos.push_back(new RenderableAABB(*mGame, mCamera, XMFLOAT4(0.1f, 0.34f, 0.1f, 1.0f)));
				mDebugVoxelZonesGizmos[i]->Initialize();
				float maxBB = voxelCascadesSizes[i] / mWorldVoxelScales[i] * 0.5f;
				mVoxelCascadesAABBs[i].first = XMFLOAT3(-maxBB, -maxBB, -maxBB);
				mVoxelCascadesAABBs[i].second = XMFLOAT3(maxBB, maxBB, maxBB);
				mDebugVoxelZonesGizmos[i]->InitializeGeometry({ mVoxelCascadesAABBs[i].first, mVoxelCascadesAABBs[i].second }, XMMatrixScaling(1, 1, 1));
				mDebugVoxelZonesGizmos[i]->SetPosition(XMFLOAT3(mVoxelCameraPositions[i].x, mVoxelCameraPositions[i].y, mVoxelCameraPositions[i].z));
			}
			mVCTMainRT = new ER_GPUTexture(mGame->Direct3DDevice(), 
				static_cast<UINT>(mGame->ScreenWidth()) * VCT_GI_MAIN_PASS_DOWNSCALE, static_cast<UINT>(mGame->ScreenHeight()) * VCT_GI_MAIN_PASS_DOWNSCALE, 1u, 
				DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, 1);
			mVCTUpsampleAndBlurRT = new ER_GPUTexture(mGame->Direct3DDevice(), static_cast<UINT>(mGame->ScreenWidth()), static_cast<UINT>(mGame->ScreenHeight()), 1u,
				DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, 1);
			mVCTVoxelizationDebugRT = new ER_GPUTexture(mGame->Direct3DDevice(), static_cast<UINT>(mGame->ScreenWidth()), static_cast<UINT>(mGame->ScreenHeight()), 1u,
				DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, 1);
			mDepthBuffer = DepthTarget::Create(mGame->Direct3DDevice(), mGame->ScreenWidth(), mGame->ScreenHeight(), 1u, DXGI_FORMAT_D24_UNORM_S8_UINT);
		}

		//callbacks for materials updates
		ER_MaterialSystems materialSystems;
		materialSystems.mCamera = &mCamera;
		materialSystems.mDirectionalLight = &mDirectionalLight;
		materialSystems.mShadowMapper = &mShadowMapper;
		materialSystems.mProbesManager = mProbesManager;

		for (auto& obj : scene->objects)
		{
			if (obj.second->IsForwardShading())
				mForwardPassObjects.emplace(obj);

			if (obj.second->IsInVoxelization())
			{
				for (int voxelCascadeIndex = 0; voxelCascadeIndex < NUM_VOXEL_GI_CASCADES; voxelCascadeIndex++)
					obj.second->MeshMaterialVariablesUpdateEvent->AddListener(MaterialHelper::voxelizationGIMaterialName + "_" + std::to_string(voxelCascadeIndex),
						[&, voxelCascadeIndex, matSystems = materialSystems](int meshIndex) { ER_MaterialsCallbacks::UpdateVoxelizationGIMaterialVariables(matSystems, obj.second, meshIndex, voxelCascadeIndex); });
			}
		}
	}

	//deferred rendering approach
	void Illumination::DrawLocalIllumination(ER_GBuffer* gbuffer, ER_GPUTexture* aRenderTarget, bool isEditorMode, bool clearInitTarget)
	{
		DrawDeferredLighting(gbuffer, aRenderTarget, clearInitTarget);
		DrawForwardLighting(gbuffer, aRenderTarget);

		if (isEditorMode) //todo move to a separate debug renderer
		{
			DrawDebugGizmos();
		}
	}

	//voxel GI based on "Interactive Indirect Illumination Using Voxel Cone Tracing" by C.Crassin et al.
	//https://research.nvidia.com/sites/default/files/pubs/2011-09_Interactive-Indirect-Illumination/GIVoxels-pg2011-authors.pdf
	void Illumination::DrawGlobalIllumination(ER_GBuffer* gbuffer, const GameTime& gameTime)
	{
		static const float clearColorBlack[] = { 0.0f, 0.0f, 0.0f, 0.0f };
		ID3D11DeviceContext* context = mGame->Direct3DDeviceContext();

		if (!mEnabled)
		{
			context->ClearUnorderedAccessViewFloat(mVCTMainRT->GetUAV(), clearColorBlack);
			context->ClearUnorderedAccessViewFloat(mVCTUpsampleAndBlurRT->GetUAV(), clearColorBlack);
			return;
		}

		D3D11_RECT rect = { 0.0f, 0.0f, mGame->ScreenWidth(), mGame->ScreenHeight() };
		D3D11_VIEWPORT viewport;
		UINT num_viewport = 1;
		context->RSGetViewports(&num_viewport, &viewport);

		ID3D11RasterizerState* oldRS;
		context->RSGetState(&oldRS);

		ID3D11RenderTargetView* nullRTVs[1] = { NULL };

		//voxelization
		{
			for (int cascade = 0; cascade < NUM_VOXEL_GI_CASCADES; cascade++)
			{
				D3D11_VIEWPORT vctViewport = { 0.0f, 0.0f, voxelCascadesSizes[cascade], voxelCascadesSizes[cascade] };
				D3D11_RECT vctRect = { 0.0f, 0.0f, voxelCascadesSizes[cascade], voxelCascadesSizes[cascade] };
				ID3D11UnorderedAccessView* UAV[1] = { mVCTVoxelCascades3DRTs[cascade]->GetUAV() };

				context->RSSetState(RasterizerStates::NoCullingNoDepthEnabledScissorRect);
				context->RSSetViewports(1, &vctViewport);
				context->RSSetScissorRects(1, &vctRect);
				context->OMSetRenderTargets(1, nullRTVs, nullptr);
				/*if (mVoxelCameraPositionsUpdated)
					*/context->ClearUnorderedAccessViewFloat(UAV[0], clearColorBlack);

				std::string materialName = MaterialHelper::voxelizationGIMaterialName + "_" + std::to_string(cascade);
				for (auto& obj : mVoxelizationObjects[cascade]) {
					auto material = static_cast<Rendering::VoxelizationGIMaterial*>(obj.second->GetMaterials()[materialName]);
					if (material)
					{
						material->VoxelCameraPos() << XMVECTOR{
							mVoxelCameraPositions[cascade].x,
							mVoxelCameraPositions[cascade].y,
							mVoxelCameraPositions[cascade].z, 1.0f };
						material->WorldVoxelScale() << mWorldVoxelScales[cascade];
						material->GetEffect()->GetEffect()->GetVariableByName("outputTexture")->AsUnorderedAccessView()->SetUnorderedAccessView(UAV[0]);
						obj.second->Draw(materialName);
					}
				}

				//voxelize extra objects
				{
					if (cascade == 0)
						mFoliageSystem->Draw(gameTime, &mShadowMapper, FoliageRenderingPass::VOXELIZATION);
				}

				//reset back
				context->RSSetViewports(1, &viewport);
				context->RSSetScissorRects(1, &rect);
			}
		}

		//voxelization debug 
		if (mDrawVoxelization)
		{
			int cascade = 0; //TODO fix for multiple cascades
			{
				float sizeTranslateShift = -voxelCascadesSizes[cascade] / mWorldVoxelScales[cascade] * 0.5f;
				mVoxelizationDebugConstantBuffer.Data.WorldVoxelCube =
					XMMatrixTranslation(
						sizeTranslateShift + mVoxelCameraPositions[cascade].x,
						sizeTranslateShift - mVoxelCameraPositions[cascade].y,
						sizeTranslateShift + mVoxelCameraPositions[cascade].z);
				mVoxelizationDebugConstantBuffer.Data.ViewProjection = XMMatrixTranspose(mCamera.ViewMatrix() * mCamera.ProjectionMatrix());
				mVoxelizationDebugConstantBuffer.ApplyChanges(context);

				ID3D11Buffer* CBs[1] = { mVoxelizationDebugConstantBuffer.Buffer() };
				ID3D11ShaderResourceView* SRVs[1] = { mVCTVoxelCascades3DRTs[cascade]->GetSRV() };
				ID3D11RenderTargetView* RTVs[1] = { mVCTVoxelizationDebugRT->GetRTV() };

				context->RSSetState(RasterizerStates::BackCulling);
				context->OMSetRenderTargets(1, RTVs, mDepthBuffer->getDSV());
				context->OMSetDepthStencilState(mDepthStencilStateRW, 0);
				context->ClearRenderTargetView(mVCTVoxelizationDebugRT->GetRTV(), clearColorBlack);
				context->ClearDepthStencilView(mDepthBuffer->getDSV(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);

				context->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_POINTLIST);
				context->IASetInputLayout(nullptr);

				context->VSSetShader(mVCTVoxelizationDebugVS, NULL, NULL);
				context->VSSetConstantBuffers(0, 1, CBs);
				context->VSSetShaderResources(0, 1, SRVs);

				context->GSSetShader(mVCTVoxelizationDebugGS, NULL, NULL);
				context->GSSetConstantBuffers(0, 1, CBs);

				context->PSSetShader(mVCTVoxelizationDebugPS, NULL, NULL);
				context->PSSetConstantBuffers(0, 1, CBs);

				context->DrawInstanced(voxelCascadesSizes[cascade] * voxelCascadesSizes[cascade] * voxelCascadesSizes[cascade], 1, 0, 0);

				context->RSSetState(oldRS);
				context->OMSetRenderTargets(1, nullRTVs, NULL);
			}
		}
		else
		{
			context->OMSetRenderTargets(1, nullRTVs, NULL);
			for (int i = 0; i < NUM_VOXEL_GI_CASCADES; i++)
			{
				context->GenerateMips(mVCTVoxelCascades3DRTs[i]->GetSRV());
				mVoxelConeTracingConstantBuffer.Data.VoxelCameraPositions[i] = mVoxelCameraPositions[i];
				mVoxelConeTracingConstantBuffer.Data.WorldVoxelScales[i] = XMFLOAT4(mWorldVoxelScales[i], 0.0, 0.0, 0.0);
			}
			mVoxelConeTracingConstantBuffer.Data.CameraPos = XMFLOAT4(mCamera.Position().x, mCamera.Position().y, mCamera.Position().z, 1);
			mVoxelConeTracingConstantBuffer.Data.UpsampleRatio = XMFLOAT2(1.0f / VCT_GI_MAIN_PASS_DOWNSCALE, 1.0f / VCT_GI_MAIN_PASS_DOWNSCALE);
			mVoxelConeTracingConstantBuffer.Data.IndirectDiffuseStrength = mVCTIndirectDiffuseStrength;
			mVoxelConeTracingConstantBuffer.Data.IndirectSpecularStrength = mVCTIndirectSpecularStrength;
			mVoxelConeTracingConstantBuffer.Data.MaxConeTraceDistance = mVCTMaxConeTraceDistance;
			mVoxelConeTracingConstantBuffer.Data.AOFalloff = mVCTAoFalloff;
			mVoxelConeTracingConstantBuffer.Data.SamplingFactor = mVCTSamplingFactor;
			mVoxelConeTracingConstantBuffer.Data.VoxelSampleOffset = mVCTVoxelSampleOffset;
			mVoxelConeTracingConstantBuffer.Data.GIPower = mVCTGIPower;
			mVoxelConeTracingConstantBuffer.Data.pad0 = XMFLOAT3(0, 0, 0);
			mVoxelConeTracingConstantBuffer.ApplyChanges(context);

			ID3D11UnorderedAccessView* UAV[1] = { mVCTMainRT->GetUAV() };
			ID3D11Buffer* CBs[1] = { mVoxelConeTracingConstantBuffer.Buffer() };
			ID3D11ShaderResourceView* SRVs[4 + NUM_VOXEL_GI_CASCADES];
			SRVs[0] = gbuffer->GetAlbedo()->GetSRV();
			SRVs[1] = gbuffer->GetNormals()->GetSRV();
			SRVs[2] = gbuffer->GetPositions()->GetSRV();
			SRVs[3] = gbuffer->GetExtraBuffer()->GetSRV();
			for (int i = 0; i < NUM_VOXEL_GI_CASCADES; i++)
				SRVs[4 + i] = mVCTVoxelCascades3DRTs[i]->GetSRV();

			ID3D11SamplerState* SSs[] = { SamplerStates::TrilinearWrap };

			context->CSSetSamplers(0, 1, SSs);
			context->CSSetShaderResources(0, 4 + mVCTVoxelCascades3DRTs.size(), SRVs);
			context->CSSetConstantBuffers(0, 1, CBs);
			context->CSSetShader(mVCTMainCS, NULL, NULL);
			context->CSSetUnorderedAccessViews(0, 1, UAV, NULL);

			context->Dispatch(DivideByMultiple(static_cast<UINT>(mVCTMainRT->GetWidth()), 8u), DivideByMultiple(static_cast<UINT>(mVCTMainRT->GetHeight()), 8u), 1u);

			ID3D11ShaderResourceView* nullSRV[] = { NULL };
			context->CSSetShaderResources(0, 1, nullSRV);
			ID3D11UnorderedAccessView* nullUAV[] = { NULL };
			context->CSSetUnorderedAccessViews(0, 1, nullUAV, 0);
			ID3D11Buffer* nullCBs[] = { NULL };
			context->CSSetConstantBuffers(0, 1, nullCBs);
			ID3D11SamplerState* nullSSs[] = { NULL };
			context->CSSetSamplers(0, 1, nullSSs);
		}

		//upsample & blur
		{
			mUpsampleBlurConstantBuffer.Data.Upsample = true;
			mUpsampleBlurConstantBuffer.ApplyChanges(context);

			ID3D11UnorderedAccessView* UAV[1] = { mVCTUpsampleAndBlurRT->GetUAV() };
			ID3D11Buffer* CBs[1] = { mUpsampleBlurConstantBuffer.Buffer() };
			ID3D11ShaderResourceView* SRVs[1] = { mVCTMainRT->GetSRV() };
			ID3D11SamplerState* SSs[] = { SamplerStates::TrilinearWrap };

			context->CSSetSamplers(0, 1, SSs);
			context->CSSetShaderResources(0, 1, SRVs);
			context->CSSetConstantBuffers(0, 1, CBs);
			context->CSSetShader(mUpsampleBlurCS, NULL, NULL);
			context->CSSetUnorderedAccessViews(0, 1, UAV, NULL);

			context->Dispatch(DivideByMultiple(static_cast<UINT>(mVCTUpsampleAndBlurRT->GetWidth()), 8u), DivideByMultiple(static_cast<UINT>(mVCTUpsampleAndBlurRT->GetHeight()), 8u), 1u);

			ID3D11ShaderResourceView* nullSRV[] = { NULL };
			context->CSSetShaderResources(0, 1, nullSRV);
			ID3D11UnorderedAccessView* nullUAV[] = { NULL };
			context->CSSetUnorderedAccessViews(0, 1, nullUAV, 0);
			ID3D11Buffer* nullCBs[] = { NULL };
			context->CSSetConstantBuffers(0, 1, nullCBs);
			ID3D11SamplerState* nullSSs[] = { NULL };
			context->CSSetSamplers(0, 1, nullSSs);
		}
	}

	void Illumination::DrawDebugGizmos()
	{
		//voxel GI
		if (mDrawVoxelZonesGizmos) 
		{
			for (int i = 0; i < NUM_VOXEL_GI_CASCADES; i++)
			{
				mDebugVoxelZonesGizmos[i]->Draw();
			}
		}

		//light probe system
		if (mProbesManager) {
			if (mDrawDiffuseProbes)
				mProbesManager->DrawDebugProbes(DIFFUSE_PROBE, mCurrentDebugProbeVolumeIndex);
			if (mDrawSpecularProbes)
				mProbesManager->DrawDebugProbes(SPECULAR_PROBE, mCurrentDebugProbeVolumeIndex);

			if (mDrawProbesVolumeGizmo)
			{
				mProbesManager->DrawDebugProbesVolumeGizmo(DIFFUSE_PROBE, mCurrentDebugProbeVolumeIndex);
				mProbesManager->DrawDebugProbesVolumeGizmo(SPECULAR_PROBE, mCurrentDebugProbeVolumeIndex);
			}
		}
	}

	void Illumination::Update(const GameTime& gameTime, const Scene* scene)
	{
		CPUCullObjectsAgainstVoxelCascades(scene);
		UpdateVoxelCameraPosition();
		UpdateImGui();
	}

	void Illumination::UpdateImGui()
	{
		if (!mShowDebug)
			return;

		ImGui::Begin("Illumination System");
		if (ImGui::CollapsingHeader("Global Illumination"))
		{
			if (ImGui::CollapsingHeader("Dynamic - Voxel Cone Tracing"))
			{
				ImGui::Checkbox("VCT GI Enabled", &mEnabled);
				ImGui::SliderFloat("VCT GI Intensity", &mVCTGIPower, 0.0f, 5.0f);
				ImGui::SliderFloat("VCT Diffuse Strength", &mVCTIndirectDiffuseStrength, 0.0f, 1.0f);
				ImGui::SliderFloat("VCT Specular Strength", &mVCTIndirectSpecularStrength, 0.0f, 1.0f);
				ImGui::SliderFloat("VCT Max Cone Trace Dist", &mVCTMaxConeTraceDistance, 0.0f, 2500.0f);
				ImGui::SliderFloat("VCT AO Falloff", &mVCTAoFalloff, 0.0f, 2.0f);
				ImGui::SliderFloat("VCT Sampling Factor", &mVCTSamplingFactor, 0.01f, 3.0f);
				ImGui::SliderFloat("VCT Sample Offset", &mVCTVoxelSampleOffset, -0.1f, 0.1f);
				for (int cascade = 0; cascade < NUM_VOXEL_GI_CASCADES; cascade++)
				{
					std::string name = "VCT Voxel Scale Cascade " + std::to_string(cascade);
					ImGui::SliderFloat(name.c_str(), &mWorldVoxelScales[cascade], 0.1f, 10.0f);
				}
				ImGui::Separator();
				ImGui::Checkbox("DEBUG - Ambient Occlusion", &mDrawAmbientOcclusionOnly);
				ImGui::Checkbox("DEBUG - Voxel Texture", &mDrawVoxelization);
				ImGui::Checkbox("DEBUG - Voxel Cascades Gizmos (Editor)", &mDrawVoxelZonesGizmos);
			}
			if (ImGui::CollapsingHeader("Static - Light Probes"))
			{
				ImGui::Checkbox("DEBUG - Skip indirect lighting", &mDebugSkipIndirectProbeLighting);
				ImGui::Separator();
				ImGui::Checkbox("DEBUG - Hide culled probes", &mProbesManager->mDebugDiscardCulledProbes);
				ImGui::Checkbox("DEBUG - Probe volume", &mDrawProbesVolumeGizmo);
				ImGui::SliderInt("DEBUG - Probe volumes index", &mCurrentDebugProbeVolumeIndex, 0, 1);
				ImGui::Checkbox("DEBUG - Diffuse probes", &mDrawDiffuseProbes);
				ImGui::Checkbox("DEBUG - Specular probes", &mDrawSpecularProbes);
			}
		}
		ImGui::End();
	}

	void Illumination::SetFoliageSystemForGI(FoliageSystem* foliageSystem)
	{
		mFoliageSystem = foliageSystem;
		if (mFoliageSystem)
		{
			//only first cascade due to performance
			mFoliageSystem->SetVoxelizationTextureOutput(mVCTVoxelCascades3DRTs[0]->GetUAV()); 
			mFoliageSystem->SetVoxelizationParams(&mWorldVoxelScales[0], &mVoxelCameraPositions[0]);
		}
	}

	void Illumination::UpdateVoxelCameraPosition()
	{
		for (int i = 0; i < NUM_VOXEL_GI_CASCADES; i++)
		{
			float halfCascadeBox = 0.5f * (voxelCascadesSizes[i] / mWorldVoxelScales[i] * 0.5f);
			XMFLOAT3 voxelGridBoundsMax = XMFLOAT3{ mVoxelCameraPositions[i].x + halfCascadeBox, mVoxelCameraPositions[i].y + halfCascadeBox, mVoxelCameraPositions[i].z + halfCascadeBox };
			XMFLOAT3 voxelGridBoundsMin = XMFLOAT3{ mVoxelCameraPositions[i].x - halfCascadeBox, mVoxelCameraPositions[i].y - halfCascadeBox, mVoxelCameraPositions[i].z - halfCascadeBox };
			
			if (mCamera.Position().x < voxelGridBoundsMin.x || mCamera.Position().y < voxelGridBoundsMin.y || mCamera.Position().z < voxelGridBoundsMin.z ||
				mCamera.Position().x > voxelGridBoundsMax.x || mCamera.Position().y > voxelGridBoundsMax.y || mCamera.Position().z > voxelGridBoundsMax.z)
			{
				mVoxelCameraPositions[i] = XMFLOAT4(mCamera.Position().x, mCamera.Position().y, mCamera.Position().z, 1.0f);
				mVoxelCameraPositionsUpdated = true;
			}
			else
				mVoxelCameraPositionsUpdated = false;

			mDebugVoxelZonesGizmos[i]->SetPosition(XMFLOAT3(mVoxelCameraPositions[i].x, mVoxelCameraPositions[i].y, mVoxelCameraPositions[i].z));
			mDebugVoxelZonesGizmos[i]->Update();
		}
	}

	void Illumination::DrawDeferredLighting(ER_GBuffer* gbuffer, ER_GPUTexture* aRenderTarget, bool clearTarget)
	{
		static const float clearColorBlack[] = { 0.0f, 0.0f, 0.0f, 0.0f };

		ID3D11DeviceContext* context = mGame->Direct3DDeviceContext();

		//compute pass
		if (aRenderTarget)
		{
			//might be cleared before (i.e., in PP)
			if (clearTarget) 
				context->ClearUnorderedAccessViewFloat(aRenderTarget->GetUAV(), clearColorBlack);

			for (size_t i = 0; i < NUM_SHADOW_CASCADES; i++)
				mDeferredLightingConstantBuffer.Data.ShadowMatrices[i] = mShadowMapper.GetViewMatrix(i) * mShadowMapper.GetProjectionMatrix(i) /** XMLoadFloat4x4(&MatrixHelper::GetProjectionShadowMatrix())*/;
			mDeferredLightingConstantBuffer.Data.ShadowCascadeDistances = XMFLOAT4{ mCamera.GetCameraFarCascadeDistance(0), mCamera.GetCameraFarCascadeDistance(1), mCamera.GetCameraFarCascadeDistance(2), 1.0f };
			mDeferredLightingConstantBuffer.Data.ShadowTexelSize = XMFLOAT4{ 1.0f / mShadowMapper.GetResolution(), 1.0f, 1.0f , 1.0f };
			mDeferredLightingConstantBuffer.Data.SunDirection = XMFLOAT4{ -mDirectionalLight.Direction().x, -mDirectionalLight.Direction().y, -mDirectionalLight.Direction().z, 1.0f };
			mDeferredLightingConstantBuffer.Data.SunColor = XMFLOAT4{ mDirectionalLight.GetDirectionalLightColor().x, mDirectionalLight.GetDirectionalLightColor().y, mDirectionalLight.GetDirectionalLightColor().z, mDirectionalLight.GetDirectionalLightIntensity() };
			mDeferredLightingConstantBuffer.Data.CameraPosition = XMFLOAT4{ mCamera.Position().x,mCamera.Position().y,mCamera.Position().z, 1.0f };
			mDeferredLightingConstantBuffer.Data.SkipIndirectProbeLighting = !mProbesManager->IsEnabled() || mDebugSkipIndirectProbeLighting;
			mDeferredLightingConstantBuffer.ApplyChanges(context);

			if (mProbesManager->IsEnabled())
			{
				for (size_t i = 0; i < NUM_PROBE_VOLUME_CASCADES; i++)
				{
					mLightProbesConstantBuffer.Data.DiffuseProbesCellsCount[i] = mProbesManager->GetProbesCellsCount(DIFFUSE_PROBE, i);
					mLightProbesConstantBuffer.Data.DiffuseProbesVolumeSizes[i] = XMFLOAT4{
						mProbesManager->GetProbesVolumeCascade(DIFFUSE_PROBE, i).x,
						mProbesManager->GetProbesVolumeCascade(DIFFUSE_PROBE, i).y,
						mProbesManager->GetProbesVolumeCascade(DIFFUSE_PROBE, i).z, 1.0f };
					mLightProbesConstantBuffer.Data.SpecularProbesCellsCount[i] = mProbesManager->GetProbesCellsCount(SPECULAR_PROBE, i);
					mLightProbesConstantBuffer.Data.SpecularProbesVolumeSizes[i] = XMFLOAT4{
						mProbesManager->GetProbesVolumeCascade(SPECULAR_PROBE, i).x,
						mProbesManager->GetProbesVolumeCascade(SPECULAR_PROBE, i).y,
						mProbesManager->GetProbesVolumeCascade(SPECULAR_PROBE, i).z, 1.0f };
					mLightProbesConstantBuffer.Data.ProbesVolumeIndexSkips[i] = XMFLOAT4(mProbesManager->GetProbesIndexSkip(i), 0.0, 0.0, 0.0);
				}
				mLightProbesConstantBuffer.Data.SceneLightProbesBounds = XMFLOAT4{ mProbesManager->GetSceneProbesVolumeMin().x, mProbesManager->GetSceneProbesVolumeMin().y, mProbesManager->GetSceneProbesVolumeMin().z, 1.0f };
				mLightProbesConstantBuffer.Data.DistanceBetweenDiffuseProbes = DISTANCE_BETWEEN_DIFFUSE_PROBES;
				mLightProbesConstantBuffer.Data.DistanceBetweenSpecularProbes = DISTANCE_BETWEEN_SPECULAR_PROBES;
				mLightProbesConstantBuffer.ApplyChanges(context);
			}

			ID3D11Buffer* CBs[2] = { mDeferredLightingConstantBuffer.Buffer(), mProbesManager->IsEnabled() ? mLightProbesConstantBuffer.Buffer() : nullptr };
			context->CSSetConstantBuffers(0, 2, CBs);

			ID3D11ShaderResourceView* SRs[24] = {
				gbuffer->GetAlbedo()->GetSRV(),
				gbuffer->GetNormals()->GetSRV(),
				gbuffer->GetPositions()->GetSRV(),
				gbuffer->GetExtraBuffer()->GetSRV(),
				gbuffer->GetExtra2Buffer()->GetSRV(),
			};

			for (int i = 0; i < NUM_SHADOW_CASCADES; i++)
				SRs[5 + i] = mShadowMapper.GetShadowTexture(i);

			SRs[8] = mProbesManager->IsEnabled() ? mProbesManager->GetCulledDiffuseProbesTextureArray(0)->GetSRV() : nullptr;
			SRs[9] = mProbesManager->IsEnabled() ? mProbesManager->GetCulledDiffuseProbesTextureArray(1)->GetSRV() : nullptr;
			SRs[10] = mProbesManager->IsEnabled() ? mProbesManager->GetGlobalDiffuseProbe()->GetCubemapSRV() : nullptr;
			SRs[11] = mProbesManager->IsEnabled() ? mProbesManager->GetCulledSpecularProbesTextureArray(0)->GetSRV() : nullptr;
			SRs[12] = mProbesManager->IsEnabled() ? mProbesManager->GetCulledSpecularProbesTextureArray(1)->GetSRV() : nullptr;
			SRs[13] = mProbesManager->IsEnabled() ? mProbesManager->GetIntegrationMap() : nullptr;
			SRs[14] = mProbesManager->IsEnabled() ? mProbesManager->GetDiffuseProbesCellsIndicesBuffer(0)->GetBufferSRV() : nullptr;
			SRs[15] = mProbesManager->IsEnabled() ? mProbesManager->GetDiffuseProbesCellsIndicesBuffer(1)->GetBufferSRV() : nullptr;
			SRs[16] = mProbesManager->IsEnabled() ? mProbesManager->GetDiffuseProbesTexArrayIndicesBuffer(0)->GetBufferSRV() : nullptr;
			SRs[17] = mProbesManager->IsEnabled() ? mProbesManager->GetDiffuseProbesTexArrayIndicesBuffer(1)->GetBufferSRV() : nullptr;
			SRs[18] = mProbesManager->IsEnabled() ? mProbesManager->GetDiffuseProbesPositionsBuffer()->GetBufferSRV() : nullptr;
			SRs[19] = mProbesManager->IsEnabled() ? mProbesManager->GetSpecularProbesCellsIndicesBuffer(0)->GetBufferSRV() : nullptr;
			SRs[20] = mProbesManager->IsEnabled() ? mProbesManager->GetSpecularProbesCellsIndicesBuffer(1)->GetBufferSRV() : nullptr;
			SRs[21] = mProbesManager->IsEnabled() ? mProbesManager->GetSpecularProbesTexArrayIndicesBuffer(0)->GetBufferSRV() : nullptr;
			SRs[22] = mProbesManager->IsEnabled() ? mProbesManager->GetSpecularProbesTexArrayIndicesBuffer(1)->GetBufferSRV() : nullptr;
			SRs[23] = mProbesManager->IsEnabled() ? mProbesManager->GetSpecularProbesPositionsBuffer()->GetBufferSRV() : nullptr;
			context->CSSetShaderResources(0, 24, SRs);

			ID3D11SamplerState* SS[2] = { SamplerStates::TrilinearWrap, SamplerStates::ShadowSamplerState };
			context->CSSetSamplers(0, 2, SS);

			ID3D11UnorderedAccessView* UAV[1] = { aRenderTarget->GetUAV() };
			context->CSSetUnorderedAccessViews(0, 1, UAV, NULL);

			context->CSSetShader(mDeferredLightingCS, NULL, NULL);

			context->Dispatch(DivideByMultiple(static_cast<UINT>(aRenderTarget->GetWidth()), 8u), DivideByMultiple(static_cast<UINT>(aRenderTarget->GetHeight()), 8u), 1u);

			ID3D11ShaderResourceView* nullSRV[] = { NULL };
			context->CSSetShaderResources(0, 1, nullSRV);
			ID3D11UnorderedAccessView* nullUAV[] = { NULL };
			context->CSSetUnorderedAccessViews(0, 1, nullUAV, 0);
			ID3D11Buffer* nullCBs[] = { NULL };
			context->CSSetConstantBuffers(0, 1, nullCBs);
			ID3D11SamplerState* nullSSs[] = { NULL };
			context->CSSetSamplers(0, 1, nullSSs);
		}
	}

	void Illumination::DrawForwardLighting(ER_GBuffer* gbuffer, ER_GPUTexture* aRenderTarget)
	{
		ID3D11DeviceContext* context = mGame->Direct3DDeviceContext();
		context->OMSetRenderTargets(1, aRenderTarget->GetRTVs(), gbuffer->GetDepth()->getDSV());

		for (auto& obj : mForwardPassObjects)
			obj.second->Draw(MaterialHelper::forwardLightingNonMaterialName);
	}

	void Illumination::PrepareForForwardLighting(Rendering::RenderingObject* aObj, int meshIndex)
	{
		ID3D11DeviceContext* context = mGame->Direct3DDeviceContext();

		if (aObj && aObj->IsForwardShading())
		{
			context->IASetInputLayout(aObj->IsInstanced() ? mForwardLightingRenderingObjectInputLayout_Instancing : mForwardLightingRenderingObjectInputLayout);

			for (size_t i = 0; i < NUM_SHADOW_CASCADES; i++)
				mForwardLightingConstantBuffer.Data.ShadowMatrices[i] = XMMatrixTranspose(mShadowMapper.GetViewMatrix(i) * mShadowMapper.GetProjectionMatrix(i) * XMLoadFloat4x4(&MatrixHelper::GetProjectionShadowMatrix()));
			mForwardLightingConstantBuffer.Data.ViewProjection = XMMatrixTranspose(mCamera.ViewMatrix() * mCamera.ProjectionMatrix());
			mForwardLightingConstantBuffer.Data.World = XMMatrixTranspose(aObj->GetTransformationMatrix());
			mForwardLightingConstantBuffer.Data.ShadowTexelSize = XMFLOAT4{ 1.0f / mShadowMapper.GetResolution(), 1.0f, 1.0f , 1.0f };
			mForwardLightingConstantBuffer.Data.ShadowCascadeDistances = XMFLOAT4{ mCamera.GetCameraFarCascadeDistance(0), mCamera.GetCameraFarCascadeDistance(1), mCamera.GetCameraFarCascadeDistance(2), 1.0f };
			mForwardLightingConstantBuffer.Data.SunDirection = XMFLOAT4{ -mDirectionalLight.Direction().x, -mDirectionalLight.Direction().y, -mDirectionalLight.Direction().z, 1.0f };
			mForwardLightingConstantBuffer.Data.SunColor = XMFLOAT4{ mDirectionalLight.GetDirectionalLightColor().x, mDirectionalLight.GetDirectionalLightColor().y, mDirectionalLight.GetDirectionalLightColor().z, mDirectionalLight.GetDirectionalLightIntensity() };
			mForwardLightingConstantBuffer.Data.CameraPosition = XMFLOAT4{ mCamera.Position().x,mCamera.Position().y,mCamera.Position().z, 1.0f };
			mForwardLightingConstantBuffer.Data.UseGlobalDiffuseProbe = aObj->GetUseGlobalLightProbeMask();
			mForwardLightingConstantBuffer.ApplyChanges(context);

			if (mProbesManager->IsEnabled())
			{
				for (size_t i = 0; i < NUM_PROBE_VOLUME_CASCADES; i++)
				{
					mLightProbesConstantBuffer.Data.DiffuseProbesCellsCount[i] = mProbesManager->GetProbesCellsCount(DIFFUSE_PROBE, i);
					mLightProbesConstantBuffer.Data.DiffuseProbesVolumeSizes[i] = XMFLOAT4{
						mProbesManager->GetProbesVolumeCascade(DIFFUSE_PROBE, i).x,
						mProbesManager->GetProbesVolumeCascade(DIFFUSE_PROBE, i).y,
						mProbesManager->GetProbesVolumeCascade(DIFFUSE_PROBE, i).z, 1.0f };
					mLightProbesConstantBuffer.Data.SpecularProbesCellsCount[i] = mProbesManager->GetProbesCellsCount(SPECULAR_PROBE, i);
					mLightProbesConstantBuffer.Data.SpecularProbesVolumeSizes[i] = XMFLOAT4{
						mProbesManager->GetProbesVolumeCascade(SPECULAR_PROBE, i).x,
						mProbesManager->GetProbesVolumeCascade(SPECULAR_PROBE, i).y,
						mProbesManager->GetProbesVolumeCascade(SPECULAR_PROBE, i).z, 1.0f };
					mLightProbesConstantBuffer.Data.ProbesVolumeIndexSkips[i] = XMFLOAT4(mProbesManager->GetProbesIndexSkip(i), 0.0, 0.0, 0.0);
				}
				mLightProbesConstantBuffer.Data.SceneLightProbesBounds = XMFLOAT4{ mProbesManager->GetSceneProbesVolumeMin().x, mProbesManager->GetSceneProbesVolumeMin().y, mProbesManager->GetSceneProbesVolumeMin().z, 1.0f };
				mLightProbesConstantBuffer.Data.DistanceBetweenDiffuseProbes = DISTANCE_BETWEEN_DIFFUSE_PROBES;
				mLightProbesConstantBuffer.Data.DistanceBetweenSpecularProbes = DISTANCE_BETWEEN_SPECULAR_PROBES;
				mLightProbesConstantBuffer.ApplyChanges(context);
			}

			ID3D11Buffer* CBs[2] = { mForwardLightingConstantBuffer.Buffer(), mProbesManager->IsEnabled() ? mLightProbesConstantBuffer.Buffer() : nullptr };
			context->VSSetShader(aObj->IsInstanced() ? mForwardLightingVS_Instancing : mForwardLightingVS, NULL, 0);
			context->VSSetConstantBuffers(0, 2, CBs);

			context->PSSetShader(mForwardLightingPS, NULL, NULL); //TODO add other passes
			context->PSSetConstantBuffers(0, 2, CBs);

			ID3D11ShaderResourceView* SRs[24] = {
				aObj->GetTextureData(meshIndex).AlbedoMap,
				aObj->GetTextureData(meshIndex).NormalMap,
				aObj->GetTextureData(meshIndex).MetallicMap,
				aObj->GetTextureData(meshIndex).RoughnessMap,
				aObj->GetTextureData(meshIndex).HeightMap
			};
			for (int i = 0; i < NUM_SHADOW_CASCADES; i++)
				SRs[5 + i] = mShadowMapper.GetShadowTexture(i);

			SRs[8] = mProbesManager->IsEnabled() ? mProbesManager->GetCulledDiffuseProbesTextureArray(0)->GetSRV() : nullptr;
			SRs[9] = mProbesManager->IsEnabled() ? mProbesManager->GetCulledDiffuseProbesTextureArray(1)->GetSRV() : nullptr;
			SRs[10] = mProbesManager->IsEnabled() ? mProbesManager->GetGlobalDiffuseProbe()->GetCubemapSRV() : nullptr;
			SRs[11] = mProbesManager->IsEnabled() ? mProbesManager->GetCulledSpecularProbesTextureArray(0)->GetSRV() : nullptr;
			SRs[12] = mProbesManager->IsEnabled() ? mProbesManager->GetCulledSpecularProbesTextureArray(1)->GetSRV() : nullptr;
			SRs[13] = mProbesManager->IsEnabled() ? mProbesManager->GetIntegrationMap() : nullptr;
			SRs[14] = mProbesManager->IsEnabled() ? mProbesManager->GetDiffuseProbesCellsIndicesBuffer(0)->GetBufferSRV() : nullptr;
			SRs[15] = mProbesManager->IsEnabled() ? mProbesManager->GetDiffuseProbesCellsIndicesBuffer(1)->GetBufferSRV() : nullptr;
			SRs[16] = mProbesManager->IsEnabled() ? mProbesManager->GetDiffuseProbesTexArrayIndicesBuffer(0)->GetBufferSRV() : nullptr;
			SRs[17] = mProbesManager->IsEnabled() ? mProbesManager->GetDiffuseProbesTexArrayIndicesBuffer(1)->GetBufferSRV() : nullptr;
			SRs[18] = mProbesManager->IsEnabled() ? mProbesManager->GetDiffuseProbesPositionsBuffer()->GetBufferSRV() : nullptr;
			SRs[19] = mProbesManager->IsEnabled() ? mProbesManager->GetSpecularProbesCellsIndicesBuffer(0)->GetBufferSRV() : nullptr;
			SRs[20] = mProbesManager->IsEnabled() ? mProbesManager->GetSpecularProbesCellsIndicesBuffer(1)->GetBufferSRV() : nullptr;
			SRs[21] = mProbesManager->IsEnabled() ? mProbesManager->GetSpecularProbesTexArrayIndicesBuffer(0)->GetBufferSRV() : nullptr;
			SRs[22] = mProbesManager->IsEnabled() ? mProbesManager->GetSpecularProbesTexArrayIndicesBuffer(1)->GetBufferSRV() : nullptr;
			SRs[23] = mProbesManager->IsEnabled() ? mProbesManager->GetSpecularProbesPositionsBuffer()->GetBufferSRV() : nullptr;

			context->PSSetShaderResources(0, 24, SRs);

			ID3D11SamplerState* SS[2] = { SamplerStates::TrilinearWrap, SamplerStates::ShadowSamplerState };
			context->PSSetSamplers(0, 2, SS);
		}
	}

	void Illumination::CPUCullObjectsAgainstVoxelCascades(const Scene* scene)
	{
		//TODO add instancing support
		//TODO fix repetition checks when the object AABB is bigger than the lower cascade (i.e. sponza)
		//TODO add optimization for culling objects by checking its volume size in second+ cascades
		//TODO add indirect drawing support (GPU cull)
		//TODO add multithreading per cascade
		for (int cascade = 0; cascade < NUM_VOXEL_GI_CASCADES; cascade++)
		{
			for (auto& objectInfo : scene->objects)
			{
				auto aabbObj = objectInfo.second->GetLocalAABB();
				XMFLOAT3 position;
				MatrixHelper::GetTranslation(objectInfo.second->GetTransformationMatrix(), position);
				aabbObj.first.x += position.x;
				aabbObj.first.y += position.y;
				aabbObj.first.z += position.z;
				aabbObj.second.x += position.x;
				aabbObj.second.y += position.y;
				aabbObj.second.z += position.z;

				//check if exists in previous cascade container
				if (cascade > 0)
				{
					auto it = mVoxelizationObjects[cascade - 1].find(objectInfo.first);
					if (it != mVoxelizationObjects[cascade - 1].end())
					{
						//check if should be removed from current cascade container
						auto it2 = mVoxelizationObjects[cascade].find(objectInfo.first);
						if (it2 != mVoxelizationObjects[cascade].end())
							mVoxelizationObjects[cascade].erase(it2);
						
						continue;
					}
				}

				auto aabbCascade = mVoxelCascadesAABBs[cascade];
				aabbCascade.first.x += mVoxelCameraPositions[cascade].x;
				aabbCascade.first.y += mVoxelCameraPositions[cascade].y;
				aabbCascade.first.z += mVoxelCameraPositions[cascade].z;		
				aabbCascade.second.x += mVoxelCameraPositions[cascade].x;
				aabbCascade.second.y += mVoxelCameraPositions[cascade].y;
				aabbCascade.second.z += mVoxelCameraPositions[cascade].z;

				bool isColliding =
					(aabbObj.first.x <= aabbCascade.second.x && aabbObj.second.x >= aabbCascade.first.x) &&
					(aabbObj.first.y <= aabbCascade.second.y && aabbObj.second.y >= aabbCascade.first.y) &&
					(aabbObj.first.z <= aabbCascade.second.z && aabbObj.second.z >= aabbCascade.first.z);

				auto it = mVoxelizationObjects[cascade].find(objectInfo.first);
				if (isColliding && (it == mVoxelizationObjects[cascade].end()))
					mVoxelizationObjects[cascade].insert(objectInfo);
				else if (!isColliding && (it != mVoxelizationObjects[cascade].end()))
					mVoxelizationObjects[cascade].erase(it);
			}
		}
	}
}