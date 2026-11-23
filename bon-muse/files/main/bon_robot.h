// Robot Bon tren Muse Gadgets SDK: banh xe, cam bien ToF chong roi,
// mat OLED va cam bien cham. Muse goi cac lenh bon.* qua link.invoke.
#pragma once

#include <stdbool.h>

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

// Khoi tao phan cung va chay task dieu khien cua Bon.
void bon_robot_init(void);

// Them cac lenh bon.* vao link.register.
void bon_robot_advertise(cJSON *commands);

// Hien cau tra loi cua Muse len man OLED (goi tu task giong noi).
void bon_robot_show_reply(const char *text);

// true neu command la mot lenh bon.*
bool bon_robot_handles(const char *command);

// Xu ly lenh bon.* ; luon tra ve ket qua {"ok":...}
cJSON *bon_robot_command(const char *command, cJSON *params);

#ifdef __cplusplus
}
#endif
