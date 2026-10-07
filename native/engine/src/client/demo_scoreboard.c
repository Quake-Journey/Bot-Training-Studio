/*
 * Q2PRO-X 1.5 — final-scoreboard adapters for locally recorded DM2.
 *
 * A client demo is what the player actually has: 6 400 DM2 against 41 MVD2 in
 * the reference tree.  A DM2 carries only the recorder's own score timeline,
 * so for a long time this module answered "result unknown" for essentially
 * every match a person ever played — which made the statistics useless for the
 * only corpus that exists.  The missing evidence was never missing: the server
 * sends the finished scoreboard to the recording client as an ordinary
 * `svc_layout`, and that message is inside the demo.
 *
 * There is no universal scoreboard grammar, and pretending otherwise is how a
 * parser starts inventing results.  The three real ones are decoded exactly:
 *
 *   baseq2    `client <x> <y> <slot> <score> <ping> <time>` — sorted by score,
 *             capped at 12 rows, carrying NO name and NO deaths.  Identity is
 *             the slot, resolved against the recorded playerskin.  Deaths stay
 *             obituary-derived: a missing field is not a zero.
 *   OpenFFA   an exact header row followed by quoted fixed-column player rows
 *             (rank, 15-char name, score, deaths, ...) and a separate
 *             spectator shape.  The mod also emits an unrelated `High Scores`
 *             layout with the same primitives, so recognition is by exact
 *             signature, never by "it had some strings in it".
 *   OpenTDM   two team-score rows, two block headers, and 15-column player
 *             rows.  Membership comes from which block a row is in — the
 *             vertical position — and the result is team score versus team
 *             score.  A personal score never decides a team result.
 *
 * Everything here is pure and bounded: no allocation, no globals, no VFS, no
 * cvars.  It runs on the Demo Library worker.
 */

#include "shared/shared.h"
#include "client/demo_scoreboard.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Bounded lexer for the Quake layout language                         */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *p;
    const char *end;
} dsb_lex_t;

static void dsb_skip_space(dsb_lex_t *l)
{
    while (l->p < l->end && (unsigned char)*l->p <= ' ')
        l->p++;
}

/* Reads one operand: a quoted string, or a bare token.  Returns false at end
 * of input.  `out` is always NUL-terminated. */
static bool dsb_token(dsb_lex_t *l, char *out, size_t out_size, bool *quoted)
{
    dsb_skip_space(l);
    if (l->p >= l->end)
        return false;

    size_t o = 0;
    if (*l->p == '"') {
        if (quoted) *quoted = true;
        l->p++;
        while (l->p < l->end && *l->p != '"') {
            if (o + 1 < out_size)
                out[o++] = *l->p;
            l->p++;
        }
        if (l->p < l->end)
            l->p++;                 /* closing quote */
        else {
            out[o < out_size ? o : out_size - 1] = 0;
            return false;           /* unterminated: caller must reject */
        }
    } else {
        if (quoted) *quoted = false;
        while (l->p < l->end && (unsigned char)*l->p > ' ') {
            if (o + 1 < out_size)
                out[o++] = *l->p;
            l->p++;
        }
    }
    out[o] = 0;
    return true;
}

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static void dsb_rtrim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = 0;
}

static void dsb_take_fixed(char *dst, size_t dst_size,
                           const char *src, size_t off, size_t len)
{
    size_t n = strlen(src);
    if (off >= n) { dst[0] = 0; return; }
    if (off + len > n)
        len = n - off;
    if (len >= dst_size)
        len = dst_size - 1;
    memcpy(dst, src + off, len);
    dst[len] = 0;
    dsb_rtrim(dst);
}

static bool dsb_add_row(dsb_board_t *b, const char *name, int slot,
                        int score, int deaths, bool have_deaths, int yv)
{
    if (b->num_rows >= DSB_MAX_ROWS) {
        b->overflowed = true;
        return false;
    }
    dsb_row_t *r = &b->rows[b->num_rows++];
    memset(r, 0, sizeof(*r));
    Q_strlcpy(r->name, name ? name : "", sizeof(r->name));
    r->slot        = slot;
    r->score       = score;
    r->deaths      = deaths;
    r->have_deaths = have_deaths;
    r->have_name   = r->name[0] != 0;
    r->yv          = yv;
    r->team        = 0;
    return true;
}

/* ------------------------------------------------------------------ */
/* Recognition signatures                                              */
/* ------------------------------------------------------------------ */

#define DSB_MAX_ITEMS       64
#define DSB_TEXT_MAX        MAX_NET_STRING

#define DSB_OPENFFA_HEADER  "Player          Frg Dth"
#define DSB_OPENTDM_HEADER  "Name            Frags Dths"

static bool dsb_contains(const char *hay, const char *needle)
{
    return hay && needle && strstr(hay, needle) != NULL;
}

/* A scoreboard row is a fixed-column line: past the name the rest of it is
 * nothing but right-aligned integers.  Anything else there — a skin, a ping
 * suffix, a server name in a footer — means the line is not a row.  Reading
 * only the first two numbers and ignoring the tail is how a footer gets
 * mistaken for a player.  Returns how many integers were read, or -1 when the
 * tail contains something that is not one. */
static int dsb_number_fields(const char *s, int *out, int want)
{
    int got = 0;
    while (*s) {
        while (*s == ' ' || *s == '\t')
            s++;
        if (!*s)
            break;
        const char *start = s;
        if (*s == '-' || *s == '+')
            s++;
        if (*s < '0' || *s > '9')
            return -1;
        while (*s >= '0' && *s <= '9')
            s++;
        if (*s && *s != ' ' && *s != '\t')
            return -1;              /* `97(female)` is not a number */
        if (got < want)
            out[got] = atoi(start);
        got++;
    }
    return got;
}

/* The leading `%2d` rank column OpenFFA renders on every player row. */
static bool dsb_rank_column(const char *s, int *out)
{
    int  value = 0;
    bool digit = false;
    for (int i = 0; i < 3 && s[i]; i++) {
        if (s[i] == ' ')
            continue;
        if (s[i] < '0' || s[i] > '9')
            return false;
        value = value * 10 + (s[i] - '0');
        digit = true;
    }
    if (!digit)
        return false;
    *out = value;
    return true;
}

/* ------------------------------------------------------------------ */
/* Parse                                                               */
/* ------------------------------------------------------------------ */

bool DSB_Parse(const char *layout, size_t length, dsb_board_t *out)
{
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));
    out->adapter = DSB_ADAPTER_NONE;
    if (!layout || !length)
        return false;

    /* Two passes on purpose.  Which grammar this is only becomes clear at the
     * header, and OpenTDM prints its two team-score rows BEFORE either block
     * header — classifying rows while still guessing the grammar silently
     * dropped exactly those rows and made every team result unknown. */
    struct {
        int  y;
        bool alt;                       /* string2/cstring2 */
        bool anchored;                  /* placed by `yv`: y is comparable */
        char text[DSB_TEXT_MAX];
    } items[DSB_MAX_ITEMS];
    int num_items = 0;

    struct { int slot, score, y; } clients[DSB_MAX_ROWS];
    int num_clients = 0;

    dsb_lex_t lex = { .p = layout, .end = layout + length };
    int cur_y = 0;
    bool cur_anchored = true;
    char tok[DSB_TEXT_MAX];
    bool quoted = false;

    while (dsb_token(&lex, tok, sizeof(tok), &quoted)) {
        if (quoted)
            continue;

        if (!strcmp(tok, "yv") || !strcmp(tok, "yt") || !strcmp(tok, "yb")) {
            /* `yt`/`yb` are anchored to the top and bottom of the screen, so
             * their coordinates do not live on the same axis as `yv`.  The
             * OpenTDM block test IS a vertical comparison, and the server-name
             * footer these demos carry is drawn with `yb -37` — comparing that
             * against a block header is meaningless. */
            bool anchored = tok[1] == 'v';
            if (!dsb_token(&lex, tok, sizeof(tok), &quoted)) return false;
            cur_y = atoi(tok);
            cur_anchored = anchored;
            continue;
        }
        if (!strcmp(tok, "xv") || !strcmp(tok, "xr") || !strcmp(tok, "xl") ||
            !strcmp(tok, "picn") || !strcmp(tok, "pic") ||
            !strcmp(tok, "stat_string")) {
            if (!dsb_token(&lex, tok, sizeof(tok), &quoted)) return false;
            continue;
        }
        if (!strcmp(tok, "num")) {
            if (!dsb_token(&lex, tok, sizeof(tok), &quoted)) return false;
            if (!dsb_token(&lex, tok, sizeof(tok), &quoted)) return false;
            continue;
        }
        if (!strcmp(tok, "hnum") || !strcmp(tok, "anum") ||
            !strcmp(tok, "rnum")) {
            continue;
        }

        if (!strcmp(tok, "client")) {
            int v[6];
            for (int i = 0; i < 6; i++) {
                if (!dsb_token(&lex, tok, sizeof(tok), &quoted))
                    return false;
                v[i] = atoi(tok);
            }
            if (v[2] < 0 || v[2] >= MAX_CLIENTS)
                continue;
            if (num_clients < DSB_MAX_ROWS) {
                clients[num_clients].slot  = v[2];
                clients[num_clients].score = v[3];
                clients[num_clients].y     = v[1];
                num_clients++;
            } else {
                out->overflowed = true;
            }
            continue;
        }

        bool alt = !strcmp(tok, "string2") || !strcmp(tok, "cstring2");
        if (alt || !strcmp(tok, "string") || !strcmp(tok, "cstring")) {
            char text[DSB_TEXT_MAX];
            if (!dsb_token(&lex, text, sizeof(text), &quoted))
                return false;
            if (!quoted)
                continue;
            if (num_items < DSB_MAX_ITEMS) {
                items[num_items].y        = cur_y;
                items[num_items].alt      = alt;
                items[num_items].anchored = cur_anchored;
                Q_strlcpy(items[num_items].text, text, DSB_TEXT_MAX);
                num_items++;
            } else {
                out->overflowed = true;
            }
            continue;
        }
        /* An unknown command: keep scanning, but it cannot contribute. */
    }

    /* Pass two: decide the grammar, then read the rows it defines. */
    int ffa_header = -1, tdm_header[2] = { -1, -1 }, num_tdm_headers = 0;
    for (int i = 0; i < num_items; i++) {
        if (ffa_header < 0 && dsb_contains(items[i].text, DSB_OPENFFA_HEADER))
            ffa_header = i;
        if (dsb_contains(items[i].text, DSB_OPENTDM_HEADER) &&
            num_tdm_headers < 2)
            tdm_header[num_tdm_headers++] = i;
    }

    if (num_tdm_headers == 2) {
        out->adapter = DSB_ADAPTER_OPENTDM;

        /* Blocks are laid out vertically, and the ITEM order is not the screen
         * order: these demos send both block headers before either block's
         * player rows, so an item-index test puts every player in the second
         * block.  The block key is the `yv` coordinate, nothing else. */
        int hy[2] = { items[tdm_header[0]].y, items[tdm_header[1]].y };
        if (!items[tdm_header[0]].anchored || !items[tdm_header[1]].anchored)
            return false;
        if (hy[0] > hy[1]) { int t = hy[0]; hy[0] = hy[1]; hy[1] = t; }
        if (hy[0] == hy[1])
            return false;               /* two blocks cannot share a line */

        /* The team summary sits above both blocks. */
        int num_team_rows = 0;
        for (int i = 0; i < num_items && num_team_rows < 2; i++) {
            if (!items[i].anchored || items[i].y >= hy[0])
                continue;
            char name[DSB_NAME_SIZE];
            dsb_take_fixed(name, sizeof(name), items[i].text, 0, 15);
            if (!name[0] || strlen(items[i].text) <= 15)
                continue;
            int sc = 0;
            if (dsb_number_fields(items[i].text + 15, &sc, 1) != 1)
                continue;
            Q_strlcpy(out->team_name[num_team_rows], name, DSB_NAME_SIZE);
            out->team_score[num_team_rows] = sc;
            num_team_rows++;
        }
        if (num_team_rows != 2)
            return false;

        /* Which team owns which block is stated by the mod: it prints
         * `<team>:<ping>(<skin>)` directly above each block header.  Deriving
         * it from the order the blocks happen to be drawn in would be a guess,
         * and a guess here silently swaps a win and a loss. */
        int block_team[2] = { -1, -1 };
        for (int k = 0; k < 2; k++) {
            int best_y = 0;
            const char *label = NULL;
            for (int i = 0; i < num_items; i++) {
                if (!items[i].anchored || items[i].y >= hy[k])
                    continue;
                if (k == 1 && items[i].y <= hy[0])
                    continue;           /* belongs to the first block */
                if (!strchr(items[i].text, ':'))
                    continue;
                if (!label || items[i].y > best_y) {
                    best_y = items[i].y;
                    label  = items[i].text;
                }
            }
            if (!label)
                return false;
            char name[DSB_NAME_SIZE];
            size_t n = strcspn(label, ":");
            dsb_take_fixed(name, sizeof(name), label, 0, n < 15 ? n : 15);
            for (int t = 0; t < 2; t++)
                if (name[0] && !Q_stricmp(name, out->team_name[t]))
                    block_team[k] = t + 1;
        }
        if (block_team[0] < 0 || block_team[1] < 0 ||
            block_team[0] == block_team[1])
            return false;

        for (int i = 0; i < num_items; i++) {
            if (i == tdm_header[0] || i == tdm_header[1])
                continue;
            if (!items[i].anchored || items[i].y <= hy[0])
                continue;               /* summary, labels and the title */
            char name[DSB_NAME_SIZE];
            dsb_take_fixed(name, sizeof(name), items[i].text, 0, 15);
            if (!name[0] || strlen(items[i].text) <= 15)
                continue;
            int f[2] = { 0, 0 };
            if (dsb_number_fields(items[i].text + 15, f, 2) < 2)
                continue;
            int team = block_team[items[i].y > hy[1] ? 1 : 0];
            dsb_add_row(out, name, -1, f[0], f[1], true, items[i].y);
            if (out->num_rows)
                out->rows[out->num_rows - 1].team = team;
        }
        out->team_mode = true;
        return out->num_rows > 0;
    }

    if (ffa_header >= 0) {
        out->adapter = DSB_ADAPTER_OPENFFA;

        /* `string` versus `string2` is the HIGHLIGHT, not the row kind: the mod
         * draws the recording player's own row in the plain colour and everyone
         * else in the alternate one.  Treating the alternate colour as
         * decoration left exactly one row on the board — the recorder's — so
         * every OpenFFA match came out a win.  Colour is ignored here; a row is
         * recognised by its shape. */
        int expect_rank = 1;
        for (int i = ffa_header + 1; i < num_items; i++) {
            int rank = 0;
            if (!dsb_rank_column(items[i].text, &rank))
                continue;
            if (rank != expect_rank)
                break;                  /* the ranked list has ended */
            char name[DSB_NAME_SIZE];
            dsb_take_fixed(name, sizeof(name), items[i].text, 3, 15);
            if (!name[0] || strlen(items[i].text) <= 18)
                break;
            int f[2] = { 0, 0 };
            if (dsb_number_fields(items[i].text + 18, f, 2) < 2)
                break;
            if (!dsb_add_row(out, name, -1, f[0], f[1], true, items[i].y))
                break;                  /* overflowed: recorded on the board */
            expect_rank++;
        }
        /* The list is sorted, so rank 1 IS the top score.  Without it the
         * board cannot say who won, and answering anyway is how a mid-table
         * finish is reported as a victory. */
        if (out->num_rows < 1)
            return false;
        return true;
    }

    if (num_clients > 0) {
        out->adapter = DSB_ADAPTER_BASEQ2;
        for (int i = 0; i < num_clients; i++)
            dsb_add_row(out, NULL, clients[i].slot, clients[i].score,
                        0, false, clients[i].y);
        return out->num_rows > 0;
    }

    return false;
}

/* ------------------------------------------------------------------ */
/* Ranking                                                             */
/* ------------------------------------------------------------------ */

int DSB_FindRowBySlot(const dsb_board_t *b, int slot)
{
    int found = -1;
    for (int i = 0; i < b->num_rows; i++) {
        if (b->rows[i].slot != slot)
            continue;
        if (found >= 0)
            return DSB_ROW_AMBIGUOUS;
        found = i;
    }
    return found;
}

int DSB_FindRowByName(const dsb_board_t *b, const char *normalized,
                      void (*normalize)(char *, size_t, const char *))
{
    if (!normalized || !normalized[0] || !normalize)
        return -1;
    int found = -1;
    for (int i = 0; i < b->num_rows; i++) {
        if (!b->rows[i].have_name)
            continue;
        char key[DSB_NAME_SIZE];
        normalize(key, sizeof(key), b->rows[i].name);
        if (strcmp(key, normalized))
            continue;
        if (found >= 0)
            return DSB_ROW_AMBIGUOUS;   /* two rows render identically */
        found = i;
    }
    return found;
}

int DSB_RankOf(const dsb_board_t *b, int row)
{
    if (row < 0 || row >= b->num_rows)
        return DSB_RESULT_UNKNOWN;

    if (b->team_mode) {
        int mine = b->rows[row].team;
        if (mine != 1 && mine != 2)
            return DSB_RESULT_UNKNOWN;
        int a = b->team_score[mine - 1];
        int c = b->team_score[2 - mine];
        if (a > c) return DSB_RESULT_WIN;
        if (a < c) return DSB_RESULT_LOSS;
        return DSB_RESULT_DRAW;
    }

    int best = b->rows[0].score, ties = 0;
    for (int i = 0; i < b->num_rows; i++)
        if (b->rows[i].score > best)
            best = b->rows[i].score;
    for (int i = 0; i < b->num_rows; i++)
        if (b->rows[i].score == best)
            ties++;

    if (b->rows[row].score != best)
        return DSB_RESULT_LOSS;
    return ties > 1 ? DSB_RESULT_DRAW : DSB_RESULT_WIN;
}
