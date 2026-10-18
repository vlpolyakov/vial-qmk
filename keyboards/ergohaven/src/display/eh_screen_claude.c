#include "eh_display.h"
#include <lvgl.h>

#include "ergohaven.h"
#include <string.h>

#include "lvgl_helpers.h"
#include "timer.h"
#include "eh_claude_pad.h"
#include "eh_claude_icon.h"
#include "eh_display_bounds.h"

LV_FONT_DECLARE(eh_font_montserrat_16);
LV_FONT_DECLARE(eh_font_montserrat_20);

// All coordinates are relative to the safe content area (230 x 270), which keeps
// text away from the bezel that hides the outermost pixels of the panel.
//
//   header   y   0.. 56  icon + model (line 1), task status (line 2: timer or "done" line)
//   idle     y  62..130  current task, 133..151 current activity, 155..173 limits,
//                        190..250 animated mascot
//   ask      y  62..132  question, 136..265 up to four options
#define CL_PAD 10
#define CL_INNER_W (EH_DISPLAY_CONTENT_WIDTH - 2 * CL_PAD)
#define CL_ICON EH_CLAUDE_ICON_SIZE
#define CL_HEAD_RULE_Y 56
#define CL_BODY_Y 62
#define CL_TASK_H 68
#define CL_ACT_Y 133
#define CL_ACT_H 20
#define CL_LIMITS_Y 155
#define CL_MASCOT_Y 192
#define CL_PROMPT_H 70
#define CL_OPT_Y 136
#define CL_OPT_H 30
#define CL_OPT_STEP 33

// Mascot ("Clawd") geometry: a grid of CL_Q pixel squares, 17 columns wide.
#define CL_Q 10
#define CL_MASCOT_X 25
#define CL_FRAME_MS 140
#define CL_ORANGE 0xD97757

static lv_obj_t *screen_claude;
static lv_obj_t *content;
static lv_obj_t *icon_claude;
static lv_obj_t *label_model;
static lv_obj_t *label_line;
static lv_obj_t *label_limits;
static lv_obj_t *head_rule;
static lv_obj_t *label_task;
static lv_obj_t *label_activity;
static lv_obj_t *label_prompt;
static lv_obj_t *option_box[EH_CLAUDE_MAX_OPTS];
static lv_obj_t *option_label[EH_CLAUDE_MAX_OPTS];

enum { M_BODY, M_ARM_L, M_ARM_R, M_EYE_L, M_EYE_R, M_LEG0, M_LEG1, M_LEG2, M_LEG3, M_COUNT };
static lv_obj_t *mascot[M_COUNT];
static const lv_coord_t mascot_x[M_COUNT] = {3, 1, 15, 5, 12, 4, 6, 11, 13};
static const lv_coord_t mascot_y[M_COUNT] = {0, 2, 2, 1, 1, 4, 4, 4, 4};
static const lv_coord_t mascot_w[M_COUNT] = {12, 2, 2, 1, 1, 1, 1, 1, 1};
static const lv_coord_t mascot_h[M_COUNT] = {4, 1, 1, 1, 1, 1, 1, 1, 1};

static bool     shown_busy;
static uint32_t shown_frame = UINT32_MAX;
static uint32_t shown_seconds = UINT32_MAX;

static lv_img_dsc_t claude_icon_dsc;

static lv_obj_t *make_label(lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h, const lv_font_t *font, lv_color_t color) {
    lv_obj_t *label = lv_label_create(content);
    lv_obj_set_size(label, w, h);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
    lv_obj_set_pos(label, x, y);
    return label;
}

static void set_hidden(lv_obj_t *obj, bool hidden) {
    if (hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static lv_obj_t *make_rect(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h, lv_color_t color) {
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, w, h);
    lv_obj_set_pos(r, x, y);
    lv_obj_set_style_bg_color(r, color, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_PART_MAIN);
    return r;
}

// frame < 0 is the resting pose; otherwise one step of the working animation:
// the body bobs, arms and leg pairs swing in turns, the eyes blink now and then.
static void pose_mascot(int32_t frame) {
    lv_coord_t bob = 0, arm_l = 0, arm_r = 0, leg_a = 0, leg_b = 0;
    bool       eyes = true;
    if (frame >= 0) {
        bool step = (frame % 4) < 2;
        bob       = (frame & 1) ? -4 : 0;
        arm_l     = step ? -5 : 0;
        arm_r     = step ? 0 : -5;
        leg_a     = step ? -4 : 0;
        leg_b     = step ? 0 : -4;
        eyes      = (frame % 22) != 21;
    }
    for (uint8_t i = 0; i < M_COUNT; i++) {
        lv_coord_t dy = 0;
        switch (i) {
            case M_BODY:
            case M_EYE_L:
            case M_EYE_R: dy = bob; break;
            case M_ARM_L: dy = bob + arm_l; break;
            case M_ARM_R: dy = bob + arm_r; break;
            case M_LEG0:
            case M_LEG2: dy = leg_a; break;
            default: dy = leg_b; break;
        }
        lv_obj_set_y(mascot[i], CL_MASCOT_Y + mascot_y[i] * CL_Q + dy);
    }
    lv_obj_set_style_bg_color(mascot[M_EYE_L], eyes ? lv_color_black() : lv_color_hex(CL_ORANGE), LV_PART_MAIN);
    lv_obj_set_style_bg_color(mascot[M_EYE_R], eyes ? lv_color_black() : lv_color_hex(CL_ORANGE), LV_PART_MAIN);
}

static void style_option(uint8_t index, bool selected) {
    lv_obj_set_style_bg_color(option_box[index], selected ? accent_color_blue : lv_color_hex(0x2B2B2B), LV_PART_MAIN);
    lv_obj_set_style_text_color(option_label[index], selected ? lv_color_black() : lv_color_white(), LV_PART_MAIN);
}

// Second header line: a live timer while Claude works, otherwise the line the host sent
// ("Crunched for 6s • done 23:40").
static void show_line(eh_claude_pad_t *pad, bool busy) {
    if (!busy) {
        lv_label_set_text(label_line, pad->line);
        shown_seconds = UINT32_MAX;
        return;
    }
    uint32_t sec = eh_claude_pad_busy_seconds();
    char     buf[32];
    char    *p = buf;
    memcpy(p, "Working ", 8);
    p += 8;
    char     digits[12];
    uint8_t  n = 0;
    uint32_t m = sec / 60, r = sec % 60;
    if (m > 0) {
        for (uint32_t v = m; v > 0; v /= 10) digits[n++] = (char)('0' + v % 10);
        while (n > 0) *p++ = digits[--n];
        *p++ = 'm';
        *p++ = ' ';
    }
    for (uint32_t v = r; v > 0; v /= 10) digits[n++] = (char)('0' + v % 10);
    if (n == 0) digits[n++] = '0';
    while (n > 0) *p++ = digits[--n];
    *p++ = 's';
    *p   = '\0';
    lv_label_set_text(label_line, buf);
    shown_seconds = sec;
}

static void render(void) {
    eh_claude_pad_t *pad  = eh_claude_pad_get();
    bool             ask  = pad->mode == EH_CLAUDE_MODE_ASK && pad->opt_count >= 2;
    bool             busy = eh_claude_pad_busy();

    lv_label_set_text(label_model, pad->status[0] ? pad->status : "Claude Code");
    lv_label_set_text(label_limits, pad->limits);
    show_line(pad, busy);

    if (ask) {
        set_hidden(label_task, true);
        set_hidden(label_activity, true);
        set_hidden(label_limits, true);
        for (uint8_t i = 0; i < M_COUNT; i++) set_hidden(mascot[i], true);
        set_hidden(label_prompt, false);
        lv_label_set_text(label_prompt, pad->prompt);
        for (uint8_t i = 0; i < EH_CLAUDE_MAX_OPTS; i++) {
            bool used = i < pad->opt_count;
            set_hidden(option_box[i], !used);
            if (!used) continue;
            lv_label_set_text(option_label[i], pad->opt[i]);
            style_option(i, i == pad->selected);
        }
    } else {
        set_hidden(label_task, false);
        set_hidden(label_activity, false);
        set_hidden(label_limits, false);
        set_hidden(label_prompt, true);
        for (uint8_t i = 0; i < M_COUNT; i++) set_hidden(mascot[i], false);
        for (uint8_t i = 0; i < EH_CLAUDE_MAX_OPTS; i++) set_hidden(option_box[i], true);
        lv_label_set_text(label_task, pad->task);
        lv_label_set_text(label_activity, pad->prompt);
        if (!busy) pose_mascot(-1);
    }
    shown_busy  = busy;
    shown_frame = UINT32_MAX;
}

static void screen_claude_init(void) {
    screen_claude = lv_obj_create(NULL);
    lv_obj_add_style(screen_claude, &style_screen, 0);
    lv_obj_clear_flag(screen_claude, LV_OBJ_FLAG_SCROLLABLE);
    content = eh_display_safe_content(screen_claude);

    claude_icon_dsc = (lv_img_dsc_t){
        .header.always_zero = 0,
        .header.w           = CL_ICON,
        .header.h           = CL_ICON,
        .header.cf          = LV_IMG_CF_ALPHA_1BIT,
        .data_size          = sizeof(eh_claude_icon_bits),
        .data               = eh_claude_icon_bits,
    };
    icon_claude = lv_img_create(content);
    lv_img_set_src(icon_claude, &claude_icon_dsc);
    lv_obj_set_style_img_recolor(icon_claude, lv_color_hex(CL_ORANGE), 0);
    lv_obj_set_style_img_recolor_opa(icon_claude, LV_OPA_COVER, 0);
    lv_obj_set_pos(icon_claude, CL_PAD, 6);

    lv_coord_t model_x = CL_PAD + CL_ICON + 8;
    label_model        = make_label(model_x, 5, EH_DISPLAY_CONTENT_WIDTH - model_x - CL_PAD, 26, &eh_font_montserrat_20, accent_color_blue);
    label_line         = make_label(CL_PAD, 33, CL_INNER_W, 20, &eh_font_montserrat_16, lv_color_hex(0xC8C8C8));
    head_rule          = make_rect(content, CL_PAD, CL_HEAD_RULE_Y, CL_INNER_W, 1, lv_color_hex(0x3A3A3A));

    label_task     = make_label(CL_PAD, CL_BODY_Y, CL_INNER_W, CL_TASK_H, &eh_font_montserrat_20, lv_color_white());
    label_activity = make_label(CL_PAD, CL_ACT_Y, CL_INNER_W, CL_ACT_H, &eh_font_montserrat_16, lv_color_hex(0x9A9A9A));
    label_limits   = make_label(CL_PAD, CL_LIMITS_Y, CL_INNER_W, 20, &eh_font_montserrat_16, lv_color_hex(0x707070));
    label_prompt   = make_label(CL_PAD, CL_BODY_Y, CL_INNER_W, CL_PROMPT_H, &eh_font_montserrat_20, lv_color_white());

    for (uint8_t i = 0; i < M_COUNT; i++) {
        bool eye  = i == M_EYE_L || i == M_EYE_R;
        mascot[i] = make_rect(content, CL_MASCOT_X + mascot_x[i] * CL_Q, CL_MASCOT_Y + mascot_y[i] * CL_Q, mascot_w[i] * CL_Q, mascot_h[i] * CL_Q,
                              eye ? lv_color_black() : lv_color_hex(CL_ORANGE));
    }
    // Eyes sit on top of the body.
    lv_obj_move_foreground(mascot[M_EYE_L]);
    lv_obj_move_foreground(mascot[M_EYE_R]);

    for (uint8_t i = 0; i < EH_CLAUDE_MAX_OPTS; i++) {
        option_box[i] = lv_obj_create(content);
        lv_obj_set_size(option_box[i], CL_INNER_W, CL_OPT_H);
        lv_obj_set_pos(option_box[i], CL_PAD, CL_OPT_Y + i * CL_OPT_STEP);
        lv_obj_clear_flag(option_box[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(option_box[i], 8, LV_PART_MAIN);
        lv_obj_set_style_border_width(option_box[i], 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(option_box[i], 0, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(option_box[i], LV_OPA_COVER, LV_PART_MAIN);

        option_label[i] = lv_label_create(option_box[i]);
        lv_obj_set_size(option_label[i], CL_INNER_W - 20, 24);
        lv_label_set_long_mode(option_label[i], LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(option_label[i], &eh_font_montserrat_20, LV_PART_MAIN);
        lv_obj_align(option_label[i], LV_ALIGN_LEFT_MID, 10, 0);
        style_option(i, false);
        set_hidden(option_box[i], true);
    }
    set_hidden(label_prompt, true);
}

static void screen_claude_load(void) {
    render();
    eh_claude_pad_get()->dirty = false;
    lv_scr_load(screen_claude);
    display_apply_brightness();
}

static void screen_claude_housekeep(void) {
    eh_claude_pad_t *pad = eh_claude_pad_get();
    bool busy = eh_claude_pad_busy();
    if (pad->dirty || busy != shown_busy) {
        pad->dirty = false;
        render();
    }
    if (busy && pad->mode != EH_CLAUDE_MODE_ASK) {
        if (eh_claude_pad_busy_seconds() != shown_seconds) show_line(pad, true);
        uint32_t frame = timer_read32() / CL_FRAME_MS;
        if (frame != shown_frame) {
            shown_frame = frame;
            pose_mascot((int32_t)(frame % 1000));
        }
    }
}

const eh_screen_t eh_screen_claude = {
    .init      = screen_claude_init,
    .load      = screen_claude_load,
    .housekeep = screen_claude_housekeep,
};
