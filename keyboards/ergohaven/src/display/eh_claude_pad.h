#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Claude Code companion state for the M4CR0Pad screen.
 *
 * Host -> device (raw HID, 32-byte packets):
 *   SET text chunk : F0 <field 0..6> <offset> <len<=28> <bytes...>
 *                    field 0 status, 1 task, 2 prompt, 3..6 option 0..3.
 *                    The chunk is written at <offset> and the string is
 *                    terminated right after it, so long strings are sent as
 *                    consecutive chunks with growing offsets.
 *   SET commit     : F0 10 <mode> <option_count> <selected> <id_lo> <id_hi>
 *                    mode 0 = idle (status + task), 1 = ask (prompt + options),
 *                    2 = wait (title + prompt only, no input is captured).
 *                    Fields 11 = title of the ask/wait screen.
 *   SET clear      : F0 11   back to idle, clears task/prompt/options/busy.
 *   SET busy       : F0 12 <0|1>   Claude is working (animation); expires
 *                    after EH_CLAUDE_BUSY_TIMEOUT_MS without a refresh.
 *   In idle mode the "prompt" field is shown as the current activity.
 *   Header fields: 0 model, 10 context ("Ctx: 14%"), 8 five-hour limit line,
 *   9 weekly limit line, 7 task status line ("Caramelising... 22 000 tokens"
 *   while busy, "Cooked for 8s * done 23:48" afterwards).
 *   POLL           : F1 <flags> <id_lo> <id_hi>    flags bit0 = acknowledge
 *                    the answer of request <id>.
 *
 * Device -> host reply to POLL:
 *   F1 01 <mode> <answered> <answer_idx> <selected> <id_lo> <id_hi>
 *      <raw_clicks> <handled_clicks> <rotations> <state_at_last_click>   (diagnostics, wrap at 256)
 *   state bits: 1 ask active, 2 mode is ask, 4 answered, 8 host seen
 *
 * Every reply to SET is: F0 <1 ok | 0 rejected> 00 ...
 */

#define EH_CLAUDE_CMD_SET 0xF0
#define EH_CLAUDE_CMD_POLL 0xF1
#define EH_CLAUDE_SUB_COMMIT 0x10
#define EH_CLAUDE_SUB_CLEAR 0x11
#define EH_CLAUDE_SUB_BUSY 0x12

#define EH_CLAUDE_MAX_OPTS 4
#define EH_CLAUDE_STATUS_MAX 48
#define EH_CLAUDE_TASK_MAX 160
#define EH_CLAUDE_PROMPT_MAX 112
#define EH_CLAUDE_OPT_MAX 40
#define EH_CLAUDE_LINE_MAX 96
#define EH_CLAUDE_TITLE_MAX 24
#define EH_CLAUDE_LIMITS_MAX 40
#define EH_CLAUDE_CTX_MAX 16

// An ask request is dropped if the host stops talking to the device, so a
// crashed hook can never leave the encoder captured.
#define EH_CLAUDE_ASK_HOST_TIMEOUT_MS 6000
// The status screen stays eligible while the host updated it this recently.
#define EH_CLAUDE_LIVE_TIMEOUT_MS 120000
// A "waiting for you in the terminal" screen goes away on its own after this long.
#define EH_CLAUDE_WAIT_TIMEOUT_MS 120000
// The busy animation stops if the host does not refresh the flag.
#define EH_CLAUDE_BUSY_TIMEOUT_MS 90000

typedef enum {
    EH_CLAUDE_MODE_IDLE = 0,
    EH_CLAUDE_MODE_ASK  = 1,
    EH_CLAUDE_MODE_WAIT = 2,   // message only: Claude waits for an answer given elsewhere
} eh_claude_mode_t;

typedef struct {
    char     status[EH_CLAUDE_STATUS_MAX];
    char     task[EH_CLAUDE_TASK_MAX];
    char     prompt[EH_CLAUDE_PROMPT_MAX];
    char     opt[EH_CLAUDE_MAX_OPTS][EH_CLAUDE_OPT_MAX];
    char     line[EH_CLAUDE_LINE_MAX];
    char     title[EH_CLAUDE_TITLE_MAX];
    char     limits[EH_CLAUDE_LIMITS_MAX];
    char     week[EH_CLAUDE_LIMITS_MAX];
    char     ctx[EH_CLAUDE_CTX_MAX];
    uint8_t  mode;
    uint8_t  opt_count;
    uint8_t  selected;
    uint8_t  answer;
    uint16_t request_id;
    bool     busy;
    bool     answered;
    bool     dirty;
} eh_claude_pad_t;

eh_claude_pad_t *eh_claude_pad_get(void);

// Raw HID entry point. Returns true when the packet was consumed and `data`
// already holds the reply.
bool eh_claude_pad_process_hid(uint8_t *data, uint8_t length);

// True while an ask request waits for the encoder (captures rotation + click).
bool eh_claude_pad_ask_active(void);
// True while the host keeps the status screen fresh.
bool eh_claude_pad_live(void);
// True while the host reports that Claude is working (and keeps saying so).
bool eh_claude_pad_busy(void);
// False for a moment after a text chunk arrived, so a multi-chunk update is drawn once, complete.
bool eh_claude_pad_text_settled(void);
// True while a wait screen is showing.
bool eh_claude_pad_wait_active(void);
// Busy or waiting: the display should stay on.
bool eh_claude_pad_keep_awake(void);
// Seconds since the current busy period started.
uint32_t eh_claude_pad_busy_seconds(void);

// Every press of the encoder click key, whether or not it is captured (diagnostics).
void eh_claude_pad_note_raw_click(void);
void eh_claude_pad_encoder(bool clockwise);
void eh_claude_pad_click(void);
