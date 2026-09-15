/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2021-2025 mausimus (mausimus.net)
Copyright (C) 2026 Thomas Eric, bmitch87
https://github.com/mausimus/ShaderGlass
GNU General Public License v3.0

Derived from DLSS5VKLayer's dlssnr-shmctl (relicensed to GPL-3.0, see RELICENSE.md).

shaderglass-ctl -- read and write the shared-memory header from a shell.

Table-driven on purpose. A setting added to the header and not to the table below is one nobody can
reach from here, and the table is short enough that the omission is obvious. Every offset is derived
from shm_protocol.h by including it, so the header and this tool cannot disagree -- the alternative,
poking at hardcoded byte offsets with dd, is right for exactly one layout and silently wrong for the
next.
*/

#include "../common/shm_protocol.h"

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace shaderglass;

namespace {

struct Setting {
    const char* name;
    std::atomic<uint32_t> ShmHeader::*field;
    bool isFloat;
    const char* help;
};

const Setting kSettings[] = {
    {"enabled", &ShmHeader::enabled, false, "0/1 run the shader chain at all"},
    {"paused", &ShmHeader::paused, false, "0/1 freeze the chain on the last composed frame"},
    {"sourcemode", &ShmHeader::sourceMode, false, "0 native, 1 divisor, 2 raster, 3 custom"},
    {"sourcedivisor", &ShmHeader::sourceDivisor, false, "2..8, for sourcemode 1"},
    {"sourcewidth", &ShmHeader::sourceWidth, false, "source raster width, for sourcemode 2/3"},
    {"sourceheight", &ShmHeader::sourceHeight, false, "source raster height, for sourcemode 2/3"},
    {"pixelsize", &ShmHeader::pixelSizeBits, true, "swapchain pixels per source pixel; 0 is Auto"},
    {"outputpolicy", &ShmHeader::outputPolicy, false,
     "0 auto, 1 stretch, 2 fit, 3 fill, 4 integer, 5 centre"},
    {"aspect", &ShmHeader::aspectRatioBits, true, "pixel-aspect correction; 1.0 is none"},
    {"fliph", &ShmHeader::flipHorizontal, false, "0/1 flip horizontally"},
    {"flipv", &ShmHeader::flipVertical, false, "0/1 flip vertically"},
    {"rotation", &ShmHeader::rotation, false, "0, 1, 2, 3 -> 0, 90, 180, 270 degrees"},
    {"cropenabled", &ShmHeader::cropEnabled, false, "0/1 confine the effect to a rectangle"},
    {"cropx", &ShmHeader::cropX, false, "crop origin x, in swapchain pixels"},
    {"cropy", &ShmHeader::cropY, false, "crop origin y, in swapchain pixels"},
    {"cropwidth", &ShmHeader::cropWidth, false, "crop width, in swapchain pixels"},
    {"cropheight", &ShmHeader::cropHeight, false, "crop height, in swapchain pixels"},
    {"frameskip", &ShmHeader::frameSkip, false, "run the chain every Nth frame; 0 and 1 mean every"},
};

void Usage() {
    fprintf(stderr,
            "usage: shaderglass-ctl <shm-path> <command> [args]\n"
            "\n"
            "commands:\n"
            "  status          print the header, one 'key=value' per line\n"
            "  settings        list the settings and their current values\n"
            "  set <key> <v>   change one setting\n"
            "  toggle <key>    flip a setting between 0 and 1\n"
            "  preset <id>     select a preset by catalogue id; empty string for none\n"
            "  capture <n>     write n matched before/after frames\n"
            "  reset           put the settings back to their defaults, leaving status alone\n");
    fprintf(stderr, "\nsettings:\n");
    for (const auto& s : kSettings) fprintf(stderr, "  %-14s %s\n", s.name, s.help);
}

ShmHeader* MapHeader(const char* path, bool create, void** base, int* fdOut) {
    const int flags = create ? (O_RDWR | O_CREAT) : O_RDWR;
    const int fd = open(path, flags, 0600);
    if (fd < 0) return nullptr;

    struct stat st {};
    if (fstat(fd, &st) != 0) {
        close(fd);
        return nullptr;
    }
    if ((size_t) st.st_size < ShmTotalBytes()) {
        if (!create) {  // nothing has ever attached; there is nothing to talk to
            close(fd);
            return nullptr;
        }
        if (ftruncate(fd, (off_t) ShmTotalBytes()) != 0) {
            close(fd);
            return nullptr;
        }
    }

    void* m = mmap(nullptr, ShmTotalBytes(), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) {
        close(fd);
        return nullptr;
    }
    *base = m;
    *fdOut = fd;
    return (ShmHeader*) m;
}

bool Initialised(const ShmHeader* h) {
    return h->magic.load() == kShmMagic && h->version.load() == kShmVersion;
}

void PrintSettings(ShmHeader* h) {
    for (const auto& s : kSettings) {
        const uint32_t raw = (h->*s.field).load();
        if (s.isFloat) printf("%s=%g\n", s.name, double(BitsToFloat(raw)));
        else printf("%s=%u\n", s.name, raw);
    }
    printf("preset=%s\n", ShmPresetId(h).c_str());
}

// Returns false when the name is not a setting, so the caller can say so rather than silently
// succeeding at nothing.
bool ApplySetting(ShmHeader* h, const char* name, const char* value) {
    for (const auto& s : kSettings) {
        if (strcmp(s.name, name) != 0) continue;
        const double v = atof(value);
        (h->*s.field).store(s.isFloat ? FloatToBits(float(v)) : uint32_t(v < 0 ? 0 : v));
        h->controlSeq.fetch_add(1);

        // Anything that forces the chain to be rebuilt also bumps the tuning sequence. A parameter
        // tweak must not, or every slider drag would rebuild the whole chain.
        static const char* kRebuild[] = {"sourcemode", "sourcedivisor", "sourcewidth",
                                         "sourceheight", "pixelsize", "outputpolicy"};
        for (const char* k : kRebuild) {
            if (strcmp(k, name) == 0) {
                h->tuningSeq.fetch_add(1);
                break;
            }
        }
        return true;
    }
    return false;
}

void PrintStatus(const ShmHeader* h) {
    printf("initialised=%d\n", Initialised(h) ? 1 : 0);
    printf("magic=%#x\nversion=%u\n", h->magic.load(), h->version.load());
    if (!Initialised(h)) return;

    static const char* kStates[] = {"detached", "idle", "active", "failed"};
    const uint32_t st = h->layerState.load();
    printf("layer_state=%s\n", st < 4 ? kStates[st] : "unknown");
    printf("layer_pid=%u\n", h->layerPid.load());
    printf("layer_frames=%llu\n", (unsigned long long) ShmLoad64(h->layerFramesLo, h->layerFramesHi));
    printf("layer_heartbeat=%u\n", h->layerHeartbeat.load());
    printf("control_seq=%u\ntuning_seq=%u\n", h->controlSeq.load(), h->tuningSeq.load());
    printf("swapchain=%ux%u fmt=%u\n", h->swapWidth.load(), h->swapHeight.load(),
           h->swapFormat.load());
    printf("source=%ux%u\n", h->sourceActualWidth.load(), h->sourceActualHeight.load());
    printf("output=%ux%u\n", h->outputActualWidth.load(), h->outputActualHeight.load());
    printf("passes=%u\n", h->passCount.load());
    printf("fps=%.1f\n", double(BitsToFloat(h->fpsBits.load())));
    printf("preset=%s\n", ShmPresetId(h).c_str());

    const std::string game = ShmLoadString(h->gameNameSeq, h->gameName, kNameBytes);
    if (!game.empty()) printf("game=%s\n", game.c_str());
    const std::string why = ShmLoadString(h->layerReasonSeq, h->layerReason, kReasonBytes);
    if (!why.empty()) printf("layer_reason=%s\n", why.c_str());
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        Usage();
        return 2;
    }
    const char* path = argv[1];
    const char* cmd = argv[2];

    const bool create = !strcmp(cmd, "set") || !strcmp(cmd, "toggle") || !strcmp(cmd, "preset") ||
                        !strcmp(cmd, "capture") || !strcmp(cmd, "reset") || !strcmp(cmd, "settings");

    void* base = nullptr;
    int fd = -1;
    ShmHeader* h = MapHeader(path, create, &base, &fd);
    if (!h) {
        fprintf(stderr, "no mapping at %s\n", path);
        return 1;
    }
    if (create && !Initialised(h)) ShmInitDefaults(h);

    int rc = 0;
    if (!strcmp(cmd, "status")) {
        PrintStatus(h);
    } else if (!strcmp(cmd, "settings")) {
        PrintSettings(h);
    } else if (!strcmp(cmd, "reset")) {
        ShmResetSettings(h);
    } else if (!strcmp(cmd, "preset")) {
        if (argc != 4) {
            Usage();
            rc = 2;
        } else {
            ShmStoreString(h->presetSeq, h->presetId, kPresetIdBytes, argv[3]);
            h->controlSeq.fetch_add(1);
            h->tuningSeq.fetch_add(1);
        }
    } else if (!strcmp(cmd, "capture")) {
        if (argc != 4) {
            Usage();
            rc = 2;
        } else {
            h->captureRequest.store((uint32_t) atoi(argv[3]));
            h->controlSeq.fetch_add(1);
        }
    } else if (!strcmp(cmd, "toggle")) {
        if (argc != 4) {
            Usage();
            rc = 2;
        } else {
            bool found = false;
            for (const auto& s : kSettings) {
                if (strcmp(s.name, argv[3]) != 0) continue;
                found = true;
                const uint32_t next = (h->*s.field).load() ? 0u : 1u;
                (h->*s.field).store(s.isFloat ? FloatToBits(float(next)) : next);
                h->controlSeq.fetch_add(1);
                printf("%s=%u\n", s.name, next);
                break;
            }
            if (!found) {
                fprintf(stderr, "unknown setting: %s\n", argv[3]);
                rc = 2;
            }
        }
    } else if (!strcmp(cmd, "set")) {
        if (argc != 5) {
            Usage();
            rc = 2;
        } else if (!ApplySetting(h, argv[3], argv[4])) {
            fprintf(stderr, "unknown setting: %s\n", argv[3]);
            rc = 2;
        }
    } else {
        Usage();
        rc = 2;
    }

    msync(base, ShmTotalBytes(), MS_SYNC);
    munmap(base, ShmTotalBytes());
    close(fd);
    return rc;
}
