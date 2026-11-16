# Bon robot

Board riêng cho robot "Bon" (ESP32-S3 N16R8), dựa trên `bread-compact-wifi` 128x64.

| Bộ phận | Chân |
|---|---|
| Mic INMP441 | WS 4, SCK 5, SD 6 |
| Loa MAX98357A | DIN 7, BCLK 15, LRC 16 |
| OLED SSD1306 128x64 + ToF TOF050C (VL6180X) | SDA 41, SCL 42 (0x3C, 0x29) |
| Cảm biến chạm TTP223 | 17 (chạm = mức cao) |
| Mạch động cơ TC1508A | IN1-IN4 = 10, 11, 12, 13 |

Lệnh giọng nói (MCP tools): `self.bon.move`, `self.bon.stop`, `self.bon.dance`,
`self.bon.get_status`, `self.bon.calibrate_floor`.

Chạm nhanh = bắt đầu/kết thúc trò chuyện; giữ tay ~1 giây = "xoa đầu" Bon;
chạm khi Bon đang chạy = dừng ngay. ToF gắn 45° dưới mũi: xa hơn mặt bàn 15 mm
= mép bàn (dừng + lùi), gần hơn 5 mm = vật cản (dừng).
