#include "ui_theme.h"

#include "settings.h"

// Press Start 2P (OFL), converted at 1 bpp so pixels stay pixels. Square
// monospace glyphs with full ASCII plus the degree sign - the proportions
// and coverage the UNSCII attempt lacked.
LV_FONT_DECLARE(lv_font_ps2p_32);
LV_FONT_DECLARE(lv_font_ps2p_16);
LV_FONT_DECLARE(lv_font_ps2p_8);

const lv_font_t *theme_font_big(void)
{
    return settings_retro() ? &lv_font_ps2p_32 : &lv_font_montserrat_48;
}

const lv_font_t *theme_font_mid(void)
{
    return settings_retro() ? &lv_font_ps2p_16 : &lv_font_montserrat_20;
}

const lv_font_t *theme_font_small(void)
{
    return settings_retro() ? &lv_font_ps2p_8 : LV_FONT_DEFAULT;
}

void theme_apply_big(lv_obj_t *label)
{
    lv_obj_set_style_text_font(label, theme_font_big(), 0);
}

int theme_radius(int normal)
{
    return settings_retro() ? 0 : normal;
}
