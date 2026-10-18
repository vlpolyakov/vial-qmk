#include "eh_claude_pad.h"

#include <string.h>

#include "timer.h"

static eh_claude_pad_t pad;
static uint32_t        last_host_contact;
static uint32_t        last_status_rx;
static uint32_t        last_busy_rx;
static uint32_t        busy_since;
static uint32_t        wait_since;
static uint32_t        last_text_rx;
static uint8_t         raw_clicks, handled_clicks, rotations, click_state;
static bool            host_seen;
static bool            status_seen;

eh_claude_pad_t *eh_claude_pad_get(void) {
    return &pad;
}

static char *field_buffer(uint8_t field, size_t *capacity) {
    switch (field) {
        case 0:
            *capacity = sizeof pad.status;
            return pad.status;
        case 1:
            *capacity = sizeof pad.task;
            return pad.task;
        case 2:
            *capacity = sizeof pad.prompt;
            return pad.prompt;
        case 3:
        case 4:
        case 5:
        case 6:
            *capacity = sizeof pad.opt[0];
            return pad.opt[field - 3];
        case 7:
            *capacity = sizeof pad.line;
            return pad.line;
        case 8:
            *capacity = sizeof pad.limits;
            return pad.limits;
        case 11:
            *capacity = sizeof pad.title;
            return pad.title;
        case 9:
            *capacity = sizeof pad.week;
            return pad.week;
        case 10:
            *capacity = sizeof pad.ctx;
            return pad.ctx;
        default:
            return NULL;
    }
}

static void touch_host(void) {
    last_host_contact = timer_read32();
    host_seen         = true;
}

static bool process_set(uint8_t *data) {
    uint8_t sub = data[1];

    if (sub <= 11) {
        uint8_t offset = data[2];
        uint8_t len    = data[3];
        size_t  capacity;
        char   *buffer = field_buffer(sub, &capacity);
        if (buffer == NULL || len > 28 || (size_t)offset + len >= capacity) return false;
        memcpy(buffer + offset, data + 4, len);
        buffer[offset + len] = '\0';
        pad.dirty            = true;
        last_text_rx         = timer_read32();
        if (sub == 0) {
            status_seen   = true;
            last_status_rx = timer_read32();
        }
        return true;
    }

    if (sub == EH_CLAUDE_SUB_COMMIT) {
        uint8_t mode  = data[2];
        uint8_t count = data[3];
        if (mode > EH_CLAUDE_MODE_WAIT || count > EH_CLAUDE_MAX_OPTS) return false;
        if (mode == EH_CLAUDE_MODE_ASK && count < 2) return false;
        uint16_t id = data[5] | ((uint16_t)data[6] << 8);
        if (mode == EH_CLAUDE_MODE_ASK && (pad.mode != EH_CLAUDE_MODE_ASK || pad.request_id != id)) {
            pad.answered = false;
            pad.answer   = 0;
        }
        if (mode == EH_CLAUDE_MODE_WAIT && pad.mode != EH_CLAUDE_MODE_WAIT) wait_since = timer_read32();
        pad.mode       = mode;
        pad.opt_count  = count;
        pad.selected   = count > 0 && data[4] < count ? data[4] : 0;
        pad.request_id = id;
        pad.dirty      = true;
        return true;
    }

    if (sub == EH_CLAUDE_SUB_END) {
        status_seen   = false;
        pad.busy      = false;
        pad.mode      = EH_CLAUDE_MODE_IDLE;
        pad.opt_count = 0;
        pad.answered  = false;
        pad.dirty     = true;
        return true;
    }

    if (sub == EH_CLAUDE_SUB_BUSY) {
        if (data[2] != 0 && !eh_claude_pad_busy()) busy_since = timer_read32();
        pad.busy     = data[2] != 0;
        last_busy_rx = timer_read32();
        pad.dirty    = true;
        return true;
    }

    if (sub == EH_CLAUDE_SUB_CLEAR) {
        pad.busy      = false;
        pad.mode      = EH_CLAUDE_MODE_IDLE;
        pad.opt_count = 0;
        pad.selected  = 0;
        pad.answered  = false;
        pad.task[0]   = '\0';
        pad.line[0]   = '\0';
        pad.title[0]  = '\0';
        pad.prompt[0] = '\0';
        for (uint8_t i = 0; i < EH_CLAUDE_MAX_OPTS; i++) pad.opt[i][0] = '\0';
        pad.dirty = true;
        return true;
    }

    return false;
}

bool eh_claude_pad_process_hid(uint8_t *data, uint8_t length) {
    if (length < 4) return false;

    if (data[0] == EH_CLAUDE_CMD_SET) {
        bool ok = process_set(data);
        if (ok) touch_host();
        memset(data + 1, 0, length - 1);
        data[1] = ok ? 1 : 0;
        return true;
    }

    if (data[0] == EH_CLAUDE_CMD_POLL) {
        uint8_t  flags = data[1];
        uint16_t id    = data[2] | ((uint16_t)data[3] << 8);
        touch_host();
        if ((flags & 1) && pad.request_id == id) {
            pad.answered = false;
            pad.dirty    = true;
        }
        memset(data, 0, length);
        data[0] = EH_CLAUDE_CMD_POLL;
        data[1] = 1;
        data[2] = pad.mode;
        data[3] = pad.answered ? 1 : 0;
        data[4] = pad.answer;
        data[5] = pad.selected;
        data[6] = pad.request_id & 0xFF;
        data[7] = pad.request_id >> 8;
        data[8] = raw_clicks;
        data[9] = handled_clicks;
        data[10] = rotations;
        data[11] = click_state;
        return true;
    }

    return false;
}

bool eh_claude_pad_ask_active(void) {
    return pad.mode == EH_CLAUDE_MODE_ASK && !pad.answered && pad.opt_count >= 2 && host_seen &&
           timer_elapsed32(last_host_contact) < EH_CLAUDE_ASK_HOST_TIMEOUT_MS;
}

bool eh_claude_pad_ask_visible(void) {
    return pad.mode == EH_CLAUDE_MODE_ASK && pad.opt_count >= 2 && host_seen && timer_elapsed32(last_host_contact) < EH_CLAUDE_ASK_HOST_TIMEOUT_MS;
}

bool eh_claude_pad_live(void) {
    return status_seen && timer_elapsed32(last_status_rx) < EH_CLAUDE_LIVE_TIMEOUT_MS;
}

bool eh_claude_pad_busy(void) {
    return pad.busy && timer_elapsed32(last_busy_rx) < EH_CLAUDE_BUSY_TIMEOUT_MS;
}

bool eh_claude_pad_text_settled(void) {
    return timer_elapsed32(last_text_rx) > 60;
}

bool eh_claude_pad_wait_active(void) {
    return pad.mode == EH_CLAUDE_MODE_WAIT && timer_elapsed32(wait_since) < EH_CLAUDE_WAIT_TIMEOUT_MS;
}

bool eh_claude_pad_keep_awake(void) {
    return eh_claude_pad_busy() || eh_claude_pad_wait_active();
}

uint32_t eh_claude_pad_busy_seconds(void) {
    return timer_elapsed32(busy_since) / 1000;
}

void eh_claude_pad_note_raw_click(void) {
    raw_clicks++;
    click_state = (eh_claude_pad_ask_active() ? 1 : 0) | (pad.mode == EH_CLAUDE_MODE_ASK ? 2 : 0) | (pad.answered ? 4 : 0) | (host_seen ? 8 : 0);
}

void eh_claude_pad_encoder(bool clockwise) {
    if (!eh_claude_pad_ask_active()) return;
    rotations++;
    if (clockwise) {
        pad.selected = (pad.selected + 1) % pad.opt_count;
    } else {
        pad.selected = (pad.selected + pad.opt_count - 1) % pad.opt_count;
    }
    pad.dirty = true;
}

void eh_claude_pad_click(void) {
    if (!eh_claude_pad_ask_active()) return;
    handled_clicks++;
    pad.answered = true;
    pad.answer   = pad.selected;
    pad.dirty    = true;
}
