/*
 * Q2PRO-X 1.5 — neutral match-fact builder and mod Adapters.
 *
 * See inc/client/demo_match_facts.h for the ownership boundary.
 *
 * The central rule this file exists to enforce is the one the PO stated
 * directly: OpenFFA is not OpenTDM, and a single "score changed => played,
 * zero frags => spectator" heuristic is wrong for both.  So the order here is
 * always PHASE first, then PARTICIPANT state, and only then contribution:
 *
 *   OpenFFA   with `g_warmup 0` a map is continuously live from spawn, so a
 *             spawned player with zero frags is an active participant, not a
 *             spectator, and no ready-up gate exists to wait for.  When a
 *             server did enable warmup, the recorded STAT_FRAGS_STRING points
 *             at the mod's own warmup/countdown configstrings and pre-live
 *             time is excluded.  A chase spectator gets the target's stats
 *             copied but keeps STAT_SPECTATOR set, so it never inherits the
 *             target's participation.
 *
 *   OpenTDM   warmup, ready messages, countdown frags and the automatic
 *             `record` that OpenTDM issues during countdown are NOT a match.
 *             The scoring epoch opens on the authoritative
 *             `CS_TDM_GAME_STATUS: Countdown -> Match` transition, an aborted
 *             countdown produces zero matches, timeout / overtime / sudden
 *             death stay inside the same epoch, and `Match End` closes it.
 *
 *   generic   no phase truth is invented.  The segment carries one implicit
 *             epoch, spectator state is only believed when the stream says
 *             so, and a slot without score authority stays unverifiable
 *             instead of being awarded or denied a match.
 */

#include "shared/shared.h"
#include "common/protocol.h"
#include "common/utils.h"
#include "common/q2prox_obituary.h"
#include "client/demo_library.h"
#include "client/demo_match_facts.h"
#include "client/demo_offline_decoder.h"
#include "client/demo_scoreboard.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Bounded capacities                                                  */
/* ------------------------------------------------------------------ */

#define DMF_MAX_SEGMENTS    DLI_MAX_SEGMENTS_PER_FILE
#define DMF_MAX_EPOCHS      512
#define DMF_MAX_TRACKS      1024
/* Bounded like every other builder array: at most one recognized final
 * table per epoch, and a table is capped at DSB_MAX_ROWS. */
#define DMF_MAX_BOARD_ROWS  (DMF_MAX_EPOCHS * 4)
#define DMF_MAX_PARTS       4096
#define DMF_MAX_WEAPON_FACTS (DMF_MAX_PARTS * (DLI_WEAPON_COUNT - 1))
#define DMF_MAX_PICKUP_FACTS 65536
#define DMF_GENERAL_WINDOW  16      /* general-band slots kept per segment */
#define DMF_GENERAL_TEXT    40

/* Mod stat slots that are not part of the shared Quake II set. */
#define DMF_STAT_OPENFFA_FRAGS_STRING   18
#define DMF_STAT_OPENTDM_GAME_STATUS    26
#define DMF_STAT_OPENTDM_TEAM_A_STATUS  20
#define DMF_STAT_OPENTDM_TEAM_B_STATUS  21

/* OpenFFA general-band offsets (openffa g_local.h). */
#define DMF_OFFA_CS_OBSERVE     1
#define DMF_OFFA_CS_SPECMODE    3
#define DMF_OFFA_CS_PREGAME     4
#define DMF_OFFA_CS_WARMUP      7
#define DMF_OFFA_CS_COUNTDOWN   8

/* OpenTDM general-band offsets (opentdm g_local.h). */
#define DMF_TDM_CS_TEAM_A_NAME    0
#define DMF_TDM_CS_TEAM_B_NAME    1
#define DMF_TDM_CS_TEAM_A_STATUS  2
#define DMF_TDM_CS_TEAM_B_STATUS  3
#define DMF_TDM_CS_TIMELIMIT      4
#define DMF_TDM_CS_GAME_STATUS    6

typedef enum {
    DMF_PHASE_UNKNOWN = 0,
    DMF_PHASE_PRELIVE,
    DMF_PHASE_LIVE,
    DMF_PHASE_POST,
} dmf_phase_t;

/* ------------------------------------------------------------------ */
/* Builder                                                             */
/* ------------------------------------------------------------------ */

struct dmf_builder_s {
    dof_sink_t      sink;
    bool          (*cancelled)(void *ud);
    void           *cancel_ud;
    char            gamedir_hint[MAX_QPATH];

    dli_segment_t  *segments;
    dli_epoch_t    *epochs;
    dli_track_t    *tracks;
    dli_part_t     *parts;
    dli_board_row_t *board_rows;
    dli_weapon_fact_t *weapon_facts;
    dli_pickup_fact_t *pickup_facts;
    int             num_segments;
    int             num_epochs;
    int             num_tracks;
    int             num_parts;
    int             num_board_rows;
    int             num_weapon_facts;
    int             num_pickup_facts;
    unsigned        quality;

    /* ---- current segment working state ---- */
    bool            in_segment;
    int             cur_segment;
    int             cur_epoch;          /* -1 when no epoch is open */
    dmf_phase_t     phase;
    dli_adapter_t   adapter;
    bool            mvd;
    int             general_base;
    int             models_base;
    int             playerskins_base;
    int             items_base;
    int             maxclients_index;
    int             pov_slot;
    int             max_clients;
    int             last_time_ms;
    int             segment_start_ms;

    char            general[DMF_GENERAL_WINDOW][DMF_GENERAL_TEXT];
    char            names[MAX_CLIENTS][DLI_NAME_SIZE];
    const char     *name_ptrs[MAX_CLIENTS];
    int             track_of_slot[MAX_CLIENTS];

    /* Per-slot previous observation, so a transition is a real transition. */
    bool            have_prev[MAX_CLIENTS];
    int             prev_frags[MAX_CLIENTS];
    bool            prev_spectator[MAX_CLIENTS];
    int             prev_pickup_cs[MAX_CLIENTS];
    int             prev_health[MAX_CLIENTS];
    char            item_names[MAX_ITEMS][DLI_ITEM_NAME_SIZE];

    /* The last observed OpenTDM scoreboard, kept until the epoch closes so a
     * team result can be resolved from it. */
    /* Epoch-owned scoreboard candidate.  A scoreboard seen during play is
     * only a candidate; it becomes the epoch's result evidence when the mod's
     * own end signal pairs with it, in either order.  A segment-global "last
     * layout" leaked one match's table into the next one on the same
     * connection. */
    dsb_board_t     cand;
    bool            cand_valid;
    int             cand_time;
    int             cand_epoch;         /* epoch open when it arrived, or -1 */
    bool            saw_truncated_layout;
    bool            saw_unsupported_layout;
    int             last_closed_epoch;
    int             last_closed_time;

    uint64_t        fingerprint;
};

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static inline uint64_t dmf_hash(uint64_t h, const void *data, size_t len)
{
    const unsigned char *p = data;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= UINT64_C(0x100000001b3);
    }
    return h;
}

static inline uint64_t dmf_hash_str(uint64_t h, const char *s)
{
    return s ? dmf_hash(h, s, strlen(s)) : h;
}

static inline uint64_t dmf_hash_int(uint64_t h, int v)
{
    int32_t t = v;
    return dmf_hash(h, &t, sizeof(t));
}

void DMF_NormalizeName(char *dst, size_t dst_size, const char *src)
{
    if (!dst || !dst_size)
        return;
    size_t o = 0;
    if (src) {
        for (size_t i = 0; src[i] && o + 1 < dst_size; i++) {
            /* Server player names carry the Quake high-bit "alternate glyph"
             * form of ordinary ASCII, so mask it off before folding case.
             * Deliberately NOT locale-sensitive: these are protocol bytes. */
            unsigned char c = (unsigned char)src[i] & 0x7f;
            if (c >= 'A' && c <= 'Z')
                c = (unsigned char)(c - 'A' + 'a');
            if (c < 0x20 || c == 0x7f)
                continue;
            dst[o++] = (char)c;
        }
    }
    /* Trim ASCII whitespace on both ends. */
    while (o && (dst[o - 1] == ' ' || dst[o - 1] == '\t'))
        o--;
    dst[o] = 0;
    size_t lead = 0;
    while (dst[lead] == ' ' || dst[lead] == '\t')
        lead++;
    if (lead)
        memmove(dst, dst + lead, o - lead + 1);
}

/* Forward: the epoch close path promotes a candidate scoreboard. */
static void dmf_close_epoch(dmf_builder_t *b, dli_conf_t end_conf,
                            bool closed);
static void dmf_apply_scoreboard(dmf_builder_t *b, int epoch_index);

static const char *dmf_general(const dmf_builder_t *b, int offset)
{
    if (offset < 0 || offset >= DMF_GENERAL_WINDOW)
        return "";
    return b->general[offset];
}

/* ------------------------------------------------------------------ */
/* Record allocation                                                   */
/* ------------------------------------------------------------------ */

static dli_track_t *dmf_track_for_slot(dmf_builder_t *b, int slot)
{
    if (slot < 0 || slot >= MAX_CLIENTS || !b->in_segment)
        return NULL;
    if (b->track_of_slot[slot] >= 0)
        return &b->tracks[b->track_of_slot[slot]];
    if (b->num_tracks >= DMF_MAX_TRACKS) {
        b->quality |= DLI_Q_LIMIT_HIT;
        return NULL;
    }
    dli_segment_t *seg = &b->segments[b->cur_segment];
    if (seg->num_tracks >= DLI_MAX_TRACKS_PER_SEGMENT) {
        b->quality |= DLI_Q_LIMIT_HIT;
        return NULL;
    }

    dli_track_t *t = &b->tracks[b->num_tracks];
    memset(t, 0, sizeof(*t));
    t->segment_index = b->cur_segment;
    t->slot          = slot;
    t->first_seen_ms = b->last_time_ms;
    t->last_seen_ms  = b->last_time_ms;
    t->is_recorder_pov = (!b->mvd && slot == b->pov_slot);
    b->track_of_slot[slot] = b->num_tracks;
    /* Only one segment is ever open, so a segment's tracks are contiguous. */
    if (!seg->num_tracks)
        seg->first_track = b->num_tracks;
    seg->num_tracks++;
    b->num_tracks++;
    return t;
}

static void dmf_track_add_alias(dli_track_t *t, const char *raw)
{
    if (!t || !raw || !raw[0])
        return;
    char key[DLI_NAME_SIZE];
    Q_strlcpy(key, raw, sizeof(key));
    for (int i = 0; i < t->alias_count; i++)
        if (!strcmp(t->aliases[i], key))
            return;
    if (t->alias_count >= DLI_MAX_TRACK_ALIASES)
        return;     /* rename storm: keep the first four, honestly bounded */
    Q_strlcpy(t->aliases[t->alias_count], key, DLI_NAME_SIZE);
    t->alias_count++;
}

static dli_part_t *dmf_part_for(dmf_builder_t *b, int epoch_index, int track_index)
{
    if (epoch_index < 0 || track_index < 0)
        return NULL;
    dli_epoch_t *ep = &b->epochs[epoch_index];
    for (int i = 0; i < ep->num_parts; i++) {
        dli_part_t *p = &b->parts[ep->first_part + i];
        if (p->track_index == track_index)
            return p;
    }
    if (b->num_parts >= DMF_MAX_PARTS) {
        b->quality |= DLI_Q_LIMIT_HIT;
        return NULL;
    }
    /* Parts of one epoch are contiguous, so a new part is only appendable
     * while this epoch is still the most recent one in the array. */
    if (ep->first_part + ep->num_parts != b->num_parts) {
        b->quality |= DLI_Q_LIMIT_HIT;
        return NULL;
    }
    dli_part_t *p = &b->parts[b->num_parts];
    memset(p, 0, sizeof(*p));
    p->epoch_index = epoch_index;
    p->track_index = track_index;
    p->baseline_score = 0;
    p->min_score = 0;
    p->max_score = 0;
    p->first_weapon_fact = -1;
    p->first_pickup_fact = -1;
    ep->num_parts++;
    b->num_parts++;
    return p;
}

static dli_weapon_fact_t *dmf_weapon_fact_for(dmf_builder_t *b,
                                               dli_part_t *p,
                                               dli_weapon_t weapon)
{
    if (!b || !p || weapon <= DLI_WEAPON_UNKNOWN || weapon >= DLI_WEAPON_COUNT)
        return NULL;
    for (int i = p->first_weapon_fact; i >= 0; i = b->weapon_facts[i].next)
        if (b->weapon_facts[i].weapon == weapon)
            return &b->weapon_facts[i];
    if (b->num_weapon_facts >= DMF_MAX_WEAPON_FACTS) {
        b->quality |= DLI_Q_LIMIT_HIT;
        return NULL;
    }
    int index = b->num_weapon_facts++;
    dli_weapon_fact_t *f = &b->weapon_facts[index];
    memset(f, 0, sizeof(*f));
    f->weapon = weapon;
    f->next = p->first_weapon_fact;
    p->first_weapon_fact = index;
    return f;
}

static dli_pickup_fact_t *dmf_pickup_fact_for(dmf_builder_t *b,
                                               dli_part_t *p,
                                               const char *name)
{
    if (!b || !p || !name || !name[0])
        return NULL;
    for (int i = p->first_pickup_fact; i >= 0; i = b->pickup_facts[i].next)
        if (!Q_stricmp(b->pickup_facts[i].name, name))
            return &b->pickup_facts[i];
    if (b->num_pickup_facts >= DMF_MAX_PICKUP_FACTS) {
        b->quality |= DLI_Q_LIMIT_HIT;
        return NULL;
    }
    int index = b->num_pickup_facts++;
    dli_pickup_fact_t *f = &b->pickup_facts[index];
    memset(f, 0, sizeof(*f));
    Q_strlcpy(f->name, name, sizeof(f->name));
    f->next = p->first_pickup_fact;
    p->first_pickup_fact = index;
    return f;
}

static dli_weapon_t dmf_weapon_from_obituary(q2px_obit_weapon_t weapon)
{
    switch (weapon) {
    case Q2PX_OBIT_WEAPON_BLASTER:          return DLI_WEAPON_BLASTER;
    case Q2PX_OBIT_WEAPON_SHOTGUN:          return DLI_WEAPON_SHOTGUN;
    case Q2PX_OBIT_WEAPON_SUPER_SHOTGUN:    return DLI_WEAPON_SUPER_SHOTGUN;
    case Q2PX_OBIT_WEAPON_MACHINEGUN:       return DLI_WEAPON_MACHINEGUN;
    case Q2PX_OBIT_WEAPON_CHAINGUN:         return DLI_WEAPON_CHAINGUN;
    case Q2PX_OBIT_WEAPON_GRENADES:         return DLI_WEAPON_GRENADES;
    case Q2PX_OBIT_WEAPON_GRENADE_LAUNCHER: return DLI_WEAPON_GRENADE_LAUNCHER;
    case Q2PX_OBIT_WEAPON_ROCKET_LAUNCHER:  return DLI_WEAPON_ROCKET_LAUNCHER;
    case Q2PX_OBIT_WEAPON_HYPERBLASTER:     return DLI_WEAPON_HYPERBLASTER;
    case Q2PX_OBIT_WEAPON_RAILGUN:          return DLI_WEAPON_RAILGUN;
    case Q2PX_OBIT_WEAPON_BFG10K:           return DLI_WEAPON_BFG10K;
    default:                                return DLI_WEAPON_UNKNOWN;
    }
}

/* ------------------------------------------------------------------ */
/* Epoch lifecycle                                                     */
/* ------------------------------------------------------------------ */

static void dmf_open_epoch(dmf_builder_t *b, dli_conf_t start_conf, unsigned quality)
{
    if (!b->in_segment || b->cur_epoch >= 0)
        return;
    if (b->num_epochs >= DMF_MAX_EPOCHS) {
        b->quality |= DLI_Q_LIMIT_HIT;
        return;
    }
    dli_segment_t *seg = &b->segments[b->cur_segment];
    if (seg->num_epochs >= DLI_MAX_EPOCHS_PER_SEGMENT) {
        b->quality |= DLI_Q_LIMIT_HIT;
        return;
    }

    dli_epoch_t *ep = &b->epochs[b->num_epochs];
    memset(ep, 0, sizeof(*ep));
    ep->segment_index = b->cur_segment;
    ep->start_ms      = b->last_time_ms;
    ep->end_ms        = b->last_time_ms;
    ep->start_conf    = start_conf;
    ep->end_conf      = DLI_CONF_UNKNOWN;
    ep->quality       = quality;
    ep->first_part    = b->num_parts;
    ep->team_conf     = DLI_CONF_UNKNOWN;

    /* A new live interval never inherits the previous one's table. */
    b->cand_valid = false;
    b->saw_truncated_layout = false;
    b->saw_unsupported_layout = false;

    if (!seg->num_epochs)
        seg->first_epoch = b->num_epochs;
    seg->num_epochs++;
    b->cur_epoch = b->num_epochs;
    b->num_epochs++;

    /* Every slot that is already active enters the epoch with its CURRENT
     * score as the baseline: warmup frags must never leak into live totals. */
    for (int slot = 0; slot < MAX_CLIENTS; slot++) {
        int ti = b->track_of_slot[slot];
        if (ti < 0 || !b->have_prev[slot])
            continue;
        dli_part_t *p = dmf_part_for(b, b->cur_epoch, ti);
        if (!p)
            continue;
        p->baseline_score = b->prev_frags[slot];
        p->final_score    = b->prev_frags[slot];
        p->min_score      = b->prev_frags[slot];
        p->max_score      = b->prev_frags[slot];
        p->spectator_in_epoch = b->prev_spectator[slot];
        p->active_in_epoch    = !b->prev_spectator[slot];
    }
}

static void dmf_close_epoch(dmf_builder_t *b, dli_conf_t end_conf, bool closed)
{
    if (b->cur_epoch < 0)
        return;
    dli_epoch_t *ep = &b->epochs[b->cur_epoch];
    ep->end_ms   = b->last_time_ms;
    ep->end_conf = end_conf;
    ep->closed   = closed;

    uint64_t fp = UINT64_C(0xcbf29ce484222325);
    fp = dmf_hash_int(fp, ep->end_ms - ep->start_ms);
    for (int i = 0; i < ep->num_parts; i++) {
        const dli_part_t *p = &b->parts[ep->first_part + i];
        const dli_track_t *t = &b->tracks[p->track_index];
        if (t->alias_count)
            fp = dmf_hash_str(fp, t->aliases[t->alias_count - 1]);
        fp = dmf_hash_int(fp, p->final_score - p->baseline_score);
        fp = dmf_hash_int(fp, p->kills);
        fp = dmf_hash_int(fp, p->deaths);
    }
    ep->fingerprint = fp;

    /* Layout-before-end: the table already arrived while this epoch was
     * open.  Only an authoritative close may promote it — a demo cut in the
     * middle keeps an unknown result rather than inheriting whatever the
     * player last had on screen. */
    if (closed && end_conf == DLI_CONF_AUTHORITATIVE)
        dmf_apply_scoreboard(b, b->cur_epoch);
    else if (!ep->have_final_scoreboard)
        ep->result_reason = DLI_RR_NO_FINAL_SCOREBOARD;

    b->last_closed_epoch = b->cur_epoch;
    b->last_closed_time  = b->last_time_ms;
    b->cur_epoch = -1;
}

static void dmf_close_segment(dmf_builder_t *b)
{
    if (!b->in_segment)
        return;
    dmf_close_epoch(b, DLI_CONF_UNKNOWN, false);

    dli_segment_t *seg = &b->segments[b->cur_segment];
    seg->end_ms = b->last_time_ms;

    uint64_t fp = UINT64_C(0xcbf29ce484222325);
    fp = dmf_hash_str(fp, seg->map);
    fp = dmf_hash_str(fp, seg->mod);
    fp = dmf_hash_int(fp, seg->end_ms - seg->start_ms);
    for (int i = 0; i < seg->num_epochs; i++)
        fp = dmf_hash(fp, &b->epochs[seg->first_epoch + i].fingerprint,
                      sizeof(uint64_t));
    seg->fingerprint = fp;

    b->in_segment = false;
}

/* ------------------------------------------------------------------ */
/* Adapters — phase truth                                              */
/* ------------------------------------------------------------------ */

/* OpenTDM publishes its ordered state through CS_TDM_GAME_STATUS. */
static dmf_phase_t dmf_opentdm_phase(const char *status)
{
    if (!status || !status[0])
        return DMF_PHASE_UNKNOWN;
    if (!Q_stricmpn(status, "Warmup", 6))
        return DMF_PHASE_PRELIVE;
    if (!Q_stricmpn(status, "Countdown", 9))
        return DMF_PHASE_PRELIVE;
    if (!Q_stricmpn(status, "Match End", 9))
        return DMF_PHASE_POST;
    if (!Q_stricmpn(status, "Match", 5))
        return DMF_PHASE_LIVE;
    if (!Q_stricmpn(status, "Overtime", 8))
        return DMF_PHASE_LIVE;
    /* Sudden death keeps a live game-status value and only changes the
     * separate timelimit string, so it needs no case of its own. */
    return DMF_PHASE_UNKNOWN;
}

static void dmf_apply_opentdm_status(dmf_builder_t *b)
{
    dmf_phase_t next = dmf_opentdm_phase(dmf_general(b, DMF_TDM_CS_GAME_STATUS));
    if (next == DMF_PHASE_UNKNOWN || next == b->phase)
        return;

    if (next == DMF_PHASE_LIVE && b->phase != DMF_PHASE_LIVE) {
        /* Authoritative Countdown -> Match.  Everything before this instant —
         * warmup kills, ready messages, countdown frags — stays outside. */
        dmf_open_epoch(b, DLI_CONF_AUTHORITATIVE, 0);
    } else if (b->phase == DMF_PHASE_LIVE && next != DMF_PHASE_LIVE) {
        dmf_close_epoch(b, DLI_CONF_AUTHORITATIVE, true);
    }
    b->phase = next;
}

/* OpenFFA points STAT_FRAGS_STRING at its own warmup/countdown/observe
 * configstrings; with the default `g_warmup 0` it stays at the private frag
 * string and the map is continuously live. */
static dmf_phase_t dmf_openffa_phase_from_stat(const dmf_builder_t *b, int frags_string)
{
    if (frags_string <= 0)
        return DMF_PHASE_LIVE;      /* live spawned play, no warmup running */
    int off = frags_string - b->general_base;
    if (off == DMF_OFFA_CS_WARMUP || off == DMF_OFFA_CS_COUNTDOWN)
        return DMF_PHASE_PRELIVE;
    /* CS_OBSERVE and the private frag string are both live-time states. */
    return DMF_PHASE_LIVE;
}

/* ------------------------------------------------------------------ */
/* Sink callbacks                                                      */
/* ------------------------------------------------------------------ */

static bool dmf_cancelled(void *ud)
{
    dmf_builder_t *b = ud;
    return b->cancelled ? b->cancelled(b->cancel_ud) : false;
}

static void dmf_on_segment(void *ud, const dof_segment_info_t *info)
{
    dmf_builder_t *b = ud;

    dmf_close_segment(b);

    if (b->num_segments >= DMF_MAX_SEGMENTS) {
        b->quality |= DLI_Q_LIMIT_HIT;
        return;
    }

    const cs_remap_t *csr = info->extended ? &cs_remap_new : &cs_remap_old;

    dli_segment_t *seg = &b->segments[b->num_segments];
    memset(seg, 0, sizeof(*seg));
    seg->index_in_file = info->index_in_file;
    seg->start_ms      = b->last_time_ms;
    seg->end_ms        = b->last_time_ms;
    seg->duplicate_of  = -1;
    seg->mode          = DLI_MODE_UNKNOWN;
    seg->mode_conf     = DLI_CONF_UNKNOWN;
    Q_strlcpy(seg->mod, info->gamedir[0] ? info->gamedir : "baseq2", sizeof(seg->mod));

    b->cur_segment      = b->num_segments;
    b->num_segments++;
    b->in_segment       = true;
    b->cur_epoch        = -1;
    b->phase            = DMF_PHASE_UNKNOWN;
    b->mvd              = info->mvd;
    b->pov_slot         = info->client_num;
    b->max_clients      = info->max_clients;
    b->general_base     = csr->general;
    b->models_base      = csr->models;
    b->playerskins_base = csr->playerskins;
    b->items_base       = info->items_base ? info->items_base : csr->items;
    b->maxclients_index = csr->maxclients;
    b->segment_start_ms = b->last_time_ms;
    b->cand_valid       = false;

    memset(b->general, 0, sizeof(b->general));
    memset(b->names, 0, sizeof(b->names));
    memset(b->have_prev, 0, sizeof(b->have_prev));
    memset(b->prev_frags, 0, sizeof(b->prev_frags));
    memset(b->prev_spectator, 0, sizeof(b->prev_spectator));
    memset(b->prev_pickup_cs, 0, sizeof(b->prev_pickup_cs));
    memset(b->prev_health, 0, sizeof(b->prev_health));
    memset(b->item_names, 0, sizeof(b->item_names));
    for (int i = 0; i < MAX_CLIENTS; i++) {
        b->track_of_slot[i] = -1;
        b->name_ptrs[i] = b->names[i];
    }

    /* Adapter selection.  The gamedir is a hint, never mode evidence: the
     * authoritative confirmation is the mod's own recorded state, applied in
     * dmf_on_configstring / dmf_on_player_state below. */
    b->adapter = DLI_ADAPTER_GENERIC;
    if (!Q_stricmp(seg->mod, "opentdm"))
        b->adapter = DLI_ADAPTER_OPENTDM;
    else if (!Q_stricmp(seg->mod, "openffa"))
        b->adapter = DLI_ADAPTER_OPENFFA;
    seg->adapter = b->adapter;

    if (!b->mvd)
        seg->quality |= DLI_Q_POV_ONLY;
}

static void dmf_on_configstring(void *ud, int time_ms, int index, const char *text)
{
    dmf_builder_t *b = ud;
    if (!b->in_segment)
        return;
    b->last_time_ms = time_ms;

    dli_segment_t *seg = &b->segments[b->cur_segment];

    if (index == b->models_base + 1) {
        char parsed[MAX_QPATH];
        if (Com_ParseMapName(parsed, text, sizeof(parsed)))
            Q_strlcpy(seg->map, parsed, sizeof(seg->map));
        return;
    }

    if (index >= b->playerskins_base &&
        index < b->playerskins_base + MAX_CLIENTS) {
        int slot = index - b->playerskins_base;
        char raw[DLI_NAME_SIZE];
        Q_strlcpy(raw, text, sizeof(raw));
        char *bs = strchr(raw, '\\');
        if (bs)
            *bs = 0;
        if (!raw[0]) {
            /* An emptied playerskin means the slot was RELEASED.  Close the
             * identity window and forget the slot binding, so a later player
             * who is handed the same slot becomes a new track instead of
             * inheriting this person's history. */
            b->names[slot][0] = 0;
            int cur = b->track_of_slot[slot];
            if (cur >= 0) {
                b->tracks[cur].identity_end_ms = time_ms;
                b->track_of_slot[slot] = -1;
            }
            return;
        }
        char norm[DLI_NAME_SIZE];
        DMF_NormalizeName(norm, sizeof(norm), raw);
        Q_strlcpy(b->names[slot], raw, sizeof(b->names[slot]));
        dli_track_t *t = dmf_track_for_slot(b, slot);
        if (t) {
            dmf_track_add_alias(t, norm);
            t->last_seen_ms = time_ms;
        }
        return;
    }

    if (index >= b->items_base && index < b->items_base + MAX_ITEMS) {
        Q_strlcpy(b->item_names[index - b->items_base], text,
                  DLI_ITEM_NAME_SIZE);
        return;
    }

    int off = index - b->general_base;
    if (off >= 0 && off < DMF_GENERAL_WINDOW) {
        Q_strlcpy(b->general[off], text, DMF_GENERAL_TEXT);
        if (b->adapter == DLI_ADAPTER_OPENTDM && off == DMF_TDM_CS_GAME_STATUS)
            dmf_apply_opentdm_status(b);
    }
}

static void dmf_on_player_state(void *ud, const dof_player_state_t *ps)
{
    dmf_builder_t *b = ud;
    if (!b->in_segment || ps->slot < 0 || ps->slot >= MAX_CLIENTS)
        return;
    b->last_time_ms = ps->time_ms;

    const int16_t *st = ps->stats;
    int frags     = (ps->num_stats > STAT_FRAGS)     ? st[STAT_FRAGS]     : 0;
    int spec_stat = (ps->num_stats > STAT_SPECTATOR) ? st[STAT_SPECTATOR] : 0;
    int chase     = (ps->num_stats > STAT_CHASE)     ? st[STAT_CHASE]     : 0;
    int health    = (ps->num_stats > STAT_HEALTH)    ? st[STAT_HEALTH]    : 0;
    int pickup_cs = (ps->num_stats > STAT_PICKUP_STRING)
                  ? st[STAT_PICKUP_STRING] : 0;

    /* Non-zero STAT_SPECTATOR is direct, mod-authoritative non-participation
     * evidence in baseq2, OpenFFA (a configstring index) and OpenTDM (1).  A
     * chase spectator has the target's stats copied in but keeps this set, so
     * it never inherits the target's participation. */
    bool spectator = spec_stat != 0;

    dli_track_t *t = dmf_track_for_slot(b, ps->slot);
    if (!t)
        return;
    t->last_seen_ms = ps->time_ms;
    if (spectator) {
        t->spectator_seen = true;
        if (chase)
            t->chase_spectator = true;
    } else {
        t->active_seen = true;
    }

    /* Phase truth, per adapter, before anything is counted. */
    if (b->adapter == DLI_ADAPTER_OPENFFA && !spectator) {
        int fs = (ps->num_stats > DMF_STAT_OPENFFA_FRAGS_STRING)
               ? st[DMF_STAT_OPENFFA_FRAGS_STRING] : 0;
        dmf_phase_t next = dmf_openffa_phase_from_stat(b, fs);
        if (next != b->phase) {
            if (next == DMF_PHASE_LIVE)
                dmf_open_epoch(b, DLI_CONF_AUTHORITATIVE, 0);
            else if (b->phase == DMF_PHASE_LIVE)
                dmf_close_epoch(b, DLI_CONF_AUTHORITATIVE, true);
            b->phase = next;
        }
    } else if (b->adapter == DLI_ADAPTER_GENERIC && b->cur_epoch < 0) {
        /* No phase truth available: one implicit interval covering the
         * segment, opened with UNKNOWN confidence so the query layer knows it
         * may not treat pre-live activity as a proven match start. */
        dmf_open_epoch(b, DLI_CONF_UNKNOWN, DLI_Q_NO_SCORE_AUTH);
        b->phase = DMF_PHASE_UNKNOWN;
    } else if (b->adapter == DLI_ADAPTER_OPENTDM && b->cur_epoch < 0 &&
               b->phase == DMF_PHASE_UNKNOWN &&
               dmf_general(b, DMF_TDM_CS_GAME_STATUS)[0]) {
        /* Recording began mid-match: the current authoritative live state is
         * accepted, but the epoch start is inferred, not observed. */
        if (dmf_opentdm_phase(dmf_general(b, DMF_TDM_CS_GAME_STATUS)) == DMF_PHASE_LIVE) {
            dmf_open_epoch(b, DLI_CONF_INFERRED, DLI_Q_MID_STREAM);
            b->phase = DMF_PHASE_LIVE;
        }
    }

    if (b->cur_epoch >= 0) {
        dli_part_t *p = dmf_part_for(b, b->cur_epoch, b->track_of_slot[ps->slot]);
        if (p) {
            if (spectator) {
                p->spectator_in_epoch = true;
            } else {
                p->active_in_epoch = true;

                /* Score authority: the recorder's own POV in a DM2, every
                 * captured slot in an MVD2.  Anyone else in a DM2 has no
                 * authoritative score timeline at all. */
                bool authoritative = b->mvd || ps->pov;
                if (authoritative) {
                    p->score_authoritative = true;
                    if (!b->have_prev[ps->slot]) {
                        p->baseline_score = frags;
                        p->final_score    = frags;
                        p->min_score      = frags;
                        p->max_score      = frags;
                    } else if (frags != b->prev_frags[ps->slot]) {
                        int delta = frags - b->prev_frags[ps->slot];
                        p->transitions++;
                        p->ever_score_changed = true;
                        if (delta > 0)
                            p->frags_earned += delta;
                        else
                            p->penalties += -delta;
                        p->final_score = frags;
                        if (frags < p->min_score) p->min_score = frags;
                        if (frags > p->max_score) p->max_score = frags;
                    } else {
                        p->final_score = frags;
                    }

                    /* STAT_PICKUP_STRING is a lingering HUD configstring.
                     * Count its rising/change edge once, then wait for it to
                     * clear before the same item can be counted again. */
                    if (pickup_cs > 0 &&
                        pickup_cs != b->prev_pickup_cs[ps->slot]) {
                        int item = pickup_cs - b->items_base;
                        if (item >= 0 && item < MAX_ITEMS &&
                            b->item_names[item][0]) {
                            const char *pickup_name = b->item_names[item];
                            /* Stock baseq2 registers every health size under
                             * the shared `Health` gitem.  The DM2 therefore
                             * carries the same pickup string for a stim and a
                             * Mega Health.  Resolve the subtype from the POV's
                             * recorded health transition, exactly when the
                             * pickup edge occurs; otherwise Mega disappears
                             * into a meaningless generic Health aggregate. */
                            if (!Q_stricmp(pickup_name, "Health") &&
                                b->have_prev[ps->slot]) {
                                int delta = health - b->prev_health[ps->slot];
                                /* A normal health entity can add at most 25;
                                 * >= 50 is therefore unambiguous Mega evidence.
                                 * Keep smaller/clipped/damaged transitions as
                                 * generic Health instead of inventing a subtype. */
                                if (delta >= 50)
                                    pickup_name = "Mega Health";
                            }
                            dli_pickup_fact_t *pf =
                                dmf_pickup_fact_for(b, p, pickup_name);
                            if (pf)
                                pf->count++;
                        }
                    }
                }
            }
        }
    }

    b->have_prev[ps->slot]      = true;
    b->prev_frags[ps->slot]     = frags;
    b->prev_spectator[ps->slot] = spectator;
    b->prev_pickup_cs[ps->slot] = pickup_cs;
    b->prev_health[ps->slot]    = health;
}

static void dmf_on_print(void *ud, int time_ms, int level, const char *text)
{
    dmf_builder_t *b = ud;
    (void)level;    /* level is unreliable across mods; the structural matcher
                     * in src/common/q2prox_obituary.c does the real filtering */
    if (!b->in_segment || !text || !text[0])
        return;
    b->last_time_ms = time_ms;

    /* The classic authoritative end of a Quake II match, and already the
     * signal the MVD path trusts for the same purpose.  Without it a baseq2
     * or OpenFFA epoch had no end at all, so a perfectly good final table
     * could never be promoted. */
    if (!strncmp(text, "Fraglimit hit.", 14) ||
        !strncmp(text, "Timelimit hit.", 14) ||
        strstr(text, "Match ended.")) {
        if (b->adapter != DLI_ADAPTER_OPENTDM && b->cur_epoch >= 0) {
            dmf_close_epoch(b, DLI_CONF_AUTHORITATIVE, true);
            b->phase = DMF_PHASE_POST;
        }
        return;
    }

    if (!Q2PROX_Obituary_IsObituaryText(text))
        return;

    int killer = -1, victim = -1;
    int limit = b->max_clients > 0 && b->max_clients <= MAX_CLIENTS
              ? b->max_clients : MAX_CLIENTS;
    if (!Q2PROX_Obituary_Match(text, b->name_ptrs, limit, &killer, &victim))
        return;

    dli_track_t *kt = dmf_track_for_slot(b, killer);
    dli_track_t *vt = dmf_track_for_slot(b, victim);
    if (kt) kt->combat_seen = true;
    if (vt) vt->combat_seen = true;

    /* Warmup and countdown kills are combat evidence, never match kills.
     * OpenTDM proves this explicitly (p_client.c gates every score and
     * detailed counter on at least MM_PLAYING); we honour the same boundary
     * for every adapter by only crediting an OPEN epoch. */
    if (b->cur_epoch < 0)
        return;

    if (kt) {
        dli_part_t *p = dmf_part_for(b, b->cur_epoch, b->track_of_slot[killer]);
        if (p) {
            p->kills++;
            dli_weapon_fact_t *wf = dmf_weapon_fact_for(
                b, p, dmf_weapon_from_obituary(Q2PROX_Obituary_Weapon(text)));
            if (wf) wf->kills++;
        }
    }
    if (vt) {
        dli_part_t *p = dmf_part_for(b, b->cur_epoch, b->track_of_slot[victim]);
        if (p) {
            p->deaths++;
            dli_weapon_fact_t *wf = dmf_weapon_fact_for(
                b, p, dmf_weapon_from_obituary(Q2PROX_Obituary_Weapon(text)));
            if (wf) wf->deaths++;
        }
    }
}

static void dmf_on_layout(void *ud, int time_ms, const char *text,
                          size_t length, bool truncated)
{
    dmf_builder_t *b = ud;
    if (!b->in_segment || !text)
        return;
    b->last_time_ms = time_ms;

    /* A cut scoreboard is not a scoreboard.  Recording that separately keeps
     * "we never saw one" and "we saw one and could not read all of it" from
     * collapsing into the same silent unknown. */
    if (truncated) {
        b->saw_truncated_layout = true;
        return;
    }

    dsb_board_t board;
    if (!DSB_Parse(text, length, &board)) {
        /* Manual scores, MOTD, help and OpenFFA's `High Scores` all use the
         * same drawing primitives.  Not recognising one is normal and must
         * not disturb an existing candidate. */
        b->saw_unsupported_layout = true;
        return;
    }

    b->cand       = board;

    /* baseq2 layouts carry a client SLOT, never a name.  Bind the slot to the
     * playerskin identity HERE, at the exact instant this layout was sent —
     * not at epoch promotion.  A slot released, reused or renamed in between
     * would otherwise answer a different question than the one the scoreboard
     * asked, and the answer would silently look correct.  A slot with no live
     * playerskin stays honestly unnamed; nothing is invented. */
    for (int r = 0; r < b->cand.num_rows; r++) {
        dsb_row_t *row = &b->cand.rows[r];
        if (row->have_name || row->slot < 0 || row->slot >= MAX_CLIENTS)
            continue;
        if (!b->names[row->slot][0])
            continue;
        Q_strlcpy(row->name, b->names[row->slot], sizeof(row->name));
        row->have_name = true;
    }

    b->cand_valid = true;
    b->cand_time  = time_ms;
    b->cand_epoch = b->cur_epoch;

    /* End-before-layout: the mod closed the epoch and then sent the final
     * table.  Accept it for the epoch that just ended, within one bounded
     * window, and never for an older one. */
    if (b->cur_epoch < 0 && b->last_closed_epoch >= 0 &&
        time_ms - b->last_closed_time <= 10000)
        dmf_apply_scoreboard(b, b->last_closed_epoch);
}

/* Attach the candidate to an epoch as ranking evidence.  This never touches
 * the personal timeline: a final table says where someone finished, not what
 * they did while the demo was recording. */
static void dmf_apply_scoreboard(dmf_builder_t *b, int epoch_index)
{
    if (epoch_index < 0 || epoch_index >= b->num_epochs)
        return;
    dli_epoch_t *ep = &b->epochs[epoch_index];
    if (ep->have_final_scoreboard)
        return;

    if (!b->cand_valid) {
        ep->result_reason = b->saw_truncated_layout
            ? DLI_RR_TRUNCATED_SCOREBOARD
            : (b->saw_unsupported_layout ? DLI_RR_UNSUPPORTED_SCOREBOARD
                                         : DLI_RR_NO_FINAL_SCOREBOARD);
        return;
    }

    const dsb_board_t *sb = &b->cand;

    ep->have_final_scoreboard = true;

    /* Keep the table itself.  It is the only record a client demo has of the
     * other players, and the result was derived from it, so discarding it
     * leaves a real win with nobody on the other side of it. */
    ep->first_board_row = b->num_board_rows;
    ep->num_board_rows  = 0;
    for (int r = 0; r < sb->num_rows &&
                    b->num_board_rows < DMF_MAX_BOARD_ROWS; r++) {
        dli_board_row_t *br = &b->board_rows[b->num_board_rows++];
        memset(br, 0, sizeof(*br));
        Q_strlcpy(br->name, sb->rows[r].name, sizeof(br->name));
        br->slot  = sb->rows[r].slot;
        br->score = sb->rows[r].score;
        br->team  = sb->rows[r].team;
        ep->num_board_rows++;
    }

    ep->scoreboard_adapter =
        sb->adapter == DSB_ADAPTER_OPENFFA ? DLI_ADAPTER_OPENFFA :
        sb->adapter == DSB_ADAPTER_OPENTDM ? DLI_ADAPTER_OPENTDM :
                                             DLI_ADAPTER_GENERIC;

    if (sb->team_mode) {
        ep->team_score[1] = sb->team_score[0];
        ep->team_score[2] = sb->team_score[1];
        ep->team_conf     = DLI_CONF_AUTHORITATIVE;
    }

    bool ambiguous = false;

    for (int i = 0; i < ep->num_parts; i++) {
        dli_part_t  *p  = &b->parts[ep->first_part + i];
        dli_track_t *tr = &b->tracks[p->track_index];

        int row;
        if (sb->adapter == DSB_ADAPTER_BASEQ2) {
            /* baseq2 sends no name at all; the slot IS the identity. */
            row = DSB_FindRowBySlot(sb, tr->slot);
        } else {
            char key[DLI_NAME_SIZE];
            if (!tr->alias_count) { row = -1; }
            else {
                DMF_NormalizeName(key, sizeof(key),
                                  tr->aliases[tr->alias_count - 1]);
                row = DSB_FindRowByName(sb, key, DMF_NormalizeName);
            }
        }

        if (row == DSB_ROW_AMBIGUOUS) {
            ambiguous = true;
            continue;
        }
        if (row < 0)
            continue;

        p->rank_score           = sb->rows[row].score;
        p->rank_conf            = DLI_CONF_AUTHORITATIVE;
        p->rank_from_scoreboard = true;
        p->rank_result          = DSB_RankOf(sb, row);
        /* Everyone else on the board, or in team mode everyone on the
         * other side of it.  Either way this is what the evidence
         * shows, not how many clients the demo happened to track. */
        if (sb->team_mode) {
            int foes = 0;
            for (int r = 0; r < sb->num_rows; r++)
                if (sb->rows[r].team &&
                    sb->rows[r].team != sb->rows[row].team)
                    foes++;
            p->rank_opponents   = foes;
        } else {
            p->rank_opponents   = sb->num_rows > 0 ? sb->num_rows - 1 : 0;
        }

        if (sb->team_mode && sb->rows[row].team) {
            tr->team      = sb->rows[row].team;
            tr->team_conf = DLI_CONF_AUTHORITATIVE;
        }
    }

    if (ambiguous)
        ep->result_reason = DLI_RR_AMBIGUOUS_ROW_IDENTITY;
    else if (sb->overflowed)
        ep->result_reason = DLI_RR_INCOMPLETE_SCOREBOARD;
}

static void dmf_on_frame(void *ud, int time_ms)
{
    dmf_builder_t *b = ud;
    b->last_time_ms = time_ms;
    if (b->in_segment)
        b->segments[b->cur_segment].end_ms = time_ms;
    if (b->cur_epoch >= 0)
        b->epochs[b->cur_epoch].end_ms = time_ms;
}

/* ------------------------------------------------------------------ */
/* OpenTDM team result adapter (conservative scoreboard read)          */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Public builder API                                                  */
/* ------------------------------------------------------------------ */

dmf_builder_t *DMF_Create(void)
{
    dmf_builder_t *b = calloc(1, sizeof(*b));
    if (!b)
        return NULL;

    b->segments = calloc(DMF_MAX_SEGMENTS, sizeof(b->segments[0]));
    b->epochs   = calloc(DMF_MAX_EPOCHS,   sizeof(b->epochs[0]));
    b->tracks   = calloc(DMF_MAX_TRACKS,   sizeof(b->tracks[0]));
    b->parts    = calloc(DMF_MAX_PARTS,    sizeof(b->parts[0]));
    b->board_rows = calloc(DMF_MAX_BOARD_ROWS, sizeof(b->board_rows[0]));
    b->weapon_facts = calloc(DMF_MAX_WEAPON_FACTS, sizeof(b->weapon_facts[0]));
    b->pickup_facts = calloc(DMF_MAX_PICKUP_FACTS, sizeof(b->pickup_facts[0]));
    if (!b->segments || !b->epochs || !b->tracks || !b->parts ||
        !b->board_rows || !b->weapon_facts || !b->pickup_facts) {
        DMF_Destroy(b);
        return NULL;
    }

    b->sink.ud            = b;
    b->sink.segment_start = dmf_on_segment;
    b->sink.configstring  = dmf_on_configstring;
    b->sink.player_state  = dmf_on_player_state;
    b->sink.print         = dmf_on_print;
    b->sink.layout        = dmf_on_layout;
    b->sink.frame         = dmf_on_frame;
    b->sink.cancelled     = dmf_cancelled;
    return b;
}

void DMF_Destroy(dmf_builder_t *b)
{
    if (!b)
        return;
    free(b->segments);
    free(b->epochs);
    free(b->tracks);
    free(b->parts);
    free(b->board_rows);
    free(b->weapon_facts);
    free(b->pickup_facts);
    free(b);
}

void DMF_Begin(dmf_builder_t *b, const char *gamedir_hint,
               bool (*cancelled)(void *ud), void *cancel_ud)
{
    if (!b)
        return;
    b->num_segments = 0;
    b->num_epochs   = 0;
    b->num_tracks   = 0;
    b->num_parts    = 0;
    b->quality      = 0;
    b->in_segment   = false;
    b->cur_segment  = -1;
    b->cur_epoch    = -1;
    b->phase        = DMF_PHASE_UNKNOWN;
    b->last_time_ms = 0;
    b->cancelled    = cancelled;
    b->cancel_ud    = cancel_ud;
    b->cand_valid = false;
    b->cand_epoch = -1;
    b->num_board_rows = 0;
    b->num_weapon_facts = 0;
    b->num_pickup_facts = 0;
    b->last_closed_epoch = -1;
    b->saw_truncated_layout = false;
    b->saw_unsupported_layout = false;
    Q_strlcpy(b->gamedir_hint, gamedir_hint ? gamedir_hint : "",
              sizeof(b->gamedir_hint));
    for (int i = 0; i < MAX_CLIENTS; i++) {
        b->track_of_slot[i] = -1;
        b->name_ptrs[i] = b->names[i];
    }
}

const dof_sink_t *DMF_Sink(dmf_builder_t *b)
{
    return b ? &b->sink : NULL;
}

void DMF_End(dmf_builder_t *b, unsigned decoder_quality)
{
    if (!b)
        return;

    if (decoder_quality & DOF_Q_TRUNCATED) b->quality |= DLI_Q_TRUNCATED;
    if (decoder_quality & DOF_Q_DESYNC)    b->quality |= DLI_Q_TRUNCATED;
    if (decoder_quality & DOF_Q_LIMIT_HIT) b->quality |= DLI_Q_LIMIT_HIT;

    dmf_close_segment(b);

    /* Mode classification, per adapter and only from real evidence. */
    for (int s = 0; s < b->num_segments; s++) {
        dli_segment_t *seg = &b->segments[s];
        int scoring_players = 0;
        bool teams_seen = false;

        for (int t = 0; t < seg->num_tracks; t++) {
            const dli_track_t *tr = &b->tracks[seg->first_track + t];
            if (tr->team == 1 || tr->team == 2)
                teams_seen = true;
            if (tr->active_seen)
                scoring_players++;
        }

        /* A client demo carries playerstate for ONE client, so counting
         * "scoring players" answers 1 for every DM2 and the mode came out
         * unknown for the whole local corpus — which then made every
         * per-opponent statement mode-less.  A recognized final table is the
         * arena-wide truth the demo does have: how many people finished the
         * match, and whether it had sides. */
        int board_rows = 0;
        bool board_teams = false;
        for (int e = 0; e < seg->num_epochs; e++) {
            const dli_epoch_t *ep = &b->epochs[seg->first_epoch + e];
            if (!ep->have_final_scoreboard)
                continue;
            if (ep->num_board_rows > board_rows)
                board_rows = ep->num_board_rows;
            if (ep->team_conf == DLI_CONF_AUTHORITATIVE)
                board_teams = true;
        }
        if (board_rows > scoring_players)
            scoring_players = board_rows;
        if (board_teams)
            teams_seen = true;

        switch (seg->adapter) {
        case DLI_ADAPTER_OPENTDM:
            /* OpenTDM is always a team product; TDM vs Insta TDM vs Duel is a
             * server cvar with no dedicated recorded configstring, so the
             * concrete flavour stays inferred rather than invented. */
            seg->mode      = teams_seen && scoring_players > 2
                           ? DLI_MODE_TDM : DLI_MODE_DUEL;
            seg->mode_conf = DLI_CONF_INFERRED;
            break;
        case DLI_ADAPTER_OPENFFA:
            seg->mode      = DLI_MODE_FFA;
            seg->mode_conf = DLI_CONF_AUTHORITATIVE;
            break;
        default:
            if (teams_seen) {
                seg->mode      = DLI_MODE_TDM;
                seg->mode_conf = DLI_CONF_INFERRED;
            } else if (scoring_players == 2) {
                seg->mode      = DLI_MODE_DUEL;
                seg->mode_conf = DLI_CONF_INFERRED;
            } else if (scoring_players > 2) {
                seg->mode      = DLI_MODE_FFA;
                seg->mode_conf = DLI_CONF_INFERRED;
            } else {
                seg->mode      = DLI_MODE_UNKNOWN;
                seg->mode_conf = DLI_CONF_UNKNOWN;
            }
            break;
        }
    }
}

int DMF_NumSegments(const dmf_builder_t *b) { return b ? b->num_segments : 0; }
int DMF_NumEpochs  (const dmf_builder_t *b) { return b ? b->num_epochs   : 0; }
int DMF_NumTracks  (const dmf_builder_t *b) { return b ? b->num_tracks   : 0; }
int DMF_NumParts   (const dmf_builder_t *b) { return b ? b->num_parts    : 0; }
int DMF_NumBoardRows(const dmf_builder_t *b)
{ return b ? b->num_board_rows : 0; }
int DMF_NumWeaponFacts(const dmf_builder_t *b)
{ return b ? b->num_weapon_facts : 0; }
int DMF_NumPickupFacts(const dmf_builder_t *b)
{ return b ? b->num_pickup_facts : 0; }

const dli_segment_t *DMF_Segments(const dmf_builder_t *b) { return b ? b->segments : NULL; }
const dli_epoch_t   *DMF_Epochs  (const dmf_builder_t *b) { return b ? b->epochs   : NULL; }
const dli_track_t   *DMF_Tracks  (const dmf_builder_t *b) { return b ? b->tracks   : NULL; }
const dli_part_t    *DMF_Parts   (const dmf_builder_t *b) { return b ? b->parts    : NULL; }
const dli_board_row_t *DMF_BoardRows(const dmf_builder_t *b)
{ return b ? b->board_rows : NULL; }
const dli_weapon_fact_t *DMF_WeaponFacts(const dmf_builder_t *b)
{ return b ? b->weapon_facts : NULL; }
const dli_pickup_fact_t *DMF_PickupFacts(const dmf_builder_t *b)
{ return b ? b->pickup_facts : NULL; }

unsigned DMF_Quality(const dmf_builder_t *b) { return b ? b->quality : 0; }
