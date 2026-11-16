// Driver toi gian cho cam bien khoang cach VL6180X (module TOF050C).
// Thanh ghi 16 bit, dia chi I2C 0x29. Trinh tu khoi tao theo ST AN4545
// (giong thu vien Adafruit_VL6180X ma anh da test bang Arduino).
#ifndef _BON_VL6180X_H_
#define _BON_VL6180X_H_

#include <driver/i2c_master.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>

class Vl6180x {
public:
    enum Result { kOk, kNoTarget, kError };

    bool Init(i2c_master_bus_handle_t bus) {
        i2c_device_config_t cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = 0x29,
            .scl_speed_hz = 400 * 1000,
            .scl_wait_us = 0,
            .flags = { .disable_ack_check = 0 },
        };
        if (i2c_master_bus_add_device(bus, &cfg, &dev_) != ESP_OK) {
            return false;
        }
        uint8_t id = 0;
        if (!Read8(0x000, id) || id != 0xB4) {
            ESP_LOGW("VL6180X", "Model id = 0x%02X (mong doi 0xB4)", id);
            return false;
        }
        static const uint16_t kSettings[][2] = {
            // Private registers (ST AN4545)
            {0x0207, 0x01}, {0x0208, 0x01}, {0x0096, 0x00}, {0x0097, 0xfd},
            {0x00e3, 0x00}, {0x00e4, 0x04}, {0x00e5, 0x02}, {0x00e6, 0x01},
            {0x00e7, 0x03}, {0x00f5, 0x02}, {0x00d9, 0x05}, {0x00db, 0xce},
            {0x00dc, 0x03}, {0x00dd, 0xf8}, {0x009f, 0x00}, {0x00a3, 0x3c},
            {0x00b7, 0x00}, {0x00bb, 0x3c}, {0x00b2, 0x09}, {0x00ca, 0x09},
            {0x0198, 0x01}, {0x01b0, 0x17}, {0x01ad, 0x00}, {0x00ff, 0x05},
            {0x0100, 0x05}, {0x0199, 0x05}, {0x01a6, 0x1b}, {0x01ac, 0x3e},
            {0x01a7, 0x1f}, {0x0030, 0x00},
            // Public registers (recommended)
            {0x0011, 0x10}, {0x010a, 0x30}, {0x003f, 0x46}, {0x0031, 0xFF},
            {0x0041, 0x63}, {0x002e, 0x01}, {0x001b, 0x09}, {0x003e, 0x31},
            {0x0014, 0x24},
            // SYSTEM__FRESH_OUT_OF_RESET = 0
            {0x0016, 0x00},
        };
        for (auto& s : kSettings) {
            if (!Write8(s[0], (uint8_t)s[1])) {
                return false;
            }
        }
        return true;
    }

    // Do 1 lan (mat ~10-15 ms). mm chi co nghia khi tra ve kOk.
    Result Measure(int& mm) {
        uint8_t v = 0;
        // Cho cam bien san sang
        if (!WaitBit(0x04D, 0x01, 0x01)) return kError;
        if (!Write8(0x018, 0x01)) return kError;          // SYSRANGE__START
        // Cho do xong (RESULT__INTERRUPT_STATUS_GPIO bit 2)
        if (!WaitBit(0x04F, 0x04, 0x04)) return kError;
        if (!Read8(0x062, v)) return kError;              // RESULT__RANGE_VAL
        uint8_t status = 0;
        if (!Read8(0x04D, status)) return kError;         // RESULT__RANGE_STATUS
        Write8(0x015, 0x07);                              // xoa co ngat
        status >>= 4;
        if (status != 0) return kNoTarget;                // qua xa / khong co vat
        mm = v;
        return kOk;
    }

private:
    i2c_master_dev_handle_t dev_ = nullptr;

    bool Write8(uint16_t reg, uint8_t val) {
        uint8_t buf[3] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF), val};
        return i2c_master_transmit(dev_, buf, 3, 50) == ESP_OK;
    }

    bool Read8(uint16_t reg, uint8_t& val) {
        uint8_t r[2] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF)};
        return i2c_master_transmit_receive(dev_, r, 2, &val, 1, 50) == ESP_OK;
    }

    bool WaitBit(uint16_t reg, uint8_t mask, uint8_t want) {
        int64_t han = esp_timer_get_time() + 60000;   // toi da 60 ms
        while (true) {
            uint8_t v = 0;
            if (!Read8(reg, v)) return false;
            if ((v & mask) == want) return true;
            if (esp_timer_get_time() > han) return false;
            vTaskDelay(1);   // nhuong CPU cho viec khac
        }
    }
};

#endif // _BON_VL6180X_H_
