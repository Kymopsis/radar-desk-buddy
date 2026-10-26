#include "ui_i18n.h"

/* SoftAP: phone locale + user picks language & gender (theme). Persist NVS later. */

static const char *const s_zh_cn[UI_STR_COUNT] = {
    [UI_STR_NEW_MSG] = "有新留言",
    [UI_STR_TOUCH_HEAD] = "向上摸摸头",
    [UI_STR_TOUCH_CLOSE] = "再摸一下关掉",
    [UI_STR_PEER_HERE] = "对端在呀",
    [UI_STR_PEER_AWAY] = "对端不在",
    [UI_STR_PEER_OFFLINE] = "对端离线",
    [UI_STR_SEND_MSG] = "给TA发消息",
    [UI_STR_CLOUD_PLAY] = "云端也能玩",
    [UI_STR_PROV_TITLE] = "配网",
    [UI_STR_PROV_SCAN] = "扫码",
    [UI_STR_PROV_BIND] = "或打开",
    [UI_STR_MSG_TITLE] = "留言",
    [UI_STR_GENDER_F] = "女版粉色",
    [UI_STR_GENDER_M] = "男版蓝色",
    [UI_STR_RADAR_OFF] = "雷达未连接",
    [UI_STR_CLOUD_OFF] = "离线",
};

static const char *const s_zh_tw[UI_STR_COUNT] = {
    [UI_STR_NEW_MSG] = "有新留言",
    [UI_STR_TOUCH_HEAD] = "向上摸摸頭",
    [UI_STR_TOUCH_CLOSE] = "再摸一下關掉",
    [UI_STR_PEER_HERE] = "對端在呀",
    [UI_STR_PEER_AWAY] = "對端不在",
    [UI_STR_PEER_OFFLINE] = "對端離線",
    [UI_STR_SEND_MSG] = "給TA發消息",
    [UI_STR_CLOUD_PLAY] = "雲端也能玩",
    [UI_STR_PROV_TITLE] = "配網",
    [UI_STR_PROV_SCAN] = "掃碼",
    [UI_STR_PROV_BIND] = "或打開",
    [UI_STR_MSG_TITLE] = "留言",
    [UI_STR_GENDER_F] = "女版粉色",
    [UI_STR_GENDER_M] = "男版藍色",
    [UI_STR_RADAR_OFF] = "雷達未連接",
    [UI_STR_CLOUD_OFF] = "離線",
};

static const char *const s_en[UI_STR_COUNT] = {
    [UI_STR_NEW_MSG] = "New message",
    [UI_STR_TOUCH_HEAD] = "Touch the head",
    [UI_STR_TOUCH_CLOSE] = "Touch to close",
    [UI_STR_PEER_HERE] = "Peer is here",
    [UI_STR_PEER_AWAY] = "Peer away",
    [UI_STR_PEER_OFFLINE] = "Peer offline",
    [UI_STR_SEND_MSG] = "Message them",
    [UI_STR_CLOUD_PLAY] = "Play in cloud",
    [UI_STR_PROV_TITLE] = "WiFi",
    [UI_STR_PROV_SCAN] = "Scan",
    [UI_STR_PROV_BIND] = "or open",
    [UI_STR_MSG_TITLE] = "Message",
    [UI_STR_GENDER_F] = "Girl pink",
    [UI_STR_GENDER_M] = "Boy blue",
    [UI_STR_RADAR_OFF] = "Radar offline",
    [UI_STR_CLOUD_OFF] = "Offline",
};

static const char *const s_ja[UI_STR_COUNT] = {
    [UI_STR_NEW_MSG] = "新しい伝言",
    [UI_STR_TOUCH_HEAD] = "頭をタッチ",
    [UI_STR_TOUCH_CLOSE] = "タッチで閉じる",
    [UI_STR_PEER_HERE] = "相手はいるよ",
    [UI_STR_PEER_AWAY] = "相手はいない",
    [UI_STR_PEER_OFFLINE] = "相手オフライン",
    [UI_STR_SEND_MSG] = "メッセージ送る",
    [UI_STR_CLOUD_PLAY] = "クラウドで遊ぶ",
    [UI_STR_PROV_TITLE] = "接続設定",
    [UI_STR_PROV_SCAN] = "QR接続",
    [UI_STR_PROV_BIND] = "言語性別コード",
    [UI_STR_MSG_TITLE] = "伝言",
    [UI_STR_GENDER_F] = "女の子ピンク",
    [UI_STR_GENDER_M] = "男の子ブルー",
    [UI_STR_RADAR_OFF] = "レーダー未接続",
    [UI_STR_CLOUD_OFF] = "オフライン",
};

static const char *const s_ko[UI_STR_COUNT] = {
    [UI_STR_NEW_MSG] = "새 메시지",
    [UI_STR_TOUCH_HEAD] = "머리를 터치",
    [UI_STR_TOUCH_CLOSE] = "다시 터치해 닫기",
    [UI_STR_PEER_HERE] = "상대 있어요",
    [UI_STR_PEER_AWAY] = "상대 없어요",
    [UI_STR_PEER_OFFLINE] = "상대 오프라인",
    [UI_STR_SEND_MSG] = "메시지 보내기",
    [UI_STR_CLOUD_PLAY] = "클라우드로 놀기",
    [UI_STR_PROV_TITLE] = "설정",
    [UI_STR_PROV_SCAN] = "QR 연결",
    [UI_STR_PROV_BIND] = "언어성별코드",
    [UI_STR_MSG_TITLE] = "메시지",
    [UI_STR_GENDER_F] = "여 핑크",
    [UI_STR_GENDER_M] = "남 블루",
    [UI_STR_RADAR_OFF] = "레이더 없음",
    [UI_STR_CLOUD_OFF] = "오프라인",
};

static const char *const s_es[UI_STR_COUNT] = {
    [UI_STR_NEW_MSG] = "Mensaje nuevo",
    [UI_STR_TOUCH_HEAD] = "Toca la cabeza",
    [UI_STR_TOUCH_CLOSE] = "Toca para cerrar",
    [UI_STR_PEER_HERE] = "Pareja aqui",
    [UI_STR_PEER_AWAY] = "Pareja lejos",
    [UI_STR_PEER_OFFLINE] = "Pareja offline",
    [UI_STR_SEND_MSG] = "Enviar mensaje",
    [UI_STR_CLOUD_PLAY] = "Jugar en nube",
    [UI_STR_PROV_TITLE] = "Configurar",
    [UI_STR_PROV_SCAN] = "Escanea para unir",
    [UI_STR_PROV_BIND] = "Idioma genero codigo",
    [UI_STR_MSG_TITLE] = "Mensaje",
    [UI_STR_GENDER_F] = "Chica rosa",
    [UI_STR_GENDER_M] = "Chico azul",
    [UI_STR_RADAR_OFF] = "Radar offline",
    [UI_STR_CLOUD_OFF] = "Offline",
};

static const char *const s_fr[UI_STR_COUNT] = {
    [UI_STR_NEW_MSG] = "Nouveau message",
    [UI_STR_TOUCH_HEAD] = "Touche la tete",
    [UI_STR_TOUCH_CLOSE] = "Touche pour fermer",
    [UI_STR_PEER_HERE] = "Partenaire ici",
    [UI_STR_PEER_AWAY] = "Partenaire loin",
    [UI_STR_PEER_OFFLINE] = "Hors ligne",
    [UI_STR_SEND_MSG] = "Envoyer message",
    [UI_STR_CLOUD_PLAY] = "Jouer au cloud",
    [UI_STR_PROV_TITLE] = "Config",
    [UI_STR_PROV_SCAN] = "Scanner pour joindre",
    [UI_STR_PROV_BIND] = "Langue genre code",
    [UI_STR_MSG_TITLE] = "Message",
    [UI_STR_GENDER_F] = "Fille rose",
    [UI_STR_GENDER_M] = "Garcon bleu",
    [UI_STR_RADAR_OFF] = "Radar offline",
    [UI_STR_CLOUD_OFF] = "Hors ligne",
};

static const char *const s_de[UI_STR_COUNT] = {
    [UI_STR_NEW_MSG] = "Neue Nachricht",
    [UI_STR_TOUCH_HEAD] = "Kopf beruehren",
    [UI_STR_TOUCH_CLOSE] = "Tippen zum Schliessen",
    [UI_STR_PEER_HERE] = "Partner hier",
    [UI_STR_PEER_AWAY] = "Partner weg",
    [UI_STR_PEER_OFFLINE] = "Partner offline",
    [UI_STR_SEND_MSG] = "Nachricht senden",
    [UI_STR_CLOUD_PLAY] = "In Cloud spielen",
    [UI_STR_PROV_TITLE] = "Setup",
    [UI_STR_PROV_SCAN] = "Scannen zum Joinen",
    [UI_STR_PROV_BIND] = "Sprache Gender Code",
    [UI_STR_MSG_TITLE] = "Nachricht",
    [UI_STR_GENDER_F] = "Maedchen pink",
    [UI_STR_GENDER_M] = "Junge blau",
    [UI_STR_RADAR_OFF] = "Radar offline",
    [UI_STR_CLOUD_OFF] = "Offline",
};

static const char *const s_pt[UI_STR_COUNT] = {
    [UI_STR_NEW_MSG] = "Nova mensagem",
    [UI_STR_TOUCH_HEAD] = "Toque a cabeca",
    [UI_STR_TOUCH_CLOSE] = "Toque para fechar",
    [UI_STR_PEER_HERE] = "Par aqui",
    [UI_STR_PEER_AWAY] = "Par longe",
    [UI_STR_PEER_OFFLINE] = "Par offline",
    [UI_STR_SEND_MSG] = "Enviar mensagem",
    [UI_STR_CLOUD_PLAY] = "Brincar na nuvem",
    [UI_STR_PROV_TITLE] = "Configurar",
    [UI_STR_PROV_SCAN] = "Escaneie para entrar",
    [UI_STR_PROV_BIND] = "Idioma genero codigo",
    [UI_STR_MSG_TITLE] = "Mensagem",
    [UI_STR_GENDER_F] = "Menina rosa",
    [UI_STR_GENDER_M] = "Menino azul",
    [UI_STR_RADAR_OFF] = "Radar offline",
    [UI_STR_CLOUD_OFF] = "Offline",
};

static const char *const s_ru[UI_STR_COUNT] = {
    [UI_STR_NEW_MSG] = "Новое сообщение",
    [UI_STR_TOUCH_HEAD] = "Коснись головы",
    [UI_STR_TOUCH_CLOSE] = "Коснись чтобы закрыть",
    [UI_STR_PEER_HERE] = "Пара рядом",
    [UI_STR_PEER_AWAY] = "Пары нет",
    [UI_STR_PEER_OFFLINE] = "Пара офлайн",
    [UI_STR_SEND_MSG] = "Написать пару",
    [UI_STR_CLOUD_PLAY] = "Играть в облаке",
    [UI_STR_PROV_TITLE] = "Настройка",
    [UI_STR_PROV_SCAN] = "Сканируй для входа",
    [UI_STR_PROV_BIND] = "Язык пол код",
    [UI_STR_MSG_TITLE] = "Сообщение",
    [UI_STR_GENDER_F] = "Девочка розовый",
    [UI_STR_GENDER_M] = "Мальчик синий",
    [UI_STR_RADAR_OFF] = "Радар офлайн",
    [UI_STR_CLOUD_OFF] = "офлайн",
};

static const char *const *const s_tables[UI_LANG_COUNT] = {
    [UI_LANG_ZH_CN] = s_zh_cn,
    [UI_LANG_ZH_TW] = s_zh_tw,
    [UI_LANG_EN] = s_en,
    [UI_LANG_JA] = s_ja,
    [UI_LANG_KO] = s_ko,
    [UI_LANG_ES] = s_es,
    [UI_LANG_FR] = s_fr,
    [UI_LANG_DE] = s_de,
    [UI_LANG_PT] = s_pt,
    [UI_LANG_RU] = s_ru,
};

static const char *const s_lang_names[UI_LANG_COUNT] = {
    "简体中文", "繁體中文", "English", "日本語", "한국어",
    "Espanol", "Francais", "Deutsch", "Portugues", "Русский",
};

const char *ui_tr(ui_lang_t lang, ui_str_id_t id)
{
    if (lang >= UI_LANG_COUNT) {
        lang = UI_LANG_EN;
    }
    if (id >= UI_STR_COUNT) {
        return "";
    }
    const char *s = s_tables[lang][id];
    return s ? s : s_en[id];
}

const char *ui_lang_name(ui_lang_t lang)
{
    if (lang >= UI_LANG_COUNT) {
        return "English";
    }
    return s_lang_names[lang];
}
