#include <stddef.h>
#include <stdint.h>
#include <unity.h>
#include "lvgl.h"
#include "font/ui_fonts.h"
#include "common/lv_i18n.h"
#include "src/misc/lv_text_private.h"

static const lv_font_t * const barlow_fonts[] = {
    &ui_font_barlow_14, &ui_font_barlow_16, &ui_font_barlow_18,
    &ui_font_barlow_20, &ui_font_barlow_22, &ui_font_barlow_28,
    &ui_font_barlow_36, &ui_font_barlow_48
};

static const lv_font_t * const glow_fonts[] = {&ui_font_glow_18, &ui_font_glow_20};
static const char * const glow_phrases18[] = {
    "Screen Brightness :", "Enable key sound", "Enable Chinese", "Chose Rotation",
    "Current Range", "AUTO", "LOW", "MID", "HIGH"
};
static const char * const glow_phrases20[] = {
    "press to Step Adjust", "press here to Fixed Set", "press here to close PPS",
    "PD Sink", "Please reboot to apply\nall changes!"
};
static const char * const * const glow_phrases[] = {glow_phrases18, glow_phrases20};
static const size_t glow_phrase_counts[] = {
    sizeof(glow_phrases18) / sizeof(glow_phrases18[0]),
    sizeof(glow_phrases20) / sizeof(glow_phrases20[0])
};

static void assert_text_glyphs(const lv_font_t *font, const char *text)
{
    uint32_t offset = 0;
    while(text[offset]) {
        uint32_t codepoint = lv_text_encoded_next(text, &offset);
        if(codepoint == '\n') continue;

        lv_font_glyph_dsc_t glyph;
        TEST_ASSERT_TRUE_MESSAGE(lv_font_get_glyph_dsc(font, &glyph, codepoint, 0),
                                 "Missing glyph descriptor");
        TEST_ASSERT_FALSE_MESSAGE(glyph.is_placeholder, "Glyph resolved to placeholder");
    }
}

void setUp(void)
{
    __lv_i18n_reset();
    lv_init();
}

void tearDown(void)
{
    lv_deinit();
    __lv_i18n_reset();
}

static void test_barlow_digits_have_pair_independent_spacing(void)
{
    for(size_t i = 0; i < sizeof(barlow_fonts) / sizeof(barlow_fonts[0]); i++) {
        lv_font_glyph_dsc_t zero;
        TEST_ASSERT_TRUE(lv_font_get_glyph_dsc(barlow_fonts[i], &zero, '0', 0));

        for(uint32_t digit = '0'; digit <= '9'; digit++) {
            for(const char *next = "0123456789.:AV "; *next; next++) {
                lv_font_glyph_dsc_t glyph;
                TEST_ASSERT_TRUE(lv_font_get_glyph_dsc(barlow_fonts[i], &glyph, digit, *next));
                TEST_ASSERT_FALSE(glyph.is_placeholder);
                TEST_ASSERT_EQUAL(zero.adv_w, glyph.adv_w);
            }
        }
    }
}

static void test_unmarked_zero_bitmap_center_is_empty(void)
{
    const lv_font_t *fonts[] = {&ui_font_barlow_36, &ui_font_barlow_48};
    for(size_t i = 0; i < sizeof(fonts) / sizeof(fonts[0]); i++) {
        lv_font_glyph_dsc_t zero;
        TEST_ASSERT_TRUE(lv_font_get_glyph_dsc(fonts[i], &zero, '0', 0));
        TEST_ASSERT_EQUAL(LV_FONT_GLYPH_FORMAT_A4, zero.format);

        zero.req_raw_bitmap = 1;
        const uint8_t *bitmap = zero.resolved_font->get_glyph_bitmap(&zero, NULL);
        TEST_ASSERT_NOT_NULL(bitmap);

        /* An unmarked zero must have an empty center, not a dot or slash. */
        uint32_t pixel = (zero.box_h / 2) * zero.box_w + zero.box_w / 2;
        uint8_t alpha = (bitmap[pixel / 2] >> (pixel % 2 ? 0 : 4)) & 0x0f;
        TEST_ASSERT_EQUAL_UINT8(0, alpha);
    }
}

static void test_micro_unit_text_fits_display_width(void)
{
    const char *unit_texts[] = {"V", "µA", "µW", "mW"};
    for(size_t i = 0; i < sizeof(unit_texts) / sizeof(unit_texts[0]); i++) {
        lv_point_t text_size;
        lv_text_get_size(&text_size, unit_texts[i], &ui_font_barlow_36, 0, 0,
                         LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        TEST_ASSERT_TRUE_MESSAGE(text_size.x <= 64, "Micro-unit text exceeds 64 pixels");
    }
}

static void test_ui_strings_have_required_glyphs(void)
{
    assert_text_glyphs(&ui_font_barlow_36, "µAmAAµWmWW");
    assert_text_glyphs(&ui_font_barlow_48, "-0123456789.:");

    for(size_t i = 0; i < sizeof(glow_fonts) / sizeof(glow_fonts[0]); i++) {
        for(size_t p = 0; p < glow_phrase_counts[i]; p++) {
            assert_text_glyphs(glow_fonts[i], glow_phrases[i][p]);
        }
        assert_text_glyphs(glow_fonts[i], "µA µW µAh mAh Ah");
    }
    assert_text_glyphs(&ui_font_glow_18, "▶");
}

static void test_chinese_translations_and_latin_fallback(void)
{
    TEST_ASSERT_EQUAL_INT(0, lv_i18n_init(lv_i18n_language_pack));
    TEST_ASSERT_EQUAL_INT(0, lv_i18n_set_locale("zh-cn"));

    for(size_t i = 0; i < sizeof(glow_fonts) / sizeof(glow_fonts[0]); i++) {
        for(size_t p = 0; p < glow_phrase_counts[i]; p++) {
            assert_text_glyphs(glow_fonts[i], lv_i18n_get_text(glow_phrases[i][p]));
        }
        assert_text_glyphs(glow_fonts[i], "µA µW µAh mAh Ah");

        lv_font_glyph_dsc_t latin;
        TEST_ASSERT_TRUE(lv_font_get_glyph_dsc(glow_fonts[i], &latin, '1', 0));
        TEST_ASSERT_EQUAL_PTR(i == 0 ? &ui_font_barlow_18 : &ui_font_barlow_20,
                              latin.resolved_font);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_barlow_digits_have_pair_independent_spacing);
    RUN_TEST(test_unmarked_zero_bitmap_center_is_empty);
    RUN_TEST(test_micro_unit_text_fits_display_width);
    RUN_TEST(test_ui_strings_have_required_glyphs);
    RUN_TEST(test_chinese_translations_and_latin_fallback);
    return UNITY_END();
}
