#pragma once

/*
 * Onboard status LED (WS2812 / addressable RGB) so a screenless device tells
 * the installer what it is doing at a glance. Driven by a small background
 * task; call status_led_set() to change what it shows.
 */

typedef enum {
    STATUS_LED_OFF = 0,
    STATUS_LED_BOOT,        /* white, solid — powering up */
    STATUS_LED_SETUP,       /* blue, breathing — SoftAP setup portal is open */
    STATUS_LED_CONNECTING,  /* amber, blinking — joining Wi-Fi / server */
    STATUS_LED_ONLINE,      /* green, dim solid — connected, room vacant */
    STATUS_LED_PRESENT,     /* green, bright pulse — someone is present */
    STATUS_LED_ERROR,       /* red, blinking — Wi-Fi/credentials failed */
} status_led_state_t;

/* Start the LED on the given GPIO (WS2812). gpio < 0 disables (no-op). */
void status_led_init(int gpio);

/* Change what the LED shows. Safe to call from any task. */
void status_led_set(status_led_state_t state);
