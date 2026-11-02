typedef void (*wifi_tx_done_cb_t)(uint8_t,uint8_t*,uint16_t*,bool);
static wifi_tx_done_cb_t registered;
#define ESP_OK 0
static int esp_wifi_set_tx_done_cb(wifi_tx_done_cb_t cb){registered=cb;return ESP_OK;}
