    /*
    * ILI9341 + ESP32 + LVGL (ESP-IDF v5)
    * SPI Interface | No Touch
    *
    * UI: Temperature + Number of Units Dashboard
    *     - Deep blue theme
    *     - Flat same-line value + unit labels
    *
    * ──────────────────────────────────────────────
    *  REQUIRED  lv_conf.h  FONT FLAGS:
    *
    *    #define LV_FONT_MONTSERRAT_14  1   // labels, sub-text
    *    #define LV_FONT_MONTSERRAT_28  1   // large numeric values
    *
    *  Both must be 1 (not 0) or you will get
    *  "undeclared" compile errors.
    * ──────────────────────────────────────────────
    */

    #include <stdio.h>
    #include <assert.h>

    #include "freertos/FreeRTOS.h"
    #include "freertos/task.h"

    #include "driver/gpio.h"
    #include "driver/spi_master.h"

    #include "esp_timer.h"
    #include "esp_log.h"
    #include "esp_err.h"

    #include "esp_lcd_panel_io.h"
    #include "esp_lcd_panel_ops.h"
    #include "esp_lcd_panel_vendor.h"
    #include "esp_lcd_ili9341.h"

    #include "lvgl.h"
    #include "cascadia_mono.c"


    static const char *TAG = "ILI9341";

    #define LCD_HOST SPI2_HOST


    //////////////////// PIN CONFIG ////////////////////

    #define PIN_NUM_MOSI 11
    #define PIN_NUM_MISO 13
    #define PIN_NUM_CLK  12
    #define PIN_NUM_CS   14
    #define PIN_NUM_DC   10
    #define PIN_NUM_RST  9
    #define PIN_NUM_BCKL 8


    //////////////////// LCD CONFIG ////////////////////

    #define LCD_H_RES 240
    #define LCD_V_RES 320

    #define LCD_PIXEL_CLOCK_HZ (20 * 1000 * 1000)

    #define LCD_CMD_BITS   8
    #define LCD_PARAM_BITS 8

    #define LVGL_TICK_PERIOD_MS 2


    ///////////////////////////////////////////////////


    static lv_disp_draw_buf_t draw_buf;
    static lv_disp_drv_t disp_drv;
    static esp_lcd_panel_handle_t panel_handle;


    ///////////////////////////////////////////////////
    /////////////// UI HANDLES ////////////////////////
    ///////////////////////////////////////////////////

    static lv_obj_t *label_temp_val;
    static lv_obj_t *label_units_val;
    static lv_obj_t *bar_temp;


    /* -----------------------------------------------
    * Call this to push new sensor readings to screen.
    * temp_x10 : temperature * 10  (e.g. 235 = 23.5 C)
    * units     : integer unit count
    * ----------------------------------------------- */
    void ui_update_values(int temp_x10, int units)
    {
        /* Temperature: "23.5" */
        char tbuf[16];
        snprintf(tbuf, sizeof(tbuf), "%d.%d",
                temp_x10 / 10,
                (temp_x10 < 0 ? -temp_x10 : temp_x10) % 10);
        lv_label_set_text(label_temp_val, tbuf);

        /* Units: "4" */
        char ubuf[8];
        snprintf(ubuf, sizeof(ubuf), "%d", units);
        lv_label_set_text(label_units_val, ubuf);

        /* Progress bar (clamp 0-100 C) */
        int bv = temp_x10 / 10;
        if (bv <   0) bv = 0;
        if (bv > 100) bv = 100;
        lv_bar_set_value(bar_temp, bv, LV_ANIM_ON);
    }


    ///////////////////////////////////////////////////
    /////////////// COLOUR PALETTE ////////////////////
    ///////////////////////////////////////////////////

    /*  Deep-blue / electric-blue theme
        BG_DEEP     #060B18   near-black navy
        BG_CARD     #0D1B2E   card surface
        BLUE_BRITE  #4D9FFF   primary electric blue
        BLUE_ICE    #A8D4FF   pale ice blue (units accent)
        BLUE_DIM    #1B3A5C   bar track / divider
        TEXT_HI     #E6F0FF   almost-white blue tint
        TEXT_MID    #7BA8D4   mid-tone blue-grey
        TEXT_LO     #3A5A80   muted label blue     
        #F72585   (hot pink)
    #B5179E   (magenta)
    #7209B7   (purple)
    #560BAD   (deep violet)
    #3A0CA3   (indigo)
    #4361EE   (royal blue)
    #4895EF   (sky blue)
    #4CC9F0   (cyan)    */


    #define COL_BG_DEEP    lv_color_hex(0x0E001A)   // very dark violet (base)
    #define COL_BG_CARD    lv_color_hex(0x1A0633)   // dark purple card

    #define COL_BLUE_BRITE lv_color_hex(0x4361EE)   // royal blue (primary accent)
    #define COL_BLUE_ICE   lv_color_hex(0x4CC9F0)   // cyan (secondary accent)

    #define COL_BLUE_DIM   lv_color_hex(0x3A0CA3)   // indigo

    #define COL_TEXT_HI    lv_color_hex(0xF72585)   // hot pink highlight text
    #define COL_TEXT_MID   lv_color_hex(0xB5179E)   // magenta secondary
    #define COL_TEXT_LO    lv_color_hex(0x7209B7)   // purple muted

    ///////////////////////////////////////////////////
    /////////////// UI BUILD //////////////////////////
    ///////////////////////////////////////////////////

    /*
    * Layout (portrait 240 x 320):
    *
    *  y=0   ┌────────────────────────┐
    *        │  HEADER  (h=46)        │
    *  y=46  ├════════════════════════┤  <- 2px electric-blue rule
    *  y=48  │                        │
    *        │  TEMPERATURE CARD      │
    *        │  (h=128)               │
    *        │                        │
    *  y=180 ├  ──  ──  ──  ──  ──   │  <- 1px dim divider
    *  y=183 │                        │
    *        │  UNITS CARD (h=106)    │
    *        │                        │
    *  y=291 ├────────────────────────┤  <- 1px dim rule
    *        │  FOOTER  (h=29)        │
    *  y=320 └────────────────────────┘
    */
    static void ui_create(void)
    {
        lv_obj_t *scr = lv_scr_act();
        lv_obj_set_style_bg_color(scr, COL_BG_DEEP, 0);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);


        /* ══════════════════════════════════════════
        *  HEADER
        * ══════════════════════════════════════════ */
        lv_obj_t *header = lv_obj_create(scr);
        lv_obj_set_size(header, 240, 46);
        lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_set_style_bg_color(header, COL_BG_CARD, 0);
        lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(header, 0, 0);
        lv_obj_set_style_radius(header, 0, 0);
        lv_obj_set_style_pad_all(header, 0, 0);

        /* Electric-blue underline rule */
        lv_obj_t *hline = lv_obj_create(scr);
        lv_obj_set_size(hline, 240, 2);
        lv_obj_align(hline, LV_ALIGN_TOP_MID, 0, 46);
        lv_obj_set_style_bg_color(hline, COL_BLUE_BRITE, 0);
        lv_obj_set_style_bg_opa(hline, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(hline, 0, 0);
        lv_obj_set_style_radius(hline, 0, 0);

        /* Title */
        lv_obj_t *lbl_title = lv_label_create(header);
        lv_label_set_text(lbl_title, "MONITOR");
        lv_obj_set_style_text_color(lbl_title, COL_TEXT_HI, 0);
        lv_obj_set_style_text_font(lbl_title, &cascadia_mono, 0);
        lv_obj_set_style_text_letter_space(lbl_title, 5, 0);
        lv_obj_align(lbl_title, LV_ALIGN_CENTER, 0, 0);

        /* Status dot */
        lv_obj_t *dot = lv_obj_create(header);
        lv_obj_set_size(dot, 8, 8);
        lv_obj_set_style_bg_color(dot, COL_BLUE_BRITE, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(dot, COL_BLUE_ICE, 0);
        lv_obj_set_style_border_width(dot, 1, 0);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_align(dot, LV_ALIGN_RIGHT_MID, -14, 0);


        /* ══════════════════════════════════════════
        *  TEMPERATURE CARD
        * ══════════════════════════════════════════ */
        lv_obj_t *card_temp = lv_obj_create(scr);
        lv_obj_set_size(card_temp, 220, 128);
        lv_obj_align(card_temp, LV_ALIGN_TOP_MID, 0, 50);
        lv_obj_set_style_bg_color(card_temp, COL_BG_CARD, 0);
        lv_obj_set_style_bg_opa(card_temp, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(card_temp, COL_BLUE_DIM, 0);
        lv_obj_set_style_border_width(card_temp, 1, 0);
        lv_obj_set_style_radius(card_temp, 6, 0);
        lv_obj_set_style_pad_all(card_temp, 0, 0);

        /* Left strip */
        lv_obj_t *strip1 = lv_obj_create(card_temp);
        lv_obj_set_size(strip1, 4, 112);
        lv_obj_align(strip1, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_set_style_bg_color(strip1, COL_BLUE_BRITE, 0);
        lv_obj_set_style_bg_opa(strip1, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(strip1, 0, 0);
        lv_obj_set_style_radius(strip1, 0, 0);

        /* "TEMPERATURE" label */
        lv_obj_t *lbl_tname = lv_label_create(card_temp);
        lv_label_set_text(lbl_tname, "TEMPERATURE");
        lv_obj_set_style_text_color(lbl_tname, COL_TEXT_MID, 0);
        lv_obj_set_style_text_font(lbl_tname, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_letter_space(lbl_tname, 2, 0);
        lv_obj_align(lbl_tname, LV_ALIGN_TOP_LEFT, 16, 10);

        /*
        * VALUE ROW — "23.5" and "°C" on the SAME line.
        *
        * Key fix: both labels share the same font size
        * (montserrat_28), so their glyph heights are identical
        * and the flex row's LV_FLEX_ALIGN_CENTER places them
        * on the same visual baseline — no superscript effect.
        */
        lv_obj_t *row_temp = lv_obj_create(card_temp);
        lv_obj_set_size(row_temp, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(row_temp, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row_temp, 0, 0);
        lv_obj_set_style_pad_all(row_temp, 0, 0);
        lv_obj_set_style_pad_column(row_temp, 5, 0);
        lv_obj_set_flex_flow(row_temp, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row_temp,
            LV_FLEX_ALIGN_START,
            LV_FLEX_ALIGN_CENTER,   /* vertical: centre all children */
            LV_FLEX_ALIGN_CENTER);
        lv_obj_align(row_temp, LV_ALIGN_LEFT_MID, 16, 2);

        label_temp_val = lv_label_create(row_temp);
        lv_label_set_text(label_temp_val, "--.-");
        lv_obj_set_style_text_color(label_temp_val, COL_BLUE_BRITE, 0);
        lv_obj_set_style_text_font(label_temp_val, &lv_font_montserrat_28, 0);

        lv_obj_t *lbl_tunit = lv_label_create(row_temp);
        lv_label_set_text(lbl_tunit, "\xC2\xB0""C");   /* UTF-8 degree + C */
        lv_obj_set_style_text_color(lbl_tunit, COL_TEXT_MID, 0);
        lv_obj_set_style_text_font(lbl_tunit, &lv_font_montserrat_28, 0); /* same size! */

        /* Thin fill bar */
        bar_temp = lv_bar_create(card_temp);
        lv_obj_set_size(bar_temp, 180, 5);
        lv_obj_align(bar_temp, LV_ALIGN_BOTTOM_LEFT, 16, -12);
        lv_bar_set_range(bar_temp, 0, 100);
        lv_bar_set_value(bar_temp, 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(bar_temp, COL_BLUE_DIM, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bar_temp, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(bar_temp, 3, LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar_temp, COL_BLUE_BRITE, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(bar_temp, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_radius(bar_temp, 3, LV_PART_INDICATOR);

        lv_obj_t *lbl_min = lv_label_create(card_temp);
        lv_label_set_text(lbl_min, "0");
        lv_obj_set_style_text_color(lbl_min, COL_TEXT_LO, 0);
        lv_obj_set_style_text_font(lbl_min, &lv_font_montserrat_14, 0);
        lv_obj_align(lbl_min, LV_ALIGN_BOTTOM_LEFT, 16, 0);

        lv_obj_t *lbl_max = lv_label_create(card_temp);
        lv_label_set_text(lbl_max, "100 C");
        lv_obj_set_style_text_color(lbl_max, COL_TEXT_LO, 0);
        lv_obj_set_style_text_font(lbl_max, &lv_font_montserrat_14, 0);
        lv_obj_align(lbl_max, LV_ALIGN_BOTTOM_RIGHT, -4, 0);


        /* ══════════════════════════════════════════
        *  DIVIDER
        * ══════════════════════════════════════════ */
        lv_obj_t *div = lv_obj_create(scr);
        lv_obj_set_size(div, 200, 1);
        lv_obj_align(div, LV_ALIGN_TOP_MID, 0, 184);
        lv_obj_set_style_bg_color(div, COL_BLUE_DIM, 0);
        lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(div, 0, 0);
        lv_obj_set_style_radius(div, 0, 0);


        /* ══════════════════════════════════════════
        *  UNITS CARD
        * ══════════════════════════════════════════ */
        lv_obj_t *card_units = lv_obj_create(scr);
        lv_obj_set_size(card_units, 220, 106);
        lv_obj_align(card_units, LV_ALIGN_TOP_MID, 0, 187);
        lv_obj_set_style_bg_color(card_units, COL_BG_CARD, 0);
        lv_obj_set_style_bg_opa(card_units, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(card_units, COL_BLUE_DIM, 0);
        lv_obj_set_style_border_width(card_units, 1, 0);
        lv_obj_set_style_radius(card_units, 6, 0);
        lv_obj_set_style_pad_all(card_units, 0, 0);

        /* Left strip — ice blue */
        lv_obj_t *strip2 = lv_obj_create(card_units);
        lv_obj_set_size(strip2, 4, 90);
        lv_obj_align(strip2, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_set_style_bg_color(strip2, COL_BLUE_ICE, 0);
        lv_obj_set_style_bg_opa(strip2, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(strip2, 0, 0);
        lv_obj_set_style_radius(strip2, 0, 0);

        /* "NO. OF UNITS" label */
        lv_obj_t *lbl_uname = lv_label_create(card_units);
        lv_label_set_text(lbl_uname, "NO. OF UNITS");
        lv_obj_set_style_text_color(lbl_uname, COL_TEXT_MID, 0);
        lv_obj_set_style_text_font(lbl_uname, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_letter_space(lbl_uname, 2, 0);
        lv_obj_align(lbl_uname, LV_ALIGN_TOP_LEFT, 16, 10);

        /*
        * VALUE ROW — "4" and "units" on the SAME line.
        * Same fix: both labels use montserrat_28.
        */
        lv_obj_t *row_units = lv_obj_create(card_units);
        lv_obj_set_size(row_units, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(row_units, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row_units, 0, 0);
        lv_obj_set_style_pad_all(row_units, 0, 0);
        lv_obj_set_style_pad_column(row_units, 6, 0);
        lv_obj_set_flex_flow(row_units, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row_units,
            LV_FLEX_ALIGN_START,
            LV_FLEX_ALIGN_CENTER,
            LV_FLEX_ALIGN_CENTER);
        lv_obj_align(row_units, LV_ALIGN_LEFT_MID, 16, 2);

        label_units_val = lv_label_create(row_units);
        lv_label_set_text(label_units_val, "--");
        lv_obj_set_style_text_color(label_units_val, COL_BLUE_ICE, 0);
        lv_obj_set_style_text_font(label_units_val, &lv_font_montserrat_28, 0);

        lv_obj_t *lbl_usub = lv_label_create(row_units);
        lv_label_set_text(lbl_usub, "units");
        lv_obj_set_style_text_color(lbl_usub, COL_TEXT_MID, 0);
        lv_obj_set_style_text_font(lbl_usub, &lv_font_montserrat_28, 0); /* same size! */


        /* ══════════════════════════════════════════
        *  FOOTER
        * ══════════════════════════════════════════ */
        /* Top rule */
        lv_obj_t *fline = lv_obj_create(scr);
        lv_obj_set_size(fline, 240, 1);
        lv_obj_align(fline, LV_ALIGN_BOTTOM_MID, 0, -29);
        lv_obj_set_style_bg_color(fline, COL_BLUE_DIM, 0);
        lv_obj_set_style_bg_opa(fline, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(fline, 0, 0);
        lv_obj_set_style_radius(fline, 0, 0);

        lv_obj_t *footer = lv_obj_create(scr);
        lv_obj_set_size(footer, 240, 29);
        lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_color(footer, COL_BG_CARD, 0);
        lv_obj_set_style_bg_opa(footer, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(footer, 0, 0);
        lv_obj_set_style_radius(footer, 0, 0);
        lv_obj_set_style_pad_all(footer, 0, 0);

        lv_obj_t *lbl_footer = lv_label_create(footer);
        lv_label_set_text(lbl_footer, "VAXIQ");
        lv_obj_set_style_text_color(lbl_footer, COL_TEXT_LO, 0);
        lv_obj_set_style_text_font(lbl_footer, &cascadia_mono, 0);
        lv_obj_set_style_text_letter_space(lbl_footer, 2, 0);
        lv_obj_align(lbl_footer, LV_ALIGN_CENTER, 0, 0);
    }


    ///////////////////////////////////////////////////
    /////////////// LVGL CALLBACKS ////////////////////
    ///////////////////////////////////////////////////

    static bool lvgl_flush_ready_cb(
            esp_lcd_panel_io_handle_t panel_io,
            esp_lcd_panel_io_event_data_t *edata,
            void *user_ctx)
    {
        lv_disp_drv_t *disp_drv = (lv_disp_drv_t *)user_ctx;
        lv_disp_flush_ready(disp_drv);
        return false;
    }

    static void lvgl_flush_cb(
            lv_disp_drv_t *drv,
            const lv_area_t *area,
            lv_color_t *color_map)
    {
        esp_lcd_panel_handle_t panel =
            (esp_lcd_panel_handle_t) drv->user_data;

        esp_lcd_panel_draw_bitmap(
                panel,
                area->x1, area->y1,
                area->x2 + 1, area->y2 + 1,
                color_map);
    }

    static void lv_tick_task(void *arg)
    {
        lv_tick_inc(LVGL_TICK_PERIOD_MS);
    }


    ///////////////////////////////////////////////////
    /////////////// APP MAIN //////////////////////////
    ///////////////////////////////////////////////////

    void app_main(void)
    {
        /* ── Backlight OFF ── */
        ESP_LOGI(TAG, "Backlight OFF");
        gpio_config_t bk_gpio = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << PIN_NUM_BCKL
        };
        ESP_ERROR_CHECK(gpio_config(&bk_gpio));

        /* ── SPI Bus ── */
        ESP_LOGI(TAG, "SPI init");
        spi_bus_config_t buscfg = {
            .sclk_io_num   = PIN_NUM_CLK,
            .mosi_io_num   = PIN_NUM_MOSI,
            .miso_io_num   = PIN_NUM_MISO,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = LCD_H_RES * 80 * sizeof(uint16_t)
        };
        ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

        /* ── Panel IO ── */
        ESP_LOGI(TAG, "Panel IO");
        esp_lcd_panel_io_handle_t io_handle = NULL;
        esp_lcd_panel_io_spi_config_t io_config = {
            .dc_gpio_num         = PIN_NUM_DC,
            .cs_gpio_num         = PIN_NUM_CS,
            .pclk_hz             = LCD_PIXEL_CLOCK_HZ,
            .lcd_cmd_bits        = LCD_CMD_BITS,
            .lcd_param_bits      = LCD_PARAM_BITS,
            .spi_mode            = 0,
            .trans_queue_depth   = 10,
            .on_color_trans_done = lvgl_flush_ready_cb,
            .user_ctx            = &disp_drv
        };
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
            (esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle));

        /* ── Panel Driver ── */
        ESP_LOGI(TAG, "Install ILI9341");
        esp_lcd_panel_dev_config_t panel_config = {
            .reset_gpio_num = PIN_NUM_RST,
            .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
            .bits_per_pixel = 16
        };
        ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(
            io_handle, &panel_config, &panel_handle));

        /* ── Init Panel ── */
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, true, false));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

        /* ── Backlight ON ── */
        ESP_LOGI(TAG, "Backlight ON");
        gpio_set_level(PIN_NUM_BCKL, 1);

        /* ── LVGL Init ── */
        ESP_LOGI(TAG, "LVGL Init");
        lv_init();

        /* ── LVGL Buffers ── */
        lv_color_t *buf1 = heap_caps_malloc(
            LCD_H_RES * 40 * sizeof(lv_color_t), MALLOC_CAP_DMA);
        lv_color_t *buf2 = heap_caps_malloc(
            LCD_H_RES * 40 * sizeof(lv_color_t), MALLOC_CAP_DMA);
        assert(buf1);
        assert(buf2);
        lv_disp_draw_buf_init(&draw_buf, buf1, buf2, LCD_H_RES * 40);

        /* ── LVGL Display Driver ── */
        lv_disp_drv_init(&disp_drv);
        disp_drv.hor_res   = LCD_H_RES;
        disp_drv.ver_res   = LCD_V_RES;
        disp_drv.flush_cb  = lvgl_flush_cb;
        disp_drv.draw_buf  = &draw_buf;
        disp_drv.user_data = panel_handle;
        lv_disp_drv_register(&disp_drv);

        /* ── LVGL Tick Timer ── */
        const esp_timer_create_args_t timer_args = {
            .callback = lv_tick_task,
            .name     = "lv_tick"
        };
        esp_timer_handle_t timer;
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer));
        ESP_ERROR_CHECK(esp_timer_start_periodic(
            timer, LVGL_TICK_PERIOD_MS * 1000));

        /* ── Build UI ── */
        ui_create();

        /* DEMO values — replace with your real sensor reads */
        ui_update_values(235, 4);   /* 23.5 °C, 4 units */

        /* ── Main Loop ── */
        while (1)
        {
            lv_timer_handler();
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }