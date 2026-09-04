/**
 * @file main.c
 * @brief FoloToy AI-Passport Electronic Cigarette Firmware (Rock-Solid No-Flicker & UI Text Edition)
 * Target: FoloToy AI-Passport (ESP32-C3, ST7789 IPS 240x320 Display)
 * Fixes: WDT reset loop eliminated, hardware debounce, built-in 8x16 font for real UI text.
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"

// ===================== 硬件引脚定义 =====================
#define BSP_LCD_WIDTH           240
#define BSP_LCD_HEIGHT          320
#define BSP_LCD_SPI_HOST        SPI2_HOST
#define BSP_LCD_CS              GPIO_NUM_1
#define BSP_LCD_SCLK            GPIO_NUM_4
#define BSP_LCD_MOSI            GPIO_NUM_6
#define BSP_LCD_DC              GPIO_NUM_5
#define BSP_LCD_BACKLIGHT       GPIO_NUM_21
#define BSP_LCD_FREQ_HZ         (40 * 1000 * 1000)

// 按键定义
#define BSP_BTN_UP_GPIO         GPIO_NUM_2   // 上键: 弹烟灰
#define BSP_BTN_DOWN_GPIO       GPIO_NUM_3   // 下键: 弹烟灰
#define BSP_BTN_OK_GPIO         GPIO_NUM_9   // OK 键: 点火 / 换新烟
#define BSP_BTN_POWER_GPIO      GPIO_NUM_8   // 电源键: 息屏/唤醒

static const char *TAG = "AI_PASSPORT_VAPE";

// ===================== 颜色常量 (ST7789 IPS RGB565) =====================
#define COLOR_BLACK         0x0000
#define COLOR_WHITE         0xFFFF
#define COLOR_DARK_BG       0x0000  // 纯黑背景
#define COLOR_STATUS_BG     0x1082  // 顶部状态栏暗底
#define COLOR_AMBER         0xD380  // 滤嘴琥珀黄
#define COLOR_GOLD          0xFD20  // 金色滤嘴带
#define COLOR_PAPER         0xFFFF  // 纯白卷烟纸
#define COLOR_PAPER_SHADOW  0xC618  // 烟纸边缘阴影
#define COLOR_CHAR_BROWN    0x8A22  // 烟丝焦化
#define COLOR_CHAR_BLACK    0x2104  // 焦化黑
#define COLOR_EMBER_RED     0xF800  // 炭火红
#define COLOR_EMBER_ORANGE  0xFD20  // 明火橙
#define COLOR_EMBER_YELLOW  0xFFE0  // 高温发亮黄
#define COLOR_ASH_GREY      0x8C71  // 烟灰浅灰
#define COLOR_ASH_DARK      0x4208  // 烟灰深灰
#define COLOR_FLAME_BLUE    0x05BF  // 点火蓝火苗
#define COLOR_SMOKE         0xD6BA  // 烟雾淡灰
#define COLOR_CYAN          0x07FF  // 烟油亮青
#define COLOR_GREEN         0x07E0  // 满电亮绿

typedef enum {
    STATE_UNLIT = 0,
    STATE_LIGHTING,
    STATE_BURNING,
    STATE_BURNED_OUT
} cig_state_t;

typedef struct {
    cig_state_t state;
    float tobacco_remaining;    // 0.0 ~ 100.0%
    float ash_length;           // 0.0 ~ 100.0%
    uint8_t realm_id;           // 0 ~ 11
    uint32_t current_realm_smoked;
    uint32_t total_smoked;
    uint8_t battery_percent;
    float juice_percent;
    uint8_t suction_strength;
    bool screen_awake;
    int64_t state_timer_ms;
} vape_firmware_t;

static vape_firmware_t g_vape = {
    .state = STATE_UNLIT,
    .tobacco_remaining = 100.0f,
    .ash_length = 0.0f,
    .realm_id = 0,
    .current_realm_smoked = 0,
    .total_smoked = 0,
    .battery_percent = 92,
    .juice_percent = 95.0f,
    .suction_strength = 0,
    .screen_awake = true,
    .state_timer_ms = 0
};

static spi_device_handle_t s_spi_lcd;

typedef struct {
    const char *title;
    uint32_t target_count;
    bool is_yandi;
} rank_info_t;

static const rank_info_t REALM_TIERS[] = {
    {"MORTAL (FAN REN)",   9,  false},
    {"QI OF SMOKE (1)",    10, false},
    {"SMOKE PRACTITIONER", 11, false},
    {"SMOKE MASTER",       12, false},
    {"GRAND SMOKE MASTER", 13, false},
    {"SMOKE SPIRIT",       14, false},
    {"SMOKE KING",         15, false},
    {"SMOKE EMPEROR",      16, false},
    {"SMOKE SECT LEADER",  17, false},
    {"SMOKE VENERABLE",    18, false},
    {"SMOKE SAINT",        19, false},
    {"SMOKE GOD (YAN DI)", 0,  true}
};

static const rank_info_t* get_current_realm(uint8_t realm_id) {
    if (realm_id >= 11) return &REALM_TIERS[11];
    return &REALM_TIERS[realm_id];
}

// NVS 持久化
static void nvs_load_smoked_count(void) {
    nvs_handle_t h;
    if (nvs_open("vape_data", NVS_READWRITE, &h) == ESP_OK) {
        uint8_t r_id = 0;
        uint32_t cur_cnt = 0;
        uint32_t tot_cnt = 0;
        if (nvs_get_u8(h, "realm_id", &r_id) == ESP_OK) g_vape.realm_id = r_id;
        if (nvs_get_u32(h, "cur_smoked", &cur_cnt) == ESP_OK) g_vape.current_realm_smoked = cur_cnt;
        if (nvs_get_u32(h, "tot_smoked", &tot_cnt) == ESP_OK) g_vape.total_smoked = tot_cnt;
        nvs_close(h);
    }
}

static void nvs_save_smoked_count(uint8_t realm_id, uint32_t cur_cnt, uint32_t tot_cnt) {
    nvs_handle_t h;
    if (nvs_open("vape_data", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "realm_id", realm_id);
        nvs_set_u32(h, "cur_smoked", cur_cnt);
        nvs_set_u32(h, "tot_smoked", tot_cnt);
        nvs_commit(h);
        nvs_close(h);
    }
}

// ===================== ST7789 底层驱动 =====================
static void lcd_cmd(spi_device_handle_t spi, const uint8_t cmd) {
    gpio_set_level(BSP_LCD_DC, 0);
    spi_transaction_t t = { .length = 8, .tx_buffer = &cmd };
    spi_device_polling_transmit(spi, &t);
}

static void lcd_data(spi_device_handle_t spi, const uint8_t *data, int len) {
    if (len <= 0) return;
    gpio_set_level(BSP_LCD_DC, 1);
    spi_transaction_t t = { .length = len * 8, .tx_buffer = data };
    spi_device_polling_transmit(spi, &t);
}

static void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    uint8_t data[4];
    lcd_cmd(s_spi_lcd, 0x2A);
    data[0] = (x0 >> 8) & 0xFF; data[1] = x0 & 0xFF;
    data[2] = (x1 >> 8) & 0xFF; data[3] = x1 & 0xFF;
    lcd_data(s_spi_lcd, data, 4);

    lcd_cmd(s_spi_lcd, 0x2B);
    data[0] = (y0 >> 8) & 0xFF; data[1] = y0 & 0xFF;
    data[2] = (y1 >> 8) & 0xFF; data[3] = y1 & 0xFF;
    lcd_data(s_spi_lcd, data, 4);

    lcd_cmd(s_spi_lcd, 0x2C);
}

static void lcd_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color) {
    if (w == 0 || h == 0 || x >= BSP_LCD_WIDTH || y >= BSP_LCD_HEIGHT) return;
    if (x + w > BSP_LCD_WIDTH) w = BSP_LCD_WIDTH - x;
    if (y + h > BSP_LCD_HEIGHT) h = BSP_LCD_HEIGHT - y;

    lcd_set_window(x, y, x + w - 1, y + h - 1);
    uint16_t buffer[64];
    uint16_t color_be = (color >> 8) | (color << 8);
    for (int i = 0; i < 64; i++) buffer[i] = color_be;

    int total_pixels = w * h;
    while (total_pixels > 0) {
        int to_send = total_pixels > 64 ? 64 : total_pixels;
        lcd_data(s_spi_lcd, (const uint8_t *)buffer, to_send * 2);
        total_pixels -= to_send;
    }
}

// ===================== 内置 8x16 简洁点阵字库 =====================
static const uint8_t font8x16_basic[96][16] = {
    [' ' - 32] = {0},
    ['!' - 32] = {0,0,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0,0x18,0x18,0,0,0,0},
    ['%' - 32] = {0,0,0x63,0x66,0x0c,0x18,0x30,0x60,0x66,0xc6,0,0,0,0,0,0},
    ['(' - 32] = {0,0,0x0c,0x18,0x30,0x30,0x30,0x30,0x30,0x18,0x0c,0,0,0,0,0},
    [')' - 32] = {0,0,0x30,0x18,0x0c,0x0c,0x0c,0x0c,0x0c,0x18,0x30,0,0,0,0,0},
    ['+' - 32] = {0,0,0,0x18,0x18,0x7e,0x18,0x18,0,0,0,0,0,0,0,0},
    ['-' - 32] = {0,0,0,0,0,0x7e,0,0,0,0,0,0,0,0,0,0},
    ['.' - 32] = {0,0,0,0,0,0,0,0,0,0x18,0x18,0,0,0,0,0},
    ['/' - 32] = {0,0,0x03,0x06,0x0c,0x18,0x30,0x60,0xc0,0,0,0,0,0,0,0},
    ['0' - 32] = {0,0,0x3c,0x66,0x6e,0x76,0x66,0x66,0x3c,0,0,0,0,0,0,0},
    ['1' - 32] = {0,0,0x18,0x38,0x18,0x18,0x18,0x18,0x7e,0,0,0,0,0,0,0},
    ['2' - 32] = {0,0,0x3c,0x66,0x06,0x0c,0x18,0x30,0x7e,0,0,0,0,0,0,0},
    ['3' - 32] = {0,0,0x3c,0x66,0x06,0x1c,0x06,0x66,0x3c,0,0,0,0,0,0,0},
    ['4' - 32] = {0,0,0x0c,0x1c,0x34,0x64,0x7e,0x04,0x0e,0,0,0,0,0,0,0},
    ['5' - 32] = {0,0,0x7e,0x60,0x7c,0x06,0x06,0x66,0x3c,0,0,0,0,0,0,0},
    ['6' - 32] = {0,0,0x3c,0x66,0x60,0x7c,0x66,0x66,0x3c,0,0,0,0,0,0,0},
    ['7' - 32] = {0,0,0x7e,0x06,0x0c,0x18,0x18,0x18,0x18,0,0,0,0,0,0,0},
    ['8' - 32] = {0,0,0x3c,0x66,0x66,0x3c,0x66,0x66,0x3c,0,0,0,0,0,0,0},
    ['9' - 32] = {0,0,0x3c,0x66,0x66,0x3e,0x06,0x66,0x3c,0,0,0,0,0,0,0},
    [':' - 32] = {0,0,0,0x18,0x18,0,0,0x18,0x18,0,0,0,0,0,0,0},
    ['A' - 32] = {0,0,0x18,0x3c,0x66,0x66,0x7e,0x66,0x66,0,0,0,0,0,0,0},
    ['B' - 32] = {0,0,0x7c,0x66,0x66,0x7c,0x66,0x66,0x7c,0,0,0,0,0,0,0},
    ['C' - 32] = {0,0,0x3c,0x66,0x60,0x60,0x60,0x66,0x3c,0,0,0,0,0,0,0},
    ['D' - 32] = {0,0,0x78,0x6c,0x66,0x66,0x66,0x6c,0x78,0,0,0,0,0,0,0},
    ['E' - 32] = {0,0,0x7e,0x60,0x60,0x7c,0x60,0x60,0x7e,0,0,0,0,0,0,0},
    ['F' - 32] = {0,0,0x7e,0x60,0x60,0x7c,0x60,0x60,0x60,0,0,0,0,0,0,0},
    ['G' - 32] = {0,0,0x3c,0x66,0x60,0x6e,0x66,0x66,0x3e,0,0,0,0,0,0,0},
    ['H' - 32] = {0,0,0x66,0x66,0x66,0x7e,0x66,0x66,0x66,0,0,0,0,0,0,0},
    ['I' - 32] = {0,0,0x3c,0x18,0x18,0x18,0x18,0x18,0x3c,0,0,0,0,0,0,0},
    ['K' - 32] = {0,0,0x66,0x6c,0x78,0x70,0x78,0x6c,0x66,0,0,0,0,0,0,0},
    ['L' - 32] = {0,0,0x60,0x60,0x60,0x60,0x60,0x60,0x7e,0,0,0,0,0,0,0},
    ['M' - 32] = {0,0,0x63,0x77,0x7f,0x6b,0x63,0x63,0x63,0,0,0,0,0,0,0},
    ['N' - 32] = {0,0,0x66,0x76,0x7e,0x7e,0x6e,0x66,0x66,0,0,0,0,0,0,0},
    ['O' - 32] = {0,0,0x3c,0x66,0x66,0x66,0x66,0x66,0x3c,0,0,0,0,0,0,0},
    ['P' - 32] = {0,0,0x7c,0x66,0x66,0x7c,0x60,0x60,0x60,0,0,0,0,0,0,0},
    ['Q' - 32] = {0,0,0x3c,0x66,0x66,0x66,0x6a,0x6c,0x36,0,0,0,0,0,0,0},
    ['R' - 32] = {0,0,0x7c,0x66,0x66,0x7c,0x6c,0x66,0x66,0,0,0,0,0,0,0},
    ['S' - 32] = {0,0,0x3c,0x66,0x60,0x3c,0x06,0x66,0x3c,0,0,0,0,0,0,0},
    ['T' - 32] = {0,0,0x7e,0x18,0x18,0x18,0x18,0x18,0x18,0,0,0,0,0,0,0},
    ['U' - 32] = {0,0,0x66,0x66,0x66,0x66,0x66,0x66,0x3c,0,0,0,0,0,0,0},
    ['V' - 32] = {0,0,0x66,0x66,0x66,0x66,0x66,0x3c,0x18,0,0,0,0,0,0,0},
    ['W' - 32] = {0,0,0x63,0x63,0x63,0x6b,0x7f,0x77,0x63,0,0,0,0,0,0,0},
    ['Y' - 32] = {0,0,0x66,0x66,0x66,0x3c,0x18,0x18,0x18,0,0,0,0,0,0,0},
    ['Z' - 32] = {0,0,0x7e,0x06,0x0c,0x18,0x30,0x60,0x7e,0,0,0,0,0,0,0},
};

static void lcd_draw_char(uint16_t x, uint16_t y, char c, uint16_t color, uint16_t bg) {
    if (c < 32 || c > 126) c = ' ';
    const uint8_t *glyph = font8x16_basic[c - 32];
    for (int r = 0; r < 16; r++) {
        uint8_t line = glyph[r];
        for (int b = 0; b < 8; b++) {
            if (line & (0x80 >> b)) {
                lcd_fill_rect(x + b, y + r, 1, 1, color);
            } else if (bg != color) {
                lcd_fill_rect(x + b, y + r, 1, 1, bg);
            }
        }
    }
}

static void lcd_draw_string(uint16_t x, uint16_t y, const char *str, uint16_t color, uint16_t bg) {
    while (*str) {
        lcd_draw_char(x, y, *str, color, bg);
        x += 8;
        str++;
    }
}

static void lcd_init_st7789(void) {
    vTaskDelay(pdMS_TO_TICKS(100));

    lcd_cmd(s_spi_lcd, 0x11); // Sleep Out
    vTaskDelay(pdMS_TO_TICKS(120));

    uint8_t madctl = 0x00; // RGB 竖屏模式
    lcd_cmd(s_spi_lcd, 0x36);
    lcd_data(s_spi_lcd, &madctl, 1);

    uint8_t colmod = 0x55; // 16-bit RGB565
    lcd_cmd(s_spi_lcd, 0x3A);
    lcd_data(s_spi_lcd, &colmod, 1);

    // 0x21 INVON 屏幕反相显示
    lcd_cmd(s_spi_lcd, 0x21);

    lcd_cmd(s_spi_lcd, 0x29); // Display ON
    vTaskDelay(pdMS_TO_TICKS(20));

    gpio_set_level(BSP_LCD_BACKLIGHT, 1);
    lcd_fill_rect(0, 0, BSP_LCD_WIDTH, BSP_LCD_HEIGHT, COLOR_DARK_BG);
}

// ===================== 增量刷新与 UI 绘制 =====================
static bool s_need_full_redraw = true;
static int s_prev_shaft_h = -1;
static int s_prev_ash_h = -1;
static int s_prev_smoke_x = -1;
static int s_prev_smoke_y = -1;
static cig_state_t s_prev_state = (cig_state_t)255;
static uint8_t s_prev_realm_id = 255;
static uint32_t s_prev_cur_smoked = 999999;
static uint32_t s_prev_tot_smoked = 999999;

static void flick_ash(void) {
    if (g_vape.ash_length < 2.0f) return;
    const int center_x = 120;
    const int cig_width = 24;
    const int filter_bottom = 240;
    const int filter_top = filter_bottom - 45;
    const int max_shaft_len = 125;
    int current_shaft = (int)((max_shaft_len * g_vape.tobacco_remaining) / 100.0f);
    int tip_y = filter_top - current_shaft;

    lcd_fill_rect(center_x - (cig_width / 2) - 2, tip_y - 48, cig_width + 4, 48, COLOR_DARK_BG);
    g_vape.ash_length = 0.0f;
    s_prev_ash_h = 0;
}

static void render_vape_ui(void) {
    if (!g_vape.screen_awake) return;

    const int center_x = 120;
    const int cig_width = 24;
    const int cig_left = center_x - (cig_width / 2);
    const int filter_bottom = 240;
    const int filter_height = 45;
    const int filter_top = filter_bottom - filter_height;
    const int max_shaft_len = 125;

    // 静态背景和框架只绘制一次
    if (s_need_full_redraw || g_vape.state != s_prev_state) {
        s_need_full_redraw = false;
        s_prev_state = g_vape.state;

        lcd_fill_rect(0, 0, BSP_LCD_WIDTH, BSP_LCD_HEIGHT, COLOR_DARK_BG);

        // 1. 顶部状态栏
        lcd_fill_rect(0, 0, BSP_LCD_WIDTH, 24, COLOR_STATUS_BG);
        uint16_t batt_color = g_vape.battery_percent < 20 ? COLOR_EMBER_RED : COLOR_GREEN;
        lcd_fill_rect(8, 6, 20, 12, batt_color);
        lcd_fill_rect(28, 9, 3, 6, batt_color);
        lcd_draw_string(34, 4, "92%", COLOR_WHITE, COLOR_STATUS_BG);

        int juice_w = (int)((g_vape.juice_percent / 100.0f) * 22);
        lcd_fill_rect(BSP_LCD_WIDTH - 64, 6, juice_w, 12, COLOR_CYAN);
        lcd_draw_string(BSP_LCD_WIDTH - 38, 4, "OIL", COLOR_CYAN, COLOR_STATUS_BG);

        // 2. 静态烟嘴
        lcd_fill_rect(cig_left, filter_top, cig_width, filter_height, COLOR_AMBER);
        lcd_fill_rect(cig_left - 1, filter_top - 2, cig_width + 2, 3, COLOR_GOLD);
        lcd_fill_rect(cig_left, filter_top, 3, filter_height, 0xFD40);
        lcd_fill_rect(cig_left + cig_width - 3, filter_top, 3, filter_height, 0x8A00);

        // 3. 底部修仙境界外框
        const rank_info_t *cur_realm = get_current_realm(g_vape.realm_id);
        lcd_fill_rect(6, 252, BSP_LCD_WIDTH - 12, 62, cur_realm->is_yandi ? 0x3180 : 0x1082);
        lcd_fill_rect(8, 254, BSP_LCD_WIDTH - 16, 58, COLOR_DARK_BG);

        // 绘制境界文字
        lcd_draw_string(14, 258, cur_realm->title, cur_realm->is_yandi ? COLOR_GOLD : COLOR_EMBER_YELLOW, COLOR_DARK_BG);

        // 绘制进度和历史总数文字
        char prog_buf[32];
        if (cur_realm->is_yandi) {
            snprintf(prog_buf, sizeof(prog_buf), "MAX GOD | TOT:%lu", (unsigned long)g_vape.total_smoked);
        } else {
            snprintf(prog_buf, sizeof(prog_buf), "PROG:%lu/%lu | T:%lu", 
                     (unsigned long)g_vape.current_realm_smoked, 
                     (unsigned long)cur_realm->target_count,
                     (unsigned long)g_vape.total_smoked);
        }
        lcd_draw_string(14, 278, prog_buf, COLOR_WHITE, COLOR_DARK_BG);

        // 绘制修仙经验条底槽与当前填充
        lcd_fill_rect(14, 298, BSP_LCD_WIDTH - 28, 6, 0x2104);
        float prog = cur_realm->target_count > 0 ? ((float)g_vape.current_realm_smoked / cur_realm->target_count) : 1.0f;
        if (prog > 1.0f) prog = 1.0f;
        int progress_w = (int)(prog * (BSP_LCD_WIDTH - 28));
        lcd_fill_rect(14, 298, progress_w, 6, cur_realm->is_yandi ? COLOR_GOLD : COLOR_CYAN);

        s_prev_shaft_h = -1;
        s_prev_ash_h = -1;
        s_prev_smoke_x = -1;
        s_prev_smoke_y = -1;
        s_prev_realm_id = g_vape.realm_id;
        s_prev_cur_smoked = g_vape.current_realm_smoked;
        s_prev_tot_smoked = g_vape.total_smoked;
    }

    // 动态增量绘制（不闪屏）
    if (g_vape.state == STATE_UNLIT) {
        int current_shaft = (int)((max_shaft_len * g_vape.tobacco_remaining) / 100.0f);
        int tip_y = filter_top - current_shaft;
        if (s_prev_shaft_h != current_shaft) {
            s_prev_shaft_h = current_shaft;
            lcd_fill_rect(cig_left, tip_y, cig_width, current_shaft, COLOR_PAPER);
            lcd_fill_rect(cig_left, tip_y, 3, current_shaft, COLOR_WHITE);
            lcd_fill_rect(cig_left + cig_width - 3, tip_y, 3, current_shaft, COLOR_PAPER_SHADOW);
            lcd_fill_rect(cig_left, tip_y - 3, cig_width, 3, COLOR_CHAR_BROWN);
            // 点火提示蓝光
            lcd_fill_rect(center_x - 1, tip_y - 12, 2, 6, COLOR_FLAME_BLUE);
            lcd_fill_rect(center_x - 4, tip_y - 9, 8, 2, COLOR_FLAME_BLUE);
            lcd_draw_string(24, 110, "PRESS OK", COLOR_WHITE, COLOR_DARK_BG);
            lcd_draw_string(24, 128, "TO LIGHT", COLOR_FLAME_BLUE, COLOR_DARK_BG);
        }
    } else if (g_vape.state == STATE_LIGHTING) {
        int current_shaft = (int)((max_shaft_len * g_vape.tobacco_remaining) / 100.0f);
        int tip_y = filter_top - current_shaft;
        lcd_fill_rect(center_x - 10, tip_y - 32, 20, 32, COLOR_DARK_BG);
        int flame_h = 24 + (esp_random() % 6);
        lcd_fill_rect(center_x - 6, tip_y - flame_h, 12, flame_h, COLOR_EMBER_ORANGE);
        lcd_fill_rect(center_x - 3, tip_y - flame_h + 4, 6, flame_h - 8, COLOR_EMBER_YELLOW);
        lcd_fill_rect(center_x - 1, tip_y - flame_h + 8, 2, flame_h - 12, COLOR_WHITE);
        lcd_draw_string(24, 110, "        ", COLOR_DARK_BG, COLOR_DARK_BG);
        lcd_draw_string(24, 128, "IGNITING", COLOR_EMBER_ORANGE, COLOR_DARK_BG);
    } else if (g_vape.state == STATE_BURNING) {
        int current_shaft = (int)((max_shaft_len * g_vape.tobacco_remaining) / 100.0f);
        int ash_h = (int)((g_vape.ash_length / 100.0f) * 125.0f);
        int tip_y = filter_top - current_shaft;

        if (s_prev_shaft_h != current_shaft || s_prev_ash_h != ash_h) {
            int old_total_top = (s_prev_shaft_h >= 0) ? (filter_top - s_prev_shaft_h - s_prev_ash_h) : (tip_y - ash_h);
            int new_total_top = tip_y - ash_h;

            if (new_total_top > old_total_top) {
                lcd_fill_rect(cig_left - 1, old_total_top - 2, cig_width + 2, (new_total_top - old_total_top) + 2, COLOR_DARK_BG);
            }

            if (s_prev_shaft_h != current_shaft && current_shaft > 0) {
                lcd_fill_rect(cig_left, tip_y, cig_width, current_shaft, COLOR_PAPER);
                lcd_fill_rect(cig_left, tip_y, 3, current_shaft, COLOR_WHITE);
                lcd_fill_rect(cig_left + cig_width - 3, tip_y, 3, current_shaft, COLOR_PAPER_SHADOW);
                lcd_fill_rect(cig_left, tip_y, cig_width, 4, COLOR_CHAR_BROWN);
            }

            if (ash_h > 0) {
                lcd_fill_rect(cig_left + 1, tip_y - ash_h, cig_width - 2, ash_h, COLOR_ASH_GREY);
                for (int i = 0; i < ash_h; i += 4) {
                    lcd_fill_rect(cig_left + 2, tip_y - ash_h + i, cig_width - 4, 2, COLOR_ASH_DARK);
                }
            }

            // 绘制剩余烟量百分比
            char rem_buf[16];
            snprintf(rem_buf, sizeof(rem_buf), "%3d%%", (int)g_vape.tobacco_remaining);
            lcd_draw_string(BSP_LCD_WIDTH - 48, 120, rem_buf, COLOR_EMBER_YELLOW, COLOR_DARK_BG);

            s_prev_shaft_h = current_shaft;
            s_prev_ash_h = ash_h;
        }

        // 烟蒂微红燃烧
        lcd_fill_rect(cig_left - 1, tip_y - 2, cig_width + 2, 4, COLOR_EMBER_RED);
        lcd_fill_rect(cig_left + 2, tip_y - 2, cig_width - 4, 3, COLOR_EMBER_YELLOW);

        // 飘烟动画
        if (s_prev_smoke_x >= 0) {
            lcd_fill_rect(s_prev_smoke_x, s_prev_smoke_y, 4, 4, COLOR_DARK_BG);
        }
        int smoke_y = tip_y - ash_h - 6 - (esp_random() % 22);
        int smoke_x = center_x + ((int)(esp_random() % 16) - 8);
        if (smoke_y >= 26) {
            lcd_fill_rect(smoke_x, smoke_y, 4, 4, COLOR_SMOKE);
            s_prev_smoke_x = smoke_x;
            s_prev_smoke_y = smoke_y;
        } else {
            s_prev_smoke_x = -1;
        }
    } else if (g_vape.state == STATE_BURNED_OUT) {
        if (s_prev_state != STATE_BURNED_OUT) {
            lcd_fill_rect(cig_left - 10, 26, cig_width + 20, filter_top - 26, COLOR_DARK_BG);
            lcd_fill_rect(cig_left, filter_top - 4, cig_width, 4, COLOR_CHAR_BLACK);
            lcd_draw_string(24, 110, "SMOKED OUT", COLOR_ASH_GREY, COLOR_DARK_BG);
            lcd_draw_string(24, 128, "PRESS OK", COLOR_WHITE, COLOR_DARK_BG);
        }
    }

    // 进度变化更新
    if (g_vape.current_realm_smoked != s_prev_cur_smoked || g_vape.realm_id != s_prev_realm_id || g_vape.total_smoked != s_prev_tot_smoked) {
        s_need_full_redraw = true; // 仅在晋级或抽完一根时刷新底部面板
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "Starting FoloToy AI-Passport Vape Firmware...");
    nvs_flash_init();
    nvs_load_smoked_count();

    // 1. 初始化物理按键 GPIO (启用内部上拉)
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << BSP_BTN_UP_GPIO) | (1ULL << BSP_BTN_DOWN_GPIO) |
                        (1ULL << BSP_BTN_OK_GPIO) | (1ULL << BSP_BTN_POWER_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&btn_cfg);

    // 2. 初始化 LCD 输出引脚
    gpio_config_t lcd_pins = {
        .pin_bit_mask = (1ULL << BSP_LCD_DC) | (1ULL << BSP_LCD_BACKLIGHT),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&lcd_pins);

    // 3. SPI2 初始化
    spi_bus_config_t buscfg = {
        .mosi_io_num = BSP_LCD_MOSI, .miso_io_num = -1, .sclk_io_num = BSP_LCD_SCLK,
        .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = BSP_LCD_WIDTH * BSP_LCD_HEIGHT * 2
    };
    spi_bus_initialize(BSP_LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = BSP_LCD_FREQ_HZ, .mode = 0,
        .spics_io_num = BSP_LCD_CS, .queue_size = 7
    };
    spi_bus_add_device(BSP_LCD_SPI_HOST, &devcfg, &s_spi_lcd);

    lcd_init_st7789();

    // 按键前一状态用于边沿检测（消除连续重入和浮空误触）
    bool last_btn_ok = true;
    bool last_btn_up = true;
    bool last_btn_down = true;
    bool last_btn_pwr = true;

    while (1) {
        bool cur_btn_up = (gpio_get_level(BSP_BTN_UP_GPIO) == 0);
        bool cur_btn_down = (gpio_get_level(BSP_BTN_DOWN_GPIO) == 0);
        bool cur_btn_ok = (gpio_get_level(BSP_BTN_OK_GPIO) == 0);
        bool cur_btn_pwr = (gpio_get_level(BSP_BTN_POWER_GPIO) == 0);

        // 电源键边沿检测 (按下瞬间触发)
        if (cur_btn_pwr && !last_btn_pwr) {
            g_vape.screen_awake = !g_vape.screen_awake;
            gpio_set_level(BSP_LCD_BACKLIGHT, g_vape.screen_awake ? 1 : 0);
            if (g_vape.screen_awake) {
                s_need_full_redraw = true;
            }
        }

        // 上/下键弹烟灰
        if ((cur_btn_up && !last_btn_up) || (cur_btn_down && !last_btn_down)) {
            flick_ash();
        }

        // OK 键点烟 / 换新烟
        if (cur_btn_ok && !last_btn_ok) {
            if (g_vape.state == STATE_UNLIT) {
                g_vape.state = STATE_LIGHTING;
                g_vape.state_timer_ms = esp_timer_get_time() / 1000;
                s_need_full_redraw = true;
            } else if (g_vape.state == STATE_BURNED_OUT) {
                g_vape.state = STATE_UNLIT;
                g_vape.tobacco_remaining = 100.0f;
                g_vape.ash_length = 0.0f;
                s_need_full_redraw = true;
            } else if (g_vape.state == STATE_BURNING) {
                // 燃烧中按 OK 键相当于大口深吸！加速燃烧
                g_vape.tobacco_remaining -= 2.5f;
                g_vape.ash_length += 2.0f;
            }
        }

        last_btn_ok = cur_btn_ok;
        last_btn_up = cur_btn_up;
        last_btn_down = cur_btn_down;
        last_btn_pwr = cur_btn_pwr;

        // 点火状态自动过渡
        if (g_vape.state == STATE_LIGHTING) {
            if ((esp_timer_get_time() / 1000) - g_vape.state_timer_ms > 1200) {
                g_vape.state = STATE_BURNING;
                s_need_full_redraw = true;
            }
        }

        // 燃烧中的自然慢速微燃
        if (g_vape.state == STATE_BURNING && g_vape.screen_awake) {
            g_vape.tobacco_remaining -= 0.035f;
            g_vape.ash_length += 0.035f;

            if (g_vape.ash_length >= 45.0f) {
                flick_ash(); // 烟灰过长自然脱落
            }

            if (g_vape.tobacco_remaining <= 0.0f) {
                g_vape.tobacco_remaining = 0.0f;
                g_vape.state = STATE_BURNED_OUT;
                g_vape.total_smoked++;
                g_vape.current_realm_smoked++;

                const rank_info_t *cur_tier = get_current_realm(g_vape.realm_id);
                if (g_vape.realm_id < 11 && cur_tier->target_count > 0 && g_vape.current_realm_smoked >= cur_tier->target_count) {
                    g_vape.realm_id++;
                    g_vape.current_realm_smoked = 0;
                    ESP_LOGI(TAG, "Rank breakthrough!");
                }

                nvs_save_smoked_count(g_vape.realm_id, g_vape.current_realm_smoked, g_vape.total_smoked);
                s_need_full_redraw = true;
            }
        }

        render_vape_ui();
        vTaskDelay(pdMS_TO_TICKS(40)); // 保证调度让出 CPU，喂狗完全正常
    }
}
