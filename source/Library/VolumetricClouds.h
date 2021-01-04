#pragma once
#include "Common.h"
#include "ConstantBuffer.h"
#include "PostProcessingStack.h"
#include "CustomRenderTarget.h"

namespace Library
{
	class FullScreenRenderTarget;
	class DirectionalLight;
	class GameComponent;
	class GameTime;
	class Camera;
	class Skybox;

	namespace VolumetricCloudsCBufferData {
		struct FrameCB
		{
			XMMATRIX	InvProj;
			XMMATRIX	InvView;
			XMVECTOR	LightDir;
			XMVECTOR	LightCol;
			XMVECTOR	CameraPos;
			XMFLOAT2	Resolution;
		};

		struct CloudsCB
		{
			XMVECTOR AmbientColor;
			XMVECTOR WindDir;
			float WindSpeed;
			float Time;
			float Crispiness;
			float Curliness;
			float Coverage;
			float Absorption;
			float CloudsBottomHeight;
			float CloudsTopHeight;
			float DensityFactor;
		};
	}

	class VolumetricClouds : public GameComponent
	{
	public:
		VolumetricClouds(Game& game, Camera& camera, DirectionalLight& light, Rendering::PostProcessingStack& stack, Skybox& skybox);
		~VolumetricClouds();

		void Initialize();

		void Draw(const GameTime& gametime);
		void Update(const GameTime& gameTime);
		void Config() { mShowDebug = !mShowDebug; }
	private:
		void UpdateImGui();

		Camera& mCamera;
		DirectionalLight& mDirectionalLight;
		Skybox& mSkybox;
		Rendering::PostProcessingStack& mPostProcessingStack;
		
		ConstantBuffer<VolumetricCloudsCBufferData::FrameCB> mFrameConstantBuffer;
		ConstantBuffer<VolumetricCloudsCBufferData::CloudsCB> mCloudsConstantBuffer;

		CustomRenderTarget* mCustomMainRenderTargetCS = nullptr;
		FullScreenRenderTarget* mMainRenderTargetPS = nullptr;
		FullScreenRenderTarget* mCompositeRenderTarget = nullptr;
		FullScreenRenderTarget* mBlurRenderTarget = nullptr;

		ID3D11ComputeShader* mMainCS = nullptr;
		ID3D11PixelShader* mMainPS = nullptr;
		ID3D11PixelShader* mCompositePS = nullptr;
		ID3D11PixelShader* mBlurPS = nullptr;

		ID3D11ShaderResourceView* mCloudTextureSRV = nullptr;
		ID3D11ShaderResourceView* mWeatherTextureSRV = nullptr;
		ID3D11ShaderResourceView* mWorleyTextureSRV = nullptr;

		ID3D11SamplerState* mCloudSS = nullptr;
		ID3D11SamplerState* mWeatherSS = nullptr;

		float mCrispiness = 43.0f;
		float mCurliness = 1.1f;
		float mCoverage = 0.305f;
		float mAmbientColor[3] = { 102.0f / 255.0f, 104.0f / 255.0f, 105.0f / 255.0f };
		float mWindSpeedMultiplier = 175.0f;
		float mLightAbsorption = 0.003f;
		float mCloudsBottomHeight = 2340.0f;
		float mCloudsTopHeight = 16400.0f;
		float mDensityFactor = 0.012f;

		bool mUseComputeShaderVersion = true;
		bool mEnabled = true;
		bool mShowDebug = false;
	};
}