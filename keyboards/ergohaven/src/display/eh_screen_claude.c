#include "eh_display.h"
#include <lvgl.h>

#include "ergohaven.h"
#include "lvgl_helpers.h"
#include "eh_claude_pad.h"
#include "eh_claude_icon.h"
#include "eh_display_bounds.h"

LV_FONT_DECLARE(eh_font_montserrat_16);
LV_FONT_DECLARE(eh_font_montserrat_20);

// All coordinates are relative to the safe content area (230 x 270), which keeps
// text away from the bezel that hides the outermost pixels of the panel.
#define CL_PAD 10
#define CL_INNER_W (EH_DISPLAY_CONTENT_WIDTH - 2 * CL_PAD)
#define CL_ICON EH_CLAUDE_ICON_SIZE
#define CL_HEAD_H 42
#define CL_BODY_Y (CL_HEAD_H + 6)
#define CL_PROMPT_H 72
#define CL_OPT_Y (CL_BODY_Y + CL_PROMPT_H + 6)
#define CL_OPT_H 32
#define CL_OPT_STEP 35

static lv_obj_t *screen_claude;
static lv_obj_t *content;
static lv_obj_t *icon_claude;
static lv_obj_t *label_status;
static lv_obj_t *label_task;
static lv_obj_t *label_prompt;
static lv_obj_t *option_box[EH_CLAUDE_MAX_OPTS];
static lv_obj_t *option_label[EH_CLAUDE_MAX_OPTS];

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

static void style_option(uint8_t index, bool selected) {
    lv_obj_set_style_bg_color(option_box[index], selected ? accent_color_blue : lv_color_hex(0x2B2B2B), LV_PART_MAIN);
    lv_obj_set_style_text_color(option_label[index], selected ? lv_color_black() : lv_color_white(), LV_PART_MAIN);
}

static void render(void) {
    eh_claude_pad_t *pad = eh_claude_pad_get();
    bool             ask = pad->mode == EH_CLAUDE_MODE_ASK && pad->opt_count >= 2;

    lv_label_set_text(label_status, pad->status[0] ? pad->status : "Claude Code");

    if (ask) {
        set_hidden(label_task, true);
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
        set_hidden(label_prompt, true);
        for (uint8_t i = 0; i < EH_CLAUDE_MAX_OPTS; i++) set_hidden(option_box[i], true);
        lv_label_set_text(label_task, pad->task);
    }
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
    lv_obj_set_style_img_recolor(icon_claude, lv_color_hex(0xD97757), 0);
    lv_obj_set_style_img_recolor_opa(icon_claude, LV_OPA_COVER, 0);
    lv_obj_set_pos(icon_claude, CL_PAD, 3);

    lv_coord_t status_x = CL_PAD + CL_ICON + 8;
    label_status = make_label(status_x, 11, EH_DISPLAY_CONTENT_WIDTH - status_x - CL_PAD, 22, &eh_font_montserrat_16, accent_color_blue);
    label_task   = make_label(CL_PAD, CL_BODY_Y, CL_INNER_W, 210, &eh_font_montserrat_20, lv_color_white());
    label_prompt = make_label(CL_PAD, CL_BODY_Y, CL_INNER_W, CL_PROMPT_H, &eh_font_montserrat_20, lv_color_white());

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
    if (!pad->dirty) return;
    pad->dirty = false;
    render();
}

const eh_screen_t eh_screen_claude = {
    .init      = screen_claude_init,
    .load      = screen_claude_load,
    .housekeep = screen_claude_housekeep,
};
