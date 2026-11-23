// ===== Robot Bon tren Muse =====
// - 2 dong co N20 qua mach TC1508A (PWM 20 kHz, khoi dong mem), IN1-IN4 = GPIO10-13
// - Cam bien ToF VL6180X (TOF050C) goc 45 do duoi mui: chong roi mep ban, dung khi co vat can
// - Man OLED SSD1306 128x64: doi mat biet chop, doi bieu cam, hien chu
// - Cam bien cham TTP223 tren GPIO17: cham vao -> Bon dung lai va cuoi
// OLED va ToF dung chung I2C (SDA 41, SCL 42). Moi viec voi phan cung deu
// chay trong mot task rieng; cac lenh tu Muse chi dat yeu cau roi tra loi ngay.
#include "bon_robot.h"

#include <ctype.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <algorithm>
#include <string>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
extern "C" {
#include "pixel_font.h"
#include "config_store.h"
#if CONFIG_HOMEHUB_VOICE
#include "button.h"
// voice_board_bon.c: biet anh con dang noi hay da im
void bon_mic_vad_reset(void);
bool bon_mic_vad(int *quiet_ms);
#endif
}
#if CONFIG_HOMEHUB_VOICE
#include "bon_tts.h"
#endif

static const char *TAG = "bon";

// ---------------- So do chan ----------------
#define BON_I2C_SDA        GPIO_NUM_41
#define BON_I2C_SCL        GPIO_NUM_42
#define BON_TOUCH_GPIO     GPIO_NUM_17   // TTP223: cham -> muc CAO
#define BON_MOTOR_L_IN1    GPIO_NUM_10   // banh trai
#define BON_MOTOR_L_IN2    GPIO_NUM_11
#define BON_MOTOR_R_IN1    GPIO_NUM_12   // banh phai
#define BON_MOTOR_R_IN2    GPIO_NUM_13
#define BON_MOTOR_L_REVERSED false
#define BON_MOTOR_R_REVERSED true        // anh Tuan test 16/11: banh phai quay nguoc

#define BON_DEFAULT_SPEED  60
#define BON_MIN_SPEED      30
#define BON_TURN_SPEED     85
#define BON_TURN_MS_360    2950          // chay pin, toc do 85%: 1 vong ~2,95 giay
#define BON_MAX_BACK_MS    2000          // phia sau khong co cam bien -> lui ngan thoi
#define BON_BACKOFF_MS     350

#define TOF_DEFAULT_FLOOR  23
#define TOF_EDGE_MIN       15
#define TOF_OBSTACLE_MIN   5
#define TOF_MAX_FLOOR      150

#define OLED_ADDR 0x3C
#define TOF_ADDR  0x29
#define OLED_W 128
#define OLED_H 64

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

// ---------------- Trang thai chung ----------------
enum Dir { DIR_STOP, DIR_FORWARD, DIR_BACKWARD, DIR_LEFT, DIR_RIGHT };

enum Face {
    FACE_NEUTRAL, FACE_HAPPY, FACE_SAD, FACE_ANGRY, FACE_SURPRISED,
    FACE_SLEEPY, FACE_LOVE, FACE_WINK, FACE_CONFUSED, FACE_COUNT
};
static const char *const kFaceNames[FACE_COUNT] = {
    "neutral", "happy", "sad", "angry", "surprised",
    "sleepy", "love", "wink", "confused",
};

struct Step { Dir dir; int speed; int ms; };
#define MAX_STEPS 12

static SemaphoreHandle_t s_mutex;       // bao ve cac bien "yeu cau" ben duoi
static TaskHandle_t s_task;
static bool s_started;

// Yeu cau tu lenh Muse (doc/ghi khi giu s_mutex)
static Step s_req_plan[MAX_STEPS];
static int s_req_plan_len;
static bool s_req_plan_changed;
static bool s_req_calibrate;
static Face s_req_face = FACE_NEUTRAL;
static int64_t s_req_face_until;        // 0 = giu mai
static bool s_req_face_changed;
static char s_req_text[200];
static int64_t s_req_text_until;
static bool s_req_text_changed;

// Trang thai task bao lai (doc khi giu s_mutex)
static bool s_moving;
static bool s_tof_ok;
static bool s_floor_found;
static int s_tof_mm = -1;               // -1 = khong thay gi, -2 = loi
static int s_floor_mm = TOF_DEFAULT_FLOOR;
static const char *s_last_event = "none";
static bool s_oled_ok;
static int s_touch_count;
static bool s_listening;            // chi task chinh dung
static int64_t s_listen_start;      // luc bat dau nghe (ms)
#define BON_LISTEN_HOLD_MS   1500   // giu tay lau hon -> tha tay la gui (kieu cu)
#define BON_LISTEN_DONE_MS   1300   // noi xong, im lang bay lau -> tu gui
#define BON_LISTEN_NOVOICE_MS 7000  // khong nghe tieng nao -> thoi nghe
#define BON_LISTEN_MAX_MS   15000   // toi da 15 giay (gioi han cua SDK)
static int64_t s_last_touch_ms;
static SemaphoreHandle_t s_calibrate_done;

// ---------------- I2C ----------------
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_oled;
static i2c_master_dev_handle_t s_tof;

static bool i2c_add(uint8_t addr, i2c_master_dev_handle_t *out) {
    i2c_device_config_t cfg = {};
    cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    cfg.device_address = addr;
    cfg.scl_speed_hz = 400 * 1000;
    return i2c_master_bus_add_device(s_bus, &cfg, out) == ESP_OK;
}

// ---------------- VL6180X ----------------
static bool tof_write8(uint16_t reg, uint8_t val) {
    uint8_t buf[3] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF), val};
    return i2c_master_transmit(s_tof, buf, 3, 50) == ESP_OK;
}

static bool tof_read8(uint16_t reg, uint8_t *val) {
    uint8_t r[2] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF)};
    return i2c_master_transmit_receive(s_tof, r, 2, val, 1, 50) == ESP_OK;
}

static bool tof_wait(uint16_t reg, uint8_t mask, uint8_t want) {
    int64_t deadline = esp_timer_get_time() + 60000;
    while (true) {
        uint8_t v = 0;
        if (!tof_read8(reg, &v)) return false;
        if ((v & mask) == want) return true;
        if (esp_timer_get_time() > deadline) return false;
        vTaskDelay(1);
    }
}

static bool tof_init(void) {
    if (!i2c_add(TOF_ADDR, &s_tof)) return false;
    uint8_t id = 0;
    if (!tof_read8(0x000, &id) || id != 0xB4) {
        ESP_LOGW(TAG, "VL6180X model id 0x%02X (expected 0xB4)", id);
        return false;
    }
    static const uint16_t kSettings[][2] = {
        {0x0207, 0x01}, {0x0208, 0x01}, {0x0096, 0x00}, {0x0097, 0xfd},
        {0x00e3, 0x00}, {0x00e4, 0x04}, {0x00e5, 0x02}, {0x00e6, 0x01},
        {0x00e7, 0x03}, {0x00f5, 0x02}, {0x00d9, 0x05}, {0x00db, 0xce},
        {0x00dc, 0x03}, {0x00dd, 0xf8}, {0x009f, 0x00}, {0x00a3, 0x3c},
        {0x00b7, 0x00}, {0x00bb, 0x3c}, {0x00b2, 0x09}, {0x00ca, 0x09},
        {0x0198, 0x01}, {0x01b0, 0x17}, {0x01ad, 0x00}, {0x00ff, 0x05},
        {0x0100, 0x05}, {0x0199, 0x05}, {0x01a6, 0x1b}, {0x01ac, 0x3e},
        {0x01a7, 0x1f}, {0x0030, 0x00},
        {0x0011, 0x10}, {0x010a, 0x30}, {0x003f, 0x46}, {0x0031, 0xFF},
        {0x0041, 0x63}, {0x002e, 0x01}, {0x001b, 0x09}, {0x003e, 0x31},
        {0x0014, 0x24}, {0x0016, 0x00},
    };
    for (size_t i = 0; i < sizeof(kSettings) / sizeof(kSettings[0]); i++) {
        if (!tof_write8(kSettings[i][0], (uint8_t)kSettings[i][1])) return false;
    }
    return true;
}

// mm >= 0: khoang cach; -1: khong thay vat; -2: loi
static int tof_measure(void) {
    if (!s_tof_ok) return -2;
    uint8_t v = 0, status = 0;
    if (!tof_wait(0x04D, 0x01, 0x01)) return -2;
    if (!tof_write8(0x018, 0x01)) return -2;
    if (!tof_wait(0x04F, 0x04, 0x04)) return -2;
    if (!tof_read8(0x062, &v)) return -2;
    if (!tof_read8(0x04D, &status)) return -2;
    tof_write8(0x015, 0x07);
    if ((status >> 4) != 0) return -1;
    return v;
}

// ---------------- Dong co ----------------
static const int kMotorPins[4] = {BON_MOTOR_L_IN1, BON_MOTOR_L_IN2, BON_MOTOR_R_IN1,
                                  BON_MOTOR_R_IN2};
// Timer 0 va kenh 0-2 de danh cho den trang thai cua SDK
static const ledc_channel_t kMotorCh[4] = {LEDC_CHANNEL_4, LEDC_CHANNEL_5, LEDC_CHANNEL_6,
                                           LEDC_CHANNEL_7};
#define MOTOR_TIMER LEDC_TIMER_2
#define MOTOR_MODE  LEDC_LOW_SPEED_MODE

static void motors_init(void) {
    ledc_timer_config_t t = {};
    t.speed_mode = MOTOR_MODE;
    t.duty_resolution = LEDC_TIMER_8_BIT;
    t.timer_num = MOTOR_TIMER;
    t.freq_hz = 20000;
    t.clk_cfg = LEDC_AUTO_CLK;
    ESP_ERROR_CHECK(ledc_timer_config(&t));
    for (int i = 0; i < 4; i++) {
        ledc_channel_config_t c = {};
        c.gpio_num = kMotorPins[i];
        c.speed_mode = MOTOR_MODE;
        c.channel = kMotorCh[i];
        c.timer_sel = MOTOR_TIMER;
        c.duty = 0;
        c.hpoint = 0;
        ESP_ERROR_CHECK(ledc_channel_config(&c));
    }
}

static void set_wheel(int a, int b, bool reversed, int speed) {
    if (reversed) speed = -speed;
    int duty = std::min(100, abs(speed)) * 255 / 100;
    ledc_set_duty(MOTOR_MODE, kMotorCh[a], speed > 0 ? duty : 0);
    ledc_update_duty(MOTOR_MODE, kMotorCh[a]);
    ledc_set_duty(MOTOR_MODE, kMotorCh[b], speed < 0 ? duty : 0);
    ledc_update_duty(MOTOR_MODE, kMotorCh[b]);
}

static void drive(Dir dir, int speed) {
    int l = 0, r = 0;
    switch (dir) {
        case DIR_FORWARD:  l = speed;  r = speed;  break;
        case DIR_BACKWARD: l = -speed; r = -speed; break;
        case DIR_LEFT:     l = -speed; r = speed;  break;
        case DIR_RIGHT:    l = speed;  r = -speed; break;
        default: break;
    }
    set_wheel(0, 1, BON_MOTOR_L_REVERSED, l);
    set_wheel(2, 3, BON_MOTOR_R_REVERSED, r);
}

// ---------------- OLED ----------------
static uint8_t s_fb[1 + OLED_W * OLED_H / 8];   // byte dau = 0x40 (du lieu)
#define FB (s_fb + 1)

static bool oled_cmds(const uint8_t *cmds, size_t n) {
    uint8_t buf[32];
    buf[0] = 0x00;
    memcpy(buf + 1, cmds, n);
    return i2c_master_transmit(s_oled, buf, n + 1, 50) == ESP_OK;
}

static bool oled_init(void) {
    if (!i2c_add(OLED_ADDR, &s_oled)) return false;
    // A1 + C8: lat ngang + doc, giong DISPLAY_MIRROR_X/Y = true ben xiaozhi
    static const uint8_t init[] = {
        0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40, 0x8D, 0x14, 0x20, 0x00,
        0xA1, 0xC8, 0xDA, 0x12, 0x81, 0xCF, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6, 0xAF,
    };
    return oled_cmds(init, sizeof(init));
}

static void oled_flush(void) {
    if (!s_oled_ok) return;
    static const uint8_t win[] = {0x21, 0x00, 0x7F, 0x22, 0x00, 0x07};
    if (!oled_cmds(win, sizeof(win))) return;
    s_fb[0] = 0x40;
    i2c_master_transmit(s_oled, s_fb, sizeof(s_fb), 100);
}

static void px(int x, int y, bool on) {
    if (x < 0 || y < 0 || x >= OLED_W || y >= OLED_H) return;
    uint8_t bit = 1 << (y & 7);
    if (on) FB[x + (y / 8) * OLED_W] |= bit;
    else FB[x + (y / 8) * OLED_W] &= ~bit;
}

static void fill_rect(int x, int y, int w, int h, bool on) {
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) px(i, j, on);
}

static void fill_circle(int cx, int cy, int r, bool on) {
    for (int j = -r; j <= r; j++)
        for (int i = -r; i <= r; i++)
            if (i * i + j * j <= r * r) px(cx + i, cy + j, on);
}

static void fill_round_rect(int x, int y, int w, int h, int r, bool on) {
    r = std::min(r, std::min(w, h) / 2);
    fill_rect(x + r, y, w - 2 * r, h, on);
    fill_rect(x, y + r, w, h - 2 * r, on);
    fill_circle(x + r, y + r, r, on);
    fill_circle(x + w - r - 1, y + r, r, on);
    fill_circle(x + r, y + h - r - 1, r, on);
    fill_circle(x + w - r - 1, y + h - r - 1, r, on);
}

static int edge_side(int ax, int ay, int bx, int by, int x, int y) {
    return (bx - ax) * (y - ay) - (by - ay) * (x - ax);
}

static void fill_triangle(int x0, int y0, int x1, int y1, int x2, int y2, bool on) {
    int minx = std::min(x0, std::min(x1, x2)), maxx = std::max(x0, std::max(x1, x2));
    int miny = std::min(y0, std::min(y1, y2)), maxy = std::max(y0, std::max(y1, y2));
    for (int y = miny; y <= maxy; y++) {
        for (int x = minx; x <= maxx; x++) {
            int a = edge_side(x0, y0, x1, y1, x, y);
            int b = edge_side(x1, y1, x2, y2, x, y);
            int c = edge_side(x2, y2, x0, y0, x, y);
            if ((a >= 0 && b >= 0 && c >= 0) || (a <= 0 && b <= 0 && c <= 0)) px(x, y, on);
        }
    }
}

// Ve mot mat. left = mat ben trai man hinh.
static void draw_eye(int cx, int cy, Face face, bool left, bool blink) {
    const int w = 30, h = 36;
    int x = cx - w / 2, y = cy - h / 2;
    if (blink && face != FACE_SLEEPY && face != FACE_LOVE) {
        fill_round_rect(x, cy - 2, w, 5, 2, true);
        return;
    }
    switch (face) {
        case FACE_HAPPY:   // mat cuoi: hinh luoi liem cong len
            fill_round_rect(x, y, w, h, 12, true);
            fill_circle(cx, cy + h / 2 + 6, 22, false);
            break;
        case FACE_SAD: {   // khoe mat ngoai cup xuong
            fill_round_rect(x, y, w, h, 8, true);
            int outer = left ? x - 1 : x + w;
            int inner = left ? x + w : x - 1;
            fill_triangle(outer, y - 1, inner, y - 1, outer, y + h / 2, false);
            break;
        }
        case FACE_ANGRY: { // khoe mat trong cup xuong
            fill_round_rect(x, y, w, h, 8, true);
            int outer = left ? x - 1 : x + w;
            int inner = left ? x + w : x - 1;
            fill_triangle(outer, y - 1, inner, y - 1, inner, y + h / 2, false);
            break;
        }
        case FACE_SURPRISED:
            fill_circle(cx, cy, 18, true);
            fill_circle(cx, cy, 7, false);
            break;
        case FACE_SLEEPY:
            fill_round_rect(x, cy + 6, w, 5, 2, true);
            break;
        case FACE_LOVE:
            fill_circle(cx - 7, cy - 5, 9, true);
            fill_circle(cx + 7, cy - 5, 9, true);
            fill_triangle(cx - 16, cy - 2, cx + 16, cy - 2, cx, cy + 17, true);
            break;
        case FACE_WINK:
            if (left) fill_round_rect(x, y, w, h, 8, true);
            else {
                fill_round_rect(x, y, w, h, 12, true);
                fill_circle(cx, cy + h / 2 + 6, 22, false);
            }
            break;
        case FACE_CONFUSED:
            if (left) fill_round_rect(x + 4, y + 10, w - 8, h - 16, 6, true);
            else fill_round_rect(x, y - 2, w, h + 2, 8, true);
            break;
        default:
            fill_round_rect(x, y, w, h, 8, true);
            break;
    }
}

static void draw_face(Face face, bool blink, int gx, int gy) {
    memset(FB, 0, OLED_W * OLED_H / 8);
    draw_eye(38 + gx, 32 + gy, face, true, blink);
    draw_eye(90 + gx, 32 + gy, face, false, blink);
}

// Bo dau tieng Viet de man OLED (chi co font ASCII) van doc duoc.
struct VnMap { const char *chars; char base; };
static const VnMap kVnMap[] = {
    {"àáảãạăằắẳẵặâầấẩẫậ", 'a'}, {"ÀÁẢÃẠĂẰẮẲẴẶÂẦẤẨẪẬ", 'A'},
    {"èéẻẽẹêềếểễệ", 'e'}, {"ÈÉẺẼẸÊỀẾỂỄỆ", 'E'},
    {"ìíỉĩị", 'i'}, {"ÌÍỈĨỊ", 'I'},
    {"òóỏõọôồốổỗộơờớởỡợ", 'o'}, {"ÒÓỎÕỌÔỒỐỔỖỘƠỜỚỞỠỢ", 'O'},
    {"ùúủũụưừứửữự", 'u'}, {"ÙÚỦŨỤƯỪỨỬỮỰ", 'U'},
    {"ỳýỷỹỵ", 'y'}, {"ỲÝỶỸỴ", 'Y'}, {"đ", 'd'}, {"Đ", 'D'},
};

static size_t utf8_len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c >> 5) == 0x6) return 2;
    if ((c >> 4) == 0xE) return 3;
    if ((c >> 3) == 0x1E) return 4;
    return 1;
}

static void to_ascii(const char *in, char *out, size_t out_size) {
    size_t o = 0;
    while (*in && o + 1 < out_size) {
        size_t n = utf8_len((unsigned char)*in);
        char ch = '?';
        if (n == 1) {
            ch = (*in >= 0x20 && *in <= 0x7e) ? *in : ' ';
        } else {
            for (const VnMap &m : kVnMap) {
                for (const char *p = m.chars; *p; p += utf8_len((unsigned char)*p)) {
                    if (strncmp(p, in, n) == 0 && utf8_len((unsigned char)*p) == n) {
                        ch = m.base;
                        break;
                    }
                }
                if (ch != '?') break;
            }
        }
        out[o++] = ch;
        for (size_t i = 0; i < n && *in; i++) in++;
    }
    out[o] = '\0';
}

// Chu 5x8, co ky tu x2 neu cau ngan. Tu dong xuong dong theo tu.
static void draw_text(const char *utf8) {
    char text[200];
    to_ascii(utf8, text, sizeof(text));
    memset(FB, 0, OLED_W * OLED_H / 8);
    int len = strlen(text);
    int scale = len <= 30 ? 2 : 1;
    int cw = 6 * scale, ch = 9 * scale;
    int cols = OLED_W / cw, rows = OLED_H / ch;
    int row = 0, col = 0;
    const char *p = text;
    while (*p && row < rows) {
        while (*p == ' ') p++;
        const char *e = p;
        while (*e && *e != ' ') e++;
        int wl = e - p;
        if (col > 0 && col + wl > cols) { row++; col = 0; if (row >= rows) break; }
        for (const char *c = p; c < e; c++) {
            if (col >= cols) { row++; col = 0; if (row >= rows) break; }
            unsigned char g = *c;
            if (g >= PIXEL_FONT_FIRST && g <= PIXEL_FONT_LAST) {
                const uint8_t *glyph = pixel_font[g - PIXEL_FONT_FIRST];
                for (int gx = 0; gx < PIXEL_FONT_WIDTH; gx++)
                    for (int gy = 0; gy < PIXEL_FONT_HEIGHT; gy++)
                        if (glyph[gx] & (1 << gy))
                            fill_rect(col * cw + gx * scale, row * ch + gy * scale, scale, scale,
                                      true);
            }
            col++;
        }
        col++;   // dau cach
        p = e;
    }
}

// ---------------- Chong roi ----------------
static bool is_edge(int mm, int floor) {
    int margin = std::max(TOF_EDGE_MIN, floor * 35 / 100);
    return mm == -1 || mm > floor + margin;
}

static bool is_obstacle(int mm, int floor) {
    int margin = std::max(TOF_OBSTACLE_MIN, floor * 30 / 100);
    return mm >= 0 && mm < floor - margin;
}

static void calibrate_floor(void) {
    int ok[9], n = 0;
    for (int i = 0; i < 9; i++) {
        int mm = tof_measure();
        if (mm >= 8 && mm <= TOF_MAX_FLOOR) ok[n++] = mm;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (n >= 5) {
        for (int i = 1; i < n; i++)          // sap xep chen (it phan tu)
            for (int j = i; j > 0 && ok[j - 1] > ok[j]; j--) std::swap(ok[j - 1], ok[j]);
        s_floor_mm = ok[n / 2];
        s_floor_found = true;
    } else {
        s_floor_mm = TOF_DEFAULT_FLOOR;
        s_floor_found = false;
    }
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "floor %s: %d mm", s_floor_found ? "found" : "NOT found", s_floor_mm);
}

// ---------------- Task chinh ----------------
static void set_event(const char *ev) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_last_event = ev;
    xSemaphoreGive(s_mutex);
}

static void bon_task(void *) {
    Step plan[MAX_STEPS];
    int plan_len = 0, idx = 0;
    int64_t step_end = 0;
    int cur_speed = 0, edge_count = 0, obstacle_count = 0;
    bool active = false;

    Face face = FACE_NEUTRAL;
    int64_t face_until = 0;
    char text[200] = "";
    int64_t text_until = 0;
    int64_t next_blink = now_ms() + 3000, blink_end = 0;
    int64_t next_gaze = now_ms() + 2000;
    int gx = 0, gy = 0;
    bool dirty = true;
    int64_t last_flush = 0;
    bool touch_prev = false;

    if (s_tof_ok) calibrate_floor();

    auto finish = [&](const char *ev, Face f) {
        drive(DIR_STOP, 0);
        active = false;
        plan_len = 0;
        cur_speed = 0;
        if (ev) {
            set_event(ev);
            face = f;
            face_until = now_ms() + 3000;
            text_until = 0;
            dirty = true;
        }
    };

    while (true) {
        int64_t now = now_ms();

        // 1. Doc yeu cau moi tu Muse
        bool do_calibrate = false;
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        if (s_req_plan_changed) {
            s_req_plan_changed = false;
            plan_len = s_req_plan_len;
            if (plan_len > 0) memcpy(plan, s_req_plan, sizeof(Step) * plan_len);
            if (plan_len > 0) s_last_event = "none";
            xSemaphoreGive(s_mutex);
            drive(DIR_STOP, 0);
            cur_speed = 0;
            idx = 0;
            edge_count = obstacle_count = 0;
            active = plan_len > 0;
            if (active) step_end = now + plan[0].ms;
            xSemaphoreTake(s_mutex, portMAX_DELAY);
        }
        if (s_req_face_changed) {
            s_req_face_changed = false;
            face = s_req_face;
            face_until = s_req_face_until;
            text_until = 0;
            dirty = true;
        }
        if (s_req_text_changed) {
            s_req_text_changed = false;
            snprintf(text, sizeof(text), "%s", s_req_text);
            text_until = s_req_text_until;
            dirty = true;
        }
        if (s_req_calibrate) {
            s_req_calibrate = false;
            do_calibrate = true;
        }
        s_moving = active;
        xSemaphoreGive(s_mutex);

        if (do_calibrate) {
            if (s_tof_ok && !active) calibrate_floor();
            xSemaphoreGive(s_calibrate_done);
        }

        // 2. Cam bien cham: cham vao -> dung lai, cuoi
        //    Co giong noi: cham 1 lan = Bon nghe; noi xong Bon tu gui khi anh im
        //    (hoac cham lan nua). Giu tay roi tha cung duoc nhu cu.
        bool touch = gpio_get_level(BON_TOUCH_GPIO) == 1;
        auto stop_listening = [&](const char *why) {
#if CONFIG_HOMEHUB_VOICE
            button_inject_press(false);
#endif
            ESP_LOGI(TAG, "stop listening: %s", why);
            s_listening = false;
            face = FACE_CONFUSED;          // dang nghi...
            face_until = now + 8000;
            dirty = true;
        };
        if (touch && !touch_prev) {
            if (active) finish(nullptr, FACE_NEUTRAL);
            if (s_listening) {
                stop_listening("second tap");
            } else {
                bool listening = false;
#if CONFIG_HOMEHUB_VOICE
                bon_mic_vad_reset();
                listening = button_inject_press(true);
#endif
                face = listening ? FACE_SURPRISED : FACE_HAPPY;   // mat tron = dang nghe
                face_until = listening ? 0 : now + 2500;
                text_until = 0;
                dirty = true;
                s_listening = listening;
                s_listen_start = now;
            }
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_touch_count++;
            s_last_touch_ms = now;
            s_moving = false;
            xSemaphoreGive(s_mutex);
        } else if (!touch && touch_prev && s_listening &&
                   now - s_listen_start >= BON_LISTEN_HOLD_MS) {
            stop_listening("released after hold");
        }
#if CONFIG_HOMEHUB_VOICE
        if (s_listening && !touch && now - s_listen_start > 600) {
            int quiet = 0;
            bool heard = bon_mic_vad(&quiet);
            if (heard && quiet >= BON_LISTEN_DONE_MS) stop_listening("done talking");
            else if (!heard && now - s_listen_start >= BON_LISTEN_NOVOICE_MS) stop_listening("no voice heard");
            else if (now - s_listen_start >= BON_LISTEN_MAX_MS) stop_listening("time limit");
        }
#endif
        touch_prev = touch;

        // 3. Do ToF; khi dang chay thi kiem tra mep ban / vat can
        int mm = tof_measure();
        int floor;
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_tof_mm = mm;
        floor = s_floor_mm;
        xSemaphoreGive(s_mutex);

        if (active) {
            const Step &st = plan[idx];
            bool guarded = st.dir == DIR_FORWARD || st.dir == DIR_LEFT || st.dir == DIR_RIGHT;
            if (guarded && mm == -2) {
                finish("tof_error", FACE_CONFUSED);
            } else if (guarded) {
                edge_count = is_edge(mm, floor) ? edge_count + 1 : 0;
                obstacle_count = (st.dir == DIR_FORWARD && is_obstacle(mm, floor))
                                     ? obstacle_count + 1 : 0;
                if (edge_count >= 2) {
                    ESP_LOGW(TAG, "EDGE (%d mm, floor %d mm)", mm, floor);
                    drive(DIR_STOP, 0);
                    vTaskDelay(pdMS_TO_TICKS(80));
                    if (st.dir == DIR_FORWARD) {
                        drive(DIR_BACKWARD, 50);
                        vTaskDelay(pdMS_TO_TICKS(BON_BACKOFF_MS));
                    }
                    finish("edge", FACE_SURPRISED);
                } else if (obstacle_count >= 2) {
                    ESP_LOGW(TAG, "OBSTACLE (%d mm)", mm);
                    finish("obstacle", FACE_SURPRISED);
                }
            } else {
                edge_count = obstacle_count = 0;
            }
        }
        if (active) {
            const Step &st = plan[idx];
            // Khoi dong mem de khong sut dien ESP32
            if (cur_speed < st.speed) cur_speed = std::min(st.speed, cur_speed + 10);
            if (cur_speed > st.speed) cur_speed = st.speed;
            drive(st.dir, cur_speed);
            if (now >= step_end) {
                idx++;
                if (idx >= plan_len) {
                    finish(nullptr, FACE_NEUTRAL);
                } else {
                    if (plan[idx].dir != st.dir) {
                        drive(DIR_STOP, 0);
                        cur_speed = 0;
                        vTaskDelay(pdMS_TO_TICKS(60));
                    }
                    step_end = now_ms() + plan[idx].ms;
                }
            }
        }

        // 4. Man hinh: chu, hoac mat (chop mat, liec mat)
        if (face_until && now > face_until) { face = FACE_NEUTRAL; face_until = 0; dirty = true; }
        if (text_until && now > text_until) { text_until = 0; dirty = true; }
        if (now >= next_blink) {
            blink_end = now + 140;
            next_blink = now + 2500 + esp_random() % 3500;
            dirty = true;
        }
        if (blink_end && now > blink_end) { blink_end = 0; dirty = true; }
        if (now >= next_gaze) {
            gx = (int)(esp_random() % 9) - 4;
            gy = (int)(esp_random() % 5) - 2;
            if (esp_random() % 3 == 0) gx = gy = 0;
            next_gaze = now + 1500 + esp_random() % 3000;
            dirty = true;
        }
        int min_gap = active ? 250 : 40;   // dang chay: uu tien do ToF
        if (dirty && now - last_flush >= min_gap) {
            if (text_until) draw_text(text);
            else draw_face(face, blink_end != 0, gx, gy);
            oled_flush();
            last_flush = now;
            dirty = false;
        }

        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(active ? 5 : 30));
    }
}

// ---------------- Khoi tao ----------------
extern "C" void bon_robot_init(void) {
    if (s_started) return;
    s_mutex = xSemaphoreCreateMutex();
    s_calibrate_done = xSemaphoreCreateBinary();
    motors_init();
    drive(DIR_STOP, 0);

    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << BON_TOUCH_GPIO;
    io.mode = GPIO_MODE_INPUT;
    io.pull_down_en = GPIO_PULLDOWN_ENABLE;
    gpio_config(&io);

    i2c_master_bus_config_t bus = {};
    bus.i2c_port = -1;   // tu chon cong I2C con trong
    bus.sda_io_num = BON_I2C_SDA;
    bus.scl_io_num = BON_I2C_SCL;
    bus.clk_source = I2C_CLK_SRC_DEFAULT;
    bus.glitch_ignore_cnt = 7;
    bus.flags.enable_internal_pullup = true;
    if (i2c_new_master_bus(&bus, &s_bus) == ESP_OK) {
        s_oled_ok = oled_init();
        s_tof_ok = tof_init();
    } else {
        ESP_LOGE(TAG, "I2C bus init failed");
    }
    ESP_LOGI(TAG, "Bon ready: oled=%d tof=%d", s_oled_ok, s_tof_ok);
    if (s_oled_ok) {
        draw_face(FACE_HAPPY, false, 0, 0);
        oled_flush();
    }
#if CONFIG_HOMEHUB_VOICE
    // Key Google TTS nap qua cong serial (>tts_key=...), luu trong NVS cua Bon
    char key[64] = "", voice[40] = "";
    config_get_str("tts_key", key, sizeof(key));
    config_get_str("tts_voice", voice, sizeof(voice));
    bon_tts_set_key(key, voice);
    memset(key, 0, sizeof(key));
#endif
    s_started = true;
    xTaskCreate(bon_task, "bon", 6144, NULL, 4, &s_task);
}

// ---------------- Lenh cho Muse ----------------
static cJSON *param(const char *type, const char *desc) {
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "type", type);
    cJSON_AddStringToObject(p, "description", desc);
    return p;
}

static void add_cmd(cJSON *commands, const char *name, const char *desc, cJSON *req,
                    cJSON *opt) {
    cJSON *c = cJSON_CreateObject();
    cJSON_AddStringToObject(c, "description", desc);
    cJSON_AddItemToObject(c, "required", req ? req : cJSON_CreateObject());
    cJSON_AddItemToObject(c, "optional", opt ? opt : cJSON_CreateObject());
    cJSON_AddItemToObject(commands, name, c);
}

extern "C" void bon_robot_advertise(cJSON *commands) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddItemToObject(req, "direction", param("string", "forward, backward, left or right"));
    cJSON *opt = cJSON_CreateObject();
    cJSON_AddItemToObject(opt, "duration_ms", param("integer", "200-5000, default 1000"));
    cJSON_AddItemToObject(opt, "speed", param("integer", "percent 30-100, default 60"));
    add_cmd(commands, "bon.move",
            "Drive Bon on the table. Stops by itself at table edges and low obstacles. "
            "Returns at once while Bon moves; call bon.status to see why it stopped.",
            req, opt);

    req = cJSON_CreateObject();
    cJSON_AddItemToObject(req, "direction", param("string", "left or right"));
    opt = cJSON_CreateObject();
    cJSON_AddItemToObject(opt, "degrees", param("integer", "10-720, default 90"));
    add_cmd(commands, "bon.turn", "Turn Bon in place by an angle (a full circle takes ~3 s).",
            req, opt);

    add_cmd(commands, "bon.stop", "Stop Bon's wheels now.", nullptr, nullptr);
    add_cmd(commands, "bon.dance", "A happy ~6 s wiggle dance on the spot.", nullptr, nullptr);

    req = cJSON_CreateObject();
    cJSON_AddItemToObject(req, "expression",
                          param("string", "neutral, happy, sad, angry, surprised, sleepy, love, "
                                          "wink or confused"));
    opt = cJSON_CreateObject();
    cJSON_AddItemToObject(opt, "seconds", param("integer", "0 = keep, default 5"));
    add_cmd(commands, "bon.face", "Show an expression with Bon's OLED eyes.", req, opt);

    req = cJSON_CreateObject();
    cJSON_AddItemToObject(req, "text", param("string", "up to 120 characters"));
    opt = cJSON_CreateObject();
    cJSON_AddItemToObject(opt, "seconds", param("integer", "1-60, default 8"));
    add_cmd(commands, "bon.show_text",
            "Show a short text on Bon's 128x64 screen (Vietnamese accents are dropped), then "
            "go back to the eyes.",
            req, opt);

    add_cmd(commands, "bon.status",
            "Bon's state: moving, distance sensor mm, floor mm, last_event (edge, obstacle, "
            "tof_error, none), touch count and seconds since last touch.",
            nullptr, nullptr);
    add_cmd(commands, "bon.calibrate_floor",
            "Re-measure the table under Bon's nose. Use after Bon is moved to a new table or "
            "when it refuses to move because it can't see the table.",
            nullptr, nullptr);
}

extern "C" void bon_robot_show_reply(const char *text) {
    if (!s_started || !text || !*text) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    snprintf(s_req_text, sizeof(s_req_text), "%s", text);
    s_req_text_until = now_ms() + 12000;
    s_req_text_changed = true;
    xSemaphoreGive(s_mutex);
    if (s_task) xTaskNotifyGive(s_task);
}

extern "C" bool bon_robot_handles(const char *command) {
    return command && strncmp(command, "bon.", 4) == 0;
}

static cJSON *err(const char *code, const std::string &msg) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", false);
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "code", code);
    cJSON_AddStringToObject(e, "message", msg.c_str());
    cJSON_AddItemToObject(r, "error", e);
    return r;
}

static cJSON *ok(cJSON *payload) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddItemToObject(r, "payload", payload ? payload : cJSON_CreateObject());
    return r;
}

static const char *get_str(cJSON *params, const char *key) {
    cJSON *v = params ? cJSON_GetObjectItem(params, key) : nullptr;
    return (cJSON_IsString(v) && v->valuestring) ? v->valuestring : nullptr;
}

static int get_int(cJSON *params, const char *key, int def, int lo, int hi) {
    cJSON *v = params ? cJSON_GetObjectItem(params, key) : nullptr;
    int x = cJSON_IsNumber(v) ? (int)v->valuedouble : def;
    return std::max(lo, std::min(hi, x));
}

static void set_plan(const Step *steps, int n) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (n > 0) memcpy(s_req_plan, steps, sizeof(Step) * n);
    s_req_plan_len = n;
    s_req_plan_changed = true;
    xSemaphoreGive(s_mutex);
    if (s_task) xTaskNotifyGive(s_task);
}

// Kiem tra truoc khi di toi / xoay. Tra ve nullptr neu an toan.
static cJSON *check_safe(bool forward) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool tof_ok = s_tof_ok, found = s_floor_found;
    int mm = s_tof_mm, floor = s_floor_mm;
    xSemaphoreGive(s_mutex);
    if (!tof_ok) {
        return err("sensor_error", "Bon's distance sensor is not working, so Bon must not drive "
                                   "forward or turn (it could fall off the table).");
    }
    if (!found) {
        return err("no_floor", "Bon could not see the table surface (sensor " +
                                   std::to_string(mm) + " mm). Ask the user to set Bon flat on "
                                   "the table, then call bon.calibrate_floor.");
    }
    if (forward && is_edge(mm, floor)) {
        return err("at_edge", "Bon is at the table edge (sensor " + std::to_string(mm) +
                                  " mm, table " + std::to_string(floor) +
                                  " mm). Go backward or turn first.");
    }
    return nullptr;
}

extern "C" cJSON *bon_robot_command(const char *command, cJSON *params) {
    if (!s_started) return err("not_ready", "Bon's hardware is not started");

    if (strcmp(command, "bon.move") == 0) {
        const char *d = get_str(params, "direction");
        Dir dir;
        if (!d) return err("missing_param", "direction is required");
        if (strcmp(d, "forward") == 0) dir = DIR_FORWARD;
        else if (strcmp(d, "backward") == 0) dir = DIR_BACKWARD;
        else if (strcmp(d, "left") == 0) dir = DIR_LEFT;
        else if (strcmp(d, "right") == 0) dir = DIR_RIGHT;
        else return err("bad_param", "direction must be forward, backward, left or right");
        int ms = get_int(params, "duration_ms", 1000, 200, 5000);
        int speed = get_int(params, "speed", BON_DEFAULT_SPEED, BON_MIN_SPEED, 100);
        if (dir != DIR_BACKWARD) {
            cJSON *e = check_safe(dir == DIR_FORWARD);
            if (e) return e;
        } else {
            ms = std::min(ms, BON_MAX_BACK_MS);
        }
        Step s = {dir, speed, ms};
        set_plan(&s, 1);
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "direction", d);
        cJSON_AddNumberToObject(p, "duration_ms", ms);
        return ok(p);
    }
    if (strcmp(command, "bon.turn") == 0) {
        const char *d = get_str(params, "direction");
        if (!d || (strcmp(d, "left") != 0 && strcmp(d, "right") != 0)) {
            return err("bad_param", "direction must be left or right");
        }
        cJSON *e = check_safe(false);
        if (e) return e;
        int deg = get_int(params, "degrees", 90, 10, 720);
        Step s = {strcmp(d, "left") == 0 ? DIR_LEFT : DIR_RIGHT, BON_TURN_SPEED,
                  deg * BON_TURN_MS_360 / 360};
        set_plan(&s, 1);
        cJSON *p = cJSON_CreateObject();
        cJSON_AddNumberToObject(p, "degrees", deg);
        return ok(p);
    }
    if (strcmp(command, "bon.stop") == 0) {
        set_plan(nullptr, 0);
        return ok(nullptr);
    }
    if (strcmp(command, "bon.dance") == 0) {
        cJSON *e = check_safe(false);
        if (e) return e;
        const int t = BON_TURN_SPEED, f = BON_DEFAULT_SPEED;
        const Step steps[] = {
            {DIR_LEFT, t, 600}, {DIR_RIGHT, t, 600}, {DIR_LEFT, t, 600}, {DIR_RIGHT, t, 600},
            {DIR_FORWARD, f, 400}, {DIR_BACKWARD, f, 400}, {DIR_LEFT, t, 1500},
            {DIR_RIGHT, t, 600}, {DIR_LEFT, t, 600},
        };
        set_plan(steps, sizeof(steps) / sizeof(steps[0]));
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_req_face = FACE_HAPPY;
        s_req_face_until = now_ms() + 7000;
        s_req_face_changed = true;
        xSemaphoreGive(s_mutex);
        return ok(nullptr);
    }
    if (strcmp(command, "bon.face") == 0) {
        const char *x = get_str(params, "expression");
        int f = -1;
        for (int i = 0; x && i < FACE_COUNT; i++)
            if (strcasecmp(x, kFaceNames[i]) == 0) f = i;
        if (f < 0) {
            return err("bad_param", "expression must be neutral, happy, sad, angry, surprised, "
                                    "sleepy, love, wink or confused");
        }
        int secs = get_int(params, "seconds", 5, 0, 3600);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_req_face = (Face)f;
        s_req_face_until = secs ? now_ms() + secs * 1000LL : 0;
        s_req_face_changed = true;
        xSemaphoreGive(s_mutex);
        if (s_task) xTaskNotifyGive(s_task);
        return ok(nullptr);
    }
    if (strcmp(command, "bon.show_text") == 0) {
        const char *t = get_str(params, "text");
        if (!t || !*t) return err("missing_param", "text is required");
        int secs = get_int(params, "seconds", 8, 1, 60);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        snprintf(s_req_text, sizeof(s_req_text), "%s", t);
        s_req_text_until = now_ms() + secs * 1000LL;
        s_req_text_changed = true;
        xSemaphoreGive(s_mutex);
        if (s_task) xTaskNotifyGive(s_task);
        return ok(nullptr);
    }
    if (strcmp(command, "bon.status") == 0) {
        cJSON *p = cJSON_CreateObject();
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        cJSON_AddBoolToObject(p, "moving", s_moving);
        cJSON_AddBoolToObject(p, "tof_ok", s_tof_ok);
        cJSON_AddNumberToObject(p, "distance_mm", s_tof_mm);
        cJSON_AddNumberToObject(p, "floor_mm", s_floor_mm);
        cJSON_AddBoolToObject(p, "floor_found", s_floor_found);
        cJSON_AddBoolToObject(p, "at_edge", s_tof_ok && is_edge(s_tof_mm, s_floor_mm));
        cJSON_AddStringToObject(p, "last_event", s_last_event);
        cJSON_AddBoolToObject(p, "screen_ok", s_oled_ok);
        cJSON_AddNumberToObject(p, "touch_count", s_touch_count);
        if (s_last_touch_ms) {
            cJSON_AddNumberToObject(p, "seconds_since_touch",
                                    (double)((now_ms() - s_last_touch_ms) / 1000));
        }
        xSemaphoreGive(s_mutex);
        return ok(p);
    }
    if (strcmp(command, "bon.calibrate_floor") == 0) {
        if (!s_tof_ok) return err("sensor_error", "Distance sensor not working.");
        xSemaphoreTake(s_calibrate_done, 0);   // xoa tin hieu cu
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        bool moving = s_moving;
        if (!moving) s_req_calibrate = true;
        xSemaphoreGive(s_mutex);
        if (moving) return err("busy", "Stop Bon first.");
        if (s_task) xTaskNotifyGive(s_task);
        if (xSemaphoreTake(s_calibrate_done, pdMS_TO_TICKS(1500)) != pdTRUE) {
            return err("timeout", "Calibration did not finish; call bon.status.");
        }
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        bool found = s_floor_found;
        int floor = s_floor_mm, mm = s_tof_mm;
        xSemaphoreGive(s_mutex);
        if (!found) {
            return err("no_floor", "Could not see the table (sensor " + std::to_string(mm) +
                                       " mm). Is Bon flat on the table?");
        }
        cJSON *p = cJSON_CreateObject();
        cJSON_AddNumberToObject(p, "floor_mm", floor);
        return ok(p);
    }
    return err("unknown_command", std::string("Unknown command ") + command);
}
