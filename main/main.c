/**
 * @file main.c
 * @brief FoloToy AI-Passport Pixel Electronic Cigarette Firmware
 * 
 * Target Hardware: FoloToy AI-Passport (ESP32-C3, 8MB Flash)
 * Display: ST7789P3 240x320 Portrait RGB565 Display
 * Reference: https://github.com/FoloToy/ai-passport
 * 
 * Features:
 * 1. Vertical pixel art cigarette: Unlit -> Lighting -> Burning -> Burned Out
 * 2. Ash flicking on UP (ArrowUp) or DOWN (ArrowDown) buttons
 * 3. OK button ignition (点烟) & reload cartridge
 * 4. POWER button screen sleep/wake & standby
 * 5. I2S microphone airflow / breath sound detection with dynamic smoke output
 * 6. Cultivation Rank System: 9根(烟之气) ~ 19根(烟帝)，到达烟帝显示【烟帝】与吸烟根数
 * 7. Real-time battery & simulated e-juice percentage
 * 8. NVS persistent storage for smoked cigarette counter
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

// ============================================================================
// RGB565 Color Definitions
// ============================================================================
#define COLOR_BLACK         0x0000
#define COLOR_WHITE         0xFFFF
#define COLOR_DARK_BG       0x0841  // #09090b
#define COLOR_GRID          0x10A2
#define COLOR_AMBER         0xD380  // Cork Filter #b45309
#define COLOR_CORK_SPOT     0xDC00  // #d97706
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
#define COLOR_PURPLE        0x9B1E

// ============================================================================
// Cigarette State Enum
// ============================================================================
typedef enum {
    STATE_UNLIT = 0,
    STATE_LIGHTING,
    STATE_BURNING,
    STATE_BURNED_OUT
} cig_state_t;

// ============================================================================
// Firmware Global State
// ============================================================================
typedef struct {
    cig_state_t state;
    float tobacco_remaining;    // 0.0 ~ 100.0%
    float ash_length;           // 0.0 ~ 100.0%
    uint32_t cigarettes_smoked; // Smoked counter
    uint8_t battery_percent;
    float juice_percent;
    uint8_t suction_strength;   // 0 ~ 100 (from Mic/sensor)
    bool screen_awake;
    int64_t state_timer_ms;
} vape_firmware_t;

static vape_firmware_t g_vape = {
    .state = STATE_UNLIT,
    .tobacco_remaining = 100.0f,
    .ash_length = 0.0f,
    .cigarettes_smoked = 0,
    .battery_percent = 88,
    .juice_percent = 95.0f,
    .suction_strength = 0,
    .screen_awake = true,
    .state_timer_ms = 0
};

static spi_device_handle_t s_spi_lcd;
static i2s_chan_handle_t s_rx_chan = NULL;

// ============================================================================
// Cultivation Rank Ladder Helper
// ============================================================================
typedef struct {
    const char *title;
    bool is_yandi;
    const char *badge;
} rank_info_t;

static rank_info_t get_rank_info(uint32_t count) {
    if (count >= 19) return (rank_info_t){"烟帝", true,  "👑【烟帝】"};
    if (count == 18) return (rank_info_t){"烟圣", false, "✨【烟圣】"};
    if (count == 17) return (rank_info_t){"烟尊", false, "💎【烟尊】"};
    if (count == 16) return (rank_info_t){"烟宗", false, "⚡【烟宗】"};
    if (count == 15) return (rank_info_t){"烟皇", false, "🦅【烟皇】"};
    if (count == 14) return (rank_info_t){"烟王", false, "🦁【烟王】"};
    if (count == 13) return (rank_info_t){"烟灵", false, "🔮【烟灵】"};
    if (count == 12) return (rank_info_t){"大烟师", false, "🛡️【大烟师】"};
    if (count == 11) return (rank_info_t){"烟师", false, "📜【烟师】"};
    if (count == 10) return (rank_info_t){"烟者", false, "🌀【烟者】"};
    if (count == 9)  return (rank_info_t){"烟之气", false, "💨【烟之气】"};
    if (count > 0)   return (rank_info_t){"烟之气", false, "💨【烟之气】"};
    return (rank_info_t){"凡人", false, "🌱【凡人】"};
}

// ============================================================================
// NVS Storage Helper
// ============================================================================
static void nvs_load_smoked_count(void) {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("vape_data", NVS_READWRITE, &my_handle);
    if (err == ESP_OK) {
        uint32_t count = 0;
        if (nvs_get_u32(my_handle, "smoked_cnt", &count) == ESP_OK) {
            g_vape.cigarettes_smoked = count;
            ESP_LOGI(TAG, "Loaded smoked count from NVS: %lu", count);
        }
        nvs_close(my_handle);
    }
}

static void nvs_save_smoked_count(uint32_t count) {
    nvs_handle_t my_handle;
    if (nvs_open("vape_data", NVS_READWRITE, &my_handle) == ESP_OK) {
        nvs_set_u32(my_handle, "smoked_cnt", count);
        nvs_commit(my_handle);
        nvs_close(my_handle);
        ESP_LOGI(TAG, "Saved smoked count to NVS: %lu", count);
    }
}

// ============================================================================
// ST7789P3 SPI Display Driver
// ============================================================================
static void lcd_cmd(spi_device_handle_t spi, const uint8_t cmd) {
    gpio_set_level(BSP_LCD_DC, 0);
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &cmd
    };
    spi_device_polling_transmit(spi, &t);
}

static void lcd_data(spi_device_handle_t spi, const uint8_t *data, int len) {
    if (len == 0) return;
    gpio_set_level(BSP_LCD_DC, 1);
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = data
    };
    spi_device_polling_transmit(spi, &t);
}

static void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    uint8_t data[4];
    lcd_cmd(s_spi_lcd, 0x2A); // CASET
    data[0] = (x0 >> 8) & 0xFF; data[1] = x0 & 0xFF;
    data[2] = (x1 >> 8) & 0xFF; data[3] = x1 & 0xFF;
    lcd_data(s_spi_lcd, data, 4);

    lcd_cmd(s_spi_lcd, 0x2B); // RASET
    data[0] = (y0 >> 8) & 0xFF; data[1] = y0 & 0xFF;
    data[2] = (y1 >> 8) & 0xFF; data[3] = y1 & 0xFF;
    lcd_data(s_spi_lcd, data, 4);

    lcd_cmd(s_spi_lcd, 0x2C); // RAMWR
}

static void lcd_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color) {
    if (w == 0 || h == 0 || x >= BSP_LCD_WIDTH || y >= BSP_LCD_HEIGHT) return;
    if (x + w > BSP_LCD_WIDTH) w = BSP_LCD_WIDTH - x;
    if (y + h > BSP_LCD_HEIGHT) h = BSP_LCD_HEIGHT - y;

    lcd_set_window(x, y, x + w - 1, y + h - 1);

    // Batch send in chunks
    #define CHUNK_PIXELS 64
    uint16_t buffer[CHUNK_PIXELS];
    uint16_t color_be = (color >> 8) | (color << 8); // Swap endian for SPI
    for (int i = 0; i < CHUNK_PIXELS; i++) buffer[i] = color_be;

    int total_pixels = w * h;
    while (total_pixels > 0) {
        int to_send = total_pixels > CHUNK_PIXELS ? CHUNK_PIXELS : total_pixels;
        lcd_data(s_spi_lcd, (const uint8_t *)buffer, to_send * 2);
        total_pixels -= to_send;
    }
}

static void lcd_init_st7789(void) {
    // Hardware reset
    gpio_set_level(BSP_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(BSP_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    lcd_cmd(s_spi_lcd, 0x11); // Sleep Out
    vTaskDelay(pdMS_TO_TICKS(120));

    lcd_cmd(s_spi_lcd, 0x36); // MADCTL: Portrait mode
    uint8_t madctl = 0x00;
    lcd_data(s_spi_lcd, &madctl, 1);

    lcd_cmd(s_spi_lcd, 0x3A); // COLMOD: 16-bit/pixel RGB565
    uint8_t colmod = 0x55;
    lcd_data(s_spi_lcd, &colmod, 1);

    lcd_cmd(s_spi_lcd, 0x29); // Display ON
    vTaskDelay(pdMS_TO_TICKS(20));

    // Turn on backlight
    gpio_set_level(BSP_LCD_BACKLIGHT, 1);

    // Initial clear to black
    lcd_fill_rect(0, 0, BSP_LCD_WIDTH, BSP_LCD_HEIGHT, COLOR_DARK_BG);
}

// ============================================================================
// I2S Microphone Initialization & RMS Breath Inhale Detection
// ============================================================================
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
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
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
        for (int i = 0; i < count; i++) {
            sum_sq += (int64_t)r_buf[i] * r_buf[i];
        }
        int rms = (int)sqrt((double)sum_sq / count);
        // Map RMS to 0 - 100 suction intensity
        int suction = (rms - 300) * 100 / 2500;
        if (suction < 0) suction = 0;
        if (suction > 100) suction = 100;
        return (uint8_t)suction;
    }
    return 0;
}

// ============================================================================
// Ash Flicking Logic (Triggered on UP or DOWN Keys)
// ============================================================================
static void flick_ash(void) {
    if (g_vape.ash_length < 3.0f) return;
    ESP_LOGI(TAG, "Ash flicked via UP/DOWN key! (Ash: %.1f%% -> 0%%)", g_vape.ash_length);
    g_vape.ash_length = 0.0f;
    // Clear ash region visually
    lcd_fill_rect(100, 30, 40, 60, COLOR_DARK_BG);
}

// ============================================================================
// Vertical Cigarette & UI Frame Render Loop
// ============================================================================
static void render_vape_ui(void) {
    if (!g_vape.screen_awake) {
        // Standby screen
        lcd_fill_rect(0, 0, BSP_LCD_WIDTH, BSP_LCD_HEIGHT, COLOR_BLACK);
        return;
    }

    // 1. Top Status Bar: Battery % & Simulated Juice %
    // Clear top bar area
    lcd_fill_rect(0, 0, BSP_LCD_WIDTH, 24, 0x18C3);
    
    // Battery indicator (Left)
    uint16_t batt_color = g_vape.battery_percent < 20 ? COLOR_EMBER_RED : COLOR_GREEN;
    lcd_fill_rect(10, 6, 24, 12, batt_color);
    lcd_fill_rect(34, 9, 3, 6, batt_color);

    // Juice indicator (Right)
    uint16_t juice_color = g_vape.juice_percent < 15 ? COLOR_EMBER_RED : COLOR_FLAME_BLUE;
    lcd_fill_rect(BSP_LCD_WIDTH - 36, 6, 26, 12, juice_color);

    // 2. Vertical Cigarette Geometry
    const int center_x = 120;
    const int cig_width = 24;
    const int cig_left = center_x - (cig_width / 2); // 108
    const int filter_bottom = 240;
    const int filter_height = 45;
    const int filter_top = filter_bottom - filter_height; // 195
    const int max_shaft_len = 125;

    // Current white shaft height based on remaining tobacco
    int current_shaft = (int)((max_shaft_len * g_vape.tobacco_remaining) / 100.0f);
    int tip_y = filter_top - current_shaft;

    // Clear cigarette area background
    lcd_fill_rect(cig_left - 30, 26, cig_width + 60, 220, COLOR_DARK_BG);

    // Draw Cork Filter (Bottom)
    lcd_fill_rect(cig_left, filter_top, cig_width, filter_height, COLOR_AMBER);
    // Gold separator band
    lcd_fill_rect(cig_left - 1, filter_top - 2, cig_width + 2, 3, COLOR_GOLD);

    // Draw White Shaft (Paper)
    if (current_shaft > 0) {
        lcd_fill_rect(cig_left, tip_y, cig_width, current_shaft, COLOR_PAPER);
        // Highlight & Shadow
        lcd_fill_rect(cig_left, tip_y, 3, current_shaft, COLOR_WHITE);
        lcd_fill_rect(cig_left + cig_width - 3, tip_y, 3, current_shaft, COLOR_PAPER_SHADOW);

        // Burn ring near tip
        if (g_vape.state == STATE_BURNING || g_vape.state == STATE_LIGHTING) {
            lcd_fill_rect(cig_left, tip_y, cig_width, 6, COLOR_CHAR_BROWN);
            lcd_fill_rect(cig_left, tip_y, cig_width, 2, COLOR_CHAR_BLACK);
        }
    }

    // State Specific Graphics
    if (g_vape.state == STATE_UNLIT) {
        // Unlit tobacco top
        lcd_fill_rect(cig_left, tip_y - 3, cig_width, 3, COLOR_CHAR_BROWN);
        // Prompt reticle (blue cross)
        lcd_fill_rect(center_x - 1, tip_y - 12, 2, 6, COLOR_FLAME_BLUE);
        lcd_fill_rect(center_x - 4, tip_y - 9, 8, 2, COLOR_FLAME_BLUE);
    } else if (g_vape.state == STATE_LIGHTING) {
        // Electric ignition flame standing on top
        int flame_height = 24 + (esp_random() % 6);
        lcd_fill_rect(center_x - 6, tip_y - flame_height, 12, flame_height, COLOR_EMBER_ORANGE);
        lcd_fill_rect(center_x - 3, tip_y - flame_height + 4, 6, flame_height - 8, COLOR_EMBER_YELLOW);
        lcd_fill_rect(center_x - 2, tip_y - 4, 4, 4, COLOR_FLAME_BLUE);
    } else if (g_vape.state == STATE_BURNING) {
        // Ash column extending upward from tip
        int ash_pixel_h = (int)((g_vape.ash_length / 100.0f) * 28.0f);
        if (ash_pixel_h > 0) {
            int ash_top = tip_y - ash_pixel_h;
            lcd_fill_rect(cig_left + 1, ash_top, cig_width - 2, ash_pixel_h, COLOR_ASH_GREY);
            // Ash texture spots
            for (int ay = ash_top; ay < tip_y; ay += 4) {
                lcd_fill_rect(cig_left + 3 + ((ay * 3) % (cig_width - 6)), ay, 2, 2, COLOR_ASH_DARK);
            }
        }

        // Active Glowing Cherry Ember
        bool is_puffing = g_vape.suction_strength > 10;
        uint16_t ember_core = is_puffing ? COLOR_WHITE : COLOR_EMBER_ORANGE;
        lcd_fill_rect(cig_left - 1, tip_y - 1, cig_width + 2, 4, COLOR_EMBER_RED);
        lcd_fill_rect(cig_left + 2, tip_y - 1, cig_width - 4, 3, ember_core);

        // Smoke particles upward
        int smoke_y = tip_y - ash_pixel_h - (esp_random() % 25);
        int smoke_x = center_x + ((int)(esp_random() % 16) - 8);
        lcd_fill_rect(smoke_x, smoke_y, 4, 4, COLOR_SMOKE);
    } else if (g_vape.state == STATE_BURNED_OUT) {
        // Burned out stub right above filter
        lcd_fill_rect(cig_left, filter_top - 4, cig_width, 4, COLOR_CHAR_BLACK);
    }

    // 3. Bottom Cultivation Rank & Smoked Count Display (y: 265 ~ 318)
    rank_info_t rank = get_rank_info(g_vape.cigarettes_smoked);
    lcd_fill_rect(6, 265, BSP_LCD_WIDTH - 12, 50, rank.is_yandi ? 0x3180 : 0x1082); // Gold border or dark
    lcd_fill_rect(8, 267, BSP_LCD_WIDTH - 16, 46, rank.is_yandi ? 0x41C0 : COLOR_DARK_BG);

    if (rank.is_yandi) {
        // REACH 烟帝 (>= 19 根): Prominently show 烟帝 and the exact count!
        // Gold Crown Banner
        lcd_fill_rect(16, 272, BSP_LCD_WIDTH - 32, 16, COLOR_GOLD);
        // Bar indicating Imperial Rank
        lcd_fill_rect(24, 294, BSP_LCD_WIDTH - 48, 12, COLOR_EMBER_YELLOW);
    } else {
        // Normal progression rank bar
        int progress_w = (int)(((g_vape.cigarettes_smoked % 19) / 19.0f) * (BSP_LCD_WIDTH - 40));
        lcd_fill_rect(20, 298, progress_w, 6, COLOR_CYAN);
    }
}

// ============================================================================
// Main Application Task Loop
// ============================================================================
void app_main(void) {
    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, "FoloToy AI-Passport Vape Firmware Starting...");
    ESP_LOGI(TAG, "Target: ESP32-C3 | Display: ST7789P3 240x320");
    ESP_LOGI(TAG, "==================================================");

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    nvs_load_smoked_count();

    // 1. Setup Button GPIOs (Internal pullups)
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << BSP_BTN_UP_GPIO) | (1ULL << BSP_BTN_DOWN_GPIO) |
                        (1ULL << BSP_BTN_OK_GPIO) | (1ULL << BSP_BTN_POWER_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&btn_cfg);

    // 2. Setup ST7789 Backlight & Control Pins
    gpio_config_t lcd_pins = {
        .pin_bit_mask = (1ULL << BSP_LCD_DC) | (1ULL << BSP_LCD_RST) | (1ULL << BSP_LCD_BACKLIGHT),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&lcd_pins);

    // 3. Setup SPI2 Host for ST7789
    spi_bus_config_t buscfg = {
        .mosi_io_num = BSP_LCD_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = BSP_LCD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BSP_LCD_WIDTH * BSP_LCD_HEIGHT * 2
    };
    spi_bus_initialize(BSP_LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = BSP_LCD_FREQ_HZ,
        .mode = 0,
        .spics_io_num = BSP_LCD_CS,
        .queue_size = 7
    };
    spi_bus_add_device(BSP_LCD_SPI_HOST, &devcfg, &s_spi_lcd);

    // Initialize ST7789 Display
    lcd_init_st7789();

    // Initialize I2S Microphone
    i2s_mic_init();

    ESP_LOGI(TAG, "Hardware Peripherals Initialized. Entering Main Loop.");

    int frame_cnt = 0;
    while (1) {
        frame_cnt++;

        // 1. Read Physical Buttons (Active Low with pullups)
        bool btn_up = (gpio_get_level(BSP_BTN_UP_GPIO) == 0);
        bool btn_down = (gpio_get_level(BSP_BTN_DOWN_GPIO) == 0);
        bool btn_ok = (gpio_get_level(BSP_BTN_OK_GPIO) == 0);
        bool btn_pwr = (gpio_get_level(BSP_BTN_POWER_GPIO) == 0);

        // Power toggle (sleep / wake)
        if (btn_pwr) {
            vTaskDelay(pdMS_TO_TICKS(150)); // Debounce
            g_vape.screen_awake = !g_vape.screen_awake;
            gpio_set_level(BSP_LCD_BACKLIGHT, g_vape.screen_awake ? 1 : 0);
            ESP_LOGI(TAG, "Screen %s", g_vape.screen_awake ? "Awake" : "Sleeping");
        }

        // UP or DOWN Button: Flick Ash!
        if (btn_up || btn_down) {
            flick_ash();
            vTaskDelay(pdMS_TO_TICKS(120)); // Debounce
        }

        // OK Button: Ignition (点火) or Reset Cartridge
        if (btn_ok) {
            vTaskDelay(pdMS_TO_TICKS(150)); // Debounce
            if (g_vape.state == STATE_UNLIT) {
                g_vape.state = STATE_LIGHTING;
                g_vape.state_timer_ms = esp_timer_get_time() / 1000;
                ESP_LOGI(TAG, "OK Pressed -> State: LIGHTING");
            } else if (g_vape.state == STATE_BURNED_OUT) {
                g_vape.state = STATE_UNLIT;
                g_vape.tobacco_remaining = 100.0f;
                g_vape.ash_length = 0.0f;
                ESP_LOGI(TAG, "OK Pressed -> Reloaded new cartridge!");
            }
        }

        // Handle lighting state timeout (transition to burning after 1.2s)
        if (g_vape.state == STATE_LIGHTING) {
            int64_t now_ms = esp_timer_get_time() / 1000;
            if (now_ms - g_vape.state_timer_ms > 1200) {
                g_vape.state = STATE_BURNING;
                ESP_LOGI(TAG, "State: BURNING");
            }
        }

        // 2. Sample Microphone for Suction Strength
        g_vape.suction_strength = sample_mic_suction_strength();

        // 3. Update Smoking Consumption Physics
        if (g_vape.state == STATE_BURNING && g_vape.screen_awake) {
            // Actively puffing
            if (g_vape.suction_strength > 10) {
                float burn_rate = 0.25f + (g_vape.suction_strength / 100.0f) * 0.45f;
                g_vape.tobacco_remaining -= burn_rate;
                g_vape.ash_length += (burn_rate * 1.2f);
                g_vape.juice_percent -= 0.08f;

                // Auto drop ash if too long (>95%)
                if (g_vape.ash_length >= 95.0f) {
                    flick_ash();
                }

                // Check if cigarette is completely burned out
                if (g_vape.tobacco_remaining <= 0.0f) {
                    g_vape.tobacco_remaining = 0.0f;
                    g_vape.state = STATE_BURNED_OUT;
                    g_vape.cigarettes_smoked++;
                    nvs_save_smoked_count(g_vape.cigarettes_smoked);
                    
                    rank_info_t rank = get_rank_info(g_vape.cigarettes_smoked);
                    if (rank.is_yandi) {
                        ESP_LOGI(TAG, "👑 Reached 【烟帝】! Total Smoked: %lu", g_vape.cigarettes_smoked);
                    } else {
                        ESP_LOGI(TAG, "Finished cigarette #%lu -> Rank: %s", g_vape.cigarettes_smoked, rank.title);
                    }
                }
            }
        }

        // 4. Render UI Frame to ST7789 Display
        render_vape_ui();

        // Target ~25 FPS
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}
