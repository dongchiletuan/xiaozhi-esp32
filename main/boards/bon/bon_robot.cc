#include "bon_robot.h"

#include <driver/ledc.h>
#include <esp_log.h>
#include <esp_timer.h>

#include <algorithm>
#include <cstdlib>

#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "config.h"
#include "display/display.h"
#include "mcp_server.h"

#define TAG "BonRobot"

namespace {
// 4 chan PWM: IN1..IN4 -> kenh LEDC 2..5 (kenh 0-1 de danh cho viec khac)
constexpr ledc_timer_t kTimer = LEDC_TIMER_1;
constexpr ledc_mode_t kMode = LEDC_LOW_SPEED_MODE;
constexpr int kPins[4] = {MOTOR_LEFT_IN1, MOTOR_LEFT_IN2, MOTOR_RIGHT_IN1, MOTOR_RIGHT_IN2};
constexpr ledc_channel_t kChannels[4] = {LEDC_CHANNEL_2, LEDC_CHANNEL_3, LEDC_CHANNEL_4,
                                         LEDC_CHANNEL_5};
constexpr int kMinSpeed = 30;          // N20 duoi ~30% thuong khong du luc quay
constexpr int kMaxBackwardMs = 2000;   // phia sau khong co cam bien -> lui ngan thoi
constexpr int kBackoffMs = 350;        // gap mep ban: lui lai mot chut

int64_t NowMs() { return esp_timer_get_time() / 1000; }
}  // namespace

BonRobot::BonRobot(i2c_master_bus_handle_t i2c_bus) {
    mutex_ = xSemaphoreCreateMutex();
    tof_mutex_ = xSemaphoreCreateMutex();
    InitMotors();

    tof_ok_ = tof_.Init(i2c_bus);
    if (tof_ok_) {
        CalibrateFloor();
    } else {
        ESP_LOGE(TAG, "Khong tim thay cam bien ToF (0x29)! Bon se khong di toi duoc.");
        last_event_ = "tof_error";
    }

    RegisterTools();
    xTaskCreate([](void* arg) { static_cast<BonRobot*>(arg)->Task(); }, "bon_robot", 4096, this,
                4, &task_);
}

// ---------------- Dong co ----------------

void BonRobot::InitMotors() {
    ledc_timer_config_t timer = {};
    timer.speed_mode = kMode;
    timer.duty_resolution = LEDC_TIMER_8_BIT;   // toc do 0..255
    timer.timer_num = kTimer;
    timer.freq_hz = 20000;                      // 20 kHz: dong co it keu rit
    timer.clk_cfg = LEDC_AUTO_CLK;
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    for (int i = 0; i < 4; i++) {
        ledc_channel_config_t ch = {};
        ch.gpio_num = kPins[i];
        ch.speed_mode = kMode;
        ch.channel = kChannels[i];
        ch.timer_sel = kTimer;
        ch.duty = 0;
        ch.hpoint = 0;
        ESP_ERROR_CHECK(ledc_channel_config(&ch));
    }
}

// speed_signed: >0 quay tien, <0 quay lui, 0 dung. Don vi %.
void BonRobot::SetWheel(int in1, int in2, bool reversed, int speed_signed) {
    if (reversed) speed_signed = -speed_signed;
    int duty = std::min(100, std::abs(speed_signed)) * 255 / 100;
    // TC1508A: 1 chan PWM, chan kia = 0 -> quay. Ca hai = 0 -> dung.
    int d1 = speed_signed > 0 ? duty : 0;
    int d2 = speed_signed < 0 ? duty : 0;
    ledc_set_duty(kMode, kChannels[in1], d1);
    ledc_update_duty(kMode, kChannels[in1]);
    ledc_set_duty(kMode, kChannels[in2], d2);
    ledc_update_duty(kMode, kChannels[in2]);
}

void BonRobot::Drive(Dir dir, int speed) {
    int l = 0, r = 0;
    switch (dir) {
        case kForward:  l = speed;  r = speed;  break;
        case kBackward: l = -speed; r = -speed; break;
        case kLeft:     l = -speed; r = speed;  break;   // xoay tai cho sang trai
        case kRight:    l = speed;  r = -speed; break;
        default: break;
    }
    SetWheel(0, 1, MOTOR_LEFT_REVERSED, l);
    SetWheel(2, 3, MOTOR_RIGHT_REVERSED, r);
}

void BonRobot::Stop() {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    plan_.clear();
    plan_changed_ = true;
    xSemaphoreGive(mutex_);
    if (task_) xTaskNotifyGive(task_);
}

void BonRobot::SetPlan(std::vector<Step> plan) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    plan_ = std::move(plan);
    plan_changed_ = true;
    xSemaphoreGive(mutex_);
    if (task_) xTaskNotifyGive(task_);
}

// ---------------- Cam bien ToF ----------------

int BonRobot::ReadTof() {
    if (!tof_ok_) return -2;
    int mm = 0;
    // Chi 1 noi duoc do tai 1 thoi diem (vong lap chinh va lenh "do lai mat ban")
    xSemaphoreTake(tof_mutex_, portMAX_DELAY);
    auto r = tof_.Measure(mm);
    xSemaphoreGive(tof_mutex_);
    int v = (r == Vl6180x::kOk) ? mm : (r == Vl6180x::kNoTarget ? -1 : -2);
    tof_mm_ = v;
    return v;
}

void BonRobot::CalibrateFloor() {
    std::vector<int> ok;
    for (int i = 0; i < 9; i++) {
        int mm = ReadTof();
        if (mm >= 8 && mm <= TOF_MAX_FLOOR_MM) ok.push_back(mm);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (ok.size() >= 5) {
        std::sort(ok.begin(), ok.end());
        baseline_mm_ = ok[ok.size() / 2];
        floor_found_ = true;
        ESP_LOGI(TAG, "Mat ban o %d mm", baseline_mm_.load());
    } else {
        baseline_mm_ = TOF_DEFAULT_BASELINE_MM;
        floor_found_ = false;
        ESP_LOGW(TAG, "Khong do duoc mat ban, dung mac dinh %d mm", TOF_DEFAULT_BASELINE_MM);
    }
}

bool BonRobot::IsEdge(int mm) const {
    // Qua xa (khong thay gi) hoac xa hon mat ban nhieu -> phia truoc la khoang trong
    // Nguong ti le voi do cao: cam bien cang cao thi so do dao dong cang nhieu
    int b = baseline_mm_.load();
    int margin = std::max(TOF_EDGE_MARGIN_MM, b * 35 / 100);
    return mm == -1 || mm > b + margin;
}

bool BonRobot::IsObstacle(int mm) const {
    int b = baseline_mm_.load();
    int margin = std::max(TOF_OBSTACLE_MARGIN_MM, b * 30 / 100);
    return mm >= 0 && mm < b - margin;
}

// ---------------- Vong lap chinh cua Bon ----------------

void BonRobot::Notify(const char* text, const char* emotion) {
    std::string t = text, e = emotion;
    Application::GetInstance().Schedule([t, e]() {
        auto display = Board::GetInstance().GetDisplay();
        if (display) {
            display->ShowNotification(t, 3000);
            display->SetEmotion(e.c_str());
        }
        Application::GetInstance().PlaySound(Lang::Sounds::OGG_EXCLAMATION);
    });
}

void BonRobot::Task() {
    std::vector<Step> plan;
    size_t idx = 0;
    int64_t step_end = 0;
    int cur_speed = 0;
    int edge_count = 0, obstacle_count = 0;
    bool active = false;

    auto finish = [&](const char* event) {
        Drive(kStop, 0);
        plan.clear();
        active = false;
        cur_speed = 0;
        if (event) {
            xSemaphoreTake(mutex_, portMAX_DELAY);
            last_event_ = event;
            xSemaphoreGive(mutex_);
        }
    };

    while (true) {
        // 1. Co lenh moi?
        xSemaphoreTake(mutex_, portMAX_DELAY);
        if (plan_changed_) {
            plan = plan_;
            plan_.clear();
            plan_changed_ = false;
            if (!plan.empty()) last_event_ = "none";
            xSemaphoreGive(mutex_);
            Drive(kStop, 0);
            cur_speed = 0;
            idx = 0;
            edge_count = obstacle_count = 0;
            active = !plan.empty();
            if (active) step_end = NowMs() + plan[0].ms;
        } else {
            xSemaphoreGive(mutex_);
        }
        moving_ = active;

        // 2. Khong co gi de lam: do ToF thinh thoang, cho lenh
        if (!active) {
            ReadTof();
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
            continue;
        }

        const Step& step = plan[idx];
        int mm = ReadTof();

        // 3. Bao ve: chi khi di toi hoac xoay (mui Bon quet qua mep ban)
        if (step.dir == kForward || step.dir == kLeft || step.dir == kRight) {
            if (mm == -2) {
                finish("tof_error");
                Notify("Loi cam bien ToF", "confused");
                continue;
            }
            edge_count = IsEdge(mm) ? edge_count + 1 : 0;
            obstacle_count = (step.dir == kForward && IsObstacle(mm)) ? obstacle_count + 1 : 0;

            if (edge_count >= 2) {          // 2 lan lien tiep cho chac
                ESP_LOGW(TAG, "MEP BAN! (%d mm, mat ban %d mm)", mm, baseline_mm_.load());
                Drive(kStop, 0);
                vTaskDelay(pdMS_TO_TICKS(80));
                if (step.dir == kForward) {   // lui lai mot chut cho an toan
                    Drive(kBackward, 50);
                    vTaskDelay(pdMS_TO_TICKS(kBackoffMs));
                }
                finish("edge");
                edge_count = 0;
                Notify("Mep ban! Bon dung lai", "shocked");
                continue;
            }
            if (obstacle_count >= 2) {
                ESP_LOGW(TAG, "VAT CAN! (%d mm)", mm);
                finish("obstacle");
                obstacle_count = 0;
                Notify("Co vat can phia truoc", "surprised");
                continue;
            }
        } else {
            edge_count = obstacle_count = 0;
        }

        // 4. Khoi dong mem: tang toc tu tu de khong lam sut dien ESP32
        if (cur_speed < step.speed) cur_speed = std::min(step.speed, cur_speed + 10);
        if (cur_speed > step.speed) cur_speed = step.speed;
        Drive(step.dir, cur_speed);

        // 5. Het thoi gian buoc nay -> sang buoc tiep theo
        if (NowMs() >= step_end) {
            idx++;
            if (idx >= plan.size()) {
                finish(nullptr);
                continue;
            }
            if (plan[idx].dir != step.dir) {   // doi huong: dung mot nhip cho nhe dong co
                Drive(kStop, 0);
                cur_speed = 0;
                vTaskDelay(pdMS_TO_TICKS(60));
            }
            step_end = NowMs() + plan[idx].ms;
        }

        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
    }
}

// ---------------- Lenh giong noi (MCP tools) ----------------

std::string BonRobot::StatusJson() {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    std::string ev = last_event_;
    xSemaphoreGive(mutex_);
    int mm = tof_mm_.load();
    std::string s = "{\"moving\": ";
    s += moving_.load() ? "true" : "false";
    s += ", \"tof_ok\": ";
    s += tof_ok_ ? "true" : "false";
    s += ", \"distance_mm\": " + std::to_string(mm);
    s += ", \"floor_mm\": " + std::to_string(baseline_mm_.load());
    s += ", \"floor_found\": ";
    s += floor_found_.load() ? "true" : "false";
    s += ", \"at_edge\": ";
    s += (tof_ok_ && IsEdge(mm)) ? "true" : "false";
    s += ", \"last_event\": \"" + ev + "\"}";
    return s;
}

void BonRobot::RegisterTools() {
    auto& mcp = McpServer::GetInstance();

    mcp.AddTool(
        "self.bon.move",
        "Make Bon (the robot) drive on the table. Use when the user says things like "
        "'Bon di toi / tien len' (forward), 'lui lai' (backward), 're trai / quay trai' (left), "
        "'re phai / quay phai' (right). For turning by an angle (quay 90 do, quay mot vong) use self.bon.turn instead. "
        "Bon automatically stops at table edges and low obstacles. The call returns at once "
        "while Bon is still moving.\n"
        "direction: forward | backward | left | right\n"
        "duration_ms: how long to move (forward default 1000)\n"
        "speed: percent, 30-100 (default 60)",
        PropertyList({
            Property("direction", kPropertyTypeString),
            Property("duration_ms", kPropertyTypeInteger, 1000, 200, 5000),
            Property("speed", kPropertyTypeInteger, MOTOR_DEFAULT_SPEED, kMinSpeed, 100),
        }),
        [this](const PropertyList& p) -> ToolResult {
            std::string d = p["direction"].value<std::string>();
            int ms = p["duration_ms"].value<int>();
            int speed = p["speed"].value<int>();
            Dir dir;
            if (d == "forward") dir = kForward;
            else if (d == "backward") dir = kBackward;
            else if (d == "left") dir = kLeft;
            else if (d == "right") dir = kRight;
            else return std::unexpected("direction must be forward, backward, left or right");

            if (dir != kBackward && !tof_ok_) {
                return std::unexpected("Bon's distance sensor is not working, so Bon must not "
                                       "drive forward or turn (it could fall off the table).");
            }
            if (dir != kBackward && !floor_found_.load()) {
                return std::unexpected("Bon could not see the table surface when it started "
                                       "(sensor reading " + std::to_string(tof_mm_.load()) +
                                       " mm). Ask the user to put Bon flat on the table and say "
                                       "'Bon do lai mat ban' (calibrate_floor).");
            }
            if (dir == kForward && IsEdge(tof_mm_.load())) {
                return std::unexpected("Bon is at the table edge right now (sensor " +
                                       std::to_string(tof_mm_.load()) + " mm, table " +
                                       std::to_string(baseline_mm_.load()) +
                                       " mm). Move backward or turn first.");
            }
            if (dir == kBackward) ms = std::min(ms, kMaxBackwardMs);
            SetPlan({{dir, speed, ms}});
            return std::string("{\"started\": true, \"direction\": \"") + d +
                   "\", \"duration_ms\": " + std::to_string(ms) + "}";
        });

    mcp.AddTool(
        "self.bon.turn",
        "Turn Bon in place by an angle. Use for 'Bon quay trai 90 do', 'quay phai', "
        "'quay mot vong' (360), 'quay lai phia sau' (180). A full circle takes about 3 "
        "seconds. Returns at once.\n"
        "direction: left | right\n"
        "degrees: angle to turn, default 90",
        PropertyList({
            Property("direction", kPropertyTypeString),
            Property("degrees", kPropertyTypeInteger, 90, 10, 720),
        }),
        [this](const PropertyList& p) -> ToolResult {
            std::string d = p["direction"].value<std::string>();
            int deg = p["degrees"].value<int>();
            Dir dir;
            if (d == "left") dir = kLeft;
            else if (d == "right") dir = kRight;
            else return std::unexpected("direction must be left or right");
            if (!tof_ok_ || !floor_found_.load()) {
                return std::unexpected("Bon's distance sensor cannot see the table, so Bon "
                                       "must not turn.");
            }
            int ms = deg * TURN_MS_PER_360 / 360;
            SetPlan({{dir, TURN_SPEED, ms}});
            return std::string("{\"started\": true, \"degrees\": ") + std::to_string(deg) +
                   ", \"seconds\": " + std::to_string((ms + 500) / 1000) + "}";
        });

    mcp.AddTool("self.bon.stop", "Stop Bon immediately ('Bon dung lai', 'dung').", PropertyList(),
                [this](const PropertyList&) -> ToolResult {
                    Stop();
                    return true;
                });

    mcp.AddTool(
        "self.bon.dance",
        "Make Bon do a happy little dance by wiggling left and right on the spot "
        "('Bon nhay di', 'Bon mua di'). Takes about 6 seconds.",
        PropertyList(), [this](const PropertyList&) -> ToolResult {
            if (!tof_ok_) {
                return std::unexpected("Distance sensor not working; Bon cannot dance safely.");
            }
            int s = TURN_SPEED;
            SetPlan({{kLeft, s, 600},  {kRight, s, 600}, {kLeft, s, 600},  {kRight, s, 600},
                     {kForward, MOTOR_DEFAULT_SPEED, 400}, {kBackward, MOTOR_DEFAULT_SPEED, 400},
                     {kLeft, s, 1500}, {kRight, s, 600}, {kLeft, s, 600}});
            return true;
        });

    mcp.AddTool(
        "self.bon.get_status",
        "Get Bon's movement status: whether Bon is moving, the distance sensor reading, "
        "and last_event (edge = stopped at a table edge, obstacle = something in front, "
        "tof_error = sensor problem). Use it when asked why Bon stopped.",
        PropertyList(), [this](const PropertyList&) -> ToolResult { return StatusJson(); });

    mcp.AddTool(
        "self.bon.calibrate_floor",
        "Re-measure the table surface under Bon's nose. Use when the user says Bon was moved "
        "to a new table or Bon keeps stopping for no reason ('Bon do lai mat ban').",
        PropertyList(), [this](const PropertyList&) -> ToolResult {
            if (moving_.load()) return std::unexpected("Stop Bon first.");
            if (!tof_ok_) return std::unexpected("Distance sensor not working.");
            CalibrateFloor();
            if (!floor_found_.load()) {
                return std::unexpected("Could not see the table surface (sensor reading " +
                                       std::to_string(tof_mm_.load()) +
                                       " mm). The sensor may be too high or not pointing at "
                                       "the table.");
            }
            return std::string("{\"floor_mm\": ") + std::to_string(baseline_mm_.load()) + "}";
        });
}
