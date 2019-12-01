#include "stdafx.h"

#include "RenderingObject.h"
#include "GameException.h"
#include "Model.h"
#include "Mesh.h"
#include "Game.h"
#include "MatrixHelper.h"
#include "Camera.h"
#include "AmbientLightingMaterial.h"
//#include "ModelMaterial.h"
#include "Utility.h"

#include <DDSTextureLoader.h>
#include <WICTextureLoader.h>

namespace Rendering
{
	RenderingObject::RenderingObject(std::string pName, Game& pGame, std::unique_ptr<Model> pModel) 
		: 
		GameComponent(pGame),
		mModel(std::move(pModel)),
		mMeshesRenderBuffers(0),
		mMeshVertices(0),
		mName(pName)
	{
		if (!mModel)
			throw GameException("Failed to create a RenderingObject from a model");

		mMeshesCount = mModel->Meshes().size();
		for (size_t i = 0; i < mMeshesCount; i++)
		{
			mMeshVertices.push_back(mModel->Meshes().at(i)->Vertices());
			mMeshesRenderBuffers.push_back(RenderBufferData());
			mMeshesTextures.push_back(TextureData());
		}

		mAABB = mModel->GenerateAABB();
		
	}

	RenderingObject::~RenderingObject()
	{

	}

	void RenderingObject::LoadMaterial(Material* pMaterial, Effect* pEffect)
	{
		assert(mModel != nullptr);

		for (size_t i = 0; i < mMeshesCount; i++)
		{
			mMeshesMaterials.push_back(pMaterial);
			mMeshesMaterials.back()->Initialize(pEffect);
		}
	}

	void RenderingObject::LoadAssignedMeshTextures()
	{
		for (size_t i = 0; i < mMeshesCount; i++)
		{
			if (mModel->Meshes()[i]->GetMaterial()->Textures().size() == 0) continue;
			auto it = mModel->Meshes()[i]->GetMaterial()->Textures().find(TextureType::TextureTypeDifffuse);

			if (it != mModel->Meshes()[i]->GetMaterial()->Textures().end())
			{
				{
					std::wstring textureRelativePath = it->second->at(0);
					std::string fullPath;
					Utility::GetDirectory(mModel->GetFileName(), fullPath);
					fullPath += "/";
					std::wstring resultPath;
					Utility::ToWideString(fullPath, resultPath);
					resultPath += textureRelativePath;

					LoadTexture(TextureType::TextureTypeDifffuse, resultPath, i);
				}

			}
		}
	}
	void RenderingObject::LoadCustomMeshTextures(int meshIndex, std::wstring albedoPath, std::wstring normalPath, std::wstring roughnessPath, std::wstring metallicPath, std::wstring extra1Path, std::wstring extra2Path, std::wstring extra3Path)
	{
		assert(meshIndex < mMeshesCount);
		
		std::string errorMessage = mModel->GetFileName() + " of mesh index: " + std::to_string(meshIndex);

		if (!albedoPath.empty())
			LoadTexture(TextureType::TextureTypeDifffuse, albedoPath, meshIndex);

		if (!normalPath.empty())
			LoadTexture(TextureType::TextureTypeNormalMap, normalPath, meshIndex);

		if (!roughnessPath.empty())
			LoadTexture(TextureType::TextureTypeSpecularMap, roughnessPath, meshIndex);

		if (!metallicPath.empty())
			LoadTexture(TextureType::TextureTypeEmissive, metallicPath, meshIndex);

		//TODO
		//if (!extra1Path.empty())
		//TODO
		//if (!extra2Path.empty())
		//TODO
		//if (!extra3Path.empty())
	}
	void RenderingObject::LoadTexture(TextureType type, std::wstring path, int meshIndex)
	{
		const wchar_t* postfix = L".dds";
		bool ddsLoader = (path.substr(path.length() - 4) == std::wstring(postfix));
		std::string errorMessage = mModel->GetFileName() + " of mesh index: " + std::to_string(meshIndex);

		std::string texType;
		ID3D11ShaderResourceView** resource;

		switch (type)
		{
		case TextureType::TextureTypeDifffuse:
			texType = "Albedo Texture";
			resource = &(mMeshesTextures[meshIndex].AlbedoMap);
			break;
		case TextureType::TextureTypeNormalMap:
			texType = "Normal Texture";
			resource = &(mMeshesTextures[meshIndex].NormalMap);
			break;
		case TextureType::TextureTypeSpecularMap:
			texType = "Roughness Texture";
			resource = &(mMeshesTextures[meshIndex].RoughnessMap);
			break;
		case TextureType::TextureTypeEmissive:
			texType = "Metallic Texture";
			resource = &(mMeshesTextures[meshIndex].MetallicMap);
			break;
		}

		if (FAILED(ddsLoader ? DirectX::CreateDDSTextureFromFile(mGame->Direct3DDevice(), mGame->Direct3DDeviceContext(), path.c_str(), nullptr, resource)
			: DirectX::CreateWICTextureFromFile(mGame->Direct3DDevice(), mGame->Direct3DDeviceContext(), path.c_str(), nullptr, resource)))
		{
			std::string status = "Failed to load " + texType;
			status += errorMessage;
			throw GameException(status.c_str());
		}
	}

	void RenderingObject::LoadRenderBuffers()
	{
		assert(mModel != nullptr);
		assert(mGame->Direct3DDevice() != nullptr);

		for (size_t i = 0; i < mMeshesCount; i++)
		{
			mMeshesMaterials[i]->CreateVertexBuffer(mGame->Direct3DDevice(), *mModel->Meshes()[i], &(mMeshesRenderBuffers[i].VertexBuffer));
			mModel->Meshes()[i]->CreateIndexBuffer(&(mMeshesRenderBuffers[i].IndexBuffer));
			mMeshesRenderBuffers[i].IndicesCount = mModel->Meshes()[i]->Indices().size();
			mMeshesRenderBuffers[i].Stride = mMeshesMaterials[i]->VertexSize();
			mMeshesRenderBuffers[i].Offset = 0;
		}
	}

	void RenderingObject::Draw()
	{
		for (size_t i = 0; i < mMeshesCount; i++)
		{
			ID3D11DeviceContext1* context = mGame->Direct3DDeviceContext();
			Pass* pass = mMeshesMaterials.at(i)->CurrentTechnique()->Passes().at(0);
			ID3D11InputLayout* inputLayout = mMeshesMaterials.at(i)->InputLayouts().at(pass);
			context->IASetInputLayout(inputLayout);

			UINT stride = mMeshesRenderBuffers[i].Stride;
			UINT offset = mMeshesRenderBuffers[i].Offset;
			context->IASetVertexBuffers(0, 1, &(mMeshesRenderBuffers[i].VertexBuffer), &stride, &offset);
			context->IASetIndexBuffer(mMeshesRenderBuffers[i].IndexBuffer, DXGI_FORMAT_R32_UINT, 0);

			for (auto listener : MeshMaterialVariablesUpdateEvent->GetListeners())
				listener(i);

			pass->Apply(0, context);
			context->DrawIndexed(mMeshesRenderBuffers[i].IndicesCount, 0, 0);
		}
	}

}

