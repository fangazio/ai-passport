/**
 * @file main.c
 * @brief FoloToy AI-Passport Pixel Electronic Cigarette Firmware
 * Target: FoloToy AI-Passport (ESP32-C3, ST7789 IPS 240x320 Display)
 * Features: Zero-Flicker Differential Rendering, ST7789 IPS Color Correction (0x21 INVON)
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
#include "driver/i2s_std.h"

#if __has_include("bsp_pins.h")
#include "bsp_pins.h"
#endif

// ===================== 自包含硬件定义（无需依赖外部头文件） =====================
#ifndef BSP_LCD_WIDTH
#define BSP_LCD_WIDTH           240
#endif
#ifndef BSP_LCD_HEIGHT
#define BSP_LCD_HEIGHT          320
#endif
#ifndef BSP_LCD_SPI_HOST
#define BSP_LCD_SPI_HOST        SPI2_HOST
#endif
#ifndef BSP_LCD_CS
#define BSP_LCD_CS              GPIO_NUM_1
#endif
#ifndef BSP_LCD_SCLK
#define BSP_LCD_SCLK            GPIO_NUM_4
#endif
#ifndef BSP_LCD_MOSI
#define BSP_LCD_MOSI            GPIO_NUM_6
#endif
#ifndef BSP_LCD_DC
#define BSP_LCD_DC              GPIO_NUM_5
#endif
#ifndef BSP_LCD_BACKLIGHT
#define BSP_LCD_BACKLIGHT       GPIO_NUM_21
#endif
#ifndef BSP_LCD_FREQ_HZ
#define BSP_LCD_FREQ_HZ         (40 * 1000 * 1000)
#endif

// 物理按键定义
#ifndef BSP_BTN_UP_GPIO
#define BSP_BTN_UP_GPIO         GPIO_NUM_2   // Arrow UP (上键: 弹烟灰)
#endif
#ifndef BSP_BTN_DOWN_GPIO
#define BSP_BTN_DOWN_GPIO       GPIO_NUM_3   // Arrow DOWN (下键: 弹烟灰)
#endif
#ifndef BSP_BTN_OK_GPIO
#define BSP_BTN_OK_GPIO         GPIO_NUM_9   // OK 按键 (点火/新烟)
#endif
#ifndef BSP_BTN_POWER_GPIO
#define BSP_BTN_POWER_GPIO      GPIO_NUM_8   // 电源键 (息屏/唤醒)
#endif

// 麦克风音频采集
#ifndef BSP_I2S_NUM
#define BSP_I2S_NUM             I2S_NUM_0
#endif
#ifndef BSP_I2S_BCLK
#define BSP_I2S_BCLK            GPIO_NUM_10
#endif
#ifndef BSP_I2S_WS
#define BSP_I2S_WS              GPIO_NUM_11
#endif
#ifndef BSP_I2S_DIN
#define BSP_I2S_DIN             GPIO_NUM_18
#endif
#ifndef BSP_I2S_SAMPLE_RATE
#define BSP_I2S_SAMPLE_RATE     16000
#endif

static const char *TAG = "AI_PASSPORT_VAPE";

// ===================== 颜色常量 (ST7789 IPS RGB565) =====================
#define COLOR_BLACK         0x0000
#define COLOR_WHITE         0xFFFF
#define COLOR_DARK_BG       0x0000  // 纯黑背景
#define COLOR_STATUS_BG     0x1082  // 顶部状态栏底色
#define COLOR_AMBER         0xD380  // 琥珀滤嘴
#define COLOR_GOLD          0xFD20  // 金色滤嘴金圈
#define COLOR_PAPER         0xFFFF  // 洁白卷烟纸
#define COLOR_PAPER_SHADOW  0xC618  // 烟纸边缘阴影
#define COLOR_CHAR_BROWN    0x8A22  // 烟丝焦化
#define COLOR_CHAR_BLACK    0x2104  // 烧焦黑边
#define COLOR_EMBER_RED     0xF800  // 炭火红
#define COLOR_EMBER_ORANGE  0xFD20  // 明火橙
#define COLOR_EMBER_YELLOW  0xFFE0  // 猛抽高光黄
#define COLOR_ASH_GREY      0x8C71  // 烟灰浅灰
#define COLOR_ASH_DARK      0x4208  // 烟灰斑驳深灰
#define COLOR_FLAME_BLUE    0x05BF  // 打火机蓝色火苗
#define COLOR_SMOKE         0xD6BA  // 烟雾白
#define COLOR_CYAN          0x07FF  // 烟油亮青
#define COLOR_GREEN         0x07E0  // 电池亮绿

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
    uint8_t realm_id;           // 0 ~ 11 (凡人 ~ 烟帝)
    uint32_t current_realm_smoked; // 进入新境界从0开始计数
    uint32_t total_smoked;      // 历史总累计吸烟根数
    uint8_t battery_percent;
    float juice_percent;
    uint8_t suction_strength;   // 0 ~ 100 from Mic
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
    .battery_percent = 88,
    .juice_percent = 95.0f,
    .suction_strength = 0,
    .screen_awake = true,
    .state_timer_ms = 0
};

static spi_device_handle_t s_spi_lcd;
static i2s_chan_handle_t s_rx_chan = NULL;

typedef struct {
    const char *title;
    uint32_t target_count;
    bool is_yandi;
    const char *badge;
} rank_info_t;

static const rank_info_t REALM_TIERS[] = {
    {"凡人",   9,  false, "🌱【凡人】"},
    {"烟之气", 10, false, "💨【烟之气】"},
    {"烟者",   11, false, "🌀【烟者】"},
    {"烟师",   12, false, "📜【烟师】"},
    {"大烟师", 13, false, "🛡️【大烟师】"},
    {"烟灵",   14, false, "🔮【烟灵】"},
    {"烟王",   15, false, "🦁【烟王】"},
    {"烟皇",   16, false, "🦅【烟皇】"},
    {"烟宗",   17, false, "⚡【烟宗】"},
    {"烟尊",   18, false, "💎【烟尊】"},
    {"烟圣",   19, false, "✨【烟圣】"},
    {"烟帝",   0,  true,  "👑【烟帝】"}
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

// ST7789 底层 SPI 写入
static void lcd_cmd(spi_device_handle_t spi, const uint8_t cmd) {
    gpio_set_level(BSP_LCD_DC, 0);
    spi_transaction_t t = { .length = 8, .tx_buffer = &cmd };
    spi_device_polling_transmit(spi, &t);
}

static void lcd_data(spi_device_handle_t spi, const uint8_t *data, int len) {
    if (len == 0) return;
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

static void lcd_init_st7789(void) {
#if defined(BSP_LCD_RST) && (BSP_LCD_RST >= 0)
    gpio_set_level(BSP_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(BSP_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
#else
    vTaskDelay(pdMS_TO_TICKS(120));
#endif

    lcd_cmd(s_spi_lcd, 0x11); // Sleep Out
    vTaskDelay(pdMS_TO_TICKS(120));

    uint8_t madctl = 0x00; // RGB 竖屏模式
    lcd_cmd(s_spi_lcd, 0x36);
    lcd_data(s_spi_lcd, &madctl, 1);

    uint8_t colmod = 0x55; // 16-bit RGB565
    lcd_cmd(s_spi_lcd, 0x3A);
    lcd_data(s_spi_lcd, &colmod, 1);

    // 核心重点：ST7789 IPS 屏必须发送 0x21 (INVON 反相)，否则黑底全白、颜色反相！
    lcd_cmd(s_spi_lcd, 0x21);

    lcd_cmd(s_spi_lcd, 0x29); // Display ON
    vTaskDelay(pdMS_TO_TICKS(20));

    gpio_set_level(BSP_LCD_BACKLIGHT, 1);
    lcd_fill_rect(0, 0, BSP_LCD_WIDTH, BSP_LCD_HEIGHT, COLOR_DARK_BG);
}

// I2S 麦克风
static void i2s_mic_init(void) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(BSP_I2S_NUM, I2S_ROLE_MASTER);
    i2s_new_channel(&chan_cfg, NULL, &s_rx_chan);

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BSP_I2S_SAMPLE_RATE),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = BSP_I2S_BCLK,
            .ws = BSP_I2S_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = BSP_I2S_DIN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false }
        }
    };
    i2s_channel_init_std_mode(s_rx_chan, &std_cfg);
    i2s_channel_enable(s_rx_chan);
}

static uint8_t sample_mic_suction_strength(void) {
    if (!s_rx_chan) return 0;
    int16_t r_buf[128];
    size_t bytes_read = 0;
    if (i2s_channel_read(s_rx_chan, r_buf, sizeof(r_buf), &bytes_read, 10) == ESP_OK && bytes_read > 0) {
        int count = bytes_read / sizeof(int16_t);
        int64_t sum_sq = 0;
        for (int i = 0; i < count; i++) sum_sq += (int64_t)r_buf[i] * r_buf[i];
        int rms = (int)sqrtf((float)(sum_sq / count));
        int strength = (rms - 200) / 25;
        if (strength < 0) strength = 0;
        if (strength > 100) strength = 100;
        return (uint8_t)strength;
    }
    return 0;
}

// 差量化缓存（彻底解决每秒 25 次清屏导致的剧烈频闪）
static bool s_need_full_redraw = true;
static int s_prev_shaft_h = -1;
static int s_prev_ash_h = -1;
static int s_prev_smoke_x = -1;
static int s_prev_smoke_y = -1;
static cig_state_t s_prev_state = (cig_state_t)255;
static uint8_t s_prev_realm_id = 255;
static uint32_t s_prev_cur_smoked = 999999;

static void flick_ash(void) {
    if (g_vape.ash_length < 2.0f) return;
    ESP_LOGI(TAG, "Ash flicked!");
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

    // 仅在开机或状态转变时重绘静态背景和固定滤嘴，避免频闪
    if (s_need_full_redraw || g_vape.state != s_prev_state) {
        s_need_full_redraw = false;
        s_prev_state = g_vape.state;

        lcd_fill_rect(0, 0, BSP_LCD_WIDTH, BSP_LCD_HEIGHT, COLOR_DARK_BG);

        // 1. 状态栏
        lcd_fill_rect(0, 0, BSP_LCD_WIDTH, 24, COLOR_STATUS_BG);
        uint16_t batt_color = g_vape.battery_percent < 20 ? COLOR_EMBER_RED : COLOR_GREEN;
        lcd_fill_rect(10, 6, 24, 12, batt_color);
        lcd_fill_rect(34, 9, 3, 6, batt_color);

        int juice_w = (int)((g_vape.juice_percent / 100.0f) * 26);
        if (juice_w < 0) juice_w = 0;
        uint16_t juice_color = g_vape.juice_percent < 15 ? COLOR_EMBER_RED : COLOR_CYAN;
        lcd_fill_rect(BSP_LCD_WIDTH - 36, 6, juice_w, 12, juice_color);

        // 2. 静态烟嘴（位置永远固定）
        lcd_fill_rect(cig_left, filter_top, cig_width, filter_height, COLOR_AMBER);
        lcd_fill_rect(cig_left - 1, filter_top - 2, cig_width + 2, 3, COLOR_GOLD);
        lcd_fill_rect(cig_left, filter_top, 3, filter_height, 0xFD40);
        lcd_fill_rect(cig_left + cig_width - 3, filter_top, 3, filter_height, 0x8A00);

        // 3. 修仙境界底框
        const rank_info_t *cur_realm = get_current_realm(g_vape.realm_id);
        lcd_fill_rect(6, 265, BSP_LCD_WIDTH - 12, 50, cur_realm->is_yandi ? 0x3180 : 0x1082);
        lcd_fill_rect(8, 267, BSP_LCD_WIDTH - 16, 46, cur_realm->is_yandi ? 0x41C0 : COLOR_DARK_BG);

        if (cur_realm->is_yandi) {
            lcd_fill_rect(16, 272, BSP_LCD_WIDTH - 32, 16, COLOR_GOLD);
            lcd_fill_rect(24, 294, BSP_LCD_WIDTH - 48, 12, COLOR_EMBER_YELLOW);
        } else {
            float prog = cur_realm->target_count > 0 ? ((float)g_vape.current_realm_smoked / cur_realm->target_count) : 1.0f;
            if (prog > 1.0f) prog = 1.0f;
            int progress_w = (int)(prog * (BSP_LCD_WIDTH - 40));
            lcd_fill_rect(20, 298, progress_w, 6, COLOR_CYAN);
        }

        s_prev_shaft_h = -1;
        s_prev_ash_h = -1;
        s_prev_smoke_x = -1;
        s_prev_smoke_y = -1;
        s_prev_realm_id = g_vape.realm_id;
        s_prev_cur_smoked = g_vape.current_realm_smoked;
    }

    // 动态无频闪增量绘制
    if (g_vape.state == STATE_UNLIT) {
        int current_shaft = (int)((max_shaft_len * g_vape.tobacco_remaining) / 100.0f);
        int tip_y = filter_top - current_shaft;
        if (s_prev_shaft_h != current_shaft) {
            s_prev_shaft_h = current_shaft;
            lcd_fill_rect(cig_left, tip_y, cig_width, current_shaft, COLOR_PAPER);
            lcd_fill_rect(cig_left, tip_y, 3, current_shaft, COLOR_WHITE);
            lcd_fill_rect(cig_left + cig_width - 3, tip_y, 3, current_shaft, COLOR_PAPER_SHADOW);
            lcd_fill_rect(cig_left, tip_y - 3, cig_width, 3, COLOR_CHAR_BROWN);
            lcd_fill_rect(center_x - 1, tip_y - 12, 2, 6, COLOR_FLAME_BLUE);
            lcd_fill_rect(center_x - 4, tip_y - 9, 8, 2, COLOR_FLAME_BLUE);
        }
    } else if (g_vape.state == STATE_LIGHTING) {
        int current_shaft = (int)((max_shaft_len * g_vape.tobacco_remaining) / 100.0f);
        int tip_y = filter_top - current_shaft;
        lcd_fill_rect(center_x - 10, tip_y - 32, 20, 32, COLOR_DARK_BG);
        int flame_h = 24 + (esp_random() % 6);
        lcd_fill_rect(center_x - 6, tip_y - flame_h, 12, flame_h, COLOR_EMBER_ORANGE);
        lcd_fill_rect(center_x - 3, tip_y - flame_h + 4, 6, flame_h - 8, COLOR_EMBER_YELLOW);
        lcd_fill_rect(center_x - 1, tip_y - flame_h + 8, 2, flame_h - 12, COLOR_WHITE);
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

            s_prev_shaft_h = current_shaft;
            s_prev_ash_h = ash_h;
        }

        bool is_puffing = g_vape.suction_strength > 10;
        uint16_t ember_core = is_puffing ? COLOR_WHITE : COLOR_EMBER_YELLOW;
        lcd_fill_rect(cig_left - 1, tip_y - 2, cig_width + 2, 4, COLOR_EMBER_RED);
        lcd_fill_rect(cig_left + 2, tip_y - 2, cig_width - 4, 3, ember_core);

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
        }
    }

    if (g_vape.current_realm_smoked != s_prev_cur_smoked || g_vape.realm_id != s_prev_realm_id) {
        s_prev_cur_smoked = g_vape.current_realm_smoked;
        s_prev_realm_id = g_vape.realm_id;
        const rank_info_t *cur_realm = get_current_realm(g_vape.realm_id);
        if (!cur_realm->is_yandi) {
            float prog = cur_realm->target_count > 0 ? ((float)g_vape.current_realm_smoked / cur_realm->target_count) : 1.0f;
            if (prog > 1.0f) prog = 1.0f;
            int progress_w = (int)(prog * (BSP_LCD_WIDTH - 40));
            lcd_fill_rect(20, 298, progress_w, 6, COLOR_CYAN);
        }
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "Starting FoloToy AI-Passport Vape Firmware...");
    nvs_flash_init();
    nvs_load_smoked_count();

    // 1. 初始化物理按键 GPIO
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << BSP_BTN_UP_GPIO) | (1ULL << BSP_BTN_DOWN_GPIO) |
                        (1ULL << BSP_BTN_OK_GPIO) | (1ULL << BSP_BTN_POWER_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&btn_cfg);

    // 2. 初始化 LCD 输出引脚（过滤负数 RST 防止编译报警）
    uint64_t lcd_mask = (1ULL << BSP_LCD_DC) | (1ULL << BSP_LCD_BACKLIGHT);
#if defined(BSP_LCD_RST) && (BSP_LCD_RST >= 0)
    lcd_mask |= (1ULL << BSP_LCD_RST);
#endif
    gpio_config_t lcd_pins = {
        .pin_bit_mask = lcd_mask,
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
    i2s_mic_init();

    while (1) {
        bool btn_up = (gpio_get_level(BSP_BTN_UP_GPIO) == 0);
        bool btn_down = (gpio_get_level(BSP_BTN_DOWN_GPIO) == 0);
        bool btn_ok = (gpio_get_level(BSP_BTN_OK_GPIO) == 0);
        bool btn_pwr = (gpio_get_level(BSP_BTN_POWER_GPIO) == 0);

        if (btn_pwr) {
            vTaskDelay(pdMS_TO_TICKS(150));
            g_vape.screen_awake = !g_vape.screen_awake;
            gpio_set_level(BSP_LCD_BACKLIGHT, g_vape.screen_awake ? 1 : 0);
            if (g_vape.screen_awake) {
                s_need_full_redraw = true;
            }
        }

        if (btn_up || btn_down) {
            flick_ash();
            vTaskDelay(pdMS_TO_TICKS(120));
        }

        if (btn_ok) {
            vTaskDelay(pdMS_TO_TICKS(150));
            if (g_vape.state == STATE_UNLIT) {
                g_vape.state = STATE_LIGHTING;
                g_vape.state_timer_ms = esp_timer_get_time() / 1000;
                s_need_full_redraw = true;
            } else if (g_vape.state == STATE_BURNED_OUT) {
                g_vape.state = STATE_UNLIT;
                g_vape.tobacco_remaining = 100.0f;
                g_vape.ash_length = 0.0f;
                s_need_full_redraw = true;
            }
        }

        if (g_vape.state == STATE_LIGHTING) {
            if ((esp_timer_get_time() / 1000) - g_vape.state_timer_ms > 1200) {
                g_vape.state = STATE_BURNING;
            }
        }

        g_vape.suction_strength = sample_mic_suction_strength();

        if (g_vape.state == STATE_UNLIT && g_vape.screen_awake && g_vape.suction_strength > 15) {
            g_vape.state = STATE_LIGHTING;
            g_vape.state_timer_ms = esp_timer_get_time() / 1000;
            s_need_full_redraw = true;
            ESP_LOGI(TAG, "Mic inhalation detected -> auto-ignite!");
        }

        if (g_vape.state == STATE_BURNING && g_vape.screen_awake) {
            float burn_rate = 0.0f;
            if (g_vape.suction_strength > 10) {
                float sound_ratio = (g_vape.suction_strength - 10.0f) / 90.0f;
                if (sound_ratio < 0.0f) sound_ratio = 0.0f;
                if (sound_ratio > 1.0f) sound_ratio = 1.0f;
                float turbulence = 0.82f + ((float)(esp_random() % 360) / 1000.0f);
                burn_rate = (0.26f + powf(sound_ratio, 0.75f) * 0.26f) * turbulence;
                g_vape.juice_percent -= 0.08f;
            } else {
                burn_rate = 0.0005f;
            }

            g_vape.tobacco_remaining -= burn_rate;
            g_vape.ash_length += burn_rate;

            if (g_vape.ash_length >= 45.0f) {
                g_vape.ash_length = 0.0f;
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
                    ESP_LOGI(TAG, "Breakthrough to realm: %s", REALM_TIERS[g_vape.realm_id].title);
                }

                nvs_save_smoked_count(g_vape.realm_id, g_vape.current_realm_smoked, g_vape.total_smoked);
            }
        }

        render_vape_ui();
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}
