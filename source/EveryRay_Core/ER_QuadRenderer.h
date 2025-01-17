#pragma once
#include "Common.h"
#include "ER_CoreComponent.h"
#include "RHI/ER_RHI.h"

namespace EveryRay_Core 
{
	struct QuadVertex
	{
		XMFLOAT3 Position;
		XMFLOAT2 TextureCoordinates;
	};
	class ER_QuadRenderer : public ER_CoreComponent
	{
		RTTI_DECLARATIONS(ER_QuadRenderer, ER_CoreComponent)
	public:
		ER_QuadRenderer(ER_Core& game);
		~ER_QuadRenderer();

		void Setup();
		void PrepareDraw(ER_RHI* rhi);
		void Draw(ER_RHI* rhi, bool unbindShader = true);

	private:
		ER_RHI_GPUShader* mVS = nullptr;
		ER_RHI_InputLayout* mInputLayout = nullptr;
		ER_RHI_GPUBuffer* mVertexBuffer = nullptr;
		ER_RHI_GPUBuffer* mIndexBuffer = nullptr;
	};
}

