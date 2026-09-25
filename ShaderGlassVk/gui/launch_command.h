/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2026 Thomas Eric, bmitch87
GNU General Public License v3.0

The launch options to paste into Steam, worked out from the current settings.

The interface cannot launch a game -- Steam or the user does -- so the gamescope route (DESIGN.md,
"The guaranteed fallback: gamescope") is delivered as text: the exact line, generated from what is
set, to paste in front of %command%.

There are two ways to run under gamescope and they draw different pictures, so the choice is the
user's and not inferred:

  In the game       gamescope -w -h ... -- env SHADERGLASS=1 %command%
                    The game renders small, the layer shades it at that size, gamescope scales the
                    shaded result up. Cheapest, and gamescope magnifies the shader's own detail with
                    everything else, so a one-pixel scanline becomes a fat bar.

  On gamescope      SHADERGLASS=1 gamescope --force-composition ... -- env SHADERGLASS_DISABLE=1 %command%
                    The layer shades gamescope's own composited output. Works for anything gamescope
                    can show -- an OpenGL game included, which the layer cannot otherwise see.

The second works on every gamescope backend. On SDL the layer shades the swapchain gamescope presents;
on Wayland, DRM, OpenVR and headless, where nothing is presented through Vulkan, it records itself into
gamescope's composite (layer/src/gamescope_output.h). What it cannot shade is a frame gamescope never
composites: a fullscreen game with nothing on top is scanned out directly, which is the common case,
so the line asks for --force-composition.

Where gamescope cannot be started -- already inside a gamescope session, or gamescope missing -- the
same promise is kept another way: the layer runs in the game, and Zink routes OpenGL through Vulkan so
an OpenGL game reaches it. A session's own gamescope could be shaded too, but only if it was started
with SHADERGLASS=1, which a game's launch options cannot do.

A custom gamescope build is named by its path in place of "gamescope". The layer recognises
gamescope's process by its name, so a build whose file is called something else is also named to the
layer, as SHADERGLASS_GAMESCOPE=<path> -- matched against the running binary, so the game, which
inherits it, is not mistaken for gamescope.

Which process carries SHADERGLASS=1 is the whole difference, and it has to be exactly one of them.
The layer is named in the session's VK_INSTANCE_LAYERS (docs/DLSS5VKLayer.md), so it is loaded into
gamescope as well as into the game; set in both, the picture would be shaded twice. So the variable
goes on one side of the "--" and SHADERGLASS_DISABLE=1 on the other where it matters.

Plain C++, no Qt, so it can be tested without a display.
*/

#pragma once

#include <cstdint>
#include <string>

namespace shaderglass {

enum class LaunchWhere {
    kGame,       // the layer runs in the game; gamescope, if used, scales the shaded result
    kGamescope,  // the layer runs in gamescope and shades its output
};

struct LaunchInput {
    LaunchWhere where = LaunchWhere::kGame;

    // Empty for the gamescope on PATH; otherwise the full path of a custom build, which is also what
    // gamescopeInstalled then says was found.
    std::string gamescopeBinary;

    bool gamescopeInstalled = false;
    bool insideGamescope = false;  // GAMESCOPE_WAYLAND_DISPLAY is set: already nested, do not nest again

    uint32_t displayW = 0, displayH = 0;  // the screen gamescope should fill
    uint32_t renderW = 0, renderH = 0;    // what the game should render at; 0 lets it choose

    uint32_t policy = 0;   // OutputPolicy, mapped onto gamescope's scaler
    bool nearest = true;   // hard pixels, for pixel art
};

struct LaunchCommand {
    std::string line;  // to paste into Steam's launch options; always ends in %command%
    std::string note;  // why it is this line, in one sentence for the interface to show
};

LaunchCommand BuildLaunchCommand(const LaunchInput& in);

}  // namespace shaderglass
