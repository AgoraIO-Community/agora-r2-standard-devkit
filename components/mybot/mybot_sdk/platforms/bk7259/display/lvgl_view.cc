/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Project Contributors */
/* Pixel-glasses geometry and process-skin colors follow the product simulator's
 * src/expressions.js and src/reference-style.js. The renderer owns only LVGL
 * objects; the BK7259 display owner retains the panel and its fixed buffers. */
#include "lvgl_view.h"
#include "pixel_expression_shapes.h"

#include <cstdio>
#include <cstring>

LV_FONT_DECLARE(mybot_lvgl_font_20);
LV_FONT_DECLARE(lv_font_montserrat_32);
LV_FONT_DECLARE(bk7259_terminal_font_15);
LV_FONT_DECLARE(bk7259_terminal_font_16);

namespace {

constexpr int kArtworkWidth = 1024;
constexpr int kArtworkHeight = 896;
constexpr unsigned kFrameParts = 13;
constexpr size_t kSsidCapacity = 33;
constexpr size_t kStateLineCapacity =
    BK7259_LCD_STATE_CAPACITY + sizeof("> state: ") + sizeof(" | vp:pending") - 2;
constexpr uint32_t kSupportedIndicators =
    MYBOT_LCD_INDICATOR_VP_REGISTERED | MYBOT_LCD_INDICATOR_SERVER_STATE_MASK;
constexpr uint32_t kTransitionMs = BK7259_EXPRESSION_TRANSITION_MS;
constexpr uint32_t kTransitionTickMs = 50;
constexpr uint32_t kCursorMs = 500;
constexpr uint32_t kBackground = 0x050708;
constexpr uint32_t kNeutral = 0xf1f1ee;
constexpr uint32_t kGreeting = 0xf1ca79;
constexpr uint32_t kTerminal = 0x639589;
constexpr uint32_t kMuted = 0xa0a6a3;
constexpr uint32_t kError = 0xe57876;
constexpr int kTapSlop = 10;

enum class SettingsPage { Closed, Menu, Confirm };
enum class ButtonAction { Settings, Return, ResetMenu, Cancel, Confirm, Retry };

struct Presentation {
    const char *title;
    const char *notice;
    uint32_t ink;
};

#if CONFIG_MYBOT_LANGUAGE_ZH_CN
constexpr Presentation kScreens[] = {
    {"启动中", "正在初始化", kNeutral},
    {"Wi-Fi 配网", "请连接设备热点", kGreeting},
    {"网络已断开", "正在重新连接", kError},
    {"连接服务中", "正在连接服务", kNeutral},
    {"等待配对", "正在获取配对码", kGreeting},
    {"配对码", "在网页控制台输入配对码", kGreeting},
    {"", "", kNeutral},
    {"", "", kGreeting},
    {"启动失败", "请重启后重试", kError},
    {"正在退出", "正在结束服务", kMuted},
};
constexpr Presentation kResetScreens[] = {
    {"正在恢复出厂设置", "请勿断电", kGreeting},
    {"恢复出厂失败", "轻触按钮重试", kError},
    {"恢复完成，正在重启", "请勿断电", kNeutral},
};
constexpr const char *kPortalNotice = "请连接设备热点\nhttp://192.168.4.1";
constexpr const char *kSettings = "设置";
constexpr const char *kReset = "恢复出厂";
constexpr const char *kReturn = "返回";
constexpr const char *kConfirmTitle = "恢复出厂？";
constexpr const char *kConfirmNotice = "将清除本机Wi-Fi、设备凭据及音量，完成后重启";
constexpr const char *kConfirm = "确认恢复";
constexpr const char *kCancel = "取消";
constexpr const char *kRetry = "重试";
constexpr const char *kNoTouch = "触控不可用";
#else
constexpr Presentation kScreens[] = {
    {"Starting", "Please wait", kNeutral},
    {"Wi-Fi setup", "Connect to device Wi-Fi", kGreeting},
    {"Wi-Fi disconnected", "Reconnecting to Wi-Fi", kError},
    {"Connecting", "Connecting to services", kNeutral},
    {"Pairing", "Getting pairing code", kGreeting},
    {"Pairing code", "Enter code in web console", kGreeting},
    {"", "", kNeutral},
    {"", "", kGreeting},
    {"Startup failed", "Restart to retry", kError},
    {"Stopping", "Closing services", kMuted},
};
constexpr Presentation kResetScreens[] = {
    {"Restoring factory settings", "Keep power connected", kGreeting},
    {"Factory reset failed", "Tap Retry to try again", kError},
    {"Reset complete. Restarting", "Keep power connected", kNeutral},
};
constexpr const char *kPortalNotice = "Connect to device Wi-Fi\nhttp://192.168.4.1";
constexpr const char *kSettings = "Setup";
constexpr const char *kReset = "Factory reset";
constexpr const char *kReturn = "Back";
constexpr const char *kConfirmTitle = "Factory reset?";
constexpr const char *kConfirmNotice =
    "Clear this device's Wi-Fi, device credentials and volume, then restart.";
constexpr const char *kConfirm = "Reset";
constexpr const char *kCancel = "Cancel";
constexpr const char *kRetry = "Retry";
constexpr const char *kNoTouch = "Touch unavailable";
#endif

static_assert(sizeof(kScreens) / sizeof(kScreens[0]) == MYBOT_LCD_SCREEN_COUNT,
              "Every SDK workflow screen needs a presentation");

struct View {
    lv_obj_t *root;
    lv_obj_t *artwork;
    lv_obj_t *cursor;
    lv_obj_t *signature;
    lv_obj_t *actor;
    lv_obj_t *terminal[4];
    lv_obj_t *face;
    lv_obj_t *frame[kFrameParts];
    pixel_shapes::Bank banks[2];
    lv_obj_t *guide;
    lv_obj_t *divider;
    lv_obj_t *title;
    lv_obj_t *code;
    lv_obj_t *detail;
    lv_obj_t *notice;
    lv_obj_t *settings_button;
    lv_obj_t *touch_hint;
    lv_obj_t *settings_overlay;
    lv_obj_t *settings_menu;
    lv_obj_t *settings_confirm;
    lv_obj_t *retry_button;
    lv_obj_t *gesture_button;
    lv_point_t gesture_start;
    SettingsPage settings_page;
    bk7259_factory_reset_screen_t reset_screen;
    bool gesture_cancelled;
    bool touch_available;
    lv_timer_t *timer;
    mybot_lcd_content_t previous;
    bk7259_lcd_runtime_status_t runtime_status;
    char provisioning_ssid[kSsidCapacity];
    char pair_code[MYBOT_LCD_PAIR_CODE_CAPACITY];
    char mood[32];
    char state_line[kStateLineCapacity];
    int artwork_width;
    int artwork_height;
    bool has_content;
    bool face_visible;
    bool transitioning;
    bool factory_reset;
    uint32_t transition_started;
    uint32_t current_ink;
    uint32_t current_terminal;
    bk7259_expression_t expression;
    unsigned active_bank;
    bool previous_face;
};

View s_view{};

int scale(int value) { return (value * s_view.artwork_width + kArtworkWidth / 2) / kArtworkWidth; }

int positive_scale(int value) {
    const int result = scale(value);
    return result > 0 ? result : 1;
}

void set_hidden(lv_obj_t *object, bool hidden) {
    if (lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN) == hidden) {
        return;
    }
    if (hidden) {
        lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    }
}

void set_text(lv_obj_t *label, const char *text) {
    if (lv_label_get_text(label) != text) {
        lv_label_set_text_static(label, text);
    }
}

void prepare_object(lv_obj_t *object, int x, int y, int width, int height) {
    lv_obj_remove_style_all(object);
    lv_obj_remove_flag(object,
                       static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
}

lv_obj_t *make_panel(lv_obj_t *parent, int x, int y, int width, int height, uint32_t ink,
                     lv_opa_t opacity = LV_OPA_COVER) {
    lv_obj_t *object = lv_obj_create(parent);
    if (object) {
        prepare_object(object, x, y, width, height);
        lv_obj_set_style_bg_color(object, lv_color_hex(ink), 0);
        lv_obj_set_style_bg_opa(object, opacity, 0);
    }
    return object;
}

lv_obj_t *make_artwork_panel(lv_obj_t *parent, int x, int y, int width, int height, uint32_t ink,
                             lv_opa_t opacity = LV_OPA_COVER) {
    return make_panel(parent, scale(x), scale(y), positive_scale(width), positive_scale(height),
                      ink, opacity);
}

lv_obj_t *make_label(lv_obj_t *parent, int x, int y, int width, int height, const lv_font_t *font,
                     uint32_t ink) {
    lv_obj_t *object = lv_label_create(parent);
    if (object) {
        int label_height = positive_scale(height);
        if (label_height < font->line_height) {
            label_height = font->line_height;
        }
        prepare_object(object, scale(x), scale(y), positive_scale(width), label_height);
        lv_obj_set_style_text_font(object, font, 0);
        lv_obj_set_style_text_color(object, lv_color_hex(ink), 0);
        lv_obj_set_style_text_align(object, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(object, LV_LABEL_LONG_WRAP);
        lv_label_set_text_static(object, "");
    }
    return object;
}

uint32_t blend_color(uint32_t from, uint32_t to, unsigned progress) {
    uint32_t result = 0;
    for (unsigned shift = 0; shift < 24; shift += 8) {
        const unsigned channel = (((from >> shift) & 255u) * (255u - progress) +
                                  ((to >> shift) & 255u) * progress + 127u) /
                                 255u;
        result |= channel << shift;
    }
    return result;
}

void paint_frame(uint32_t ink, uint32_t terminal) {
    const lv_color_t color = lv_color_hex(ink);
    if (ink != s_view.current_ink) {
        for (auto *part : s_view.frame) {
            lv_obj_set_style_bg_color(part, color, 0);
        }
        lv_obj_set_style_bg_color(s_view.cursor, color, 0);
        lv_obj_set_style_text_color(s_view.signature, color, 0);
        s_view.current_ink = ink;
    }
    if (terminal != s_view.current_terminal) {
        for (auto *label : s_view.terminal) {
            lv_obj_set_style_text_color(label, lv_color_hex(terminal), 0);
        }
        s_view.current_terminal = terminal;
    }
}

void update_terminal(bk7259_expression_t expression) {
    const char *name = bk7259_expression_name(expression);
    constexpr char prefix[] = "> mood: ";
    std::memcpy(s_view.mood, prefix, sizeof(prefix) - 1);
    unsigned out = sizeof(prefix) - 1;
    while (*name && out < sizeof(s_view.mood) - 1) {
        const char letter = *name++;
        s_view.mood[out++] = letter >= 'A' && letter <= 'Z' ? letter + 'a' - 'A' : letter;
    }
    s_view.mood[out] = '\0';
    /* The owned buffer address is stable; explicitly refresh its changed bytes. */
    lv_label_set_text_static(s_view.terminal[1], s_view.mood);
}

void update_runtime_terminal(const mybot_lcd_content_t &content,
                             const bk7259_lcd_runtime_status_t &status) {
    const bool conversation =
        status.in_conversation && content.screen == MYBOT_LCD_SCREEN_IN_CONVERSATION;
    const char *state = status.state;
    const char *voiceprint = "";
    if (conversation) {
        /* A malformed multi-phase mask has a deterministic display priority. */
        if (content.indicators & MYBOT_LCD_INDICATOR_SPEAKING) {
            state = "speaking";
        } else if (content.indicators & MYBOT_LCD_INDICATOR_THINKING) {
            state = "thinking";
        } else if (content.indicators & MYBOT_LCD_INDICATOR_LISTENING) {
            state = "listening";
        } else {
            state = "in_conversation";
        }
        voiceprint =
            content.indicators & MYBOT_LCD_INDICATOR_VP_REGISTERED ? " | vp:ok" : " | vp:pending";
    }
    char text[kStateLineCapacity]{};
    std::snprintf(text, sizeof(text), "> state: %s%s", state, voiceprint);
    if (std::strcmp(text, s_view.state_line) != 0) {
        std::memcpy(s_view.state_line, text, sizeof(s_view.state_line));
        /* Static labels retain the address; explicitly invalidate changed bytes. */
        lv_label_set_text_static(s_view.terminal[2], s_view.state_line);
    }
}

void render_expression(uint32_t now) {
    unsigned progress = 255;
#if CONFIG_MYBOT_LVGL_UI_ANIMATIONS
    if (s_view.transitioning) {
        const uint32_t elapsed = now - s_view.transition_started;
        progress = elapsed >= kTransitionMs ? 255u : elapsed * 255u / kTransitionMs;
    }
#endif
    auto &current = s_view.banks[s_view.active_bank];
    const auto &config = pixel_shapes::kConfigs[s_view.expression];
    const unsigned brightness =
        pixel_shapes::brightness(s_view.expression, now, s_view.transition_started);
    unsigned alpha = progress * brightness / 1000u;
    if (alpha > 255)
        alpha = 255;
    pixel_shapes::paint(current, now, static_cast<uint8_t>(alpha), brightness,
                        static_cast<uint8_t>(progress));
    const unsigned shade = brightness > 1000 ? 255 : brightness * 255u / 1000u;
    const uint32_t target_ink = blend_color(kBackground, config.ink, shade);
    const uint32_t target_terminal = blend_color(kBackground, kTerminal, shade);
    uint32_t old_ink = kBackground, old_terminal = kBackground;
    auto &previous = s_view.banks[1 - s_view.active_bank];
    if (progress < 255 && s_view.previous_face) {
        const unsigned previous_brightness =
            pixel_shapes::brightness(previous.expression, now, previous.changed_at);
        unsigned previous_alpha = (255u - progress) * previous_brightness / 1000u;
        if (previous_alpha > 255)
            previous_alpha = 255;
        pixel_shapes::paint(previous, now, static_cast<uint8_t>(previous_alpha),
                            previous_brightness, static_cast<uint8_t>(255u - progress));
        const unsigned previous_shade =
            previous_brightness > 1000 ? 255 : previous_brightness * 255u / 1000u;
        old_ink = blend_color(kBackground, pixel_shapes::kConfigs[previous.expression].ink,
                              previous_shade);
        old_terminal = blend_color(kBackground, kTerminal, previous_shade);
    }
    paint_frame(blend_color(old_ink, target_ink, progress),
                blend_color(old_terminal, target_terminal, progress));
    if (progress == 255) {
        s_view.transitioning = false;
        set_hidden(previous.root, true);
    }
}

void update_timer() {
#if CONFIG_MYBOT_LVGL_UI_ANIMATIONS
    if (s_view.factory_reset || !s_view.face_visible) {
        lv_timer_pause(s_view.timer);
        return;
    }
    const uint32_t now = lv_tick_get();
    const bool flash = (pixel_shapes::kConfigs[s_view.expression].motion & pixel_shapes::Flash) &&
                       (uint32_t)(now - s_view.transition_started) < 90;
    const uint32_t period =
        s_view.transitioning || pixel_shapes::animated(s_view.expression) || flash
            ? kTransitionTickMs
            : kCursorMs - now % kCursorMs;
    lv_timer_set_period(s_view.timer, period);
    if (lv_timer_get_paused(s_view.timer)) {
        lv_timer_reset(s_view.timer);
        lv_timer_resume(s_view.timer);
    }
#else
    lv_timer_pause(s_view.timer);
#endif
}

void timer_callback(lv_timer_t *) {
#if CONFIG_MYBOT_LVGL_UI_ANIMATIONS
    const uint32_t now = lv_tick_get();
    if (s_view.transitioning || pixel_shapes::animated(s_view.expression))
        render_expression(now);
    set_hidden(s_view.cursor, (now / kCursorMs) % 2 != 0);
#endif
    update_timer();
}

void show_face(bk7259_expression_t expression) {
    const bool was_visible = s_view.face_visible;
    set_hidden(s_view.guide, true);
    set_hidden(s_view.face, false);
    set_hidden(s_view.cursor, false);
    s_view.face_visible = true;
    s_view.previous_face = was_visible;
    s_view.active_bank = 1 - s_view.active_bank;
    auto &bank = s_view.banks[s_view.active_bank];
    pixel_shapes::configure(bank, expression);
    bank.changed_at = lv_tick_get();
    s_view.expression = expression;
    s_view.transition_started = bank.changed_at;
    update_terminal(expression);
    lv_obj_set_y(s_view.actor, scale(pixel_shapes::kConfigs[expression].offset_y));
    set_hidden(bank.root, false);
    /* Keep current shapes above previous shapes without a transform/layer. */
    lv_obj_move_to_index(bank.root, -1);
    set_hidden(s_view.banks[1 - s_view.active_bank].root, !was_visible);
#if CONFIG_MYBOT_LVGL_UI_ANIMATIONS
    s_view.transitioning = true;
#else
    s_view.transitioning = false;
#endif
    render_expression(lv_tick_get());
    update_timer();
}

void show_guide(const Presentation &presentation) {
    s_view.face_visible = false;
    s_view.transitioning = false;
    lv_obj_set_y(s_view.actor, 0);
    update_terminal(BK7259_EXPRESSION_NEUTRAL);
    set_hidden(s_view.face, true);
    set_hidden(s_view.cursor, true);
    set_hidden(s_view.guide, false);
    set_hidden(s_view.code, true);
    set_hidden(s_view.detail, true);
    set_text(s_view.title, presentation.title);
    set_text(s_view.notice, presentation.notice);
    lv_obj_set_style_text_color(s_view.title, lv_color_hex(presentation.ink), 0);
    lv_obj_set_style_text_color(s_view.signature, lv_color_hex(presentation.ink), 0);
    lv_obj_set_style_bg_color(s_view.divider, lv_color_hex(presentation.ink), 0);
    /* Guide chrome can change independently while the hidden face retains its
     * old color. Force a complete primitive repaint when returning to a face. */
    s_view.current_ink = 0xffffffffu;
    for (auto *label : s_view.terminal) {
        lv_obj_set_style_text_color(label, lv_color_hex(kTerminal), 0);
    }
    s_view.current_terminal = kTerminal;
    update_timer();
}

bool create_face() {
    s_view.face = make_panel(s_view.actor, 0, 0, s_view.artwork_width, s_view.artwork_height,
                             kBackground, LV_OPA_TRANSP);
    if (!s_view.face) {
        return false;
    }
    unsigned part = 0;
    for (unsigned eye = 0; eye < 2; ++eye) {
        const int x = eye == 0 ? 214 : 534;
        s_view.frame[part++] = make_artwork_panel(s_view.face, x + 16, 354, 245, 11, kNeutral);
        s_view.frame[part++] = make_artwork_panel(s_view.face, x + 5, 365, 11, 204, kNeutral);
        s_view.frame[part++] = make_artwork_panel(s_view.face, x + 261, 365, 11, 204, kNeutral);
        s_view.frame[part++] = make_artwork_panel(s_view.face, x + 16, 569, 245, 11, kNeutral);
        s_view.frame[part++] = make_artwork_panel(s_view.face, x + 28, 581, 220, 5, kNeutral);
    }
    s_view.frame[part++] = make_artwork_panel(s_view.face, 486, 392, 48, 12, kNeutral);
    s_view.frame[part++] = make_artwork_panel(s_view.face, 204, 377, 12, 13, kNeutral);
    s_view.frame[part++] = make_artwork_panel(s_view.face, 806, 377, 12, 13, kNeutral);
    for (auto *object : s_view.frame) {
        if (!object) {
            return false;
        }
    }
    for (auto &bank : s_view.banks) {
        if (!pixel_shapes::create(bank, s_view.face, s_view.artwork_width, s_view.artwork_height))
            return false;
    }
    return true;
}

bool create_chrome() {
    for (unsigned i = 0; i < 3; ++i) {
        const int radius = i == 1 ? 19 : 13;
        lv_obj_t *dot =
            make_artwork_panel(s_view.artwork, 390 + static_cast<int>(i) * 122 - radius,
                               90 - radius, radius * 2, radius * 2, i == 1 ? 0x151923 : 0x595d5f);
        if (!dot) {
            return false;
        }
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    }
    s_view.actor = make_panel(s_view.artwork, 0, 0, s_view.artwork_width, s_view.artwork_height,
                              kBackground, LV_OPA_TRANSP);
    if (!s_view.actor)
        return false;
    for (unsigned line = 0; line < 4; ++line) {
        s_view.terminal[line] = make_label(s_view.actor, 104, 108 + static_cast<int>(line) * 37,
                                           650, 40, &bk7259_terminal_font_15, kTerminal);
        if (!s_view.terminal[line])
            return false;
        lv_obj_set_style_text_align(s_view.terminal[line], LV_TEXT_ALIGN_LEFT, 0);
    }
    s_view.signature =
        make_label(s_view.actor, 795, 112, 145, 40, &bk7259_terminal_font_16, kNeutral);
    /* Place the terminal cursor from the actual fixed-width font advance. */
    lv_point_t extent{};
    lv_text_get_size(&extent, "> agora / realtime", &bk7259_terminal_font_15, 0, 0, LV_COORD_MAX,
                     LV_TEXT_FLAG_NONE);
    s_view.cursor = make_panel(s_view.actor, scale(104) + extent.x + scale(8), scale(219 + 9),
                               positive_scale(13), positive_scale(24), kNeutral);
    if (!s_view.signature || !s_view.cursor) {
        return false;
    }
    lv_obj_set_style_text_align(s_view.signature, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_text_static(s_view.terminal[0], "> OIM / R2");
    lv_label_set_text_static(s_view.terminal[3], "> agora / realtime");
    update_terminal(BK7259_EXPRESSION_NEUTRAL);
    lv_label_set_text_static(s_view.signature, "</>");
    return true;
}

bool create_guide() {
    s_view.guide = make_panel(s_view.artwork, 0, 0, s_view.artwork_width, s_view.artwork_height,
                              kBackground, LV_OPA_TRANSP);
    if (!s_view.guide) {
        return false;
    }
    s_view.divider = make_artwork_panel(s_view.guide, 104, 292, 816, 2, kNeutral);
    s_view.title = make_label(s_view.guide, 72, 350, 880, 116, &mybot_lvgl_font_20, kNeutral);
    s_view.code = make_label(s_view.guide, 72, 478, 880, 92, &lv_font_montserrat_32, kGreeting);
    s_view.detail = make_label(s_view.guide, 72, 478, 880, 130, &mybot_lvgl_font_20, kNeutral);
    s_view.notice = make_label(s_view.guide, 72, 660, 880, 130, &mybot_lvgl_font_20, kMuted);
    return s_view.divider && s_view.title && s_view.code && s_view.detail && s_view.notice;
}

void cancel_button_gesture(bool wait_release) {
    s_view.gesture_button = nullptr;
    s_view.gesture_cancelled = true;
    lv_indev_t *input = lv_indev_active();
    if (wait_release && input && lv_indev_get_type(input) == LV_INDEV_TYPE_POINTER) {
        lv_indev_wait_release(input);
    }
}

void show_settings(SettingsPage page) {
    cancel_button_gesture(true);
    s_view.settings_page = page;
    set_hidden(s_view.settings_overlay, page == SettingsPage::Closed);
    set_hidden(s_view.settings_menu, page != SettingsPage::Menu);
    set_hidden(s_view.settings_confirm, page != SettingsPage::Confirm);
}

bool action_available(ButtonAction action) {
    if (!s_view.touch_available) {
        return false;
    }
    if (s_view.factory_reset) {
        return action == ButtonAction::Retry && s_view.reset_screen == BK7259_FACTORY_RESET_FAILED;
    }
    switch (action) {
    case ButtonAction::Settings:
        return s_view.settings_page == SettingsPage::Closed;
    case ButtonAction::Return:
    case ButtonAction::ResetMenu:
        return s_view.settings_page == SettingsPage::Menu;
    case ButtonAction::Cancel:
    case ButtonAction::Confirm:
        return s_view.settings_page == SettingsPage::Confirm;
    case ButtonAction::Retry:
        return false;
    }
    return false;
}

void perform_button_action(ButtonAction action) {
    switch (action) {
    case ButtonAction::Settings:
        show_settings(SettingsPage::Menu);
        break;
    case ButtonAction::ResetMenu:
        show_settings(SettingsPage::Confirm);
        break;
    case ButtonAction::Return:
    case ButtonAction::Cancel:
        show_settings(SettingsPage::Closed);
        break;
    case ButtonAction::Confirm:
    case ButtonAction::Retry:
        /* Lock locally before posting. The owner may not publish its recovery
         * snapshot until the next control tick; no second request can escape. */
        bk7259_lvgl_view_show_factory_reset(BK7259_FACTORY_RESET_RESTORING);
        bk7259_factory_reset_request();
        break;
    }
}

bool moved_too_far(const lv_point_t &point) {
    const int32_t dx = point.x - s_view.gesture_start.x;
    const int32_t dy = point.y - s_view.gesture_start.y;
    return dx * dx + dy * dy > kTapSlop * kTapSlop;
}

void button_event(lv_event_t *event) {
    const lv_event_code_t code = lv_event_get_code(event);
    auto *button = static_cast<lv_obj_t *>(lv_event_get_current_target(event));
    if (code == LV_EVENT_PRESS_LOST || code == LV_EVENT_CANCEL || code == LV_EVENT_DELETE) {
        if (s_view.gesture_button == button) {
            cancel_button_gesture(false);
        }
        return;
    }
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING && code != LV_EVENT_RELEASED) {
        return;
    }
    auto action =
        static_cast<ButtonAction>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    lv_indev_t *input = lv_indev_active();
    if (!input || lv_indev_get_type(input) != LV_INDEV_TYPE_POINTER || !action_available(action)) {
        cancel_button_gesture(false);
        return;
    }
    lv_point_t point{};
    lv_indev_get_point(input, &point);
    if (code == LV_EVENT_PRESSED) {
        s_view.gesture_button = button;
        s_view.gesture_start = point;
        s_view.gesture_cancelled = false;
        return;
    }
    if (s_view.gesture_button != button) {
        return;
    }
    if (moved_too_far(point)) {
        s_view.gesture_cancelled = true;
    }
    if (code == LV_EVENT_RELEASED) {
        lv_area_t bounds{};
        lv_obj_get_coords(button, &bounds);
        const bool valid = !s_view.gesture_cancelled && point.x >= bounds.x1 &&
                           point.x <= bounds.x2 && point.y >= bounds.y1 && point.y <= bounds.y2;
        cancel_button_gesture(true);
        if (valid) {
            perform_button_action(action);
        }
    }
}

lv_obj_t *make_ui_label(lv_obj_t *parent, int x, int y, int width, int height, const char *text,
                        uint32_t ink) {
    lv_obj_t *label = lv_label_create(parent);
    if (!label) {
        return nullptr;
    }
    prepare_object(label, x, y, width, height);
    lv_obj_set_style_text_font(label, &mybot_lvgl_font_20, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(ink), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_label_set_text_static(label, text);
    return label;
}

lv_obj_t *make_button(lv_obj_t *parent, int x, int y, int width, int height, const char *text,
                      ButtonAction action, bool destructive = false) {
    lv_obj_t *button = make_panel(parent, x, y, width, height, destructive ? 0x552b2c : 0x202a2b);
    if (!button) {
        return nullptr;
    }
    lv_obj_add_flag(button,
                    static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK));
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(destructive ? kError : kTerminal), 0);
    lv_obj_set_style_radius(button, 8, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(destructive ? 0x733b3c : 0x354849),
                              LV_STATE_PRESSED);
    lv_obj_set_style_opa(button, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_add_event_cb(button, button_event, LV_EVENT_ALL,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(action)));
    if (!make_ui_label(button, 0, (height - mybot_lvgl_font_20.line_height) / 2, width,
                       mybot_lvgl_font_20.line_height, text, kNeutral)) {
        return nullptr;
    }
    return button;
}

void update_touch_controls() {
    set_hidden(s_view.settings_button, s_view.factory_reset);
    set_hidden(s_view.touch_hint, s_view.touch_available || s_view.factory_reset);
    const bool failed = s_view.factory_reset && s_view.reset_screen == BK7259_FACTORY_RESET_FAILED;
    set_hidden(s_view.retry_button, !failed);
    if (s_view.touch_available) {
        lv_obj_remove_state(s_view.settings_button, LV_STATE_DISABLED);
        lv_obj_remove_state(s_view.retry_button, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(s_view.settings_button, LV_STATE_DISABLED);
        lv_obj_add_state(s_view.retry_button, LV_STATE_DISABLED);
    }
    if (failed) {
        set_text(s_view.notice, s_view.touch_available
                                    ? kResetScreens[BK7259_FACTORY_RESET_FAILED].notice
                                    : kNoTouch);
    }
}

bool create_settings(int width, int height) {
    s_view.settings_button =
        make_button(s_view.root, width - 80, 12, 64, 48, kSettings, ButtonAction::Settings);
    s_view.touch_hint = make_ui_label(s_view.root, width - 190, 70, 174, 30, kNoTouch, kMuted);
    s_view.retry_button = make_button(s_view.root, (width - 184) / 2, (height - 56) / 2 + 72, 184,
                                      56, kRetry, ButtonAction::Retry, true);
    s_view.settings_overlay = make_panel(s_view.root, 0, 0, width, height, 0x000000, LV_OPA_90);
    if (!s_view.settings_button || !s_view.touch_hint || !s_view.retry_button ||
        !s_view.settings_overlay) {
        return false;
    }
    lv_obj_add_flag(s_view.settings_overlay,
                    static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK));
    constexpr int panel_width = 464;
    constexpr int panel_height = 316;
    s_view.settings_menu =
        make_panel(s_view.settings_overlay, (width - panel_width) / 2, (height - panel_height) / 2,
                   panel_width, panel_height, 0x151b1d);
    s_view.settings_confirm =
        make_panel(s_view.settings_overlay, (width - panel_width) / 2, (height - panel_height) / 2,
                   panel_width, panel_height, 0x151b1d);
    if (!s_view.settings_menu || !s_view.settings_confirm) {
        return false;
    }
    lv_obj_t *panels[] = {s_view.settings_menu, s_view.settings_confirm};
    for (auto *panel : panels) {
        lv_obj_set_style_radius(panel, 12, 0);
        lv_obj_set_style_border_width(panel, 1, 0);
        lv_obj_set_style_border_color(panel, lv_color_hex(kTerminal), 0);
    }
    if (!make_ui_label(s_view.settings_menu, 24, 22, 416, 36, kSettings, kNeutral) ||
        !make_button(s_view.settings_menu, 40, 92, 384, 64, kReset, ButtonAction::ResetMenu,
                     true) ||
        !make_button(s_view.settings_menu, 40, 204, 384, 56, kReturn, ButtonAction::Return) ||
        !make_ui_label(s_view.settings_confirm, 24, 22, 416, 36, kConfirmTitle, kNeutral) ||
        !make_button(s_view.settings_confirm, 32, 224, 184, 56, kCancel, ButtonAction::Cancel) ||
        !make_button(s_view.settings_confirm, 248, 224, 184, 56, kConfirm, ButtonAction::Confirm,
                     true)) {
        return false;
    }
    lv_obj_t *description =
        make_ui_label(s_view.settings_confirm, 32, 78, 400, 112, kConfirmNotice, kMuted);
    if (!description) {
        return false;
    }
    lv_obj_set_style_text_align(description, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_line_space(description, 6, 0);
    show_settings(SettingsPage::Closed);
    update_touch_controls();
    return true;
}

} // namespace

int mybot_lvgl_view_create(lv_display_t *display) {
    if (!display || s_view.root) {
        return -1;
    }
    const int width = lv_display_get_horizontal_resolution(display);
    const int height = lv_display_get_vertical_resolution(display);
    if (width < 240 || height < 240) {
        return -1;
    }
    /* At 640x480 the source artwork scales uniformly by 9/16 to 576x504
     * at (32, -12). Only unused top/bottom artwork space is clipped, so the
     * glasses fill the R2 panel without moving text away from the source grid. */
    s_view.artwork_width = width - 64;
    s_view.artwork_height = s_view.artwork_width * kArtworkHeight / kArtworkWidth;
    s_view.current_ink = kNeutral;
    s_view.root =
        make_panel(lv_display_get_screen_active(display), 0, 0, width, height, kBackground);
    if (!s_view.root) {
        mybot_lvgl_view_destroy();
        return -1;
    }
    /* A drag beginning on artwork stays on that background until release;
     * sliding into Settings must never become a fresh button press. */
    lv_obj_add_flag(s_view.root,
                    static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK));
    s_view.artwork = make_panel(s_view.root, (width - s_view.artwork_width) / 2,
                                (height - s_view.artwork_height) / 2, s_view.artwork_width,
                                s_view.artwork_height, kBackground, LV_OPA_TRANSP);
    if (!s_view.artwork || !create_chrome() || !create_face() || !create_guide() ||
        !create_settings(width, height)) {
        mybot_lvgl_view_destroy();
        return -1;
    }
    s_view.timer = lv_timer_create(timer_callback, kTransitionTickMs, nullptr);
    if (!s_view.timer) {
        mybot_lvgl_view_destroy();
        return -1;
    }
    lv_timer_pause(s_view.timer);
    const mybot_lcd_content_t initial = {MYBOT_LCD_SCREEN_STARTING, {}, 0};
    mybot_lvgl_view_update(&initial, nullptr, nullptr);
    return 0;
}

void mybot_lvgl_view_update(const mybot_lcd_content_t *content, const char *ssid,
                            const bk7259_lcd_runtime_status_t *status) {
    if (!s_view.root || !content || s_view.factory_reset ||
        content->screen < MYBOT_LCD_SCREEN_STARTING || content->screen >= MYBOT_LCD_SCREEN_COUNT) {
        return;
    }
    mybot_lcd_content_t normalized{};
    normalized.screen = content->screen;
    normalized.indicators = content->indicators & kSupportedIndicators;
    bk7259_lcd_runtime_status_t copied_status{};
    if (status) {
        for (size_t i = 0; i < sizeof(copied_status.state) - 1 && status->state[i]; ++i) {
            copied_status.state[i] = status->state[i];
        }
        copied_status.in_conversation = status->in_conversation;
    }
    if (!copied_status.state[0]) {
        std::memcpy(copied_status.state, "stopped", sizeof("stopped"));
    }
    const bool pairing_code = content->screen == MYBOT_LCD_SCREEN_PAIR_CODE;
    const bool provisioning = content->screen == MYBOT_LCD_SCREEN_WIFI_PROVISIONING;
    if (pairing_code) {
        std::memcpy(normalized.pair_code, content->pair_code, sizeof(normalized.pair_code) - 1);
    }
    char copied_ssid[kSsidCapacity]{};
    if (provisioning && ssid) {
        for (size_t i = 0; i < sizeof(copied_ssid) - 1 && ssid[i]; ++i) {
            copied_ssid[i] = ssid[i];
        }
    }
    const bool workflow_changed =
        !s_view.has_content || normalized.screen != s_view.previous.screen ||
        std::strcmp(normalized.pair_code, s_view.previous.pair_code) != 0 ||
        std::strcmp(copied_ssid, s_view.provisioning_ssid) != 0;
    const bool status_changed =
        !s_view.has_content || normalized.indicators != s_view.previous.indicators ||
        copied_status.in_conversation != s_view.runtime_status.in_conversation ||
        std::strcmp(copied_status.state, s_view.runtime_status.state) != 0;
    if (!workflow_changed && !status_changed) {
        return;
    }
    update_runtime_terminal(normalized, copied_status);
    /* The display owner chooses face expressions after updating this snapshot.
     * Workflow/status changes never insert an intermediate default face or
     * restart either expression clock. Guidance still replaces a visible face. */
    if (workflow_changed && content->screen != MYBOT_LCD_SCREEN_READY &&
        content->screen != MYBOT_LCD_SCREEN_IN_CONVERSATION) {
        show_guide(kScreens[content->screen]);
        if (provisioning) {
            std::memcpy(s_view.provisioning_ssid, copied_ssid, sizeof(s_view.provisioning_ssid));
            lv_label_set_text_static(s_view.detail, s_view.provisioning_ssid);
            set_text(s_view.notice, kPortalNotice);
            set_hidden(s_view.detail, !copied_ssid[0]);
        }
        if (pairing_code) {
            lv_point_t extent{};
            lv_text_get_size(&extent, normalized.pair_code, &lv_font_montserrat_32, 0, 0,
                             LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            lv_obj_set_style_text_font(s_view.code,
                                       extent.x <= lv_obj_get_width(s_view.code)
                                           ? &lv_font_montserrat_32
                                           : &mybot_lvgl_font_20,
                                       0);
            std::memcpy(s_view.pair_code, normalized.pair_code, sizeof(s_view.pair_code));
            lv_label_set_text_static(s_view.code, s_view.pair_code);
            set_hidden(s_view.code, false);
        }
    }
    s_view.previous = normalized;
    s_view.runtime_status = copied_status;
    std::memcpy(s_view.provisioning_ssid, copied_ssid, sizeof(s_view.provisioning_ssid));
    s_view.has_content = true;
}

int bk7259_lvgl_view_show_expression(bk7259_expression_t expression) {
    if (!s_view.root || !s_view.has_content || s_view.factory_reset ||
        (s_view.previous.screen != MYBOT_LCD_SCREEN_READY &&
         s_view.previous.screen != MYBOT_LCD_SCREEN_IN_CONVERSATION) ||
        expression < BK7259_EXPRESSION_NEUTRAL || expression >= BK7259_EXPRESSION_COUNT)
        return -1;
    if (!s_view.face_visible || expression != s_view.expression)
        show_face(expression);
    return 0;
}

int bk7259_lvgl_view_replay_expression(bk7259_expression_t expression) {
    if (!s_view.root || !s_view.has_content || s_view.factory_reset ||
        (s_view.previous.screen != MYBOT_LCD_SCREEN_READY &&
         s_view.previous.screen != MYBOT_LCD_SCREEN_IN_CONVERSATION) ||
        expression < BK7259_EXPRESSION_NEUTRAL || expression >= BK7259_EXPRESSION_COUNT)
        return -1;
    show_face(expression);
    return 0;
}

void bk7259_lvgl_view_show_factory_reset(bk7259_factory_reset_screen_t screen) {
    if (!s_view.root || screen < BK7259_FACTORY_RESET_RESTORING ||
        screen > BK7259_FACTORY_RESET_REBOOTING) {
        return;
    }
    s_view.factory_reset = true;
    s_view.reset_screen = screen;
    show_settings(SettingsPage::Closed);
    show_guide(kResetScreens[screen]);
    /* Recovery is product-owned; do not retain phase/VP text from its session. */
    lv_label_set_text_static(s_view.terminal[2], "> state: factory_reset");
    update_touch_controls();
}

void bk7259_lvgl_view_set_touch_available(bool available) {
    if (!s_view.root || s_view.touch_available == available) {
        return;
    }
    s_view.touch_available = available;
    if (!available) {
        show_settings(SettingsPage::Closed);
    }
    update_touch_controls();
}

void mybot_lvgl_view_destroy(void) {
    /* The same display owner serializes timer callbacks and destruction. */
    if (s_view.timer) {
        lv_timer_delete(s_view.timer);
    }
    if (s_view.root) {
        lv_obj_delete(s_view.root);
    }
    s_view = View{};
}
