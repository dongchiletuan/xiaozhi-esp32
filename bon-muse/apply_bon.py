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

    # ---- Giong noi: mic INMP441 + loa MAX98357A, cham dau de noi ----
    insert(main_dir / "Kconfig.projbuild",
           "        config HOMEHUB_VOICE_BOARD_WAVESHARE_154G\n"
           "            bool \"Waveshare ESP32-S3-ePaper-1.54G\"\n"
           "            depends on IDF_TARGET_ESP32S3\n",
           "\n        config HOMEHUB_VOICE_BOARD_BON\n"
           "            bool \"Robot Bon (INMP441 mic + MAX98357A amp)\"\n"
           "            depends on IDF_TARGET_ESP32S3\n")
    sel = main_dir / "voice_board_select.cmake"
    insert(sel, "    WAVESHARE_154G\n)", "    BON\n", before=True)
    insert(sel, "else()\n    message(FATAL_ERROR \"HOMEHUB_VOICE_BOARD_${HOMEHUB_VOICE_BOARD_SELECTED} has no driver source\")",
           "elseif(HOMEHUB_VOICE_BOARD_SELECTED STREQUAL \"BON\")\n"
           "    set(HOMEHUB_VOICE_BOARD_SOURCE \"voice_board_bon.c\")\n",
           before=True)
    # Cam bien cham cua Bon dong vai nut "giu de noi"
    btn = main_dir / "button.c"
    s = btn.read_text()
    if "button_inject_press" not in s:
        s += ("\n// Robot Bon: the touch sensor acts as the push-to-talk button.\n"
              "bool button_inject_press(bool pressed) {\n"
              "    button_press_cb cb = s_press_cb;\n"
              "    return cb && cb(pressed);\n"
              "}\n")
        btn.write_text(s)
    insert(main_dir / "button.h", "void button_set_press_cb(button_press_cb cb);\n",
           "// Robot Bon: feed a press or release from another input (the touch sensor).\n"
           "bool button_inject_press(bool pressed);\n")
    # Cau tra loi cua Muse hien len man OLED
    voice = main_dir / "voice.c"
    insert(voice, '#include "voice_player.h"\n',
           '#if CONFIG_MUSE_BON_ROBOT\n#include "bon_robot.h"\n#endif\n')
    insert(voice, "            case MUSE_HATCH_EV_REPLY:\n",
           "#if CONFIG_MUSE_BON_ROBOT\n"
           "                bon_robot_show_reply(text);\n"
           "#endif\n")

    # ---- Google Cloud TTS: Bon doc cau tra loi ----
    muse_dir = esp32 / "components" / "muse"
    insert(muse_dir / "CMakeLists.txt",
           "    list(APPEND srcs \"muse_options.c\" \"muse_options_json.c\")\n"
           "    list(APPEND embed \"test_reply.mp3\")\n",
           "    if(CONFIG_MUSE_BON_ROBOT)\n"
           "        list(APPEND srcs \"bon_tts.c\")\n"
           "    endif()\n")
    sess = muse_dir / "muse_chat_session.cpp"
    insert(sess, '#include "muse_options.h"\n',
           '#if CONFIG_MUSE_BON_ROBOT\n#include "bon_tts.h"\n#endif\n')
    insert(sess,
           "        m.pcm_start = s_turn.pcm_out;\n"
           "        /* Paced to the text kept:",
           "#if CONFIG_MUSE_BON_ROBOT\n"
           "        if (bon_tts_enabled() && s_turn.texts && s_turn.mp3 &&\n"
           "            bon_tts_start(s_turn.texts + i * TEXT_MAX)) {\n"
           "            m.tts = TTS_ACTIVE;\n"
           "            s_turn.tts_msg = i;\n"
           "            s_turn.silent = false;\n"
           "            m.pcm_start = s_turn.pcm_out;\n"
           "            m.pcm_frames = 0;\n"
           "            s_turn.mp3_len = 0;\n"
           "            s_turn.mp3_ended = false;\n"
           "            s_turn.kbps = 0;\n"
           "            s_turn.down_rate = 0;\n"
           "            mp3dec_init(&s_turn.dec);\n"
           "            ESP_LOGI(TAG, \"speaking message %s with Google TTS\", m.id);\n"
           "            show_reply_start(m);\n"
           "            return;\n"
           "        }\n"
           "#endif\n",
           before=True)
    insert(sess, "/* Speaker off: queues the shown message's silence",
           "#if CONFIG_MUSE_BON_ROBOT\n"
           "/* Robot Bon: moves the Google TTS MP3 into the decode buffer as room allows. */\n"
           "static void bon_tts_feed(void)\n"
           "{\n"
           "    if (s_turn.tts_msg < 0 || s_turn.silent || s_turn.mp3_ended) {\n"
           "        return;\n"
           "    }\n"
           "    int st = bon_tts_state();\n"
           "    if (st < 0) {\n"
           "        ESP_LOGW(TAG, \"Google TTS failed; reply stays as text\");\n"
           "        s_turn.msgs[s_turn.tts_msg].tts = TTS_FINISHED;\n"
           "        s_turn.tts_msg = -1;\n"
           "        return;\n"
           "    }\n"
           "    if (st == 0) {\n"
           "        return;\n"
           "    }\n"
           "    s_turn.mp3_len += bon_tts_take(s_turn.mp3 + s_turn.mp3_len, MP3_BUF - s_turn.mp3_len);\n"
           "    if (!bon_tts_remaining()) {\n"
           "        s_turn.mp3_ended = true;\n"
           "    }\n"
           "}\n"
           "#endif\n\n",
           before=True)
    insert(sess, "            start_tts();\n            decode();\n",
           "#if CONFIG_MUSE_BON_ROBOT\n            bon_tts_feed();\n#endif\n",
           before=True)
    # Nap key qua cong serial:  >tts_key=AIza...   (tuy chon >tts_voice=vi-VN-Neural2-D)
    con = main_dir / "sdk_token_console.c"
    insert(con, '#include "sdkconfig.h"\n',
           '#if CONFIG_MUSE_BON_ROBOT\n#include "config_store.h"\n#endif\n')
    insert(con, "    if (strncmp(line, COMMAND, strlen(COMMAND)) != 0) return false;\n",
           "#if CONFIG_MUSE_BON_ROBOT\n"
           "    if (strncmp(line, \"tts_key=\", 8) == 0 || strncmp(line, \"tts_voice=\", 10) == 0) {\n"
           "        bool key = line[4] == 'k';\n"
           "        const char *v = strchr(line, '=') + 1;\n"
           "        bool ok = config_set_str(key ? \"tts_key\" : \"tts_voice\", v);\n"
           "        printf(\"@%s %s\\n\", key ? \"tts_key\" : \"tts_voice\", ok ? \"ok\" : \"error storage\");\n"
           "        fflush(stdout);\n"
           "        if (ok) {\n"
           "            vTaskDelay(pdMS_TO_TICKS(500));\n"
           "            esp_restart();\n"
           "        }\n"
           "        return true;\n"
           "    }\n"
           "#endif\n",
           before=True)
    print("Bon added to", esp32)


if __name__ == "__main__":
    main()
