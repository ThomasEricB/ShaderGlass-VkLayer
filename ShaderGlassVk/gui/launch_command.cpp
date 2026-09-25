/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2026 Thomas Eric, bmitch87
GNU General Public License v3.0
*/

#include "launch_command.h"

#include "../common/shm_protocol.h"

#include <cctype>
#include <cstring>

namespace shaderglass {
namespace {

// gamescope's scaler for each output policy (DESIGN.md's table). Centre 1:1 has no scaler of its own;
// integer capped at one times is the same picture.
std::string ScalerArgs(uint32_t policy) {
    switch (policy) {
        case kOutputStretch: return "-S stretch";
        case kOutputFit: return "-S fit";
        case kOutputFill: return "-S fill";
        case kOutputInteger: return "-S integer";
        case kOutputCentre: return "-S integer -m 1";
        case kOutputAuto:
        default: return "-S auto";
    }
}

// A word for Steam's launch options, which splits on spaces and honours shell quoting. Paths are
// usually plain and stay readable; anything else is single-quoted.
std::string ShellWord(const std::string& w) {
    bool plain = !w.empty();
    for (char c : w)
        if (!(isalnum((unsigned char) c) || strchr("/._-+:,@%=", c))) plain = false;
    if (plain) return w;
    std::string q = "'";
    for (char c : w) q += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return q + "'";
}

std::string Basename(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

}  // namespace

// What routes an OpenGL game's rendering through Vulkan, into a layer in the game's own process. Harmless
// for a Vulkan game, which never loads a GL driver for these to affect.
const char* kZink = "MESA_LOADER_DRIVER_OVERRIDE=zink GALLIUM_DRIVER=zink __GLX_VENDOR_LIBRARY_NAME=mesa";

namespace {

std::string Missing(const LaunchInput& in) {
    return in.gamescopeBinary.empty() ? std::string("gamescope is not installed")
                                      : in.gamescopeBinary + " was not found";
}

}  // namespace

LaunchCommand BuildLaunchCommand(const LaunchInput& in) {
    LaunchCommand out;

    // Shading gamescope's output needs a gamescope this line starts. When it cannot start one, the
    // layer goes into the game instead, with Zink so that OpenGL still reaches it: the promise of this
    // mode is "OpenGL games too", and this keeps it.
    if (in.where == LaunchWhere::kGamescope && (in.insideGamescope || !in.gamescopeInstalled)) {
        out.line = std::string("SHADERGLASS=1 ") + kZink + " %command%";
        out.note = in.insideGamescope
                       ? "Already inside gamescope, which was not started with the layer switched "
                         "on, so the layer runs in the game and Zink carries OpenGL to it."
                       : Missing(in) + ", so the layer runs in the game and Zink carries OpenGL "
                                       "to it.";
        return out;
    }

    // Already inside gamescope -- a Steam Deck in game mode, or a nested session -- another one would
    // only put a compositor inside a compositor.
    if (in.insideGamescope) {
        out.line = "SHADERGLASS=1 %command%";
        out.note = "Already running inside gamescope, so it is not nested again.";
        return out;
    }
    if (!in.gamescopeInstalled) {
        out.line = "SHADERGLASS=1 %command%";
        out.note = Missing(in) + ", so the layer runs in the game on its own.";
        return out;
    }

    // On gamescope's output, only a composited frame can be shaded, and gamescope skips composition
    // whenever it can scan the game out directly -- a fullscreen game with nothing on top, which is
    // most of the time. In the game it does not matter: the layer has shaded the frame already.
    const std::string binary =
        in.gamescopeBinary.empty() ? std::string("gamescope") : ShellWord(in.gamescopeBinary);
    std::string gs = in.where == LaunchWhere::kGamescope ? binary + " --force-composition" : binary;
    if (in.where == LaunchWhere::kGame && in.renderW && in.renderH)
        gs += " -w " + std::to_string(in.renderW) + " -h " + std::to_string(in.renderH);
    if (in.displayW && in.displayH)
        gs += " -W " + std::to_string(in.displayW) + " -H " + std::to_string(in.displayH);
    gs += " " + ScalerArgs(in.policy);
    gs += in.nearest ? " -F nearest" : " -F linear";
    gs += " -f";

    if (in.where == LaunchWhere::kGame) {
        out.line = gs + " -- env SHADERGLASS=1 %command%";
        out.note = in.renderW && in.renderH
                       ? "The game renders at " + std::to_string(in.renderW) + "x" +
                             std::to_string(in.renderH) +
                             ", the layer shades it there, and gamescope scales the result up."
                       : "The layer runs in the game, and gamescope scales the result to the screen.";
    } else {
        // A custom build under another file name would not be recognised as gamescope by the layer
        // inside it; naming it makes it so.
        std::string named;
        if (!in.gamescopeBinary.empty() && Basename(in.gamescopeBinary) != "gamescope")
            named = "SHADERGLASS_GAMESCOPE=" + ShellWord(in.gamescopeBinary) + " ";
        out.line = "SHADERGLASS=1 " + named + gs + " -- env SHADERGLASS_DISABLE=1 %command%";
        out.note = "The layer shades gamescope's own output, so it works for OpenGL games too; it is "
                   "switched off inside the game so the picture is not shaded twice.";
    }
    return out;
}

}  // namespace shaderglass
