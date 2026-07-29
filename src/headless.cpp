// Copyright (c) 2026 Bitflash developers
// Distributed under the MIT/X11 software license.
//
// Stand-ins for the handful of symbols the node core expects the GUI to
// provide, so a headless binary can be linked without ImGui, GLFW or OpenGL.
//
// This exists because linking the GUI made the node need libGL at load time
// even when started with -nogui: the dynamic loader resolves everything before
// main() runs, so the flag came far too late. On a clean server -- which is
// where a node most naturally lives -- that is a hard failure with an error
// message about shared libraries and no hint that the program would otherwise
// have worked fine.

#include "headers_core.h"

// The GUI uses this to mark itself dirty. With no window to repaint there is
// nothing to do, and the call sites in main.cpp and net.cpp stay unchanged.
void MainFrameRepaint()
{
}
