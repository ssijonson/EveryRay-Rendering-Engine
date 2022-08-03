#include "stdafx.h"
#include <memory>

#include "..\Library\ER_CoreException.h"
#include "..\Library\RHI\ER_RHI.h"
#include "..\Library\RHI\DX11\ER_RHI_DX11.h"
#include "RenderingGame.h"

#if defined(DEBUG) || defined(_DEBUG)
#define _CRTDBG_MAP_ALLOC
#include <stdlib.h>
#include <crtdbg.h>
#endif

using namespace Library;
using namespace Rendering;

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previousInstance, LPSTR commandLine, int showCommand)
{
	//#if defined(DEBUG) || defined(_DEBUG)
	//_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF|_CRTDBG_LEAK_CHECK_DF);
	//#endif

#if defined(DEBUG) || defined(_DEBUG)
	std::unique_ptr<RenderingGame> game(new RenderingGame(new ER_RHI_DX11(), instance, L"EveryRay Main Window Class", L"EveryRay - Rendering Engine | Win64 DX11 (Debug)", showCommand, 1920, 1080, false));
#else
	std::unique_ptr<RenderingGame> game(new RenderingGame(new ER_RHI_DX11(), instance, L"EveryRay Main Window Class", L"EveryRay - Rendering Engine | Win64 DX11 (Release)", showCommand, 1920, 1080, false));
#endif
	try {
		game->Run();
	}
	catch (ER_CoreException ex)
	{
		MessageBox(game->WindowHandle(), ex.whatw().c_str(), game->WindowTitle().c_str(), MB_ABORTRETRYIGNORE);
	}

	return 0;

}
