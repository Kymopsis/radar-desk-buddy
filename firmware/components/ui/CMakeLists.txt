idf_component_register(
    SRCS "ui.c" "ui_i18n.c" "fonts/ui_font_16.c" "fonts/ui_font_32_digits.c"
    INCLUDE_DIRS "include"
    REQUIRES board display esp_timer qrcode esp_rom
)

