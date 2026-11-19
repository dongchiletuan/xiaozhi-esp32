#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

// ===== Robot Bon: so do chan (GPIO) =====
// Dua tren bread-compact-wifi (128x64), them dong co, cam bien cham, cam bien ToF.

#include <driver/gpio.h>

#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

// Mic INMP441
#define AUDIO_I2S_MIC_GPIO_WS   GPIO_NUM_4
#define AUDIO_I2S_MIC_GPIO_SCK  GPIO_NUM_5
#define AUDIO_I2S_MIC_GPIO_DIN  GPIO_NUM_6
// Loa MAX98357A
#define AUDIO_I2S_SPK_GPIO_DOUT GPIO_NUM_7
#define AUDIO_I2S_SPK_GPIO_BCLK GPIO_NUM_15
#define AUDIO_I2S_SPK_GPIO_LRCK GPIO_NUM_16

#define BUILTIN_LED_GPIO        GPIO_NUM_48
#define BOOT_BUTTON_GPIO        GPIO_NUM_0

// Cam bien cham TTP223: cham vao -> chan len muc CAO (active high)
#define TOUCH_GPIO              GPIO_NUM_17

// Man hinh OLED SSD1306 128x64 + cam bien ToF TOF050C (VL6180X) dung chung I2C
#define DISPLAY_SDA_PIN GPIO_NUM_41
#define DISPLAY_SCL_PIN GPIO_NUM_42
#define DISPLAY_WIDTH   128
#define DISPLAY_HEIGHT  64
#define DISPLAY_MIRROR_X true
#define DISPLAY_MIRROR_Y true

// Mach dong co TC1508A
#define MOTOR_LEFT_IN1   GPIO_NUM_10   // dong co A = banh trai
#define MOTOR_LEFT_IN2   GPIO_NUM_11
#define MOTOR_RIGHT_IN1  GPIO_NUM_12   // dong co B = banh phai
#define MOTOR_RIGHT_IN2  GPIO_NUM_13

// Neu Bon noi "di toi" ma banh nao quay nguoc, doi false -> true cho banh do
#define MOTOR_LEFT_REVERSED   false
#define MOTOR_RIGHT_REVERSED  true    // anh Tuan test 16/11: banh phai quay nguoc

// Toc do mac dinh (%). Dang dung nguon USB nen de vua phai.
#define MOTOR_DEFAULT_SPEED   60

// Xoay tai cho: anh Tuan do 19/11 chay PIN: toc do 85% -> 14 giay = 4,75 vong
// => 1 vong ~ 2950 ms o toc do 85%
#define TURN_SPEED             85
#define TURN_MS_PER_360        2950   // thoi gian xoay 1 vong o TURN_SPEED

// Cam bien ToF goc 45 do duoi mui: mat ban ~22-25 mm
#define TOF_DEFAULT_BASELINE_MM  23   // dung khi luc khoi dong khong do duoc mat ban
#define TOF_EDGE_MARGIN_MM       15   // xa hon mat ban > 15 mm -> mep ban
#define TOF_OBSTACLE_MARGIN_MM   5    // gan hon mat ban > 5 mm -> vat can thap
#define TOF_MAX_FLOOR_MM         150  // mat ban xa hon muc nay coi nhu khong thay
// Nguong thuc te tu dong tang theo do cao: mep = xa hon 35%, vat can = gan hon 30%

#endif // _BOARD_CONFIG_H_
