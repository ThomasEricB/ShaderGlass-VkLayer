/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2021-2025 mausimus (mausimus.net)
Copyright (C) 2026 Thomas Eric, bmitch87
https://github.com/mausimus/ShaderGlass
GNU General Public License v3.0

Where this layer sits relative to DLSS5VKLayer, and whether that is the right way round.

Both layers hook vkQueuePresentKHR, and both compose into the swapchain image and then call down the
chain. So whichever of them runs *last* has its work land on top of the other's, and the order is not
a matter of taste: DLSS5VKLayer reconstructs and upscales the frame, and a CRT shader belongs on the
finished picture. Run the other way round, DLSS is handed an already-shadered image to reconstruct --
scanlines, mask and all -- which is not what either layer is for.

The loader gives a layer no way to ask for a position, and no way to move once placed: implicit layer
order comes out of discovery, and neither VK_INSTANCE_LAYERS nor the manifest's filename reorders a
layer that is already implicitly enabled -- both were tried. What a layer *can* do is find out where
it landed, and say so.

The trick is that a function pointer knows which library it came from. Ask the next link in the chain
for vkQueuePresentKHR and hand the answer to dladdr: if it resolves inside DLSS5VKLayer's object,
DLSS is below this layer and will compose last. Anything else, and either DLSS is above -- the right
way round -- or it is not in the chain at all, which the loaded-object check tells apart.

The one case this cannot resolve is a third layer sitting between the two, which makes the pointer
somebody else's and leaves the question open. Reported as unknown rather than guessed.
*/

#pragma once

#include <string>

namespace shaderglass {

enum class DlssPosition {
    Absent,   // DLSS5VKLayer is not loaded in this process
    Above,    // it runs before us, so our shader lands on its output -- the right way round
    Below,    // it runs after us, so it reconstructs an image we have already shaded
    Unknown,  // loaded, but something else sits between us and it
};

// nextPresent is the vkQueuePresentKHR the layer below this one supplied. When `nextOwner` is given
// it receives the object that pointer resolved to, which is the evidence the verdict rests on and
// the only way to tell a wrong answer from a right one when the chain is not what anyone expected.
DlssPosition FindDlssPosition(void* nextPresent, std::string* nextOwner = nullptr);

// One line for the log, and for the interface's reason field when the order is wrong.
const char* DescribeDlssPosition(DlssPosition p);

// True when the order is one the user should be told about.
inline bool DlssOrderIsWrong(DlssPosition p) { return p == DlssPosition::Below; }

}  // namespace shaderglass
