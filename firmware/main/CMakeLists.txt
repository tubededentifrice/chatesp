idf_component_register(
    SRCS
        "app_main.cpp"
        "audio_capture.cpp"
        "audio_playback.cpp"
        "power_control.cpp"
        "ui.cpp"
    INCLUDE_DIRS "."
    REQUIRES chatesp_board esp_codec_dev
)
