// LVGL version: 9.2
// Project name: PowerPico

#include "./ui.h"
#include "./screens/ui_StartPage.h"
#include "./screens/ui_mainPage.h"
#include "./screens/ui_ChartPage.h"
#include "./screens/ui_SetPage.h"
#include "./screens/ui_PPSPage.h"
#include "./screens/ui_PDFixedPage.h"
#include "BL24C02.h"
#include "key.h"

static lv_obj_t *storage_warning;
static bool storage_warning_dismissed;

static void ui_dismiss_storage_warning(void)
{
    if (storage_warning != NULL) {
        lv_obj_del(storage_warning);
        storage_warning = NULL;
        storage_warning_dismissed = true;
    }
}

static void ui_storage_warning_dismiss_event(lv_event_t *event)
{
    (void)event;
    ui_dismiss_storage_warning();
}

static void ui_storage_warning_screen_loaded(lv_event_t *event)
{
    EEPROM_LoadStatus_t settings_status = Sys_Get_SettingsLoadStatus();
    EEPROM_LoadStatus_t calibration_status = Sys_Get_CalibrationLoadStatus();
    const char *settings_warning = NULL;
    const char *calibration_warning = NULL;
    char message[128];
    lv_obj_t *title;
    lv_obj_t *body;
    lv_obj_t *button;
    lv_obj_t *button_label;

    if (lv_event_get_code(event) != LV_EVENT_SCREEN_LOADED
        || storage_warning_dismissed
        || storage_warning != NULL) {
        return;
    }

    if (settings_status == EEPROM_LOAD_DEFAULTS) {
        settings_warning = "User settings defaults active.";
    } else if (settings_status == EEPROM_LOAD_RECOVERED) {
        settings_warning = "User settings recovered.";
    } else if (settings_status != EEPROM_LOAD_OK
               && settings_status != EEPROM_LOAD_MIGRATED) {
        settings_warning = "User settings unavailable.";
    }

    if (calibration_status == EEPROM_LOAD_RECOVERED) {
        calibration_warning = "Calibration recovered from legacy backup.";
    } else if (calibration_status != EEPROM_LOAD_OK
               && calibration_status != EEPROM_LOAD_MIGRATED) {
        calibration_warning = "Calibration unavailable.\nUsing nominal values.";
    }

    if (settings_warning == NULL && calibration_warning == NULL) {
        return;
    }
    if (settings_warning != NULL && calibration_warning != NULL) {
        snprintf(message, sizeof(message), "%s\n%s", settings_warning, calibration_warning);
    } else {
        snprintf(message,
                 sizeof(message),
                 "%s",
                 settings_warning != NULL ? settings_warning : calibration_warning);
    }

    storage_warning = lv_obj_create(lv_layer_top());
    lv_obj_set_size(storage_warning, 230, 190);
    lv_obj_center(storage_warning);
    lv_obj_remove_flag(storage_warning, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(storage_warning, lv_color_hex(0x503030), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(storage_warning, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

    title = lv_label_create(storage_warning);
    lv_label_set_text(title, "Storage warning");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 5);
    lv_obj_set_style_text_font(title, &ui_font_barlow_18, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN | LV_STATE_DEFAULT);

    body = lv_label_create(storage_warning);
    lv_label_set_text(body, message);
    lv_obj_set_width(body, 210);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(body, &ui_font_barlow_18, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(body, lv_color_white(), LV_PART_MAIN | LV_STATE_DEFAULT);

    button = lv_button_create(storage_warning);
    lv_obj_set_size(button, 110, 34);
    lv_obj_align(button, LV_ALIGN_BOTTOM_MID, 0, -5);
    lv_obj_add_event_cb(button, ui_storage_warning_dismiss_event, LV_EVENT_CLICKED, NULL);
    button_label = lv_label_create(button);
    lv_label_set_text(button_label, "Dismiss");
    lv_obj_center(button_label);
    lv_obj_set_style_text_font(button_label, &ui_font_barlow_18, LV_PART_MAIN | LV_STATE_DEFAULT);
}

static void ui_dispatch_page_key_event(void *event, void (*handler)(void *))
{
    key_event_t *key_event = (key_event_t *)event;

    if (storage_warning != NULL
        && key_event != NULL
        && key_event->type == KEY_EVT_CLICK
        && (key_event->id == KEY_ID_Y || key_event->id == KEY_ID_N)) {
        ui_dismiss_storage_warning();
        return;
    }
    handler(event);
}

static void ui_main_page_key_event(void *event)
{
    ui_dispatch_page_key_event(event, ui_main_page_key_handler);
}

static void ui_set_page_key_event(void *event)
{
    ui_dispatch_page_key_event(event, ui_set_page_key_handler);
}

static void ui_pps_page_key_event(void *event)
{
    ui_dispatch_page_key_event(event, ui_pps_page_key_handler);
}

static void ui_pdfixed_page_key_event(void *event)
{
    ui_dispatch_page_key_event(event, ui_pdfixed_page_key_handler);
}


///////////////////// VARIABLES ////////////////////

// pages
Page_t pages[] = {
    {
        .init = ui_main_screen_init,
        .deinit = ui_main_screen_destroy,
        .page_obj = &ui_HomeScreen,
        .key_event_handler = ui_main_page_key_event,
        .name = "Main Page"
    },
    // {
    //     .init = ui_ChartPage_screen_init,
    //     .deinit = ui_ChartPage_screen_destroy,
    //     .page_obj = &ui_ChartPage,
    //     .key_event_handler = ui_chart_page_key_handler,
    //     .name = "Chart Page"
    // },
    {
        .init = ui_SetPage_screen_init,
        .deinit = ui_SetPage_screen_destroy,
        .page_obj = &ui_SetPage,
        .key_event_handler = ui_set_page_key_event,
        .name = "Set Page"
    },
    {
        .init = ui_PPSPage_screen_init,
        .deinit = ui_PPSPage_screen_destroy,
        .page_obj = &ui_PPSPage,
        .key_event_handler = ui_pps_page_key_event,
        .name = "PPS Page"
    },
    {
        .init = ui_PDFixedPage_screen_init,
        .deinit = ui_PDFixedPage_screen_destroy,
        .page_obj = &ui_PDFixedPage,
        .key_event_handler = ui_pdfixed_page_key_event,
        .name = "PDFixed Page"
    },
    // 可以在这里添加更多页面
};

///////////////////// TEST LVGL SETTINGS ////////////////////
#if LV_COLOR_DEPTH != 16
    #error "LV_COLOR_DEPTH should be 16bit to match SquareLine Studio's settings"
#endif

/////////////////////// Timer //////////////////////
/**
 * Main timer for Refreshing the screens
 */
static void main_timer_cb(lv_timer_t * timer)
{
    // 1s 刷新一次全屏，防止有时候不知什么原因的LCD刷新错误（需要后续找到问题从根本解决不知是不是屏幕本身问题）
    ui_full_screen_refresh(lv_screen_active());
}

////////////////////////// Animation //////////////////////

void ui_start_animation_done() {
    // 初始化页面并加载主要界面
    lv_lib_pm_Init();
    for (uint8_t i = 0; i < sizeof(pages) / sizeof(pages[0]); i++) {
        lv_lib_pm_register(&pages[i]);
    }
    lv_lib_pm_load_init_screen();
    lv_obj_add_event_cb(ui_HomeScreen,
                        ui_storage_warning_screen_loaded,
                        LV_EVENT_SCREEN_LOADED,
                        NULL);
}

void ui_anima_set_light_level(void * var, int32_t v) {
    ui_set_back_light_level(v);
}

static void _ui_Start_animation(void) {
    uint8_t target_level = ui_get_back_light_level();
    lv_lib_anim_user_animation(NULL, 0, 2000, 0, target_level, 0, 0, 0, 0, lv_anim_path_ease_in, ui_anima_set_light_level, ui_start_animation_done);
}

/////////////////////// ui_initialize //////////////////////
void ui_init(void)
{
    lv_disp_t * dispp = lv_display_get_default();
    lv_theme_t * theme = lv_theme_default_init(dispp, lv_palette_main(LV_PALETTE_BLUE), lv_palette_main(LV_PALETTE_RED), true, LV_FONT_DEFAULT);
    lv_disp_set_theme(dispp, theme);

    // set system settings
    ui_set_display_rotation(ui_get_display_rotation());

    // internationalization init
    lv_i18n_init(lv_i18n_language_pack); //lv_i18n 的语言初始化
    if(ui_get_language_select() == 0)
        lv_i18n_set_locale("en");      //lv_i18n 设置当前语言
    else
        lv_i18n_set_locale("zh-cn");  //lv_i18n 设置当前语言

    // main timer (you can add someting to do in the timer_cb if needed)
    lv_timer_t * ui_MainTimer = lv_timer_create(main_timer_cb, 1000,  NULL);

    // start up, just load one time only
    ui_StartPage_screen_init();
    lv_screen_load(ui_StartPage);
    _ui_Start_animation();
}
