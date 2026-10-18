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
// Two views share the screen:
//   status  y   0..131  icon + model + context, 5h limit, weekly limit, task status (up to 3 lines)
//           y 135       rule
//           y 137..270  the mascot, centered, fills the lower half
//   action  (question, permission, "answer in the terminal") replaces the status view:
//           y   0.. 36  icon + title, 38 rule, 44..113 prompt, 120..249 up to four options,
//           y 252       hint
#define CL_PAD 10
#define CL_INNER_W (EH_DISPLAY_CONTENT_WIDTH - 2 * CL_PAD)
#define CL_ICON EH_CLAUDE_ICON_SIZE
#define CL_RULE_Y 135
#define CL_MASCOT_Y 152
#define CL_LINE_Y 75
#define CL_LINE_H 56
#define CL_ASK_RULE_Y 38
#define CL_PROMPT_Y 44
#define CL_PROMPT_H 70
#define CL_PROMPT_WAIT_H 150
#define CL_OPT_Y 120
#define CL_OPT_H 30
#define CL_OPT_STEP 33
#define CL_HINT_Y 252

// Mascot ("Clawd") geometry: a grid of CL_QX x CL_QY pixel cells, 17 columns wide, 5 rows high.
// Columns 1..16 are used, so the figure is 16 * CL_QX = 192 px wide and centered in 230 px.
#define CL_QX 12
#define CL_QY 20
#define CL_MASCOT_X 7
#define CL_FRAME_MS 140
#define CL_ORANGE 0xD97757

enum { VIEW_STATUS, VIEW_ASK, VIEW_WAIT };

static lv_obj_t *screen_claude;
static lv_obj_t *content;

// status view
static lv_obj_t *icon_claude;
static lv_obj_t *label_model;
static lv_obj_t *label_ctx;
static lv_obj_t *label_limits;
static lv_obj_t *label_week;
static lv_obj_t *label_line;
static lv_obj_t *head_rule;

// action view
static lv_obj_t *icon_ask;
static lv_obj_t *label_title;
static lv_obj_t *ask_rule;
static lv_obj_t *label_prompt;
static lv_obj_t *label_hint;
static lv_obj_t *option_box[EH_CLAUDE_MAX_OPTS];
static lv_obj_t *option_label[EH_CLAUDE_MAX_OPTS];

enum { M_BODY, M_ARM_L, M_ARM_R, M_EYE_L, M_EYE_R, M_LEG0, M_LEG1, M_LEG2, M_LEG3, M_COUNT };
static lv_obj_t *mascot[M_COUNT];
static const lv_coord_t mascot_x[M_COUNT] = {3, 1, 15, 5, 12, 4, 6, 11, 13};
static const lv_coord_t mascot_y[M_COUNT] = {0, 2, 2, 1, 1, 4, 4, 4, 4};
static const lv_coord_t mascot_w[M_COUNT] = {12, 2, 2, 1, 1, 1, 1, 1, 1};
static const lv_coord_t mascot_h[M_COUNT] = {4, 1, 1, 1, 1, 1, 1, 1, 1};

static uint8_t  shown_view = 0xFF;
static bool     shown_busy;
static uint32_t shown_frame   = UINT32_MAX;
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

static lv_obj_t *make_icon(lv_coord_t x, lv_coord_t y) {
    lv_obj_t *icon = lv_img_create(content);
    lv_img_set_src(icon, &claude_icon_dsc);
    lv_obj_set_style_img_recolor(icon, lv_color_hex(CL_ORANGE), 0);
    lv_obj_set_style_img_recolor_opa(icon, LV_OPA_COVER, 0);
    lv_obj_set_pos(icon, x, y);
    return icon;
}

static lv_coord_t text_width(const char *text, const lv_font_t *font) {
    lv_point_t size;
    lv_txt_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

// frame < 0 is the resting pose; otherwise one step of the working animation:
// the body bobs, arms and leg pairs swing in turns, the eyes blink now and then.
static void pose_mascot(int32_t frame) {
    lv_coord_t bob = 0, arm_l = 0, arm_r = 0, leg_a = 0, leg_b = 0;
    bool       eyes = true;
    if (frame >= 0) {
        bool step = (frame % 4) < 2;
        bob       = (frame & 1) ? -6 : 0;
        arm_l     = step ? -8 : 0;
        arm_r     = step ? 0 : -8;
        leg_a     = step ? -6 : 0;
        leg_b     = step ? 0 : -6;
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
        lv_obj_set_y(mascot[i], CL_MASCOT_Y + mascot_y[i] * CL_QY + dy);
    }
    lv_obj_set_style_bg_color(mascot[M_EYE_L], eyes ? lv_color_black() : lv_color_hex(CL_ORANGE), LV_PART_MAIN);
    lv_obj_set_style_bg_color(mascot[M_EYE_R], eyes ? lv_color_black() : lv_color_hex(CL_ORANGE), LV_PART_MAIN);
}

static void style_option(uint8_t index, bool selected) {
    lv_obj_set_style_bg_color(option_box[index], selected ? accent_color_blue : lv_color_hex(0x2B2B2B), LV_PART_MAIN);
    lv_obj_set_style_text_color(option_label[index], selected ? lv_color_black() : lv_color_white(), LV_PART_MAIN);
}

static char *put_uint(char *p, uint32_t v) {
    char    digits[11];
    uint8_t n = 0;
    do {
        digits[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v > 0);
    while (n > 0) *p++ = digits[--n];
    return p;
}

// The host sends the task status with a "{t}" placeholder where the live elapsed time goes
// ("Proofing... ({t} * 3.1k tokens)"), so the clock keeps running between host updates.
static void show_line(eh_claude_pad_t *pad, bool busy) {
    const char *src = pad->line;
    const char *ph  = busy ? strstr(src, "{t}") : NULL;
    if (ph == NULL) {
        lv_label_set_text(label_line, src);
        shown_seconds = UINT32_MAX;
        return;
    }
    char     buf[EH_CLAUDE_LINE_MAX + 16];
    char    *p = buf;
    uint32_t sec = eh_claude_pad_busy_seconds();
    size_t   head = (size_t)(ph - src);
    memcpy(p, src, head);
    p += head;
    if (sec >= 3600) {
        p = put_uint(p, sec / 3600);
        *p++ = 'h';
        *p++ = ' ';
        sec %= 3600;
        p = put_uint(p, sec / 60);
        *p++ = 'm';
    } else if (sec >= 60) {
        p = put_uint(p, sec / 60);
        *p++ = 'm';
        *p++ = ' ';
        if (sec % 60 < 10) *p++ = '0';
        p = put_uint(p, sec % 60);
        *p++ = 's';
    } else {
        p = put_uint(p, sec);
        *p++ = 's';
    }
    strcpy(p, ph + 3);
    lv_label_set_text(label_line, buf);
    shown_seconds = eh_claude_pad_busy_seconds();
}

static void layout_header(eh_claude_pad_t *pad) {
    const char *model = pad->status[0] ? pad->status : "Claude Code";
    lv_coord_t  left  = CL_PAD + CL_ICON + 6;
    lv_coord_t  avail = EH_DISPLAY_CONTENT_WIDTH - CL_PAD - left;
    lv_coord_t  cw    = pad->ctx[0] ? text_width(pad->ctx, &eh_font_montserrat_16) : 0;
    lv_coord_t  gap   = cw > 0 ? 10 : 0;
    bool        big   = text_width(model, &eh_font_montserrat_20) + gap + cw <= avail;
    const lv_font_t *font = big ? &eh_font_montserrat_20 : &eh_font_montserrat_16;
    lv_coord_t  mw    = text_width(model, font) + 2;
    if (mw > avail - gap - cw) mw = avail - gap - cw;

    lv_label_set_text(label_model, model);
    lv_obj_set_style_text_font(label_model, font, LV_PART_MAIN);
    lv_obj_set_pos(label_model, left, big ? 5 : 8);
    lv_obj_set_size(label_model, mw, big ? 26 : 20);

    lv_label_set_text(label_ctx, pad->ctx);
    lv_obj_set_pos(label_ctx, left + mw + gap, 8);
    lv_obj_set_size(label_ctx, cw + 4, 20);
}

static uint8_t current_view(const eh_claude_pad_t *pad) {
    if (pad->mode == EH_CLAUDE_MODE_ASK && pad->opt_count >= 2 && eh_claude_pad_ask_active()) return VIEW_ASK;
    if (pad->mode == EH_CLAUDE_MODE_WAIT && eh_claude_pad_wait_active()) return VIEW_WAIT;
    return VIEW_STATUS;
}

static void render(void) {
    eh_claude_pad_t *pad  = eh_claude_pad_get();
    uint8_t          view = current_view(pad);
    bool             busy = eh_claude_pad_busy();
    bool             status = view == VIEW_STATUS;

    // status view
    set_hidden(icon_claude, !status);
    set_hidden(label_model, !status);
    set_hidden(label_ctx, !status);
    set_hidden(label_limits, !status);
    set_hidden(label_week, !status);
    set_hidden(label_line, !status);
    set_hidden(head_rule, !status);
    for (uint8_t i = 0; i < M_COUNT; i++) set_hidden(mascot[i], !status);

    // action view
    set_hidden(icon_ask, status);
    set_hidden(label_title, status);
    set_hidden(ask_rule, status);
    set_hidden(label_prompt, status);
    set_hidden(label_hint, status);

    if (status) {
        layout_header(pad);
        lv_label_set_text(label_limits, pad->limits);
        lv_label_set_text(label_week, pad->week);
        show_line(pad, busy);
        if (!busy) pose_mascot(-1);
        for (uint8_t i = 0; i < EH_CLAUDE_MAX_OPTS; i++) set_hidden(option_box[i], true);
    } else {
        bool ask = view == VIEW_ASK;
        lv_label_set_text(label_title, pad->title[0] ? pad->title : (ask ? "Вопрос" : "Ожидание"));
        lv_obj_set_height(label_prompt, ask ? CL_PROMPT_H : CL_PROMPT_WAIT_H);
        lv_label_set_text(label_prompt, pad->prompt);
        lv_label_set_text(label_hint, ask ? "Ручка: выбор, нажатие: OK" : "Ответьте в терминале");
        for (uint8_t i = 0; i < EH_CLAUDE_MAX_OPTS; i++) {
            bool used = ask && i < pad->opt_count;
            set_hidden(option_box[i], !used);
            if (!used) continue;
            lv_label_set_text(option_label[i], pad->opt[i]);
            style_option(i, i == pad->selected);
        }
    }
    shown_view  = view;
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

    // status view
    icon_claude    = make_icon(CL_PAD, 6);
    label_model    = make_label(CL_PAD + CL_ICON + 6, 5, 100, 26, &eh_font_montserrat_20, accent_color_blue);
    label_ctx      = make_label(CL_PAD + CL_ICON + 110, 8, 70, 20, &eh_font_montserrat_16, lv_color_hex(0xC8C8C8));
    label_limits   = make_label(CL_PAD, 33, CL_INNER_W, 20, &eh_font_montserrat_16, lv_color_hex(0xB0B0B0));
    label_week     = make_label(CL_PAD, 54, CL_INNER_W, 20, &eh_font_montserrat_16, lv_color_hex(0xB0B0B0));
    label_line     = make_label(CL_PAD, CL_LINE_Y, CL_INNER_W, CL_LINE_H, &eh_font_montserrat_16, lv_color_hex(CL_ORANGE));
    head_rule      = make_rect(content, CL_PAD, CL_RULE_Y, CL_INNER_W, 1, lv_color_hex(0x3A3A3A));

    for (uint8_t i = 0; i < M_COUNT; i++) {
        bool eye  = i == M_EYE_L || i == M_EYE_R;
        mascot[i] = make_rect(content, CL_MASCOT_X + mascot_x[i] * CL_QX, CL_MASCOT_Y + mascot_y[i] * CL_QY, mascot_w[i] * CL_QX, mascot_h[i] * CL_QY,
                              eye ? lv_color_black() : lv_color_hex(CL_ORANGE));
    }
    // Eyes sit on top of the body.
    lv_obj_move_foreground(mascot[M_EYE_L]);
    lv_obj_move_foreground(mascot[M_EYE_R]);

    // action view
    icon_ask     = make_icon(CL_PAD, 6);
    label_title  = make_label(CL_PAD + CL_ICON + 6, 5, EH_DISPLAY_CONTENT_WIDTH - CL_PAD - (CL_PAD + CL_ICON + 6), 26, &eh_font_montserrat_20, accent_color_blue);
    ask_rule     = make_rect(content, CL_PAD, CL_ASK_RULE_Y, CL_INNER_W, 1, lv_color_hex(0x3A3A3A));
    label_prompt = make_label(CL_PAD, CL_PROMPT_Y, CL_INNER_W, CL_PROMPT_H, &eh_font_montserrat_20, lv_color_white());
    label_hint   = make_label(CL_PAD, CL_HINT_Y, CL_INNER_W, 18, &eh_font_montserrat_16, lv_color_hex(0x707070));

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
}

static void screen_claude_load(void) {
    render();
    eh_claude_pad_get()->dirty = false;
    lv_scr_load(screen_claude);
    display_apply_brightness();
}

static void screen_claude_housekeep(void) {
    eh_claude_pad_t *pad  = eh_claude_pad_get();
    bool             busy = eh_claude_pad_busy();
    uint8_t          view = current_view(pad);
    if ((pad->dirty && eh_claude_pad_text_settled()) || busy != shown_busy || view != shown_view) {
        pad->dirty = false;
        render();
    }
    if (view != VIEW_STATUS) return;
    if (busy) {
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
