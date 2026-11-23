// Robot Bon: Google Cloud Text-to-Speech cho cau tra loi cua Muse.
#include "bon_tts.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"

static const char *TAG = "bon.tts";

#define TTS_URL "https://texttospeech.googleapis.com/v1/text:synthesize?key="
#define TEXT_CAP 2000          // byte; Google cho toi da 5000
#define RESP_MAX (900 * 1024)  // JSON tra ve (MP3 base64)

static char s_key[64];
static char s_voice[40] = "vi-VN-Neural2-A";
static TaskHandle_t s_task;
static SemaphoreHandle_t s_lock;

// Yeu cau / ket qua (giu s_lock)
static char *s_text;           // PSRAM, TEXT_CAP
static uint32_t s_req_gen;
static uint8_t *s_mp3;         // PSRAM, ket qua da giai base64
static size_t s_mp3_len, s_mp3_off;
static uint32_t s_done_gen;
static int s_state;            // 0 dang tai, 1 xong, -1 loi

bool bon_tts_enabled(void) { return s_task != NULL && s_key[0]; }

// Bo emoji (ky tu UTF-8 4 byte) va dau markdown de giong doc khong vap
static void clean_text(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 1 < cap;) {
        if ((*p & 0xF8) == 0xF0) { p += 4; continue; }
        if (*p == '*' || *p == '#' || *p == '`' || *p == '_' || *p == '~') { p++; continue; }
        out[o++] = (char)*p++;
    }
    out[o] = '\0';
}

bool bon_tts_start(const char *text) {
    if (!bon_tts_enabled() || !text || !*text) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    clean_text(text, s_text, TEXT_CAP);
    s_req_gen++;
    s_state = 0;
    s_mp3_len = s_mp3_off = 0;
    xSemaphoreGive(s_lock);
    xTaskNotifyGive(s_task);
    return true;
}

int bon_tts_state(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int st = (s_done_gen == s_req_gen) ? s_state : 0;
    xSemaphoreGive(s_lock);
    return st;
}

size_t bon_tts_remaining(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t n = (s_done_gen == s_req_gen && s_state == 1) ? s_mp3_len - s_mp3_off : 0;
    xSemaphoreGive(s_lock);
    return n;
}

size_t bon_tts_take(uint8_t *out, size_t cap) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t n = 0;
    if (s_done_gen == s_req_gen && s_state == 1) {
        n = s_mp3_len - s_mp3_off;
        if (n > cap) n = cap;
        memcpy(out, s_mp3 + s_mp3_off, n);
        s_mp3_off += n;
    }
    xSemaphoreGive(s_lock);
    return n;
}

// Goi API, tra ve so byte MP3 trong *mp3 (PSRAM, nguoi goi free), 0 neu loi.
static size_t synthesize(const char *text, uint8_t **mp3_out) {
    *mp3_out = NULL;
    cJSON *root = cJSON_CreateObject();
    cJSON *input = cJSON_AddObjectToObject(root, "input");
    cJSON_AddStringToObject(input, "text", text);
    cJSON *voice = cJSON_AddObjectToObject(root, "voice");
    cJSON_AddStringToObject(voice, "languageCode", "vi-VN");
    cJSON_AddStringToObject(voice, "name", s_voice);
    cJSON *audio = cJSON_AddObjectToObject(root, "audioConfig");
    cJSON_AddStringToObject(audio, "audioEncoding", "MP3");
    cJSON_AddNumberToObject(audio, "sampleRateHertz", 16000);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return 0;

    char url[sizeof(TTS_URL) + sizeof(s_key)];
    snprintf(url, sizeof(url), "%s%s", TTS_URL, s_key);
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    size_t out_len = 0, got = 0;
    char *resp = NULL;
    cJSON *j = NULL;
    int status = 0;
    if (!c) goto done;
    esp_http_client_set_header(c, "Content-Type", "application/json; charset=utf-8");
    if (esp_http_client_open(c, strlen(body)) != ESP_OK) {
        ESP_LOGW(TAG, "can't connect to Google TTS");
        goto done;
    }
    if (esp_http_client_write(c, body, strlen(body)) < 0) goto done;
    esp_http_client_fetch_headers(c);
    status = esp_http_client_get_status_code(c);
    resp = heap_caps_malloc(RESP_MAX + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!resp) goto done;
    while (got < RESP_MAX) {
        int n = esp_http_client_read(c, resp + got, RESP_MAX - got);
        if (n <= 0) break;
        got += n;
    }
    resp[got] = '\0';
    if (status != 200) {
        ESP_LOGW(TAG, "Google TTS HTTP %d: %.200s", status, resp);
        goto done;
    }
    j = cJSON_Parse(resp);
    cJSON *ac = j ? cJSON_GetObjectItem(j, "audioContent") : NULL;
    if (cJSON_IsString(ac) && ac->valuestring) {
        size_t b64 = strlen(ac->valuestring);
        size_t cap = b64 * 3 / 4 + 4;
        uint8_t *mp3 = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        size_t olen = 0;
        if (mp3 && mbedtls_base64_decode(mp3, cap, &olen, (const unsigned char *)ac->valuestring,
                                         b64) == 0) {
            *mp3_out = mp3;
            out_len = olen;
        } else {
            free(mp3);
        }
    } else {
        ESP_LOGW(TAG, "no audioContent in reply");
    }
    cJSON_Delete(j);
done:
    if (c) {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
    }
    free(resp);
    cJSON_free(body);
    return out_len;
}

static void tts_task(void *arg) {
    char *text = heap_caps_malloc(TEXT_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        uint32_t gen = s_req_gen;
        strlcpy(text, s_text, TEXT_CAP);
        xSemaphoreGive(s_lock);

        int64_t t0 = esp_timer_get_time();
        uint8_t *mp3 = NULL;
        size_t len = synthesize(text, &mp3);
        ESP_LOGI(TAG, "%u chars -> %u bytes MP3 in %d ms", (unsigned)strlen(text),
                 (unsigned)len, (int)((esp_timer_get_time() - t0) / 1000));

        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (gen == s_req_gen) {
            free(s_mp3);
            s_mp3 = mp3;
            s_mp3_len = len;
            s_mp3_off = 0;
            s_state = len ? 1 : -1;
            s_done_gen = gen;
            mp3 = NULL;
        }
        xSemaphoreGive(s_lock);
        free(mp3);   // cau cu bi bo qua
    }
}

void bon_tts_set_key(const char *api_key, const char *voice) {
    if (!api_key || strlen(api_key) < 20 || strlen(api_key) >= sizeof(s_key)) {
        ESP_LOGW(TAG, "no Google TTS key: replies stay as text (go >tts_key=AIza... vao Serial Monitor)");
        return;
    }
    strlcpy(s_key, api_key, sizeof(s_key));
    if (voice && *voice) strlcpy(s_voice, voice, sizeof(s_voice));
    s_lock = xSemaphoreCreateMutex();
    s_text = heap_caps_calloc(1, TEXT_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    // Stack trong RAM noi: TLS can, va task nay khong dung PSRAM stack
    if (!s_lock || !s_text || xTaskCreate(tts_task, "bon_tts", 8192, NULL, 4, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "can't start Google TTS");
        s_task = NULL;
        return;
    }
    ESP_LOGI(TAG, "Google TTS on, voice %s", s_voice);
}
