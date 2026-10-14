#pragma once

#include <string>

struct SDL_Window;

// The launcher's window: SDL3 with Dear ImGui drawn through Vulkan, the only
// renderer the SDK's SDL3 build has (no SDL_Renderer or OpenGL). Vulkan is
// loaded at run time, like the game does.
namespace launcher_window {

// Creates the window (size in logical pixels) and initializes ImGui for it.
bool Open(const char* title, int width, int height, std::string& error);
SDL_Window* Get();
// Between BeginFrame and EndFrame, build the ImGui frame.
void BeginFrame();
void EndFrame();
void Close();

}  // namespace launcher_window
