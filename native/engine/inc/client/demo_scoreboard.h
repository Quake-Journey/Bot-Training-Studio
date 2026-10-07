/*
 * Q2PRO-X 1.5 — final-scoreboard adapters (internal Seam of the fact builder).
 *
 * Pure, bounded and worker-safe: no allocation, no globals, no filesystem, no
 * cvars.  It turns one `svc_layout` string into typed rows, or refuses.
 *
 * This is deliberately an INTERNAL seam.  The Demo Library's public Interface
 * does not gain three parsers; consumers keep seeing neutral facts.
 */
#ifndef Q2PROX_DEMO_SCOREBOARD_H
#define Q2PROX_DEMO_SCOREBOARD_H

#include "shared/shared.h"

#define DSB_MAX_ROWS        32
#define DSB_NAME_SIZE       16

#define DSB_ROW_AMBIGUOUS   (-2)

typedef enum {
    DSB_ADAPTER_NONE = 0,
    DSB_ADAPTER_BASEQ2,
    DSB_ADAPTER_OPENFFA,
    DSB_ADAPTER_OPENTDM,
} dsb_adapter_t;

typedef enum {
    DSB_RESULT_UNKNOWN = 0,
    DSB_RESULT_WIN,
    DSB_RESULT_LOSS,
    DSB_RESULT_DRAW,
} dsb_result_t;

typedef struct {
    char    name[DSB_NAME_SIZE];    /* empty for baseq2: it sends no name */
    int     slot;                   /* -1 when the grammar carries no slot */
    int     score;
    int     deaths;
    bool    have_deaths;            /* false means "not stated", not zero */
    bool    have_name;
    int     yv;                     /* vertical position: OpenTDM block key */
    int     team;                   /* 1/2 in team mode, else 0 */
} dsb_row_t;

typedef struct {
    dsb_adapter_t   adapter;
    int             num_rows;
    dsb_row_t       rows[DSB_MAX_ROWS];
    bool            team_mode;
    char            team_name[2][DSB_NAME_SIZE];
    int             team_score[2];
    bool            overflowed;
} dsb_board_t;

/* Parse one layout.  Returns false unless a KNOWN scoreboard grammar was
 * recognized in full; a partially understood layout is never evidence. */
bool DSB_Parse(const char *layout, size_t length, dsb_board_t *out);

/* Row lookup.  Both return the row index, -1 for none, or DSB_ROW_AMBIGUOUS
 * when more than one row could be the same person — never first-match-wins. */
int  DSB_FindRowBySlot(const dsb_board_t *b, int slot);
int  DSB_FindRowByName(const dsb_board_t *b, const char *normalized,
                       void (*normalize)(char *, size_t, const char *));

/* Unique top score wins, a tie at the top draws, anything below loses.  In
 * team mode the comparison is team score against team score. */
int  DSB_RankOf(const dsb_board_t *b, int row);

#endif /* Q2PROX_DEMO_SCOREBOARD_H */
