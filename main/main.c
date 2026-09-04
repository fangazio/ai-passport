/**
 * @file main.c
 * @brief FoloToy AI-Passport Pixel Electronic Cigarette Firmware
 * Target: FoloToy AI-Passport (ESP32-C3, ST7789P3 240x320 Portrait Display)
 * Reference: https://github.com/FoloToy/ai-passport
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

// 自包含硬件管脚定义（针对 FoloToy AI-Passport ESP32-C3）
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
#define BSP_BTN_UP_GPIO         GPIO_NUM_2   // Arrow UP (上键: 抖烟灰)
#endif
#ifndef BSP_BTN_DOWN_GPIO
#define BSP_BTN_DOWN_GPIO       GPIO_NUM_3   // Arrow DOWN (下键: 抖烟灰)
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

// RGB565 颜色定义
#define COLOR_BLACK         0x0000
#define COLOR_WHITE         0xFFFF
#define COLOR_DARK_BG       0x0841
#define COLOR_AMBER         0xD380
#define COLOR_GOLD          0xFD20
#define COLOR_PAPER         0xE71C
#define COLOR_PAPER_SHADOW  0xA514
#define COLOR_CHAR_BROWN    0x79A0
#define COLOR_CHAR_BLACK    0x18C3
#define COLOR_EMBER_RED     0xD8A0
#define COLOR_EMBER_ORANGE  0xFA60
#define COLOR_EMBER_YELLOW  0xFFE0
#define COLOR_ASH_GREY      0x738E
#define COLOR_ASH_DARK      0x39E7
#define COLOR_FLAME_BLUE    0x3DEF
#define COLOR_SMOKE         0xDEFB
#define COLOR_CYAN          0x067F
#define COLOR_GREEN         0x3706

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

static void flick_ash(void) {
    if (g_vape.ash_length > 0.0f) {
        ESP_LOGI(TAG, "Ash flicked! Dropped: %.1f%%", g_vape.ash_length);
        g_vape.ash_length = 0.0f;
    }
}

// ST7789 驱动
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

    lcd_cmd(s_spi_lcd, 0x11);
    vTaskDelay(pdMS_TO_TICKS(120));

    uint8_t madctl = 0x00;
    lcd_cmd(s_spi_lcd, 0x36);
    lcd_data(s_spi_lcd, &madctl, 1);

    uint8_t colmod = 0x55;
    lcd_cmd(s_spi_lcd, 0x3A);
    lcd_data(s_spi_lcd, &colmod, 1);

    lcd_cmd(s_spi_lcd, 0x29);
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

static void render_vape_ui(void) {
    if (!g_vape.screen_awake) return;

    // 1. 顶部状态栏 (y: 0 ~ 24)
    lcd_fill_rect(0, 0, BSP_LCD_WIDTH, 24, 0x1082);
    int bat_w = (int)((g_vape.battery_percent / 100.0f) * 22);
    lcd_fill_rect(BSP_LCD_WIDTH - 30, 6, 24, 12, COLOR_ASH_GREY);
    lcd_fill_rect(BSP_LCD_WIDTH - 29, 7, bat_w, 10, COLOR_GREEN);
    lcd_fill_rect(BSP_LCD_WIDTH - 6, 9, 2, 6, COLOR_ASH_GREY);

    int juice_w = (int)((g_vape.juice_percent / 100.0f) * 22);
    lcd_fill_rect(BSP_LCD_WIDTH - 60, 6, 24, 12, COLOR_ASH_GREY);
    lcd_fill_rect(BSP_LCD_WIDTH - 59, 7, juice_w, 10, COLOR_CYAN);

    // 2. 电子烟拟真本体 (y: 35 ~ 255)
    uint16_t cig_width = 38;
    uint16_t cig_left = (BSP_LCD_WIDTH - cig_width) / 2;
    uint16_t total_cig_h = 210;
    uint16_t filter_h = 45;
    uint16_t filter_top = 35 + total_cig_h - filter_h;

    // 滤嘴
    lcd_fill_rect(cig_left, filter_top, cig_width, filter_h, COLOR_AMBER);
    lcd_fill_rect(cig_left, filter_top, 4, filter_h, 0x8A40);
    lcd_fill_rect(cig_left + cig_width - 5, filter_top, 5, filter_h, 0x8A40);

    // 烟杆主体背景
    lcd_fill_rect(cig_left, 35, cig_width, total_cig_h - filter_h, COLOR_DARK_BG);

    if (g_vape.state == STATE_UNLIT) {
        lcd_fill_rect(cig_left, 35, cig_width, total_cig_h - filter_h, COLOR_PAPER);
        lcd_fill_rect(cig_left, 35, 4, total_cig_h - filter_h, COLOR_PAPER_SHADOW);
        lcd_fill_rect(cig_left + cig_width - 4, 35, 4, total_cig_h - filter_h, COLOR_PAPER_SHADOW);
    } else if (g_vape.state == STATE_LIGHTING) {
        lcd_fill_rect(cig_left, 35, cig_width, total_cig_h - filter_h, COLOR_PAPER);
        // 点火火焰
        int flame_h = 24 + (esp_random() % 6);
        lcd_fill_rect(cig_left - 4, 35 - flame_h, cig_width + 8, flame_h, COLOR_FLAME_BLUE);
        lcd_fill_rect(cig_left + 4, 35 - flame_h + 4, cig_width - 8, flame_h - 6, COLOR_GOLD);
        lcd_fill_rect(cig_left + 10, 35 - flame_h + 8, cig_width - 20, flame_h - 10, COLOR_WHITE);
    } else if (g_vape.state == STATE_BURNING) {
        float unburned_ratio = g_vape.tobacco_remaining / 100.0f;
        float ash_ratio = g_vape.ash_length / 100.0f;
        int max_tobacco_h = total_cig_h - filter_h;

        int unburned_h = (int)(unburned_ratio * max_tobacco_h);
        int ash_h = (int)(ash_ratio * max_tobacco_h);
        if (ash_h > 45) ash_h = 45;

        int unburned_top = filter_top - unburned_h;
        if (unburned_h > 0) {
            lcd_fill_rect(cig_left, unburned_top, cig_width, unburned_h, COLOR_PAPER);
            lcd_fill_rect(cig_left, unburned_top, 4, unburned_h, COLOR_PAPER_SHADOW);
            lcd_fill_rect(cig_left + cig_width - 4, unburned_top, 4, unburned_h, COLOR_PAPER_SHADOW);
        }

        // 燃烧火圈
        int ember_top = unburned_top - 6;
        if (ember_top >= 35) {
            uint16_t ember_col = (g_vape.suction_strength > 10) ? COLOR_EMBER_YELLOW : COLOR_EMBER_ORANGE;
            lcd_fill_rect(cig_left - 2, ember_top, cig_width + 4, 6, ember_col);
            lcd_fill_rect(cig_left + 4, ember_top + 1, cig_width - 8, 4, COLOR_EMBER_RED);
        }

        // 烟灰
        int ash_top = ember_top - ash_h;
        if (ash_h > 0 && ash_top >= 35) {
            lcd_fill_rect(cig_left, ash_top, cig_width, ash_h, COLOR_ASH_GREY);
            for (int i = 0; i < ash_h; i += 4) {
                lcd_fill_rect(cig_left + 2, ash_top + i, cig_width - 4, 2, COLOR_ASH_DARK);
            }
        }
    } else if (g_vape.state == STATE_BURNED_OUT) {
        lcd_fill_rect(cig_left, filter_top - 4, cig_width, 4, COLOR_CHAR_BLACK);
    }

    // 3. 境界进度展示 (y: 265 ~ 318)
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
}

void app_main(void) {
    ESP_LOGI(TAG, "Starting FoloToy AI-Passport Vape Firmware...");
    nvs_flash_init();
    nvs_load_smoked_count();

    // 按键 GPIO 初始化
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << BSP_BTN_UP_GPIO) | (1ULL << BSP_BTN_DOWN_GPIO) |
                        (1ULL << BSP_BTN_OK_GPIO) | (1ULL << BSP_BTN_POWER_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&btn_cfg);

    // LCD 引脚配置（安全过滤负数引脚，避免位移报错）
    uint64_t lcd_mask = (1ULL << BSP_LCD_DC) | (1ULL << BSP_LCD_BACKLIGHT);
#if defined(BSP_LCD_RST) && (BSP_LCD_RST >= 0)
    lcd_mask |= (1ULL << BSP_LCD_RST);
#endif
    gpio_config_t lcd_pins = {
        .pin_bit_mask = lcd_mask,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&lcd_pins);

    // SPI2 初始化
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
            } else if (g_vape.state == STATE_BURNED_OUT) {
                g_vape.state = STATE_UNLIT;
                g_vape.tobacco_remaining = 100.0f;
                g_vape.ash_length = 0.0f;
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
            ESP_LOGI(TAG, "Mic sound inhalation detected -> auto-igniting cigarette!");
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
                ESP_LOGW(TAG, "Ash reached 45%% limit! Gravity snap, ash auto-dropped.");
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
                    ESP_LOGI(TAG, "Breakthrough! Entering realm: %s", REALM_TIERS[g_vape.realm_id].title);
                }

                nvs_save_smoked_count(g_vape.realm_id, g_vape.current_realm_smoked, g_vape.total_smoked);
            }
        }

        render_vape_ui();
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}
