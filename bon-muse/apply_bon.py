#!/usr/bin/env python3
"""Them robot Bon vao ban Muse Gadgets SDK (thu muc esp32/).

Cach dung: python3 apply_bon.py <duong-dan-toi-muse-gadget-sdk/esp32>

Chep cac file trong files/ vao, roi chen vai dong vao app.c,
noise_control.cpp, Kconfig.projbuild va main/CMakeLists.txt. Neu ban SDK
doi va khong tim thay cho chen, script bao loi ngay.
"""
import pathlib
import shutil
import sys

HERE = pathlib.Path(__file__).resolve().parent


def insert(path, anchor, text, before=False):
    s = path.read_text()
    if text.strip() in s:
        return
    n = s.count(anchor)
    if n != 1:
        sys.exit(f"{path}: anchor found {n} times: {anchor!r}")
    s = s.replace(anchor, text + anchor if before else anchor + text)
    path.write_text(s)


def main():
    esp32 = pathlib.Path(sys.argv[1]).resolve()
    for src in (HERE / "files").rglob("*"):
        if src.is_file():
            dst = esp32 / src.relative_to(HERE / "files")
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst)

    main_dir = esp32 / "main"

    insert(main_dir / "Kconfig.projbuild",
           "    config HOMEHUB_SENSECAP_SENSORS\n",
           "    config MUSE_BON_ROBOT\n"
           "        bool \"Robot Bon: wheels, ToF edge guard, OLED eyes, touch (bon.*)\"\n"
           "        depends on IDF_TARGET_ESP32S3\n"
           "        default n\n"
           "        help\n"
           "            Robot Bon: TC1508A motors on GPIO10-13, VL6180X and SSD1306 on\n"
           "            I2C SDA 41 / SCL 42, TTP223 touch on GPIO17.\n\n",
           before=True)

    insert(main_dir / "CMakeLists.txt",
           "if(CONFIG_HOMEHUB_SENSECAP_SENSORS)\n",
           "if(CONFIG_MUSE_BON_ROBOT)\n"
           "    list(APPEND GADGET_SRCS \"bon_robot.cpp\")\n"
           "endif()\n",
           before=True)

    nc = main_dir / "noise_control.cpp"
    insert(nc, '#include "cJSON.h"\n',
           '#if CONFIG_MUSE_BON_ROBOT\n#include "bon_robot.h"\n#endif\n')
    insert(nc, "#if CONFIG_HOMEHUB_SENSECAP_SENSORS\n    add_command(commands, \"sensors.read\",",
           "#if CONFIG_MUSE_BON_ROBOT\n    bon_robot_advertise(commands);\n#endif\n\n",
           before=True)

    app = main_dir / "app.c"
    insert(app, '#include "cJSON.h"\n',
           '#if CONFIG_MUSE_BON_ROBOT\n#include "bon_robot.h"\n#endif\n')
    insert(app,
           "#if CONFIG_HOMEHUB_BLE_COMMANDS\n"
           "    if (ble_commands_handles(command)) {\n",
           "#if CONFIG_MUSE_BON_ROBOT\n"
           "    if (bon_robot_handles(command)) {\n"
           "        return bon_robot_command(command, params);\n"
           "    }\n"
           "#endif\n",
           before=True)
    # bon_robot_init() ngay truoc khoi tao voice (truoc khi ket noi Wi-Fi)
    insert(app, "#if CONFIG_HOMEHUB_VOICE\n    voice_init();\n#endif\n",
           "#if CONFIG_MUSE_BON_ROBOT\n    bon_robot_init();\n#endif\n",
           before=True)
    print("Bon added to", esp32)


if __name__ == "__main__":
    main()
