// ===== Bo dieu khien chuyen dong cua robot Bon =====
// - 2 dong co N20 qua mach TC1508A (PWM 20 kHz, khoi dong mem)
// - Cam bien ToF goc 45 do duoi mui: chong roi mep ban + dung khi co vat can thap
// - Cac "cong cu MCP" de AI ra lenh bang giong noi: di toi, lui, re, nhay, dung
#ifndef _BON_ROBOT_H_
#define _BON_ROBOT_H_

#include <driver/i2c_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <string>
#include <vector>

#include "vl6180x.h"

class BonRobot {
public:
    enum Dir { kStop, kForward, kBackward, kLeft, kRight };

    explicit BonRobot(i2c_master_bus_handle_t i2c_bus);

    // Dung ngay lap tuc (an toan goi tu bat ky dau, vd. khi be cham vao Bon)
    void Stop();
    bool IsMoving() const { return moving_.load(); }

private:
    struct Step {
        Dir dir;
        int speed;     // 0-100 %
        int ms;        // thoi gian
    };

    Vl6180x tof_;
    bool tof_ok_ = false;
    std::atomic<int> tof_mm_{-1};       // -1 = khong co vat (qua xa)
    std::atomic<int> baseline_mm_{23};  // khoang cach toi mat ban luc dung yen
    std::atomic<bool> moving_{false};
    std::string last_event_ = "none";   // none / edge / obstacle / tof_error

    SemaphoreHandle_t mutex_;
    SemaphoreHandle_t tof_mutex_;
    std::vector<Step> plan_;            // chuoi buoc dang cho chay
    bool plan_changed_ = false;
    TaskHandle_t task_ = nullptr;

    void InitMotors();
    void SetWheel(int in1, int in2, bool reversed, int speed_signed);
    void Drive(Dir dir, int speed);
    void CalibrateFloor();
    int ReadTof();                      // tra ve mm, -1 neu qua xa, -2 neu loi
    bool IsEdge(int mm) const;
    bool IsObstacle(int mm) const;
    void SetPlan(std::vector<Step> plan);
    void Task();
    void Notify(const char* text, const char* emotion);
    void RegisterTools();
    std::string StatusJson();
};

#endif // _BON_ROBOT_H_
