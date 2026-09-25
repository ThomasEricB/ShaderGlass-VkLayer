/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2026 Thomas Eric, bmitch87
GNU General Public License v3.0

A backtrace when the process dies, for the games whose crashes never reach the host.

A Steam game runs inside pressure-vessel. When it faults, systemd-coredump on the host sees nothing,
Proton reports nothing, and the layer's log simply stops mid-frame -- which says the process died but
not where. This installs handlers that write a backtrace into that same log before the process goes,
which is the difference between "it dies somewhere" and a list of frames to read.

Off unless SHADERGLASS_CRASH_TRACE=1, because a library mapped into every game on the system has no
business taking over its fatal signals uninvited. The previous handler is restored and the signal
re-raised, so whatever the game or the runtime would have done still happens.
*/

#pragma once

namespace shaderglass {

// Safe to call more than once; only the first call does anything.
void InstallCrashTrace();

}  // namespace shaderglass
