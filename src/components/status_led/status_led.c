#include "include/status_led.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "esp_log.h"

static const char *TAG = "status_led";

/* WS2812 timing at 0.1us/tick (10 MHz): T0H=0.3us T0L=0.9us, T1H=0.9us T0L=0.3us. */
#define WS2812_RES_HZ 10000000U

static rmt_channel_handle_t s_chan;
static rmt_encoder_handle_t s_encoder;
static volatile status_led_state_t s_state = STATUS_LED_BOOT;
static int s_gpio = -1;

static void ws2812_write(uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t grb[3] = { g, r, b };   /* WS2812 wire order is GRB */
    rmt_transmit_config_t tx = { .loop_count = 0 };
    if (rmt_transmit(s_chan, s_encoder, grb, sizeof(grb), &tx) == ESP_OK) {
        rmt_tx_wait_all_done(s_chan, 50);
    }
}

/* triangle wave 0->255->0 over `period_ms`, for smooth breathing */
static uint8_t tri(uint32_t t_ms, uint32_t period_ms)
{
    uint32_t x = t_ms % period_ms;
    uint32_t half = period_ms / 2U;
    return (x < half) ? (uint8_t)(x * 255U / half)
                      : (uint8_t)(255U - (x - half) * 255U / half);
}

/* square wave: bright for the first half of the period */
static uint8_t blink(uint32_t t_ms, uint32_t period_ms)
{
    return ((t_ms % period_ms) < (period_ms / 2U)) ? 255U : 0U;
}

static uint8_t scale(uint8_t v, uint8_t w) { return (uint8_t)(((uint32_t)v * w) / 255U); }

static void status_led_task(void *arg)
{
    (void)arg;
    uint32_t t = 0U;
    for (;;) {
        uint8_t r = 0, g = 0, b = 0;
        switch (s_state) {
        case STATUS_LED_BOOT:
            r = g = b = 40U;                              /* dim white, solid */
            break;
        case STATUS_LED_SETUP: {
            uint8_t w = 12U + scale(60U, tri(t, 2200U));  /* blue breathing */
            b = w;
            break;
        }
        case STATUS_LED_CONNECTING:
            if (blink(t, 500U)) { r = 70U; g = 26U; }     /* amber blink */
            break;
        case STATUS_LED_ONLINE:
            g = 22U;                                       /* dim green, vacant */
            break;
        case STATUS_LED_PRESENT:
            g = 24U + scale(120U, tri(t, 1100U));          /* bright green pulse */
            break;
        case STATUS_LED_ERROR:
            if (blink(t, 320U)) { r = 80U; }               /* red blink */
            break;
        case STATUS_LED_OFF:
        default:
            break;
        }
        ws2812_write(r, g, b);
        vTaskDelay(pdMS_TO_TICKS(40));
        t += 40U;
    }
}

void status_led_init(int gpio)
{
    if (gpio < 0) {
        return;
    }
    s_gpio = gpio;

    rmt_tx_channel_config_t ch = {
        .gpio_num = gpio,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = WS2812_RES_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
    };
    if (rmt_new_tx_channel(&ch, &s_chan) != ESP_OK) {
        ESP_LOGW(TAG, "rmt channel init failed on gpio %d; LED disabled", gpio);
        return;
    }

    rmt_bytes_encoder_config_t enc = {
        .bit0 = { .level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9 },
        .bit1 = { .level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 3 },
        .flags = { .msb_first = 1 },
    };
    if (rmt_new_bytes_encoder(&enc, &s_encoder) != ESP_OK) {
        ESP_LOGW(TAG, "rmt encoder init failed; LED disabled");
        return;
    }
    rmt_enable(s_chan);
    xTaskCreate(status_led_task, "status_led", 2560, NULL, 2, NULL);
    ESP_LOGI(TAG, "status LED on gpio %d", gpio);
}

void status_led_set(status_led_state_t state)
{
    s_state = state;
}
