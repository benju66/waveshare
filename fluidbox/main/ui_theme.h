#pragma once

#include "lvgl.h"

// Theme helpers: every page picks its fonts through these at creation time.
// The retro setting swaps the smooth Montserrat set for UNSCII bitmap pixel
// fonts; big "hero" numerals get the pixel font scaled up chunky. Changing
// the theme restarts the device so creation-time styling stays simple.

const lv_font_t *theme_font_big(void);    // countdown digits, clock, temp
const lv_font_t *theme_font_mid(void);    // row labels, conditions
const lv_font_t *theme_font_small(void);  // footnotes, version, stale age

// Applies the big font plus, in retro, a center-pivot chunky upscale.
void theme_apply_big(lv_obj_t *label);

// Corner radius: squares in retro, the given radius otherwise.
int theme_radius(int normal);
