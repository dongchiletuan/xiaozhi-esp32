#include "bon_display.h"

#include <esp_random.h>
#include <esp_log.h>

#define TAG "BonDisplay"

namespace {
// Tren OLED cua xiaozhi: mau "den" = diem anh SANG, mau "trang" = TAT
inline lv_color_t Lit() { return lv_color_black(); }
inline lv_color_t Off() { return lv_color_white(); }

constexpr int kFaceTop = 16;              // duoi thanh trang thai
constexpr int kFaceW = 128, kFaceH = 48;
constexpr int kEyeCx[2] = {36, 92};       // tam 2 mat (trong vung mat)
constexpr int kEyeCy = 24;

lv_obj_t* MakeBox(lv_obj_t* parent, lv_color_t color) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_style_bg_color(o, color, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

int Rand(int lo, int hi) { return lo + (int)(esp_random() % (uint32_t)(hi - lo + 1)); }
}  // namespace

void BonDisplay::SetupUI() {
    OledDisplay::SetupUI();
    if (face_ != nullptr || height_ != 64) {
        return;
    }
    DisplayLockGuard lock(this);
    face_ = MakeBox(lv_screen_active(), Off());
    lv_obj_set_pos(face_, 0, kFaceTop);
    lv_obj_set_size(face_, kFaceW, kFaceH);
    CreateEye(0);
    CreateEye(1);
    ShapeFor("neutral");
    Draw();
    timer_ = lv_timer_create(TimerCb, 100, this);
}

void BonDisplay::CreateEye(int i) {
    auto& o = objs_[i];
    o.eye = MakeBox(face_, Lit());
    o.smile = MakeBox(face_, Off());
    lv_obj_set_style_radius(o.smile, LV_RADIUS_CIRCLE, 0);
    o.lid = MakeBox(face_, Off());
    for (int k = 0; k < 3; k++) {
        o.brow[k] = lv_line_create(face_);
        lv_obj_remove_style_all(o.brow[k]);
        lv_obj_set_style_line_width(o.brow[k], 9, 0);
        lv_obj_set_style_line_color(o.brow[k], Off(), 0);
        lv_obj_set_style_line_opa(o.brow[k], LV_OPA_COVER, 0);
        lv_obj_set_pos(o.brow[k], 0, 0);
        lv_obj_add_flag(o.brow[k], LV_OBJ_FLAG_HIDDEN);
    }
}

// Bang cam xuc -> hinh dang mat. Ten cam xuc la bo chuan cua xiaozhi.
void BonDisplay::ShapeFor(const std::string& e) {
    Eye a;   // mat mac dinh (neutral)
    Eye L = a, R = a;

    if (e == "happy" || e == "laughing" || e == "funny" || e == "loving" ||
        e == "delicious" || e == "kissy" || e == "confident" || e == "relaxed") {
        L.smile = R.smile = true;               // mat cuoi ^ ^
        if (e == "laughing" || e == "funny") { L.w = R.w = 30; }
    } else if (e == "sad" || e == "crying") {
        L.brow = R.brow = 1;                    // khoe mat rui xuong
        L.dy = R.dy = 3;
        if (e == "crying") { L.h = R.h = 24; }
    } else if (e == "angry") {
        L.brow = R.brow = 2;
        L.h = R.h = 26;
    } else if (e == "surprised" || e == "shocked") {
        L.w = R.w = 30; L.h = R.h = 32;         // mat tron xoe
        L.radius = R.radius = LV_RADIUS_CIRCLE;
        if (e == "shocked") { L.w = R.w = 34; L.h = R.h = 36; }   // hoang hot: mat to hon nua
    } else if (e == "sleepy") {
        L.top_lid = R.top_lid = 20;             // lim dim
        L.dy = R.dy = 2;
    } else if (e == "thinking" || e == "confused") {
        L.h = 20; L.dy = -4; R.dy = -4;         // mot mat nheo, nhin len
        if (e == "confused") { L.h = 30; R.h = 18; }
    } else if (e == "winking") {
        R.smile = true; R.h = 24;               // nhay mat
    } else if (e == "cool") {
        L.top_lid = R.top_lid = 11;             // ngau, mi ha mot nua
    } else if (e == "embarrassed") {
        L.w = R.w = 22; L.h = R.h = 24;         // ngai, nhin xuong
        L.dy = R.dy = 6; L.dx = 4; R.dx = -4;
    } else if (e == "silly") {
        L.w = 30; L.h = 32; R.w = 18; R.h = 18;
        L.radius = R.radius = LV_RADIUS_CIRCLE;
    }
    shape_[0] = L;
    shape_[1] = R;
}

void BonDisplay::Draw() {
    if (face_ == nullptr) return;
    for (int i = 0; i < 2; i++) {
        const Eye& s = shape_[i];
        auto& o = objs_[i];
        int w = s.w, h = s.h;
        int x = kEyeCx[i] - w / 2 + s.dx + gaze_x_;
        int y = kEyeCy - h / 2 + s.dy + gaze_y_;
        bool details = true;
        if (blinking_) {             // chop mat: mat thanh mot gach ngang
            y = kEyeCy + s.dy + gaze_y_ - 2;
            h = 4;
            details = false;
        }
        lv_obj_set_pos(o.eye, x, y);
        lv_obj_set_size(o.eye, w, h);
        lv_obj_set_style_radius(o.eye, blinking_ ? 2 : s.radius, 0);

        if (details && s.top_lid > 0) {
            lv_obj_set_pos(o.lid, x - 2, y - 2);
            lv_obj_set_size(o.lid, w + 4, s.top_lid + 2);
            lv_obj_remove_flag(o.lid, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(o.lid, LV_OBJ_FLAG_HIDDEN);
        }

        if (details && s.smile) {    // phan duoi bi "ma" che bang vong cung -> mat cuoi
            lv_obj_set_pos(o.smile, x - 5, y + h * 45 / 100);
            lv_obj_set_size(o.smile, w + 10, h);
            lv_obj_remove_flag(o.smile, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(o.smile, LV_OBJ_FLAG_HIDDEN);
        }

        for (int k = 0; k < 3; k++) {
            if (details && s.brow != 0) {
                // khoe ngoai: mat trai = ben trai, mat phai = ben phai
                int outer = (i == 0) ? x - 3 : x + w + 3;
                int inner = (i == 0) ? x + w + 3 : x - 3;
                int low = y + h * 45 / 100 - k * 7;
                int high = y - 2 - k * 7;
                // buon: ngoai thap, trong cao. gian: trong thap, ngoai cao
                int y_outer = (s.brow == 1) ? low : high;
                int y_inner = (s.brow == 1) ? high : low;
                o.pts[k][0] = {(lv_value_precise_t)outer, (lv_value_precise_t)y_outer};
                o.pts[k][1] = {(lv_value_precise_t)inner, (lv_value_precise_t)y_inner};
                lv_line_set_points(o.brow[k], o.pts[k], 2);
                lv_obj_remove_flag(o.brow[k], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(o.brow[k], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

void BonDisplay::TimerCb(lv_timer_t* t) {
    static_cast<BonDisplay*>(lv_timer_get_user_data(t))->Tick();
}

// Chay moi 100 ms trong luong cua LVGL (da giu khoa man hinh)
void BonDisplay::Tick() {
    bool changed = false;
    if (hold_ticks_ > 0 && --hold_ticks_ == 0) {   // het gio giu net mat -> ve cam xuc hien tai
        emotion_ = pending_emotion_;
        ShapeFor(emotion_);
        changed = true;
    }
    if (blinking_) {
        blinking_ = false;
        changed = true;
        blink_wait_ = Rand(25, 55);             // 2.5 - 5.5 giay nua chop tiep
        if (Rand(0, 5) == 0) blink_wait_ = 2;   // thinh thoang chop 2 lan lien
        if (extra_blinks_ > 0) { extra_blinks_--; blink_wait_ = 2; }
    } else if (--blink_wait_ <= 0) {
        blinking_ = (emotion_ != "sleepy" || Rand(0, 1));
        changed = true;
    }

    if (--gaze_wait_ <= 0) {
        gaze_wait_ = Rand(30, 80);
        bool calm = (emotion_ == "neutral" || emotion_ == "relaxed" || emotion_ == "thinking");
        if (calm && Rand(0, 2) > 0) {           // liec nhin xung quanh
            gaze_x_ = Rand(-1, 1) * 8;
            gaze_y_ = Rand(-1, 1) * 3;
        } else {
            gaze_x_ = gaze_y_ = 0;
        }
        changed = true;
    }
    if (changed) Draw();
}

bool BonDisplay::SetEmotion(const char* emotion) {
    bool ok = OledDisplay::SetEmotion(emotion);   // bieu tuong cu (dang bi mat che)
    if (face_ == nullptr || emotion == nullptr) return ok;
    DisplayLockGuard lock(this);
    if (hold_ticks_ > 0) {          // dang lam net mat theo yeu cau: de danh, lat nua moi doi
        pending_emotion_ = emotion;
        return true;
    }
    emotion_ = emotion;
    ShapeFor(emotion_);
    gaze_x_ = gaze_y_ = 0;
    Draw();
    return true;
}

void BonDisplay::SetChatMessage(const char* role, const char* content) {
    OledDisplay::SetChatMessage(role, content);
    if (face_ == nullptr || role == nullptr) return;
    bool has_text = content != nullptr && content[0] != '\0';
    DisplayLockGuard lock(this);
    if (std::string(role) == "system" && has_text) {
        // Thong bao he thong (Wi-Fi, ma kich hoat, loi): an mat de doc duoc chu
        lv_obj_add_flag(face_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(face_, LV_OBJ_FLAG_HIDDEN);
    }
}

bool BonDisplay::ShowExpression(const std::string& e, int ms) {
    static const char* kNames[] = {
        "neutral", "happy", "laughing", "loving", "sad", "crying", "angry", "surprised",
        "shocked", "sleepy", "thinking", "confused", "winking", "cool", "embarrassed",
        "silly", "blink"};
    bool valid = false;
    for (auto n : kNames) valid = valid || e == n;
    if (!valid || face_ == nullptr) return false;

    DisplayLockGuard lock(this);
    lv_obj_remove_flag(face_, LV_OBJ_FLAG_HIDDEN);
    if (hold_ticks_ == 0) pending_emotion_ = emotion_;   // nho cam xuc dang co de quay lai
    gaze_x_ = gaze_y_ = 0;
    if (e == "blink") {               // chop mat 3 cai lien
        blinking_ = true;
        extra_blinks_ = 2;
        hold_ticks_ = 0;
    } else {
        emotion_ = e;
        ShapeFor(emotion_);
        hold_ticks_ = ms / 100 > 0 ? ms / 100 : 1;
    }
    Draw();
    return true;
}
