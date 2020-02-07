#include "vulkan\vulkan_core.h"

struct SDL_Window;

namespace CG
{
    namespace Vk 
    {
        class SwapChain;
        class Device;
    }
    struct EngineConfig;

    class Engine
    {
    public:
        Engine(const CG::EngineConfig& engineConfig);

		virtual VkPhysicalDeviceFeatures GetEnabledDeviceFeatures();

        void Run();

    private:
        bool Init();
        void MainLoop();
        void Cleanup();

        bool InitSDL();
        bool InitWindow();
        bool InitGraphicsAPI();
        bool InitSurface();

		bool CreateVkInstance();
		bool SetupDebugging();
		bool CreateDevices();
        bool CreateSwapChain();
        bool SetupSemaphores();

        void CleanupSDL();
        void PollEvents();

        const CG::EngineConfig& engineConfig;

        SDL_Window* window = nullptr;
        Vk::Device* vkDevice = nullptr;
        Vk::SwapChain* vkSwapChain = nullptr;
        VkQueue queue = {};

		VkPhysicalDevice vkPhysicalDevice = {};
		VkInstance vkInstance = {};
		VkSurfaceKHR surface = {};

        struct {
            // Swap chain image presentation
            VkSemaphore presentComplete;
            // Command buffer submission and execution
            VkSemaphore renderComplete;
        } semaphores;

        bool isRunning = false;
    };
}