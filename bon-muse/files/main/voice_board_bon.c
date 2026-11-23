// Robot Bon: mic INMP441 (I2S0, WS 4, SCK 5, SD 6, L/R -> GND) and
// amp MAX98357A (I2S1, BCLK 15, LRC 16, DIN 7). The ESP32-S3 is the I2S
// master for both. No codec: volume is applied in software.
#include "voice_board.h"

#include <stdatomic.h>

#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#ifndef CONFIG_HOMEHUB_VOICE_BOARD_BON
#error "voice_board_bon.c requires CONFIG_HOMEHUB_VOICE_BOARD_BON"
#endif

#define BON_MIC_WS    4
#define BON_MIC_SCK   5
#define BON_MIC_SD    6
#define BON_SPK_BCLK  15
#define BON_SPK_LRC   16
#define BON_SPK_DOUT  7
#define BON_SPK_RATE  16000            // voice_player sends 48 kHz; we take every 3rd
#define MIC_CHUNK     320
#define SPK_CHUNK     64

static const char *TAG = "bon.voice";
static i2s_chan_handle_t s_rx;
static i2s_chan_handle_t s_tx;
static int32_t s_raw[MIC_CHUNK * 2];
static bool s_mic_on;
static bool s_ready;
static atomic_int s_gain = 60;         // 0..100

void voice_board_amp(bool on) { (void)on; }

void voice_board_set_volume(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    atomic_store(&s_gain, percent);
}

bool voice_board_muted(void) { return !s_ready; }
bool voice_board_echo_cancelled(void) { return false; }
int voice_board_dial_steps(void) { return 0; }

esp_err_t voice_board_mic_start(void) {
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (s_mic_on) return ESP_OK;
    esp_err_t err = i2s_channel_enable(s_rx);
    if (err == ESP_OK) s_mic_on = true;
    return err;
}

void voice_board_mic_stop(void) {
    if (!s_mic_on) return;
    i2s_channel_disable(s_rx);
    s_mic_on = false;
}

size_t voice_board_mic_read(int16_t *pcm, size_t frames, int *peak) {
    *peak = 0;
    if (!s_ready || !s_mic_on || frames == 0) return 0;
    if (frames > MIC_CHUNK) frames = MIC_CHUNK;
    size_t bytes = 0;
    if (i2s_channel_read(s_rx, s_raw, frames * 2 * sizeof(int32_t), &bytes, 500) != ESP_OK) {
        return 0;
    }
    size_t count = bytes / (2 * sizeof(int32_t));
    for (size_t i = 0; i < count; i++) {
        // INMP441 o kenh trai (L/R noi GND), 24 bit nam o phan cao cua 32 bit
        int32_t v = s_raw[2 * i] >> 12;
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        pcm[i] = (int16_t)v;
        int mag = v < 0 ? -v : v;
        if (mag > *peak) *peak = mag;
    }
    return count;
}

// frames: 48 kHz stereo 32-bit (count = so frame). Lay trung binh 3 frame -> 16 kHz.
esp_err_t voice_board_speaker_write(const int32_t *frames, size_t count) {
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (count % 3) return ESP_ERR_INVALID_SIZE;
    int32_t out[SPK_CHUNK * 2];
    int gain = atomic_load(&s_gain);
    // Am luong theo duong cong binh phuong cho de nghe
    int64_t scale = (int64_t)gain * gain;   // 0..10000
    while (count) {
        size_t n = count / 3;
        if (n > SPK_CHUNK) n = SPK_CHUNK;
        for (size_t i = 0; i < n; i++) {
            for (int ch = 0; ch < 2; ch++) {
                int64_t sum = (int64_t)frames[6 * i + ch] + frames[6 * i + 2 + ch] +
                              frames[6 * i + 4 + ch];
                out[2 * i + ch] = (int32_t)(sum / 3 * scale / 10000);
            }
        }
        size_t want = n * 2 * sizeof(int32_t), written = 0;
        esp_err_t err = i2s_channel_write(s_tx, out, want, &written, 1000);
        if (err != ESP_OK) return err;
        if (written != want) return ESP_ERR_TIMEOUT;
        frames += 6 * n;
        count -= 3 * n;
    }
    return ESP_OK;
}

esp_err_t voice_board_init(void) {
    s_ready = false;
    // Loa: I2S1, chu dong phat xung
    i2s_chan_config_t tx_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    tx_cfg.auto_clear = true;
    esp_err_t err = i2s_new_channel(&tx_cfg, &s_tx, NULL);
    if (err != ESP_OK) return err;
    i2s_std_config_t tx_std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BON_SPK_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = BON_SPK_BCLK,
            .ws = BON_SPK_LRC,
            .dout = BON_SPK_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
        },
    };
    err = i2s_channel_init_std_mode(s_tx, &tx_std);
    if (err == ESP_OK) err = i2s_channel_enable(s_tx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "speaker: %s", esp_err_to_name(err));
        return err;
    }

    // Mic: I2S0
    i2s_chan_config_t rx_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    err = i2s_new_channel(&rx_cfg, NULL, &s_rx);
    if (err != ESP_OK) return err;
    i2s_std_config_t rx_std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(VOICE_MIC_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = BON_MIC_SCK,
            .ws = BON_MIC_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = BON_MIC_SD,
            .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
        },
    };
    err = i2s_channel_init_std_mode(s_rx, &rx_std);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mic: %s", esp_err_to_name(err));
        return err;
    }
    s_ready = true;
    ESP_LOGI(TAG, "mic INMP441 + amp MAX98357A ready");
    return ESP_OK;
}
