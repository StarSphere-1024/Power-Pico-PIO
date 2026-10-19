#include <assert.h>
#include <stdio.h>
#include "lvgl.h"
#include "font/ui_fonts.h"
#include "common/lv_i18n.h"
#include "src/misc/lv_text_private.h"

static void check_text(const lv_font_t *font, const char *text)
{
    uint32_t offset = 0;
    while (text[offset]) {
        uint32_t codepoint = lv_text_encoded_next(text, &offset);
        if (codepoint == '\n') continue;
        lv_font_glyph_dsc_t glyph;
        if (!lv_font_get_glyph_dsc(font, &glyph, codepoint, 0) || glyph.is_placeholder) {
            fprintf(stderr, "Missing U+%04X at line height %d\n", codepoint, (int)font->line_height);
            assert(0);
        }
    }
}

int main(void)
{
    lv_init();
    const lv_font_t *barlow[] = {
        &ui_font_barlow_14, &ui_font_barlow_16, &ui_font_barlow_18,
        &ui_font_barlow_20, &ui_font_barlow_22, &ui_font_barlow_28,
        &ui_font_barlow_36, &ui_font_barlow_48
    };
    for (unsigned i = 0; i < sizeof(barlow) / sizeof(*barlow); ++i) {
        lv_font_glyph_dsc_t zero;
        assert(lv_font_get_glyph_dsc(barlow[i], &zero, '0', 0));
        for (uint32_t digit = '0'; digit <= '9'; ++digit) {
            for (const char *next = "0123456789.:AV "; *next; ++next) {
                lv_font_glyph_dsc_t glyph;
                assert(lv_font_get_glyph_dsc(barlow[i], &glyph, digit, *next));
                assert(!glyph.is_placeholder && glyph.adv_w == zero.adv_w);
            }
        }
        if (barlow[i] == &ui_font_barlow_36 || barlow[i] == &ui_font_barlow_48) {
            assert(zero.format == LV_FONT_GLYPH_FORMAT_A4);
            zero.req_raw_bitmap = 1;
            const uint8_t *bitmap = zero.resolved_font->get_glyph_bitmap(&zero, NULL);
            assert(bitmap);
            /* An unmarked zero must have an empty center, not a dot or slash. */
            uint32_t pixel = (zero.box_h / 2) * zero.box_w + zero.box_w / 2;
            uint8_t alpha = (bitmap[pixel / 2] >> (pixel % 2 ? 0 : 4)) & 0x0f;
            assert(alpha == 0);
        }
    }
    check_text(&ui_font_barlow_36, "µAmAAµWmWW");
    const char *unit_texts[] = {"V", "µA", "µW", "mW"};
    for (unsigned i = 0; i < sizeof(unit_texts) / sizeof(*unit_texts); ++i) {
        lv_point_t text_size;
        lv_text_get_size(&text_size, unit_texts[i], &ui_font_barlow_36, 0, 0,
                         LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        assert(text_size.x <= 64);
    }
    check_text(&ui_font_barlow_48, "-0123456789.:");

    const lv_font_t *glow[] = {&ui_font_glow_18, &ui_font_glow_20};
    const char *phrases18[] = {
        "Screen Brightness :", "Enable key sound", "Enable Chinese", "Chose Rotation",
        "Current Range", "AUTO", "LOW", "MID", "HIGH"
    };
    const char *phrases20[] = {
        "press to Step Adjust", "press here to Fixed Set", "press here to close PPS",
        "PD Sink", "Please reboot to apply\nall changes!"
    };
    const char **phrases[] = {phrases18, phrases20};
    const size_t phrase_counts[] = {
        sizeof(phrases18) / sizeof(*phrases18), sizeof(phrases20) / sizeof(*phrases20)
    };
    lv_i18n_init(lv_i18n_language_pack);
    lv_i18n_set_locale("zh-cn");
    for (unsigned i = 0; i < sizeof(glow) / sizeof(*glow); ++i) {
        for (unsigned p = 0; p < phrase_counts[i]; ++p) {
            check_text(glow[i], lv_i18n_get_text(phrases[i][p]));
        }
        check_text(glow[i], "µA µW µAh mAh Ah");
        lv_font_glyph_dsc_t latin;
        assert(lv_font_get_glyph_dsc(glow[i], &latin, '1', 0));
        assert(latin.resolved_font == (i == 0 ? &ui_font_barlow_18 : &ui_font_barlow_20));
    }
    check_text(&ui_font_glow_18, "▶");
    puts("PASS: tabular digits without pair-dependent spacing, micro units, translated Chinese, Latin fallback");
    lv_deinit();
    return 0;
}
