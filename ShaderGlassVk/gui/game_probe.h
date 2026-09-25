/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2021-2025 mausimus (mausimus.net)
Copyright (C) 2026 Thomas Eric, bmitch87
https://github.com/mausimus/ShaderGlass
GNU General Public License v3.0

What a game is rendering with, asked from the outside.

The layer only ever sees Vulkan. A game that renders with OpenGL -- or one that starts on Vulkan for
an intro movie and then hands the screen to OpenGL, which is what several Wine ports do -- simply
stops presenting through the layer, and from the mapping alone that is indistinguishable from the
game having exited. The difference matters: one is nothing to report, the other is a game the user
can still shade by routing OpenGL through Vulkan with Zink.

The evidence is the process's own address space. Two mappings are worth believing:

  libGLX_<vendor>.so  the GLX vendor library, which glvnd loads only once a GLX context exists
  opengl32.so         Wine's translator, loaded only when the Windows binary imports opengl32.dll

Both mean a real context, not a dependency someone pulled in. libGL/libEGL on their own do not --
they ride along with unrelated things -- so they are recorded and never reported. That distinction
is the whole point of the header: a minimised Vulkan game also stops presenting, and accusing it of
being OpenGL would be worse than staying quiet.
*/

#pragma once

#include <cstdint>
#include <string>

namespace shaderglass {

struct RenderApis {
    bool openglContext = false;  // a GLX vendor library or Wine's opengl32.so is mapped
    bool openglWeak = false;     // only libGL/libEGL, which prove nothing on their own
    bool vulkan = false;
    bool zink = false;  // already running OpenGL on Vulkan; nothing to suggest

    // The one question the interface actually asks.
    bool RendersWithOpenGL() const { return openglContext && !zink; }
};

// False once the process is gone, which is the ordinary end of a game and not worth reporting.
bool ProcessAlive(uint32_t pid);

// Everything false when the process is gone or /proc is unreadable, so a failed probe reports
// nothing rather than guessing.
RenderApis ProbeRenderApis(uint32_t pid);

// The launch options that put OpenGL on Vulkan, ready to paste in front of %command%.
std::string ZinkLaunchOptions();

}  // namespace shaderglass
