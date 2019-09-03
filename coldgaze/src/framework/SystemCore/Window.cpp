#include "Window.h"
#include "vulkan\vulkan.h"
#include <SDL2/SDL_vulkan.h>
#include <SDL2/SDL.h>

using namespace CG;

namespace SWindow
{
	static const int width = 800;
	static const int height = 600;
}

Window::Window(const VScopedPtr<VkInstance>& instance)
	: _instance(instance)
	, _surface(instance, vkDestroySurfaceKHR)
{
	_init_window();
}

Window::~Window()
{
	if (_window != nullptr)
	{
		terminate();
	}
}

bool Window::is_window_alive()
{
	return _is_window_alive;
}

void Window::poll_events()
{
	SDL_Event event;
	while (SDL_PollEvent(&event)) {
		/* handle your event here */
	}
}

void Window::terminate()
{
	SDL_DestroyWindow(_window);
	SDL_Quit();
}

int Window::_init_window()
{
	using namespace SWindow;

	SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS);

	_width = width;
	_height = height;

	_window = SDL_CreateWindow("CG window", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, _width, _height, SDL_WINDOW_VULKAN);

	return CG_INIT_SUCCESS;
}

VkSurfaceKHR CG::Window::create_surface(eRenderApi renderApi)
{
	switch (renderApi)
	{
	case eRenderApi::none:
		break;

	case eRenderApi::vulkan:
	{
		if (!SDL_Vulkan_CreateSurface(_window, _instance, _surface.replace())) {
			CG_ASSERT(false);
			throw std::runtime_error("failed to create window surface!");
		}
		break;
	}
	default:
		break;
	}

	return _surface;
}
