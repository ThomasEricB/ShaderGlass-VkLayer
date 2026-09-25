/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2026 Thomas Eric, bmitch87
GNU General Public License v3.0

The Steam launch options the interface generates.

The invariant that matters most is the one a wrong line would break silently: SHADERGLASS=1 reaches
exactly one process. The layer is loaded into gamescope as well as the game, so a line that enables it
in both shades the picture twice, and one that enables it in neither shades nothing -- and both look
like "it runs". Every case here checks which side of the "--" the variable landed on.
*/

#include "../gui/launch_command.h"
#include "../common/shm_protocol.h"

#include <cstdio>
#include <string>

using namespace shaderglass;

namespace {

int g_failures = 0;

void Check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("  FAIL  %s\n", what.c_str());
        ++g_failures;
    }
}

// Split at the " -- " that separates gamescope's arguments from the game's command.
void Sides(const std::string& line, std::string* before, std::string* after) {
    const size_t cut = line.find(" -- ");
    *before = cut == std::string::npos ? line : line.substr(0, cut);
    *after = cut == std::string::npos ? std::string() : line.substr(cut + 4);
}

bool Has(const std::string& s, const char* what) { return s.find(what) != std::string::npos; }

void Expect(const char* name, const LaunchInput& in, const char* want) {
    const LaunchCommand c = BuildLaunchCommand(in);
    const bool ok = c.line == want;
    Check(ok, std::string(name) + ": got \"" + c.line + "\"");
    static const std::string tail = "%command%";
    Check(c.line.size() >= tail.size() &&
              c.line.compare(c.line.size() - tail.size(), tail.size(), tail) == 0,
          std::string(name) + ": ends in %command%");
    if (ok) std::printf("  ok    %-30s %s\n", name, c.line.c_str());
}

LaunchInput Base() {
    LaunchInput in;
    in.gamescopeInstalled = true;
    in.displayW = 2560;
    in.displayH = 1080;
    in.renderW = 320;
    in.renderH = 224;
    in.policy = kOutputInteger;
    return in;
}

}  // namespace

int main() {
    std::printf("launch options\n");

    Expect("in the game, integer", Base(),
           "gamescope -w 320 -h 224 -W 2560 -H 1080 -S integer -F nearest -f -- env SHADERGLASS=1 "
           "%command%");
    {
        LaunchInput in = Base();
        in.where = LaunchWhere::kGamescope;
        Expect("on gamescope's output", in,
               "SHADERGLASS=1 gamescope --force-composition -W 2560 -H 1080 -S integer -F nearest "
               "-f -- env SHADERGLASS_DISABLE=1 %command%");
    }
    {
        LaunchInput in = Base();
        in.policy = kOutputCentre;
        in.nearest = false;
        Expect("centre 1:1, linear", in,
               "gamescope -w 320 -h 224 -W 2560 -H 1080 -S integer -m 1 -F linear -f -- env "
               "SHADERGLASS=1 %command%");
    }
    {
        LaunchInput in = Base();
        in.insideGamescope = true;
        Expect("already inside gamescope", in, "SHADERGLASS=1 %command%");
    }
    {
        LaunchInput in = Base();
        in.gamescopeInstalled = false;
        Expect("no gamescope", in, "SHADERGLASS=1 %command%");
    }

    // Where gamescope's output cannot be shaded, the OpenGL promise is kept by Zink in the game.
    const char* zinkLine = "SHADERGLASS=1 MESA_LOADER_DRIVER_OVERRIDE=zink GALLIUM_DRIVER=zink "
                           "__GLX_VENDOR_LIBRARY_NAME=mesa %command%";
    {
        LaunchInput in = Base();
        in.where = LaunchWhere::kGamescope;
        in.insideGamescope = true;
        Expect("output, inside gamescope", in, zinkLine);
    }
    {
        LaunchInput in = Base();
        in.where = LaunchWhere::kGamescope;
        in.gamescopeInstalled = false;
        Expect("output, no gamescope", in, zinkLine);
    }

    std::printf("\na custom gamescope build\n");
    {
        LaunchInput in = Base();
        in.gamescopeBinary = "/home/u/gamescope-lanczos/build/src/gamescope";
        Expect("in the game, custom build", in,
               "/home/u/gamescope-lanczos/build/src/gamescope -w 320 -h 224 -W 2560 -H 1080 -S "
               "integer -F nearest -f -- env SHADERGLASS=1 %command%");
        in.where = LaunchWhere::kGamescope;
        Expect("output, custom build named gamescope: no need to name it to the layer", in,
               "SHADERGLASS=1 /home/u/gamescope-lanczos/build/src/gamescope --force-composition -W "
               "2560 -H 1080 -S integer -F nearest -f -- env SHADERGLASS_DISABLE=1 %command%");
    }
    {
        LaunchInput in = Base();
        in.where = LaunchWhere::kGamescope;
        in.gamescopeBinary = "/opt/gs/gamescope-lanczos";
        Expect("output, custom build under another name is named to the layer", in,
               "SHADERGLASS=1 SHADERGLASS_GAMESCOPE=/opt/gs/gamescope-lanczos "
               "/opt/gs/gamescope-lanczos --force-composition -W 2560 -H 1080 -S integer -F "
               "nearest -f -- env SHADERGLASS_DISABLE=1 %command%");
    }
    {
        LaunchInput in = Base();
        in.gamescopeBinary = "/home/u/My Builds/gamescope";
        const std::string line = BuildLaunchCommand(in).line;
        Check(Has(line, "'/home/u/My Builds/gamescope' -w"), "a path with a space is quoted");
    }
    {
        LaunchInput in = Base();
        in.gamescopeBinary = "/nowhere/gamescope";
        in.gamescopeInstalled = false;
        const LaunchCommand c = BuildLaunchCommand(in);
        Check(c.line == "SHADERGLASS=1 %command%" && Has(c.note, "/nowhere/gamescope was not found"),
              "a custom build that is not there says so, and runs without it");
    }

    // The regression this guards against: a line that shades gamescope's output while letting it scan
    // a fullscreen game out directly, which is never composited and so never shaded. The backend is
    // left to gamescope -- every one of them is shaded.
    std::printf("\nshading gamescope's output always forces composition\n");
    for (uint32_t policy = 0; policy < kOutputPolicyCount; ++policy) {
        LaunchInput in = Base();
        in.where = LaunchWhere::kGamescope;
        in.policy = policy;
        const std::string line = BuildLaunchCommand(in).line;
        if (Has(line, "gamescope ")) {
            Check(Has(line, "--force-composition"),
                  "policy " + std::to_string(policy) + ": forces composition");
            Check(!Has(line, "--backend"),
                  "policy " + std::to_string(policy) + ": leaves the backend to gamescope");
        }
    }
    std::printf("  ok    checked every policy\n");

    std::printf("\nSHADERGLASS=1 reaches exactly one process\n");
    for (LaunchWhere where : {LaunchWhere::kGame, LaunchWhere::kGamescope})
        for (uint32_t policy = 0; policy < kOutputPolicyCount; ++policy) {
            LaunchInput in = Base();
            in.where = where;
            in.policy = policy;
            std::string before, after;
            Sides(BuildLaunchCommand(in).line, &before, &after);
            const bool gs = Has(before, "SHADERGLASS=1"), game = Has(after, "SHADERGLASS=1");
            Check(gs != game, "policy " + std::to_string(policy) + ": enabled in exactly one");
            // The side that is not enabled must be disabled explicitly only when it would otherwise
            // inherit: the game inherits gamescope's environment.
            if (gs) Check(Has(after, "SHADERGLASS_DISABLE=1"), "on gamescope: the game is disabled");
        }
    std::printf("  ok    checked every policy, both ways\n");

    std::printf("\n%s\n", g_failures ? "FAILURES" : "all launch-command checks passed");
    return g_failures ? 1 : 0;
}
