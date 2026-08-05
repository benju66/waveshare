#pragma once

#include "sim.h"

// Draws one frame of the fluid, band by band, straight to the panel.
//
// The renderer is a plain software rasteriser: it projects each particle
// through a pinhole camera, picks a colour from a precomputed ramp, and fills
// a disc. There is no framebuffer for the whole screen; see display.h.

void render_init(void);

// Takes a snapshot of the simulation and paints it. Blocks until the last
// band has been handed to the DMA engine.
void render_frame(void);
