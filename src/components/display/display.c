#include "include/display.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "font8x16.h"

static const char *TAG = "display";

/* ESP32-S3-EYE v2.x LCD wiring (matches esp-bsp). */
#define LCD_SPI_HOST   SPI2_HOST
#define LCD_PIN_SCLK   21
#define LCD_PIN_MOSI   47
#define LCD_PIN_CS     44
#define LCD_PIN_DC     43
#define LCD_PIN_BL     48
#define LCD_PIXEL_HZ   (40 * 1000 * 1000)
#define LCD_W          240
#define LCD_H          240

#define BTN_PAGE_GPIO  0   /* BOOT button, active low */

#define RENDER_PERIOD_US (500 * 1000)
#define N_PAGES 3

/* RGB565, byte-swapped for the panel's MSB-first 16-bit writes. */
#define RGB(r, g, b) \
    ((uint16_t)((((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)) >> 8) | \
                (((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)) << 8)))

#define COL_BG      RGB(0, 0, 0)
#define COL_HEADER  RGB(24, 48, 96)
#define COL_TEXT    RGB(230, 230, 230)
#define COL_DIM     RGB(140, 140, 140)
#define COL_GOOD    RGB(60, 220, 90)
#define COL_WARN    RGB(250, 180, 40)
#define COL_BAD     RGB(240, 70, 60)

/* The SPI DMA path needs internal memory; the full 113KB frame lives in
 * PSRAM and is flushed through a small internal bounce buffer in stripes. */
#define STRIPE_H 24

static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_fb;
static uint16_t *s_stripe;
static SemaphoreHandle_t s_flush_done;
static int s_page;
static bool s_backlight_on = true;
static int s_btn_low_streak;
static bool s_btn_latched;
static int64_t s_last_render_us;
static bool s_dirty = true;
static uint32_t s_render_count;

static void fill_rect(int x, int y, int w, int h, uint16_t c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > LCD_W) { w = LCD_W - x; }
    if (y + h > LCD_H) { h = LCD_H - y; }
    for (int j = 0; j < h; j++) {
        uint16_t *row = s_fb + (y + j) * LCD_W + x;
        for (int i = 0; i < w; i++) {
            row[i] = c;
        }
    }
}

static void draw_text(int x, int y, int scale, uint16_t c, const char *str)
{
    for (; *str != '\0'; str++) {
        unsigned ch = (unsigned char)*str;
        if (ch < FONT_FIRST || ch > FONT_LAST) {
            ch = '?';
        }
        const uint8_t *glyph = font8x16[ch - FONT_FIRST];
        for (int gy = 0; gy < FONT_H; gy++) {
            uint8_t bits = glyph[gy];
            for (int gx = 0; gx < FONT_W; gx++) {
                if (bits & (0x80 >> gx)) {
                    fill_rect(x + gx * scale, y + gy * scale, scale, scale, c);
                }
            }
        }
        x += FONT_W * scale;
        if (x >= LCD_W) {
            break;
        }
    }
}

static void draw_text_centered(int y, int scale, uint16_t c, const char *str)
{
    int w = (int)strlen(str) * FONT_W * scale;
    draw_text((LCD_W - w) / 2, y, scale, c, str);
}

static bool flush_done_cb(esp_lcd_panel_io_handle_t io,
                          esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_flush_done, &woken);
    return woken == pdTRUE;
}

static void flush(void)
{
    for (int y = 0; y < LCD_H; y += STRIPE_H) {
        int h = (y + STRIPE_H > LCD_H) ? LCD_H - y : STRIPE_H;
        memcpy(s_stripe, s_fb + y * LCD_W, (size_t)h * LCD_W * 2);
        if (esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_W, y + h,
                                      s_stripe) != ESP_OK) {
            return;
        }
        /* The stripe buffer is reused: wait for the DMA to finish. */
        xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(100));
    }
}

static void backlight_set(bool on)
{
    /* S3-EYE backlight is active LOW (esp-bsp drives duty 0 at 100%). */
    gpio_set_level(LCD_PIN_BL, on ? 0 : 1);
    s_backlight_on = on;
}

static void wifi_info(char *ip_out, size_t ip_len, int *rssi_out)
{
    snprintf(ip_out, ip_len, "-");
    *rssi_out = 0;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    if (netif != NULL && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0) {
        snprintf(ip_out, ip_len, IPSTR, IP2STR(&ip.ip));
    }
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        *rssi_out = ap.rssi;
    }
}

static void render_page_main(const display_status_t *st)
{
    char line[48];
    char ip[20];
    int rssi;
    wifi_info(ip, sizeof(ip), &rssi);
    bool wifi_up = (ip[0] != '-');

    fill_rect(0, 0, LCD_W, 28, COL_HEADER);
    draw_text(6, 6, 1, COL_TEXT, st->device_id != NULL ? st->device_id : "?");

    const char *state_txt;
    uint16_t state_col;
    switch (st->presence_state) {
    case 2:  state_txt = "PRESENT"; state_col = COL_GOOD; break;
    case 1:  state_txt = "ABSENT";  state_col = COL_DIM;  break;
    default: state_txt = "WARMUP";  state_col = COL_WARN; break;
    }
    draw_text_centered(64, 3, state_col, state_txt);

    snprintf(line, sizeof(line), "score %3u / >=%u", st->score, st->enter_threshold);
    draw_text_centered(126, 1, COL_TEXT, line);

    draw_text(6, 168, 1, COL_TEXT, "WiFi");
    draw_text(46, 168, 1, wifi_up ? COL_GOOD : COL_BAD, wifi_up ? "OK" : "DOWN");
    draw_text(120, 168, 1, COL_TEXT, "LINK");
    draw_text(160, 168, 1, st->session_up ? COL_GOOD : COL_BAD,
              st->session_up ? "OK" : "DOWN");

    draw_text(6, 194, 1, COL_DIM, ip);
    if (wifi_up) {
        snprintf(line, sizeof(line), "%ddBm", rssi);
        draw_text(168, 194, 1, COL_DIM, line);
    }
    draw_text(6, 218, 1, COL_DIM, "BOOT btn: next page");
}

static void render_page_net(const display_status_t *st)
{
    char line[64];
    char ip[20];
    int rssi;
    wifi_info(ip, sizeof(ip), &rssi);

    fill_rect(0, 0, LCD_W, 28, COL_HEADER);
    draw_text(6, 6, 1, COL_TEXT, "NETWORK / FW");

    int y = 40;
    snprintf(line, sizeof(line), "ip   %s", ip);
    draw_text(6, y, 1, COL_TEXT, line); y += 22;
    snprintf(line, sizeof(line), "rssi %d dBm", rssi);
    draw_text(6, y, 1, COL_TEXT, line); y += 22;
    snprintf(line, sizeof(line), "srv  %s:%u",
             st->server_host != NULL ? st->server_host : "?", st->server_port);
    draw_text(6, y, 1, COL_TEXT, line); y += 22;
    snprintf(line, sizeof(line), "hb   %lu", (unsigned long)st->heartbeats);
    draw_text(6, y, 1, COL_TEXT, line); y += 22;
    snprintf(line, sizeof(line), "frm  %lu", (unsigned long)st->frames_total);
    draw_text(6, y, 1, COL_TEXT, line); y += 22;

    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *part = esp_ota_get_running_partition();
    snprintf(line, sizeof(line), "fw   %s @%s", app->version,
             part != NULL ? part->label : "?");
    draw_text(6, y, 1, COL_TEXT, line); y += 22;
    snprintf(line, sizeof(line), "up   %lld min", esp_timer_get_time() / 60000000LL);
    draw_text(6, y, 1, COL_TEXT, line);
}

esp_err_t display_init(void)
{
    gpio_config_t bl_cfg = {
        .pin_bit_mask = 1ULL << LCD_PIN_BL,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&bl_cfg);
    gpio_set_level(LCD_PIN_BL, 0); /* active low: on */

    gpio_config_t btn_cfg = {
        .pin_bit_mask = 1ULL << BTN_PAGE_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&btn_cfg);

    spi_bus_config_t bus_cfg = {
        .sclk_io_num = LCD_PIN_SCLK,
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_W * STRIPE_H * 2 + 16,
    };
    esp_err_t err = spi_bus_initialize(LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(err));
        return err;
    }

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = LCD_PIN_DC,
        .cs_gpio_num = LCD_PIN_CS,
        .pclk_hz = LCD_PIXEL_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 1,
        .on_color_trans_done = flush_done_cb,
    };
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST,
                                   &io_cfg, &io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel_io_spi: %s", esp_err_to_name(err));
        return err;
    }

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    err = esp_lcd_new_panel_st7789(io, &panel_cfg, &s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "new_panel_st7789: %s", esp_err_to_name(err));
        return err;
    }
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_invert_color(s_panel, true);
    esp_lcd_panel_swap_xy(s_panel, false);
    esp_lcd_panel_mirror(s_panel, false, false);
    esp_lcd_panel_disp_on_off(s_panel, true);

    /* PSRAM only: falling back to 113KB of internal RAM would starve the
     * QUIC/TLS stack (see SPIRAM_MALLOC_ALWAYSINTERNAL in sdkconfig.defaults). */
    s_fb = heap_caps_calloc(LCD_W * LCD_H, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    s_stripe = heap_caps_malloc(LCD_W * STRIPE_H * 2, MALLOC_CAP_DMA);
    s_flush_done = xSemaphoreCreateBinary();
    if (s_fb == NULL || s_stripe == NULL || s_flush_done == NULL) {
        ESP_LOGE(TAG, "framebuffer alloc failed");
        s_panel = NULL;
        return ESP_ERR_NO_MEM;
    }

    fill_rect(0, 0, LCD_W, LCD_H, COL_BG);
    draw_text_centered(96, 2, COL_TEXT, "AIoT");
    draw_text_centered(132, 1, COL_DIM, "presence sensor");
    flush();
    ESP_LOGI(TAG, "ST7789 up (240x240)");
    return ESP_OK;
}

void display_update(const display_status_t *st)
{
    if (s_panel == NULL || s_fb == NULL || st == NULL) {
        return;
    }

    /* BOOT button: debounced press (3 consecutive low reads) cycles pages. */
    if (gpio_get_level(BTN_PAGE_GPIO) == 0) {
        if (++s_btn_low_streak >= 3 && !s_btn_latched) {
            s_btn_latched = true;
            s_page = (s_page + 1) % N_PAGES;
            s_dirty = true;
            printf("[DISP] button -> page %d\n", s_page);
            if (s_page == N_PAGES - 1) {
                backlight_set(false);
            } else if (!s_backlight_on) {
                backlight_set(true);
            }
        }
    } else {
        s_btn_low_streak = 0;
        s_btn_latched = false;
    }

    if (s_page == N_PAGES - 1) {
        return; /* screen off */
    }

    int64_t now = esp_timer_get_time();
    if (!s_dirty && (now - s_last_render_us) < RENDER_PERIOD_US) {
        return;
    }
    s_last_render_us = now;
    s_dirty = false;

    if ((s_render_count++ % 20) == 0) {
        printf("[DISP] alive page=%d bl=%d btn=%d\n",
               s_page, (int)s_backlight_on, gpio_get_level(BTN_PAGE_GPIO));
    }

    fill_rect(0, 0, LCD_W, LCD_H, COL_BG);
    if (s_page == 0) {
        render_page_main(st);
    } else {
        render_page_net(st);
    }
    flush();
}
