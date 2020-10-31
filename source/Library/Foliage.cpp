#include "Foliage.h"
#include "GameException.h"
#include "Game.h"
#include "MatrixHelper.h"
#include "Utility.h"
#include "VertexDeclarations.h"
#include "RasterizerStates.h"
#include "FoliageMaterial.h"

#include "TGATextureLoader.h"
#include <DDSTextureLoader.h>
#include <WICTextureLoader.h>

namespace Library
{
	Foliage::Foliage(Game& pGame, Camera& pCamera, DirectionalLight& pLight, int pPatchesCount, std::string textureName, float scale, float distributionRadius)
		:
		GameComponent(pGame),
		mCamera(pCamera),
		mDirectionalLight(pLight),
		mPatchesCount(pPatchesCount),
		mScale(scale),
		mDistributionRadius(distributionRadius)
	{
		Effect* effect = new Effect(pGame);
		effect->CompileFromFile(Utility::GetFilePath(L"content\\effects\\Foliage.fx"));

		mMaterial = new FoliageMaterial();
		mMaterial->Initialize(effect);

		if (FAILED(DirectX::CreateWICTextureFromFile(mGame->Direct3DDevice(), mGame->Direct3DDeviceContext(), Utility::ToWideString(textureName).c_str(), nullptr, &mAlbedoTexture)))
		{
			std::string message = "Failed to create Foliage Albedo Map: ";
			message += textureName;
			throw GameException(message.c_str());
		}

		CreateBlendStates();
		Initialize();
	}

	Foliage::~Foliage()
	{
		ReleaseObject(mVertexBuffer);
		ReleaseObject(mInstanceBuffer);
		ReleaseObject(mIndexBuffer);
		ReleaseObject(mAlbedoTexture);
		ReleaseObject(mAlphaToCoverageState);
		ReleaseObject(mNoBlendState);
		DeleteObject(mPatchesBufferCPU);
		DeleteObject(mPatchesBufferGPU);
		DeleteObject(mMaterial);
	}

	void Foliage::Initialize()
	{
		InitializeBuffersCPU();
		InitializeBuffersGPU();
	}

	void Foliage::CreateBlendStates()
	{
		D3D11_BLEND_DESC blendStateDescription;
		ZeroMemory(&blendStateDescription, sizeof(D3D11_BLEND_DESC));

		// Create an alpha enabled blend state description.
		blendStateDescription.AlphaToCoverageEnable = TRUE;
		blendStateDescription.RenderTarget[0].BlendEnable = TRUE;
		blendStateDescription.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
		blendStateDescription.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
		blendStateDescription.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
		blendStateDescription.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
		blendStateDescription.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
		blendStateDescription.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
		blendStateDescription.RenderTarget[0].RenderTargetWriteMask = 0x0f;

		// Create the blend state using the description.
		if (FAILED(mGame->Direct3DDevice()->CreateBlendState(&blendStateDescription, &mAlphaToCoverageState)))
			throw GameException("ID3D11Device::CreateBlendState() failed while create alpha-to-coverage blend state for foliage");

		blendStateDescription.RenderTarget[0].BlendEnable = FALSE;
		blendStateDescription.AlphaToCoverageEnable = FALSE;
		if (FAILED(mGame->Direct3DDevice()->CreateBlendState(&blendStateDescription, &mNoBlendState)))
			throw GameException("ID3D11Device::CreateBlendState() failed while create no blend state for foliage");
	}

	void Foliage::InitializeBuffersGPU()
	{
		D3D11_BUFFER_DESC vertexBufferDesc, instanceBufferDesc;
		D3D11_SUBRESOURCE_DATA vertexData, instanceData;

		int vertexCount = 6;
		FoliagePatchData* patchData = new FoliagePatchData[vertexCount];

		// Load the vertex array with data.
		patchData[0].pos = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);  // Bottom left.
		patchData[0].uv = XMFLOAT2(0.0f, 1.0f);

		patchData[1].pos = XMFLOAT4(0.0f, 1.0f, 0.0f, 1.0f);  // Top left.
		patchData[1].uv = XMFLOAT2(0.0f, 0.0f);

		patchData[2].pos = XMFLOAT4(1.0f, 0.0f, 0.0f, 1.0f);  // Bottom right.
		patchData[2].uv = XMFLOAT2(1.0f, 1.0f);

		patchData[3].pos = XMFLOAT4(1.0f, 0.0f, 0.0f, 1.0f);  // Bottom right.
		patchData[3].uv = XMFLOAT2(1.0f, 1.0f);

		patchData[4].pos = XMFLOAT4(0.0f, 1.0f, 0.0f, 1.0f);  // Top left.
		patchData[4].uv = XMFLOAT2(0.0f, 0.0f);

		patchData[5].pos = XMFLOAT4(1.0f, 1.0f, 0.0f, 1.0f);  // Top right.
		patchData[5].uv = XMFLOAT2(1.0f, 0.0f);

		// Set up the description of the vertex buffer.
		vertexBufferDesc.Usage = D3D11_USAGE_DEFAULT;
		vertexBufferDesc.ByteWidth = sizeof(FoliagePatchData) * vertexCount;
		vertexBufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
		vertexBufferDesc.CPUAccessFlags = 0;
		vertexBufferDesc.MiscFlags = 0;
		vertexBufferDesc.StructureByteStride = 0;

		// Give the subresource structure a pointer to the vertex data.
		vertexData.pSysMem = patchData;
		vertexData.SysMemPitch = 0;
		vertexData.SysMemSlicePitch = 0;

		if (FAILED(mGame->Direct3DDevice()->CreateBuffer(&vertexBufferDesc, &vertexData, &mVertexBuffer)))
			throw GameException("ID3D11Device::CreateBuffer() failed while generating vertex buffer of foliage mesh patch");

		delete patchData;
		patchData = nullptr;

		// instance buffer
		int instanceCount = mPatchesCount;
		mPatchesBufferGPU = new FoliageInstanceData[instanceCount];

		for (int i = 0; i < instanceCount; i++)
		{
			float randomScale = Utility::RandomFloat(mScale - 1.0f, mScale + 1.0f);
			mPatchesBufferCPU[i].scale = randomScale;
			mPatchesBufferGPU[i].worldMatrix = XMMatrixTranslation(mPatchesBufferCPU[i].xPos, mPatchesBufferCPU[i].yPos, mPatchesBufferCPU[i].zPos) * XMMatrixScaling(randomScale, randomScale, randomScale);
			mPatchesBufferGPU[i].color = XMFLOAT3(mPatchesBufferCPU[i].r, mPatchesBufferCPU[i].g, mPatchesBufferCPU[i].b);
		}

		// Set up the description of the instance buffer.
		instanceBufferDesc.Usage = D3D11_USAGE_DYNAMIC;
		instanceBufferDesc.ByteWidth = sizeof(FoliageInstanceData) * instanceCount;
		instanceBufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
		instanceBufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		instanceBufferDesc.MiscFlags = 0;
		instanceBufferDesc.StructureByteStride = 0;

		// Give the subresource structure a pointer to the instance data.
		instanceData.pSysMem = mPatchesBufferGPU;
		instanceData.SysMemPitch = 0;
		instanceData.SysMemSlicePitch = 0;

		if (FAILED(mGame->Direct3DDevice()->CreateBuffer(&instanceBufferDesc, &instanceData, &mInstanceBuffer)))
			throw GameException("ID3D11Device::CreateBuffer() failed while generating instance buffer of foliage patches");
	}

	void Foliage::InitializeBuffersCPU()
	{
		// randomly generate positions and color
		mPatchesBufferCPU = new FoliageData[mPatchesCount];
		
		for (int i = 0; i < mPatchesCount; i++)
		{
			mPatchesBufferCPU[i].xPos = ((float)rand() / (float)(RAND_MAX)) * mDistributionRadius - mDistributionRadius /2;
			mPatchesBufferCPU[i].yPos = 0.0f;
			mPatchesBufferCPU[i].zPos = ((float)rand() / (float)(RAND_MAX)) * mDistributionRadius - mDistributionRadius /2;

			mPatchesBufferCPU[i].r = ((float)rand() / (float)(RAND_MAX)) * 1.0f + 1.0f;
			mPatchesBufferCPU[i].g = ((float)rand() / (float)(RAND_MAX)) * 1.0f + 0.5f;
			mPatchesBufferCPU[i].b = 0.0f;
		}
	}

	void Foliage::Draw()
	{
		ID3D11DeviceContext* context = GetGame()->Direct3DDeviceContext();

		// dont forget to set alpha blend state if not using the one declared in .fx
		float blendFactor[] = { 0.0f, 0.0f, 0.0f, 0.0f };
		context->OMSetBlendState(mAlphaToCoverageState, blendFactor, 0xffffffff);

		unsigned int strides[2];
		unsigned int offsets[2];
		ID3D11Buffer* bufferPointers[2];

		strides[0] = sizeof(FoliagePatchData);
		strides[1] = sizeof(FoliageInstanceData);

		offsets[0] = 0;
		offsets[1] = 0;

		// Set the array of pointers to the vertex and instance buffers.
		bufferPointers[0] = mVertexBuffer;
		bufferPointers[1] = mInstanceBuffer;

		context->IASetVertexBuffers(0, 2, bufferPointers, strides, offsets);
		context->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

		Pass* pass = mMaterial->CurrentTechnique()->Passes().at(0);
		ID3D11InputLayout* inputLayout = mMaterial->InputLayouts().at(pass);
		context->IASetInputLayout(inputLayout);

		mMaterial->World() << XMMatrixIdentity();
		mMaterial->View() << mCamera.ViewMatrix();
		mMaterial->Projection() << mCamera.ProjectionMatrix();
		mMaterial->albedoTexture() << mAlbedoTexture;
		mMaterial->SunDirection() << XMVectorNegate(mDirectionalLight.DirectionVector());
		mMaterial->SunColor() << XMVECTOR{ mDirectionalLight.GetDirectionalLightColor().x,  mDirectionalLight.GetDirectionalLightColor().y, mDirectionalLight.GetDirectionalLightColor().z , 1.0f };
		mMaterial->AmbientColor() << XMVECTOR{ mDirectionalLight.GetAmbientLightColor().x,  mDirectionalLight.GetAmbientLightColor().y, mDirectionalLight.GetAmbientLightColor().z , 1.0f };
		mMaterial->ShadowTexelSize() << XMVECTOR{ 1.0f, 1.0f, 1.0f , 1.0f }; //todo
		mMaterial->ShadowCascadeDistances() << XMVECTOR{ mCamera.GetCameraFarCascadeDistance(0), mCamera.GetCameraFarCascadeDistance(1), mCamera.GetCameraFarCascadeDistance(2), 1.0f };
		//mMaterial->CameraPosition() << mCamera.PositionVector();
		pass->Apply(0, context);

		if (mIsWireframe)
		{
			context->RSSetState(RasterizerStates::Wireframe);
			context->DrawInstanced(6, mPatchesCount, 0, 0);
			context->RSSetState(nullptr);
		}
		else
			context->DrawInstanced(6, mPatchesCount, 0, 0);

		context->OMSetBlendState(mNoBlendState, blendFactor, 0xffffffff);
	}

	void Foliage::Update(const GameTime& gameTime)
	{
		ID3D11DeviceContext* context = GetGame()->Direct3DDeviceContext();

		double angle;
		float rotation, windRotation;
		XMMATRIX rotationMatrix;
		XMMATRIX translationMatrix;

		for (int i = 0; i < mPatchesCount; i++)
		{
			// Get the position of this piece of foliage.
			translationMatrix = XMMatrixTranslation(mPatchesBufferCPU[i].xPos, mPatchesBufferCPU[i].yPos, mPatchesBufferCPU[i].zPos);

			if (mRotateFromCamPosition)
				angle = atan2(mPatchesBufferCPU[i].xPos - mCamera.Position().x, mPatchesBufferCPU[i].zPos - mCamera.Position().z) * (180.0 / XM_PI);
			else 
				angle = atan2(mCamera.Direction().x, mCamera.Direction().z) * (180.0 / XM_PI);

			// Convert rotation into radians.
			rotation = (float)angle * 0.0174532925f;
			rotationMatrix = XMMatrixRotationY(rotation);

			// Get the wind rotation for the foliage.
			//windRotation = m_windRotation * 0.0174532925f;

			// Setup the wind rotation.
			//D3DXMatrixRotationX(&rotateMatrix2, windRotation);

			mPatchesBufferGPU[i].worldMatrix = XMMatrixScaling(mPatchesBufferCPU[i].scale, mPatchesBufferCPU[i].scale, mPatchesBufferCPU[i].scale) * rotationMatrix * translationMatrix ;
		}

		D3D11_MAPPED_SUBRESOURCE mappedResource;
		FoliageInstanceData* instancesPtr;

		// Lock the instance buffer so it can be written to.
		if (FAILED(context->Map(mInstanceBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource)))
			throw GameException("Map() failed while updating instance buffer of foliage patches");


		// Get a pointer to the data in the instance buffer.
		instancesPtr = (FoliageInstanceData*)mappedResource.pData;

		// Copy the instances array into the instance buffer.
		memcpy(instancesPtr, (void*)mPatchesBufferGPU, (sizeof(FoliageInstanceData) * mPatchesCount));

		// Unlock the instance buffer.
		context->Unmap(mInstanceBuffer, 0);
	}

}