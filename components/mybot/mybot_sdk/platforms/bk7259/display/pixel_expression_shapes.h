/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Project Contributors */
/* Integer geometry follows the product simulator's src/expressions.js geek
 * face. Filled shapes use five small immutable A8 masks; no full-frame bitmap,
 * canvas, transformed group layer, or runtime object allocation is required. */
#ifndef BK7259_PIXEL_EXPRESSION_SHAPES_H_
#define BK7259_PIXEL_EXPRESSION_SHAPES_H_

#include "pixel_expression_masks.h"

namespace pixel_shapes {

enum class Eye { Dot, Dash, Arc, Down, Capsule, Slant, SlantUp, Cross, Heart, Star, Orbit };
enum class Mouth {
    Smile,
    TinySmile,
    Frown,
    TinyFrown,
    Open,
    Yawn,
    Dot,
    Smirk,
    Wave,
    Dots,
    Flat,
    Soft,
    Side,
    None
};
enum Motion { Still = 0, Bounce = 1, Drift = 2, Orbit = 4, Yawn = 8, Breathe = 16, Flash = 32 };
struct Config {
    Eye eye[2];
    Mouth mouth;
    uint32_t ink;
    unsigned brightness;
    unsigned motion;
    int offset_y;
    bool cheeks;
    bool tear;
};
constexpr Config kConfigs[] = {
    {{Eye::Arc, Eye::Arc}, Mouth::Smile, 0xf1f1ee, 1000, Still, 0, false, false},
    {{Eye::Arc, Eye::Dot}, Mouth::Smile, 0xf1ca79, 1000, Still, 0, false, false},
    {{Eye::Arc, Eye::Arc}, Mouth::Smile, 0xf1ca79, 1000, Still, 0, false, false},
    {{Eye::Star, Eye::Star}, Mouth::Open, 0xffd276, 1000, Bounce, 0, false, false},
    {{Eye::Dot, Eye::Capsule}, Mouth::Dot, 0xf1f1ee, 1000, Still, 0, false, false},
    {{Eye::Slant, Eye::Dot}, Mouth::Wave, 0xf1f1ee, 1000, Still, 0, false, false},
    {{Eye::Dash, Eye::Dot}, Mouth::Side, 0x8fcbdc, 1000, Drift, 0, false, false},
    {{Eye::Dot, Eye::Dot}, Mouth::Soft, 0xf1f1ee, 1000, Still, 18, false, false},
    {{Eye::Capsule, Eye::Capsule}, Mouth::Open, 0xf1f1ee, 1000, Flash, 0, false, false},
    {{Eye::Arc, Eye::Arc}, Mouth::TinySmile, 0xe3a6b8, 1000, Still, 0, true, false},
    {{Eye::SlantUp, Eye::SlantUp}, Mouth::Smirk, 0xf1f1ee, 1000, Still, 0, false, false},
    {{Eye::Heart, Eye::Heart}, Mouth::Smile, 0xef9fb5, 1000, Still, 0, false, false},
    {{Eye::Dash, Eye::Dash}, Mouth::Yawn, 0x9caccc, 480, Yawn, 28, false, false},
    {{Eye::Dash, Eye::Dash}, Mouth::Flat, 0xf1f1ee, 1000, Drift, 0, false, false},
    {{Eye::Down, Eye::Down}, Mouth::Frown, 0x9caccc, 620, Still, 28, false, false},
    {{Eye::Capsule, Eye::Capsule}, Mouth::TinyFrown, 0x9ccce6, 1000, Still, 0, false, true},
    {{Eye::Down, Eye::Down}, Mouth::TinyFrown, 0xf1f1ee, 760, Still, 28, false, false},
    {{Eye::Dot, Eye::Dot}, Mouth::Wave, 0xe6c17e, 1000, Still, 0, false, false},
    {{Eye::Slant, Eye::Slant}, Mouth::Flat, 0xc8b198, 1000, Still, 0, false, false},
    {{Eye::Cross, Eye::Cross}, Mouth::Flat, 0xc0b6a3, 1000, Still, 0, false, false},
    {{Eye::Orbit, Eye::Orbit}, Mouth::Dots, 0x8fcbdc, 1000, Orbit, 0, false, false},
    {{Eye::Capsule, Eye::Capsule}, Mouth::Flat, 0xe57876, 1000, Flash, 0, false, false},
    {{Eye::Dash, Eye::Dash}, Mouth::None, 0x7588a5, 450, Breathe, 0, false, false},
};
static_assert(sizeof(kConfigs) / sizeof(kConfigs[0]) == BK7259_EXPRESSION_COUNT,
              "Every simulator expression needs geometry");
constexpr unsigned kPoints = 33;
constexpr int kPolygonCenter = 80;
constexpr int kLineCenter = 100;
template <unsigned Capacity> struct Stroke {
    lv_obj_t *object;
    lv_point_precise_t points[Capacity];
    unsigned count;
};
using Line = Stroke<17>;
using MouthLine = Stroke<kPoints>;
struct Polygon {
    lv_obj_t *object;
    const pixel_masks::Mask *mask;
    int zoom_x;
    int zoom_y;
    int center;
    uint32_t ink;
    uint8_t opacity;
};
struct Bank {
    lv_obj_t *root;
    Line eyes[2][2];
    lv_obj_t *eye_dots[2];
    Polygon eye_polygons[2];
    MouthLine mouth;
    Polygon mouth_polygon;
    lv_obj_t *mouth_dots[3];
    Polygon cheeks[2];
    Polygon tear;
    bk7259_expression_t expression;
    uint32_t changed_at;
    uint32_t painted_ink;
    uint32_t painted_line_ink;
    uint8_t painted_opacity;
    uint8_t painted_line_opacity;
    int width;
    int height;
    bool configured;
};

inline int scale(const Bank &bank, int source) { return (source * bank.width + 512) / 1024; }
inline int scale100(const Bank &bank, int source) { return (source * bank.width + 51200) / 102400; }
inline void hide(lv_obj_t *object, bool hidden) {
    if (lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN) == hidden)
        return;
    if (hidden)
        lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
}
inline int sine(uint32_t now, uint32_t period, unsigned phase = 0) {
    return lv_trigo_sin(static_cast<int16_t>((now % period) * 360u / period + phase));
}
inline unsigned brightness(bk7259_expression_t expression, uint32_t now, uint32_t changed_at) {
    const Config &config = kConfigs[expression];
    unsigned value = config.brightness;
#if CONFIG_MYBOT_LVGL_UI_ANIMATIONS
    if (config.motion & Breathe) {
        const int wave = sine(now, 5027);
        value = value * (720 + wave * 200 / 32768) / 1000;
        value = value * (650 + wave * 140 / 32768) / 1000;
    }
    if ((config.motion & Flash) && (uint32_t)(now - changed_at) < 90)
        value = 1500;
#else
    (void)now;
    (void)changed_at;
    if (config.motion & Breathe)
        value = value * 720u * 650u / 1000000u;
#endif
    return value;
}
inline bool animated(bk7259_expression_t expression) {
    return (kConfigs[expression].motion & ~Flash) != Still;
}

inline void polygon_draw(lv_event_t *event) {
    auto *polygon = static_cast<Polygon *>(lv_event_get_user_data(event));
    if (!polygon->opacity || !polygon->mask)
        return;
    lv_area_t area;
    lv_obj_get_coords(polygon->object, &area);
    lv_draw_image_dsc_t descriptor;
    lv_draw_image_dsc_init(&descriptor);
    descriptor.src = &polygon->mask->image;
    descriptor.recolor = lv_color_hex(polygon->ink);
    descriptor.recolor_opa = LV_OPA_COVER;
    descriptor.opa = polygon->opacity;
    descriptor.scale_x = polygon->zoom_x;
    descriptor.scale_y = polygon->zoom_y;
    descriptor.pivot = {polygon->mask->center_x, polygon->mask->center_y};
    lv_area_t image;
    image.x1 = area.x1 + polygon->center - polygon->mask->center_x;
    image.y1 = area.y1 + polygon->center - polygon->mask->center_y;
    image.x2 = image.x1 + polygon->mask->image.header.w - 1;
    image.y2 = image.y1 + polygon->mask->image.header.h - 1;
    lv_draw_image(lv_event_get_layer(event), &descriptor, &image);
}
inline void prepare(lv_obj_t *object, int size) {
    lv_obj_remove_style_all(object);
    lv_obj_remove_flag(object,
                       static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_set_size(object, size, size);
}
template <unsigned Capacity>
inline bool create_line(Bank &bank, Stroke<Capacity> &line, int width) {
    line.object = lv_line_create(bank.root);
    if (!line.object)
        return false;
    prepare(line.object, scale(bank, kLineCenter * 2));
    lv_obj_set_style_line_color(line.object, lv_color_hex(0xf1f1ee), 0);
    lv_obj_set_style_line_opa(line.object, 255, 0);
    lv_obj_set_style_line_width(line.object, scale(bank, width), 0);
    lv_obj_set_style_line_rounded(line.object, true, 0);
    hide(line.object, true);
    return true;
}
inline bool create_polygon(Bank &bank, Polygon &polygon) {
    polygon.object = lv_obj_create(bank.root);
    if (!polygon.object)
        return false;
    prepare(polygon.object, scale(bank, kPolygonCenter * 2));
    lv_obj_set_style_bg_opa(polygon.object, LV_OPA_TRANSP, 0);
    if (!lv_obj_add_event_cb(polygon.object, polygon_draw, LV_EVENT_DRAW_MAIN, &polygon))
        return false;
    polygon.center = scale(bank, kPolygonCenter);
    hide(polygon.object, true);
    return true;
}
inline bool create_circle(Bank &bank, lv_obj_t *&object) {
    object = lv_obj_create(bank.root);
    if (!object)
        return false;
    prepare(object, 1);
    lv_obj_set_style_bg_color(object, lv_color_hex(0xf1f1ee), 0);
    lv_obj_set_style_bg_opa(object, 255, 0);
    lv_obj_set_style_radius(object, LV_RADIUS_CIRCLE, 0);
    hide(object, true);
    return true;
}
inline bool create(Bank &bank, lv_obj_t *parent, int width, int height) {
    bank.width = width;
    bank.height = height;
    bank.root = lv_obj_create(parent);
    if (!bank.root)
        return false;
    prepare(bank.root, width);
    lv_obj_set_size(bank.root, width, height);
    lv_obj_set_style_bg_opa(bank.root, LV_OPA_TRANSP, 0);
    for (unsigned eye = 0; eye < 2; ++eye) {
        if (!create_line(bank, bank.eyes[eye][0], 27) ||
            !create_line(bank, bank.eyes[eye][1], 27) || !create_circle(bank, bank.eye_dots[eye]) ||
            !create_polygon(bank, bank.eye_polygons[eye]) ||
            !create_polygon(bank, bank.cheeks[eye]))
            return false;
    }
    if (!create_line(bank, bank.mouth, 24) || !create_polygon(bank, bank.mouth_polygon) ||
        !create_polygon(bank, bank.tear))
        return false;
    for (auto *&dot : bank.mouth_dots)
        if (!create_circle(bank, dot))
            return false;
    hide(bank.root, true);
    return true;
}
template <unsigned Capacity>
inline void line_points(Bank &bank, Stroke<Capacity> &line, const int *coordinates,
                        unsigned count) {
    for (unsigned i = 0; i < count; ++i) {
        line.points[i].x = scale(bank, kLineCenter + coordinates[i * 2]);
        line.points[i].y = scale(bank, kLineCenter + coordinates[i * 2 + 1]);
    }
    line.count = count;
    lv_line_set_points(line.object, line.points, count);
}
template <unsigned Capacity>
inline void quadratic(Bank &bank, Stroke<Capacity> &line, int x0, int y0, int cx, int cy, int x1,
                      int y1) {
    constexpr int steps = 16;
    for (int i = 0; i <= steps; ++i) {
        const int r = steps - i;
        line.points[i].x =
            scale(bank, kLineCenter + (r * r * x0 + 2 * r * i * cx + i * i * x1) / (steps * steps));
        line.points[i].y =
            scale(bank, kLineCenter + (r * r * y0 + 2 * r * i * cy + i * i * y1) / (steps * steps));
    }
    line.count = steps + 1;
    lv_line_set_points(line.object, line.points, line.count);
}
inline int cubic100(int a, int b, int c, int d, int t) {
    const int r = 16 - t;
    return (r * r * r * a + 3 * r * r * t * b + 3 * r * t * t * c + t * t * t * d) / 4096;
}
inline void wave(Bank &bank, MouthLine &line) {
    for (unsigned half = 0; half < 2; ++half) {
        for (int t = half ? 1 : 0; t <= 16; ++t) {
            const unsigned index = half * 16 + t;
            const int x =
                half ? cubic100(0, 2200, 4200, 7000, t) : cubic100(-7000, -3500, -1500, 0, t);
            const int y = cubic100(0, -2400, 2400, 0, t);
            line.points[index].x = scale100(bank, kLineCenter * 100 + x);
            line.points[index].y = scale100(bank, kLineCenter * 100 + y);
        }
    }
    line.count = kPoints;
    lv_line_set_points(line.object, line.points, line.count);
}
inline void ellipse(Bank &bank, Polygon &polygon, int radius_x100, int radius_y100, int angle = 0) {
    (void)angle; /* The tear's exact -0.2 rad rotation is baked into its mask. */
    polygon.mask = radius_x100 == 5400   ? &pixel_masks::kCheek
                   : radius_x100 == 1300 ? &pixel_masks::kTear
                                         : &pixel_masks::kEllipse;
    polygon.zoom_x = bank.width * LV_SCALE_NONE / 576;
    polygon.zoom_y = polygon.zoom_x;
    if (radius_x100 == 3800)
        polygon.zoom_y = polygon.zoom_y * radius_y100 / 4800;
}
inline void filled_eye(Bank &bank, Polygon &polygon, Eye eye) {
    polygon.mask = eye == Eye::Star ? &pixel_masks::kStar : &pixel_masks::kHeart;
    polygon.zoom_x = polygon.zoom_y = bank.width * LV_SCALE_NONE / 576;
}
inline void configure_eye(Bank &bank, unsigned index, Eye eye) {
    Line &line = bank.eyes[index][0];
    const int dash[] = {-48, 0, 48, 0}, capsule[] = {0, -54, 0, 54};
    const int slant[] = {-48, -22, 48, 18}, proud[] = {-48, 15, 48, -26};
    const int cross_a[] = {-40, -40, 40, 40}, cross_b[] = {40, -40, -40, 40};
    hide(line.object, true);
    hide(bank.eyes[index][1].object, true);
    hide(bank.eye_dots[index], true);
    hide(bank.eye_polygons[index].object, true);
    switch (eye) {
    case Eye::Dot:
    case Eye::Orbit:
        hide(bank.eye_dots[index], false);
        break;
    case Eye::Heart:
    case Eye::Star:
        filled_eye(bank, bank.eye_polygons[index], eye);
        hide(bank.eye_polygons[index].object, false);
        break;
    case Eye::Arc:
        quadratic(bank, line, -52, 22, 0, -42, 52, 22);
        break;
    case Eye::Down:
        quadratic(bank, line, -52, -18, 0, 40, 52, -18);
        break;
    case Eye::Dash:
        line_points(bank, line, dash, 2);
        break;
    case Eye::Capsule:
        line_points(bank, line, capsule, 2);
        break;
    case Eye::Slant:
        line_points(bank, line, slant, 2);
        break;
    case Eye::SlantUp:
        line_points(bank, line, proud, 2);
        break;
    case Eye::Cross:
        line_points(bank, line, cross_a, 2);
        line_points(bank, bank.eyes[index][1], cross_b, 2);
        hide(bank.eyes[index][1].object, false);
        break;
    }
    if (eye != Eye::Dot && eye != Eye::Orbit && eye != Eye::Heart && eye != Eye::Star)
        hide(line.object, false);
}
inline void configure(Bank &bank, bk7259_expression_t expression) {
    const Config &config = kConfigs[expression];
    for (unsigned eye = 0; eye < 2; ++eye)
        configure_eye(bank, eye, config.eye[eye]);
    hide(bank.mouth.object, true);
    hide(bank.mouth_polygon.object, true);
    for (auto *dot : bank.mouth_dots)
        hide(dot, true);
    switch (config.mouth) {
    case Mouth::Smile:
        quadratic(bank, bank.mouth, -78, -8, 0, 44, 78, -8);
        break;
    case Mouth::TinySmile:
        quadratic(bank, bank.mouth, -42, -8, 0, 44, 42, -8);
        break;
    case Mouth::Frown:
        quadratic(bank, bank.mouth, -72, 18, 0, -30, 72, 18);
        break;
    case Mouth::TinyFrown:
        quadratic(bank, bank.mouth, -42, 18, 0, -30, 42, 18);
        break;
    case Mouth::Smirk:
        quadratic(bank, bank.mouth, -65, 8, 10, 28, 72, -22);
        break;
    case Mouth::Wave:
        wave(bank, bank.mouth);
        break;
    case Mouth::Open:
    case Mouth::Yawn:
        ellipse(bank, bank.mouth_polygon, 3800, config.mouth == Mouth::Open ? 4800 : 3800);
        hide(bank.mouth_polygon.object, false);
        break;
    case Mouth::Dot:
        hide(bank.mouth_dots[1], false);
        break;
    case Mouth::Dots:
        for (auto *dot : bank.mouth_dots)
            hide(dot, false);
        break;
    case Mouth::Flat:
    case Mouth::Soft:
    case Mouth::Side: {
        const int width = config.mouth == Mouth::Soft ? 54 : 70;
        const int coordinates[] = {-width, 0, width, 0};
        line_points(bank, bank.mouth, coordinates, 2);
        break;
    }
    case Mouth::None:
        break;
    }
    if (config.mouth != Mouth::None && config.mouth != Mouth::Dot && config.mouth != Mouth::Dots &&
        config.mouth != Mouth::Open && config.mouth != Mouth::Yawn)
        hide(bank.mouth.object, false);
    for (unsigned side = 0; side < 2; ++side) {
        ellipse(bank, bank.cheeks[side], 5400, 2000);
        hide(bank.cheeks[side].object, !config.cheeks);
    }
    ellipse(bank, bank.tear, 1300, 2300, -11);
    hide(bank.tear.object, !config.tear);
    bank.expression = expression;
    bank.configured = true;
    bank.painted_ink = 0xffffffffu;
    bank.painted_line_ink = 0xffffffffu;
}
template <unsigned Capacity>
inline void position_line(const Bank &bank, Stroke<Capacity> &line, int x, int y) {
    lv_obj_set_pos(line.object, scale(bank, x - kLineCenter), scale(bank, y - kLineCenter));
}
inline void position_polygon(const Bank &bank, Polygon &polygon, int x, int y, uint32_t ink,
                             uint8_t opacity) {
    lv_obj_set_pos(polygon.object, scale(bank, x - kPolygonCenter),
                   scale(bank, y - kPolygonCenter));
    polygon.ink = ink;
    polygon.opacity = opacity;
    if (!lv_obj_has_flag(polygon.object, LV_OBJ_FLAG_HIDDEN))
        lv_obj_invalidate(polygon.object);
}
inline void position_circle(const Bank &bank, lv_obj_t *circle, int x, int y, int radius,
                            uint32_t ink, uint8_t opacity) {
    lv_obj_set_pos(circle, scale(bank, x - radius), scale(bank, y - radius));
    lv_obj_set_size(circle, scale(bank, radius * 2), scale(bank, radius * 2));
    lv_obj_set_style_bg_color(circle, lv_color_hex(ink), 0);
    lv_obj_set_style_bg_opa(circle, opacity, 0);
}
inline uint32_t dim_ink(uint32_t ink, unsigned brightness) {
    if (brightness >= 1000)
        return ink;
    constexpr uint32_t background = 0x050708;
    uint32_t result = 0;
    for (unsigned shift = 0; shift < 24; shift += 8) {
        result |= ((((ink >> shift) & 255u) * brightness +
                    ((background >> shift) & 255u) * (1000u - brightness) + 500u) /
                   1000u)
                  << shift;
    }
    return result;
}
inline void paint(Bank &bank, uint32_t now, uint8_t opacity, unsigned brightness, uint8_t fade) {
    const Config &config = kConfigs[bank.expression];
    const uint32_t line_ink = dim_ink(config.ink, brightness);
    const bool line_ink_changed = bank.painted_line_ink != line_ink;
    const bool line_opacity_changed = line_ink_changed || bank.painted_line_opacity != fade;
    int drift = 0, bounce = 0, orbit = 0;
#if CONFIG_MYBOT_LVGL_UI_ANIMATIONS
    if (config.motion & Drift)
        drift = sine(now, 4189) * 26 / 32768;
    if (config.motion & Bounce)
        bounce = sine(now, 524) * 16 / 32768;
    if (config.motion & Orbit)
        orbit = sine(now, 1257) * 24 / 32768;
    if (config.motion & Yawn)
        ellipse(bank, bank.mouth_polygon, 3800, 3800 + sine(now, 2094) * 800 / 32768);
#else
    (void)now;
#endif
    for (unsigned eye = 0; eye < 2; ++eye) {
        int x = (eye ? 670 : 354) + drift;
        if (config.eye[eye] == Eye::Orbit)
            x += eye ? -orbit : orbit;
        const int y = 430 + (eye ? -bounce : bounce);
        for (auto &line : bank.eyes[eye]) {
            if (lv_obj_has_flag(line.object, LV_OBJ_FLAG_HIDDEN))
                continue;
            position_line(bank, line, x, y);
            if (line_ink_changed)
                lv_obj_set_style_line_color(line.object, lv_color_hex(line_ink), 0);
            if (line_opacity_changed)
                lv_obj_set_style_line_opa(line.object, fade, 0);
        }
        if (!lv_obj_has_flag(bank.eye_dots[eye], LV_OBJ_FLAG_HIDDEN))
            position_circle(bank, bank.eye_dots[eye], x, y, config.eye[eye] == Eye::Orbit ? 25 : 31,
                            config.ink, opacity);
        if (!lv_obj_has_flag(bank.eye_polygons[eye].object, LV_OBJ_FLAG_HIDDEN))
            position_polygon(bank, bank.eye_polygons[eye], x, y, config.ink, opacity);
        if (config.cheeks)
            position_polygon(bank, bank.cheeks[eye], eye ? 799 : 225, 575, 0xff7891,
                             opacity * 107u / 255u);
    }
    if (!lv_obj_has_flag(bank.mouth.object, LV_OBJ_FLAG_HIDDEN)) {
        position_line(bank, bank.mouth, config.mouth == Mouth::Side ? 554 : 512, 650);
        if (line_ink_changed)
            lv_obj_set_style_line_color(bank.mouth.object, lv_color_hex(line_ink), 0);
        if (line_opacity_changed)
            lv_obj_set_style_line_opa(bank.mouth.object, fade, 0);
    }
    if (!lv_obj_has_flag(bank.mouth_polygon.object, LV_OBJ_FLAG_HIDDEN))
        position_polygon(bank, bank.mouth_polygon, 512, 650, config.ink, opacity);
    for (unsigned i = 0; i < 3; ++i) {
        if (lv_obj_has_flag(bank.mouth_dots[i], LV_OBJ_FLAG_HIDDEN))
            continue;
        position_circle(bank, bank.mouth_dots[i], 512 + (int(i) - 1) * 34, 650,
                        config.mouth == Mouth::Dot ? 18 : 9, config.ink, opacity);
    }
    if (config.tear)
        position_polygon(bank, bank.tear, 742, 515, 0xdbefff, opacity);
    bank.painted_ink = config.ink;
    bank.painted_opacity = opacity;
    bank.painted_line_ink = line_ink;
    bank.painted_line_opacity = fade;
}
} // namespace pixel_shapes
#endif /* BK7259_PIXEL_EXPRESSION_SHAPES_H_ */
