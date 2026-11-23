# Robot Bon tren Muse Gadgets SDK

Nhanh nay chi chua phan rieng cua Bon. Khi push, GitHub Actions tai
Muse Gadgets SDK (facebookincubator/muse-gadget-sdk, ghim commit),
chay `apply_bon.py` de chen Bon vao, roi build cho ESP32-S3.

- `files/main/bon_robot.cpp`: banh xe, ToF chong roi, mat OLED, cam bien cham
- `files/devices/sdkconfig.bon`: cau hinh board Bon (nap sau devkitc-1)
- `files/devices/guides/bon.md`: huong dan Muse cach dung Bon
- `HUONG-DAN-NAP.txt`: cach nap file build

Lenh Muse goi duoc: bon.move, bon.turn, bon.stop, bon.dance, bon.face,
bon.show_text, bon.status, bon.calibrate_floor.

Khong bao gio dua SDK token vao repo nay (repo cong khai). Token duoc gui
qua cong serial va nam trong NVS cua board.
