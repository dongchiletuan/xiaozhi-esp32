// ===== Khuon mat cua Bon tren OLED 128x64 =====
// Giu thanh trang thai 16 px phia tren cua xiaozhi, phan duoi 128x48 la doi mat:
//   - tu chop mat, thinh thoang liec qua lai
//   - doi hinh theo cam xuc ma server gui ve (happy, sad, surprised, sleepy...)
// Khi co thong bao he thong (cai Wi-Fi, ma kich hoat, loi) thi an mat de hien chu.
#ifndef _BON_DISPLAY_H_
#define _BON_DISPLAY_H_

#include "display/oled_display.h"

#include <string>

class BonDisplay : public OledDisplay {
public:
    BonDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
               int height, bool mirror_x, bool mirror_y)
        : OledDisplay(panel_io, panel, width, height, mirror_x, mirror_y) {}

    void SetupUI() override;
    bool SetEmotion(const char* emotion) override;
    void SetChatMessage(const char* role, const char* content) override;

private:
    // Hinh dang 1 con mat
    struct Eye {
        int w = 26, h = 30;     // kich thuoc
        int dx = 0, dy = 0;     // lech khoi vi tri goc
        int radius = 8;         // bo goc
        int top_lid = 0;        // mi tren che xuong bao nhieu px (buon ngu, ngau)
        bool smile = false;     // mat cuoi (hinh vong cung)
        int brow = 0;           // 0 khong, 1 buon (khoe ngoai rui xuong), 2 gian (khoe trong)
    };

    struct EyeObjs {
        lv_obj_t* eye = nullptr;
        lv_obj_t* lid = nullptr;
        lv_obj_t* smile = nullptr;
        lv_obj_t* brow[3] = {nullptr, nullptr, nullptr};
        lv_point_precise_t pts[3][2];
    };

    lv_obj_t* face_ = nullptr;
    EyeObjs objs_[2];
    Eye shape_[2];
    std::string emotion_ = "neutral";
    bool blinking_ = false;
    int gaze_x_ = 0, gaze_y_ = 0;
    int blink_wait_ = 30;      // so nhip 100 ms toi lan chop mat tiep theo
    int gaze_wait_ = 50;
    lv_timer_t* timer_ = nullptr;

    void CreateEye(int i);
    void ShapeFor(const std::string& emotion);
    void Draw();               // phai goi khi dang giu khoa man hinh
    void Tick();
    static void TimerCb(lv_timer_t* t);
};

#endif // _BON_DISPLAY_H_
