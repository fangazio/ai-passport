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
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/i2s_std.h"
#include "bsp_pins.h"

static const char *TAG = "AI_PASSPORT_VAPE";

// RGB565 Colors
#define COLOR_BLACK         0x0000
#define COLOR_WHITE         0xFFFF
#define COLOR_DARK_BG       0x0841  // #09090b
#define COLOR_AMBER         0xD380  // Cork Filter #b45309
#define COLOR_GOLD          0xFD20  // #facc15
#define COLOR_PAPER         0xE71C  // #e4e4e7
#define COLOR_PAPER_SHADOW  0xA514  // #a1a1aa
#define COLOR_CHAR_BROWN    0x79A0  // #78350f
#define COLOR_CHAR_BLACK    0x18C3  // #1c1917
#define COLOR_EMBER_RED     0xD8A0  // #dc2626
#define COLOR_EMBER_ORANGE  0xFA60  // #f97316
#define COLOR_EMBER_YELLOW  0xFFE0  // #fef08a
#define COLOR_ASH_GREY      0x738E  // #71717a
#define COLOR_ASH_DARK      0x39E7  // #3f3f46
#define COLOR_FLAME_BLUE    0x3DEF  // #38bdf8
#define COLOR_SMOKE         0xDEFB  // #d4d4d8
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
    uint32_t target_count; // 晋升下一境界所需吸烟根数 (进入本境界后从0计数)
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

// NVS Persistence
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

// ST7789 Low-Level Driver
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
    gpio_set_level(BSP_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(BSP_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

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

// I2S Microphone
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
        int rms = (int)sqrt((double)sum_sq / count);
        int suction = (rms - 300) * 100 / 2500;
        if (suction < 0) suction = 0;
        if (suction > 100) suction = 100;
        return (uint8_t)suction;
    }
    return 0;
}

// Ash Flicking on UP / DOWN buttons
static void flick_ash(void) {
    if (g_vape.ash_length < 3.0f) return;
    ESP_LOGI(TAG, "Ash flicked! Reset ash to 0%%");
    g_vape.ash_length = 0.0f;
    lcd_fill_rect(95, 26, 50, 70, COLOR_DARK_BG);
}

// Vertical Cigarette & OLED UI Renderer
static void render_vape_ui(void) {
    if (!g_vape.screen_awake) {
        lcd_fill_rect(0, 0, BSP_LCD_WIDTH, BSP_LCD_HEIGHT, COLOR_BLACK);
        return;
    }

    // 1. Top status bar
    lcd_fill_rect(0, 0, BSP_LCD_WIDTH, 24, 0x18C3);
    uint16_t batt_color = g_vape.battery_percent < 20 ? COLOR_EMBER_RED : COLOR_GREEN;
    lcd_fill_rect(10, 6, 24, 12, batt_color);
    lcd_fill_rect(34, 9, 3, 6, batt_color);

    uint16_t juice_color = g_vape.juice_percent < 15 ? COLOR_EMBER_RED : COLOR_FLAME_BLUE;
    lcd_fill_rect(BSP_LCD_WIDTH - 36, 6, 26, 12, juice_color);

    // 2. Vertical Cigarette
    const int center_x = 120;
    const int cig_width = 24;
    const int cig_left = center_x - (cig_width / 2);
    const int filter_bottom = 240;
    const int filter_height = 45;
    const int filter_top = filter_bottom - filter_height;
    const int max_shaft_len = 125;

    int current_shaft = (int)((max_shaft_len * g_vape.tobacco_remaining) / 100.0f);
    int tip_y = filter_top - current_shaft;

    lcd_fill_rect(cig_left - 30, 26, cig_width + 60, 220, COLOR_DARK_BG);

    // Filter
    lcd_fill_rect(cig_left, filter_top, cig_width, filter_height, COLOR_AMBER);
    lcd_fill_rect(cig_left - 1, filter_top - 2, cig_width + 2, 3, COLOR_GOLD);

    // Paper shaft
    if (current_shaft > 0) {
        lcd_fill_rect(cig_left, tip_y, cig_width, current_shaft, COLOR_PAPER);
        lcd_fill_rect(cig_left, tip_y, 3, current_shaft, COLOR_WHITE);
        lcd_fill_rect(cig_left + cig_width - 3, tip_y, 3, current_shaft, COLOR_PAPER_SHADOW);

        if (g_vape.state == STATE_BURNING || g_vape.state == STATE_LIGHTING) {
            lcd_fill_rect(cig_left, tip_y, cig_width, 6, COLOR_CHAR_BROWN);
            lcd_fill_rect(cig_left, tip_y, cig_width, 2, COLOR_CHAR_BLACK);
        }
    }

    if (g_vape.state == STATE_UNLIT) {
        lcd_fill_rect(cig_left, tip_y - 3, cig_width, 3, COLOR_CHAR_BROWN);
        lcd_fill_rect(center_x - 1, tip_y - 12, 2, 6, COLOR_FLAME_BLUE);
        lcd_fill_rect(center_x - 4, tip_y - 9, 8, 2, COLOR_FLAME_BLUE);
    } else if (g_vape.state == STATE_LIGHTING) {
        int flame_h = 24 + (esp_random() % 6);
        lcd_fill_rect(center_x - 6, tip_y - flame_h, 12, flame_h, COLOR_EMBER_ORANGE);
        lcd_fill_rect(center_x - 3, tip_y - flame_h + 4, 6, flame_h - 8, COLOR_EMBER_YELLOW);
        lcd_fill_rect(center_x - 2, tip_y - 4, 4, 4, COLOR_FLAME_BLUE);
    } else if (g_vape.state == STATE_BURNING) {
        // Ash column length proportional to 125px cigarette shaft
        int ash_h = (int)((g_vape.ash_length / 100.0f) * 125.0f);
        if (ash_h > 0) {
            lcd_fill_rect(cig_left + 1, tip_y - ash_h, cig_width - 2, ash_h, COLOR_ASH_GREY);
        }

        bool is_puffing = g_vape.suction_strength > 10;
        uint16_t ember_core = is_puffing ? COLOR_WHITE : COLOR_EMBER_ORANGE;
        lcd_fill_rect(cig_left - 1, tip_y - 1, cig_width + 2, 4, COLOR_EMBER_RED);
        lcd_fill_rect(cig_left + 2, tip_y - 1, cig_width - 4, 3, ember_core);

        int smoke_y = tip_y - ash_h - (esp_random() % 25);
        int smoke_x = center_x + ((int)(esp_random() % 16) - 8);
        lcd_fill_rect(smoke_x, smoke_y, 4, 4, COLOR_SMOKE);
    } else if (g_vape.state == STATE_BURNED_OUT) {
        lcd_fill_rect(cig_left, filter_top - 4, cig_width, 4, COLOR_CHAR_BLACK);
    }

    // 3. Cultivation Rank Display (y: 265 ~ 318)
    const rank_info_t *cur_realm = get_current_realm(g_vape.realm_id);
    lcd_fill_rect(6, 265, BSP_LCD_WIDTH - 12, 50, cur_realm->is_yandi ? 0x3180 : 0x1082);
    lcd_fill_rect(8, 267, BSP_LCD_WIDTH - 16, 46, cur_realm->is_yandi ? 0x41C0 : COLOR_DARK_BG);

    if (cur_realm->is_yandi) {
        // Imperial 👑【烟帝】Banner + 吸烟根数
        lcd_fill_rect(16, 272, BSP_LCD_WIDTH - 32, 16, COLOR_GOLD);
        lcd_fill_rect(24, 294, BSP_LCD_WIDTH - 48, 12, COLOR_EMBER_YELLOW);
    } else {
        // Per-realm progress bar: resets to 0 upon entering each new realm!
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

    // Buttons
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << BSP_BTN_UP_GPIO) | (1ULL << BSP_BTN_DOWN_GPIO) |
                        (1ULL << BSP_BTN_OK_GPIO) | (1ULL << BSP_BTN_POWER_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&btn_cfg);

    // LCD Pins
    gpio_config_t lcd_pins = {
        .pin_bit_mask = (1ULL << BSP_LCD_DC) | (1ULL << BSP_LCD_RST) | (1ULL << BSP_LCD_BACKLIGHT),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&lcd_pins);

    // SPI2
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

        // 麦克风吸气声控引燃：未点燃时吸气，声控气流自动点火
        if (g_vape.state == STATE_UNLIT && g_vape.screen_awake && g_vape.suction_strength > 15) {
            g_vape.state = STATE_LIGHTING;
            g_vape.state_timer_ms = esp_timer_get_time() / 1000;
            ESP_LOGI(TAG, "Mic sound inhalation detected -> auto-igniting cigarette!");
        }

        if (g_vape.state == STATE_BURNING && g_vape.screen_awake) {
            float burn_rate = 0.0f;
            if (g_vape.suction_strength > 10) {
                // 每次吸烟非匀速燃烧：随机根据吸入声音强弱与气流湍流判定燃烧长度 (通常 9-15 口吸完)
                float sound_ratio = (g_vape.suction_strength - 10.0f) / 90.0f;
                if (sound_ratio < 0.0f) sound_ratio = 0.0f;
                if (sound_ratio > 1.0f) sound_ratio = 1.0f;
                float turbulence = 0.82f + ((float)(esp_random() % 360) / 1000.0f); // ±18%
                burn_rate = (0.26f + powf(sound_ratio, 0.75f) * 0.26f) * turbulence;
                g_vape.juice_percent -= 0.08f;
            } else {
                // 没有吸入操作时速度明显极慢减缓 (自然阴燃微耗 ~0.0005f / tick，比抽吸慢 300+ 倍)
                burn_rate = 0.0005f;
            }

            g_vape.tobacco_remaining -= burn_rate;
            g_vape.ash_length += burn_rate;

            // 烟灰物理规则：平时不抖烟灰不掉；超过 45% 临界长度时重力超限自动掉落，掉落后重新从 0 开始计算累积
            if (g_vape.ash_length >= 45.0f) {
                ESP_LOGW(TAG, "Ash reached 45%% limit! Gravity snap, ash auto-dropped, recalculating from 0%%.");
                g_vape.ash_length = 0.0f; // 自然脱落，重置为 0，后续燃烧重新计算
            }

                if (g_vape.tobacco_remaining <= 0.0f) {
                    g_vape.tobacco_remaining = 0.0f;
                    g_vape.state = STATE_BURNED_OUT;
                    g_vape.total_smoked++;
                    g_vape.current_realm_smoked++;

                    const rank_info_t *cur_tier = get_current_realm(g_vape.realm_id);
                    if (g_vape.realm_id < 11 && cur_tier->target_count > 0 && g_vape.current_realm_smoked >= cur_tier->target_count) {
                        // 进入一个新境界，吸烟数量从0开始计数！
                        g_vape.realm_id++;
                        g_vape.current_realm_smoked = 0;
                        ESP_LOGI(TAG, "Breakthrough! Entering realm: %s, smoked count reset to 0",
                                 REALM_TIERS[g_vape.realm_id].title);
                    }

                    nvs_save_smoked_count(g_vape.realm_id, g_vape.current_realm_smoked, g_vape.total_smoked);
                }
            }
        }

        render_vape_ui();
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}
