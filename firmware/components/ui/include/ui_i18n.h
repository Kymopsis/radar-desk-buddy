#pragma once

#include "ui.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_STR_NEW_MSG = 0,
    UI_STR_TOUCH_HEAD,
    UI_STR_TOUCH_CLOSE,
    UI_STR_PEER_HERE,
    UI_STR_PEER_AWAY,
    UI_STR_PEER_OFFLINE,
    UI_STR_SEND_MSG,
    UI_STR_CLOUD_PLAY,
    UI_STR_PROV_TITLE,
    UI_STR_PROV_SCAN,
    UI_STR_PROV_BIND,
    UI_STR_MSG_TITLE,
    UI_STR_GENDER_F,
    UI_STR_GENDER_M,
    UI_STR_RADAR_OFF,
    UI_STR_CLOUD_OFF,
    UI_STR_COUNT,
} ui_str_id_t;

const char *ui_tr(ui_lang_t lang, ui_str_id_t id);
const char *ui_lang_name(ui_lang_t lang);

#ifdef __cplusplus
}
#endif
