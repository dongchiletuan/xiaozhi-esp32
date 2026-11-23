// Robot Bon: doc cau tra loi cua Muse bang Google Cloud Text-to-Speech.
// Mot task rieng goi API (HTTPS), nhan MP3; phien chat lay MP3 ra dan de giai ma.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Goi luc khoi dong (tu task thuong, khong phai task co stack PSRAM).
void bon_tts_set_key(const char *api_key, const char *voice);
// Co key va task da chay.
bool bon_tts_enabled(void);
// Bat dau doc mot doan text (bo cau dang doc truoc do). false neu khong lam duoc.
bool bon_tts_start(const char *text);
// Trang thai: 0 dang tai, 1 xong (con du lieu hoac het), -1 loi.
int bon_tts_state(void);
// Lay toi da cap byte MP3 chua lay. Tra ve so byte.
size_t bon_tts_take(uint8_t *out, size_t cap);
// So byte MP3 con lai chua lay (chi co nghia khi state == 1).
size_t bon_tts_remaining(void);

#ifdef __cplusplus
}
#endif
