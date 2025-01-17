#include "ER_VoxelizationMaterial.h"
#include "ER_MaterialsCallbacks.h"
#include "ER_Utility.h"
#include "ER_CoreException.h"
#include "ER_Core.h"
#include "ER_Camera.h"
#include "ER_RenderingObject.h"
#include "ER_Mesh.h"
#include "ER_ShadowMapper.h"
#include "ER_DirectionalLight.h"
#include "ER_Illumination.h"

namespace EveryRay_Core
{
	ER_VoxelizationMaterial::ER_VoxelizationMaterial(ER_Core& game, const MaterialShaderEntries& entries, unsigned int shaderFlags, bool instanced)
		: ER_Material(game, entries, shaderFlags)
	{
		mIsStandard = false;

		if (shaderFlags & HAS_VERTEX_SHADER)
		{
			if (!instanced)
			{
				ER_RHI_INPUT_ELEMENT_DESC inputElementDescriptions[] =
				{
					{ "POSITION", 0, ER_FORMAT_R32G32B32A32_FLOAT, 0, 0,	  true, 0 },
					{ "TEXCOORD", 0, ER_FORMAT_R32G32_FLOAT, 0, 0xffffffff,   true, 0 },
					{ "NORMAL", 0, ER_FORMAT_R32G32B32_FLOAT, 0, 0xffffffff,  true, 0 },
					{ "TANGENT", 0, ER_FORMAT_R32G32B32_FLOAT, 0, 0xffffffff, true, 0 }
				};
				ER_Material::CreateVertexShader("content\\shaders\\Voxelization.hlsl", inputElementDescriptions, ARRAYSIZE(inputElementDescriptions));
			}
			else
			{
				ER_RHI_INPUT_ELEMENT_DESC inputElementDescriptionsInstanced[] =
				{
					{ "POSITION", 0, ER_FORMAT_R32G32B32A32_FLOAT, 0, 0,	 true, 0 },
					{ "TEXCOORD", 0, ER_FORMAT_R32G32_FLOAT, 0, 0xffffffff,  true, 0 },
					{ "NORMAL", 0, ER_FORMAT_R32G32B32_FLOAT, 0, 0xffffffff, true, 0 },
					{ "TANGENT", 0, ER_FORMAT_R32G32B32_FLOAT, 0, 0xffffffff,true, 0 },
					{ "WORLD", 0, ER_FORMAT_R32G32B32A32_FLOAT, 1, 0, false, 1 },
					{ "WORLD", 1, ER_FORMAT_R32G32B32A32_FLOAT, 1, 16,false, 1 },
					{ "WORLD", 2, ER_FORMAT_R32G32B32A32_FLOAT, 1, 32,false, 1 },
					{ "WORLD", 3, ER_FORMAT_R32G32B32A32_FLOAT, 1, 48,false, 1 }
				};
				ER_Material::CreateVertexShader("content\\shaders\\Voxelization.hlsl", inputElementDescriptionsInstanced, ARRAYSIZE(inputElementDescriptionsInstanced));
			}
		}

		if (shaderFlags & HAS_GEOMETRY_SHADER)
			ER_Material::CreateGeometryShader("content\\shaders\\Voxelization.hlsl");

		if (shaderFlags & HAS_PIXEL_SHADER)
			ER_Material::CreatePixelShader("content\\shaders\\Voxelization.hlsl");

		mConstantBuffer.Initialize(ER_Material::GetCore()->GetRHI(), "ER_RHI_GPUBuffer: VoxelizationMaterial CB");
	}

	ER_VoxelizationMaterial::~ER_VoxelizationMaterial()
	{
		mConstantBuffer.Release();
		ER_Material::~ER_Material();
	}

	void ER_VoxelizationMaterial::PrepareForRendering(ER_MaterialSystems neededSystems, ER_RenderingObject* aObj, int meshIndex, 
		float voxelScale, float voxelTexSize, const XMFLOAT4& voxelCameraPos, ER_RHI_GPURootSignature* rs)
	{
		auto rhi = ER_Material::GetCore()->GetRHI();
		ER_Camera* camera = (ER_Camera*)(ER_Material::GetCore()->GetServices().FindService(ER_Camera::TypeIdClass()));

		assert(aObj);
		assert(camera);
		assert(neededSystems.mShadowMapper);
		assert(neededSystems.mDirectionalLight);

		int shadowCascadeIndex = 1;
		mConstantBuffer.Data.World = XMMatrixTranspose(aObj->GetTransformationMatrix());
		mConstantBuffer.Data.ViewProjection = XMMatrixTranspose(camera->ViewMatrix() * camera->ProjectionMatrix());
		mConstantBuffer.Data.ShadowMatrix = XMMatrixTranspose(neededSystems.mShadowMapper->GetViewMatrix(shadowCascadeIndex) * neededSystems.mShadowMapper->GetProjectionMatrix(shadowCascadeIndex) * XMLoadFloat4x4(&ER_MatrixHelper::GetProjectionShadowMatrix()));
		mConstantBuffer.Data.ShadowTexelSize = XMFLOAT4{ 1.0f / neededSystems.mShadowMapper->GetResolution(), 1.0f, 1.0f, 1.0f };
		mConstantBuffer.Data.ShadowCascadeDistances = XMFLOAT4{ camera->GetCameraFarShadowCascadeDistance(0), camera->GetCameraFarShadowCascadeDistance(1), camera->GetCameraFarShadowCascadeDistance(2), 1.0f };
		mConstantBuffer.Data.VoxelCameraPos = voxelCameraPos;
		mConstantBuffer.Data.VoxelTextureDimension = voxelTexSize;
		mConstantBuffer.Data.WorldVoxelScale = voxelScale;
		mConstantBuffer.ApplyChanges(rhi);
		rhi->SetConstantBuffers(ER_VERTEX, { mConstantBuffer.Buffer() }, 0, rs, VOXELIZATION_MAT_ROOT_DESCRIPTOR_TABLE_CBV_INDEX);
		rhi->SetConstantBuffers(ER_GEOMETRY, { mConstantBuffer.Buffer() }, 0, rs, VOXELIZATION_MAT_ROOT_DESCRIPTOR_TABLE_CBV_INDEX);
		rhi->SetConstantBuffers(ER_PIXEL, { mConstantBuffer.Buffer() }, 0, rs, VOXELIZATION_MAT_ROOT_DESCRIPTOR_TABLE_CBV_INDEX);

		rhi->SetShaderResources(ER_PIXEL, { aObj->GetTextureData(meshIndex).AlbedoMap, neededSystems.mShadowMapper->GetShadowTexture(1) }, 0, rs, VOXELIZATION_MAT_ROOT_DESCRIPTOR_TABLE_SRV_INDEX);
		rhi->SetSamplers(ER_PIXEL, { ER_RHI_SAMPLER_STATE::ER_TRILINEAR_WRAP, ER_RHI_SAMPLER_STATE::ER_SHADOW_SS });
	}

	void ER_VoxelizationMaterial::PrepareResourcesForStandardMaterial(ER_MaterialSystems neededSystems, ER_RenderingObject* aObj, int meshIndex, ER_RHI_GPURootSignature* rs)
	{
		//not used because this material is not standard
	}

	void ER_VoxelizationMaterial::CreateVertexBuffer(const ER_Mesh& mesh, ER_RHI_GPUBuffer* vertexBuffer)
	{
		mesh.CreateVertexBuffer_PositionUvNormalTangent(vertexBuffer);
	}

	int ER_VoxelizationMaterial::VertexSize()
	{
		return sizeof(VertexPositionTextureNormalTangent);
	}

}