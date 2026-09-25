/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2021-2025 mausimus (mausimus.net)
Copyright (C) 2026 Thomas Eric, bmitch87
https://github.com/mausimus/ShaderGlass
GNU General Public License v3.0
*/

#include "game_probe.h"

#include <cstdio>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <signal.h>
#include <sys/types.h>

namespace shaderglass {
namespace {

// The path column of a /proc/<pid>/maps line, or empty for an anonymous mapping. Paths can contain
// spaces, so the field is everything past the fifth column rather than the sixth token.
std::string MappedPath(const std::string& line) {
    int fields = 0;
    size_t i = 0;
    while (i < line.size() && fields < 5) {
        while (i < line.size() && line[i] != ' ') ++i;
        while (i < line.size() && line[i] == ' ') ++i;
        ++fields;
    }
    return i < line.size() ? line.substr(i) : std::string();
}

bool Contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

bool ProcessAlive(uint32_t pid) {
    if (!pid) return false;
    // Signal 0 checks for existence without delivering anything. ESRCH is the only answer that
    // means gone; EPERM means alive and owned by somebody else, which still counts.
    return kill((pid_t) pid, 0) == 0 || errno != ESRCH;
}

RenderApis ProbeRenderApis(uint32_t pid) {
    RenderApis apis {};
    if (!pid) return apis;

    char path[64];
    snprintf(path, sizeof(path), "/proc/%u/maps", pid);
    std::ifstream maps(path);
    if (!maps) return apis;  // gone, or not ours to read

    std::string line;
    while (std::getline(maps, line)) {
        const std::string file = MappedPath(line);
        if (file.empty() || file[0] != '/') continue;

        // GStreamer ships libgstopengl.so for its own video pipeline. That is Proton playing a
        // cut-scene, not the game choosing a renderer, and counting it would mislabel every game
        // that shows a movie.
        if (Contains(file, "/gstreamer-1.0/")) continue;

        if (Contains(file, "/libGLX_") || Contains(file, "/opengl32.so"))
            apis.openglContext = true;
        else if (Contains(file, "/libGL.so") || Contains(file, "/libEGL.so"))
            apis.openglWeak = true;

        if (Contains(file, "/libvulkan.so")) apis.vulkan = true;
        if (Contains(file, "zink")) apis.zink = true;
    }
    return apis;
}

std::string ZinkLaunchOptions() {
    return "MESA_LOADER_DRIVER_OVERRIDE=zink GALLIUM_DRIVER=zink __GLX_VENDOR_LIBRARY_NAME=mesa";
}

}  // namespace shaderglass
