/*
 * Q2PRO-X 1.5 — Demo Library Index Module (public Interface)
 *
 * ONE discovery/refresh pipeline, ONE global catalog/cache, ONE immutable
 * published snapshot, ONE re-entrant offline decoder.  The Demo Browser, the
 * Game Statistics overlay, the bounded Intro consumer and the existing Demo
 * Analytics adapter are all read-only consumers of this module; none of them
 * enumerates a directory, opens a demo file or parses a protocol message.
 *
 * Vocabulary used consistently in the code and comments:
 *
 *   Module     the ownership boundary implemented by demo_library.c —
 *              discovery, physical identity, metadata, offline decoding,
 *              neutral facts, the persistent cache, the serial worker,
 *              cancellation and snapshot publication.
 *   Interface  this header.  Consumers receive immutable snapshots, query
 *              results and a small progress copy.  They never receive a file
 *              handle, parser state or a worker-owned pointer.
 *   Seam       the source collector and the format/mode decoders.  Production
 *              and fixture adapters are interchangeable behind them.
 *   Adapter    DM2 / MVD2 and the mod-specific evidence translators
 *              (OpenTDM, OpenFFA, conservative generic).
 *   Depth      a small public API hiding filesystem, protocol, cache and
 *              thread complexity.
 *   Leverage   one parse serves the Browser, Statistics, the Intro card
 *              projection and Analytics.
 *   Locality   match truth lives inside the Module.  UI code never decides
 *              what a match is.
 *
 * The facts stored here are NEUTRAL: they do not depend on which aliases the
 * user is currently tracking.  Changing `cl_game_stats_names` is a query over
 * facts already in memory and never reparses an unchanged demo.
 */
#ifndef Q2PROX_DEMO_LIBRARY_H
#define Q2PROX_DEMO_LIBRARY_H

#include "shared/shared.h"

/* ------------------------------------------------------------------ */
/* Persistent cache identity                                           */
/* ------------------------------------------------------------------ */

/* The ONE live analysis cache.  Opened and replaced through FS_PATH_BASE so
 * it is genuinely global; the pre-existing v2 text cache used FS_PATH_GAME
 * and therefore produced one copy per mod.  `demo_browser_state.txt` stays
 * separate: it is user intent, not an analysis cache. */
#define DLI_CACHE_PATH              "q2pro-x/demo_index.dat"
#define DLI_CACHE_TMP_PATH          "q2pro-x/demo_index.dat.tmp"

/* Exact legacy generated files retired after the first successful v3 publish.
 * Only this generated name is ever removed, and only under a discovered game
 * directory.  No cfg, demo, favorite, playlist or other user data is touched. */
#define DLI_LEGACY_CACHE_NAME       "q2pro-x/demo_index.txt"

#define DLI_SCHEMA_VERSION          4
#define DLI_ENDIAN_MARKER           0x01020304u

/* Bumping either version invalidates exactly the affected half of every
 * cached record: metadata rebuilds keep neutral facts where possible, a fact
 * decoder bump rebuilds neutral facts and keeps metadata. */
#define DLI_META_DECODER_VERSION    1
/* 4: the OpenFFA and OpenTDM row readers were both wrong, so every cached
 *    scoreboard-derived result is wrong too and must be recomputed.
 * 5: the final table's other rows are kept now, so a match knows who it was
 *    played against; older caches have no board rows at all.
 * 6: weapon and pickup facts were added to meaningful personal matches.
 * 7: shared baseq2 `Health` pickup strings are resolved from the recorded
 *    health transition, so Mega Health is no longer folded into Health.
 * 8: DM2 state uses its recorded delta-frame reference and full-frame resets;
 *    absent references no longer produce plausible but incorrect facts. */
#define DLI_FACT_DECODER_VERSION    8

/* ------------------------------------------------------------------ */
/* Bounded record limits (untrusted bytes must never drive allocation) */
/* ------------------------------------------------------------------ */

#define DLI_MAX_ROOTS               8
#define DLI_MAX_FILES               65536
#define DLI_MAX_SEGMENTS_PER_FILE   64
#define DLI_MAX_EPOCHS_PER_SEGMENT  32
#define DLI_MAX_TRACKS_PER_SEGMENT  64
#define DLI_MAX_TRACK_ALIASES       4
#define DLI_MAX_ALIAS_QUERY         32
#define DLI_NAME_SIZE               16      /* matches mvd_player_t.name */
#define DLI_ITEM_NAME_SIZE          64
#define DLI_MAX_PICKUP_TYPES        MAX_ITEMS
#define DLI_INTRO_MAX_PICKUPS       24

/* ------------------------------------------------------------------ */
/* Small value types                                                   */
/* ------------------------------------------------------------------ */

typedef enum {
    DLI_TYPE_UNKNOWN = 0,
    DLI_TYPE_DM2,
    DLI_TYPE_MVD2,
} dli_type_t;

/* Every mode / team / result fact carries its provenance.  There is no
 * "probably" tier: a fact is either backed by explicit recorded evidence, a
 * conservative inference the code can name, or unknown. */
typedef enum {
    DLI_CONF_UNKNOWN = 0,
    DLI_CONF_INFERRED,
    DLI_CONF_AUTHORITATIVE,
} dli_conf_t;

typedef enum {
    DLI_MODE_UNKNOWN = 0,
    DLI_MODE_FFA,
    DLI_MODE_DUEL,
    DLI_MODE_TDM,
    DLI_MODE_INSTA_TDM,
    DLI_MODE_COOP_OR_SP,
    DLI_MODE_COUNT
} dli_mode_t;

/* Which mod adapter produced the phase/participant truth for a segment. */
typedef enum {
    DLI_ADAPTER_GENERIC = 0,
    DLI_ADAPTER_OPENFFA,
    DLI_ADAPTER_OPENTDM,
    DLI_ADAPTER_COUNT
} dli_adapter_t;

typedef enum {
    DLI_RESULT_UNKNOWN = 0,
    DLI_RESULT_WIN,
    DLI_RESULT_LOSS,
    DLI_RESULT_DRAW,
} dli_result_t;

/* Direct player-on-player means of death recorded by Quake II obituaries.
 * Environment, telefrags and unresolved mod prose never compete for
 * "favourite weapon": UNKNOWN is counted only as data quality. */
typedef enum {
    DLI_WEAPON_UNKNOWN = 0,
    DLI_WEAPON_BLASTER,
    DLI_WEAPON_SHOTGUN,
    DLI_WEAPON_SUPER_SHOTGUN,
    DLI_WEAPON_MACHINEGUN,
    DLI_WEAPON_CHAINGUN,
    DLI_WEAPON_GRENADES,
    DLI_WEAPON_GRENADE_LAUNCHER,
    DLI_WEAPON_ROCKET_LAUNCHER,
    DLI_WEAPON_HYPERBLASTER,
    DLI_WEAPON_RAILGUN,
    DLI_WEAPON_BFG10K,
    DLI_WEAPON_COUNT
} dli_weapon_t;

/* Per (epoch, tracked identity) classification.  Only DLI_CLASS_REAL_MATCH
 * contributes to match totals; every other class stays visible in the Data
 * tab so evidence is never silently discarded. */
typedef enum {
    DLI_CLASS_NONE = 0,         /* identity not present in this epoch */
    DLI_CLASS_SPECTATED,
    DLI_CLASS_ACTIVE_PLAY,      /* mod-authoritative active, live not proven */
    DLI_CLASS_WARMUP_ONLY,
    DLI_CLASS_REAL_MATCH,
    DLI_CLASS_UNVERIFIABLE,
    DLI_CLASS_AMBIGUOUS,
    DLI_CLASS_COUNT
} dli_class_t;

/* File record flags. */
#define DLI_F_BROKEN            0x0001  /* unreadable / no first message */
#define DLI_F_PARTIAL           0x0002  /* opened, metadata incomplete, playable */
#define DLI_F_PACKED            0x0004  /* found inside a VFS pack/container */
#define DLI_F_COMPRESSED        0x0008  /* .gz — Browser keeps it, Statistics excludes it */
#define DLI_F_STATS_ELIGIBLE    0x0010  /* real physical uncompressed .dm2/.mvd2 */
#define DLI_F_FACTS_PRESENT     0x0020  /* neutral facts decoded for this record */
#define DLI_F_FACTS_PARTIAL     0x0040  /* fact decode aborted mid-file */
#define DLI_F_DUPLICATE_EXACT   0x0080  /* collapsed into another record by content digest */
#define DLI_F_DUPLICATE_MAYBE   0x0100  /* possible duplicate — shown, never silently merged */
#define DLI_F_VFS_PLAYABLE      0x0200  /* reachable through the current VFS projection */

/* Why an epoch produced no result.  Exactly one primary reason per epoch,
 * so "unknown" is always accountable instead of silent. */
typedef enum {
    DLI_RR_NONE = 0,
    DLI_RR_NO_FINAL_SCOREBOARD,
    DLI_RR_UNSUPPORTED_SCOREBOARD,
    DLI_RR_TRUNCATED_SCOREBOARD,
    DLI_RR_AMBIGUOUS_ROW_IDENTITY,
    DLI_RR_INCOMPLETE_SCOREBOARD,
    DLI_RR_SCORE_CONFLICT,
    DLI_RR_STALE_SCOREBOARD,
    DLI_RR_COUNT
} dli_result_reason_t;

/* Segment / epoch quality flags — the honest reason a record is limited. */
#define DLI_Q_POV_ONLY          0x0001  /* DM2: only the recorder POV is authoritative */
#define DLI_Q_TRUNCATED         0x0002  /* stream ended inside the segment */
#define DLI_Q_LIMIT_HIT         0x0004  /* a bounded record cap was reached */
#define DLI_Q_NO_SCORE_AUTH     0x0008  /* no authoritative score source for this epoch */
#define DLI_Q_MID_STREAM        0x0010  /* recording began after the epoch had started */

/* ------------------------------------------------------------------ */
/* Neutral fact records                                                */
/* ------------------------------------------------------------------ */

/* One canonical physical storage root (basedir or homedir, deduplicated). */
typedef struct {
    char       *os_path;        /* absolute, canonical; worker-side only */
    bool        home;
} dli_root_t;

/* One physical demo file.  Identity is (root_id, game_dir, rel_path); the
 * same `demos/foo.dm2` under two mods is two records and never collides. */
typedef struct {
    int         root_id;
    char       *game_dir;
    char       *rel_path;           /* normalized, always begins "demos/" */
    char       *os_path;            /* absolute physical path — never shown */
    char       *vfs_path;           /* engine-openable path, or NULL */
    char       *filename;           /* raw basename, byte-exact */
    char       *display_path;       /* sanitized for UI only */
    char       *display_filename;
    int64_t     size;
    int64_t     mtime;
    dli_type_t  type;
    unsigned    flags;
    unsigned    quality;

    /* Accepted Demo Browser metadata (v2 parity). */
    char       *map;
    char       *pov;
    char       *mod;
    char       *players;
    char       *maps;
    char       *mods;
    char       *pov_keys;
    int         segment_count;

    /* Decoder provenance for incremental invalidation. */
    int         meta_version;
    int         fact_version;

    /* Slices into the snapshot's segment array. */
    int         first_segment;
    int         num_segments;

    /* Exact-content digest of the decoded stream, used to collapse byte
     * duplicates after parsing.  Zero when facts were never decoded. */
    uint64_t    content_digest;
    int         duplicate_of;       /* file index, or -1 */
} dli_file_t;

/* One serverdata/connection epoch inside a file.  This — not the file — is
 * the unit counted by "Observed sessions". */
typedef struct {
    int         file_index;
    int         index_in_file;
    char        map[MAX_QPATH];
    char        mod[MAX_QPATH];
    int         start_ms;
    int         end_ms;
    dli_mode_t  mode;
    dli_conf_t  mode_conf;
    dli_adapter_t adapter;
    unsigned    quality;
    int         first_track;
    int         num_tracks;
    int         first_epoch;
    int         num_epochs;
    uint64_t    fingerprint;        /* strong event/result fingerprint */
    int         duplicate_of;       /* segment index, or -1 */
} dli_segment_t;

/* One bounded scoring interval inside a segment.  A segment can carry zero
 * (warmup only, aborted countdown) or several of these. */
/* One row of an epoch's recognized final scoreboard, kept as a neutral fact.
 *
 * A client demo records playerstate for the recorder and nobody else, so the
 * table it received at the end is the ONLY record it holds of who else was
 * there.  Ranking against those rows and then discarding them is what left a
 * real win as a win over nobody. */
typedef struct {
    char        name[DLI_NAME_SIZE];    /* as rendered; empty for baseq2 rows */
    int         slot;                   /* -1 when the grammar carries none */
    int         score;
    int         team;                   /* 1/2 in team mode, else 0 */
} dli_board_row_t;

/* Sparse, part-owned lists.  Index 0 is a valid record; -1 terminates.  The
 * indirection keeps a participant record compact while retaining every item
 * name a mod actually recorded. */
typedef struct {
    int             next;
    dli_weapon_t    weapon;
    int             kills;
    int             deaths;
} dli_weapon_fact_t;

typedef struct {
    int             next;
    char            name[DLI_ITEM_NAME_SIZE];
    int             count;
} dli_pickup_fact_t;

typedef struct {
    int         segment_index;
    int         start_ms;
    int         end_ms;
    dli_conf_t  start_conf;
    dli_conf_t  end_conf;
    bool        intermission_seen;
    bool        closed;             /* an authoritative end was observed */
    unsigned    quality;
    int         first_part;
    int         num_parts;
    int         team_score[3];      /* index 1,2 used; 0 unused */
    dli_conf_t  team_conf;
    uint64_t    fingerprint;

    /* Set when a recognized final scoreboard was associated with THIS epoch. */
    bool        have_final_scoreboard;
    dli_adapter_t scoreboard_adapter;
    dli_result_reason_t result_reason;

    /* The rows of that table, so the match knows who it was played against. */
    int         first_board_row;
    int         num_board_rows;
} dli_epoch_t;

/* One participant identity inside a segment: a player slot's lifetime. */
typedef struct {
    int         segment_index;
    int         slot;
    int         alias_count;
    char        aliases[DLI_MAX_TRACK_ALIASES][DLI_NAME_SIZE];
    int         team;               /* 0 unknown, 1/2 playable, 3 spectator */
    dli_conf_t  team_conf;
    int         first_seen_ms;
    int         last_seen_ms;
    /* When the slot was RELEASED (its playerskin cleared).  0 means the
     * identity was still current at the end of the segment.  A slot that is
     * released and later reused belongs to a different person, so identity
     * questions use this window and not `last_seen_ms`, which keeps advancing
     * for as long as the stream carries the slot at all. */
    int         identity_end_ms;
    bool        active_seen;        /* mod-authoritative spawned/active */
    bool        spectator_seen;     /* mod-authoritative spectator/observe */
    bool        chase_spectator;    /* stats copied from a chase target */
    bool        combat_seen;
    bool        is_recorder_pov;    /* DM2: this is the authoritative POV */
} dli_track_t;

/* One (epoch, track) scoring record. */
typedef struct {
    int         epoch_index;
    int         track_index;
    bool        active_in_epoch;
    bool        spectator_in_epoch;
    bool        score_authoritative;
    bool        ever_score_changed;
    int         baseline_score;
    int         final_score;
    int         min_score;
    int         max_score;
    int         transitions;
    int         frags_earned;       /* sum of positive score deltas */
    int         penalties;          /* sum of |negative score deltas| */
    int         kills;              /* accepted obituaries */
    int         deaths;
    int         first_weapon_fact;  /* dli_weapon_fact_t chain, or -1 */
    int         first_pickup_fact;  /* dli_pickup_fact_t chain, or -1 */

    /* Ranking evidence, kept SEPARATE from the timeline above on purpose.
     * A final scoreboard says where a player finished; it does not say what
     * they did while the demo was being recorded.  Folding a cumulative
     * scoreboard number into frags_earned would invent contribution — and
     * would let a meaningless Intro card become eligible. */
    int         rank_score;
    dli_conf_t  rank_conf;
    bool        rank_from_scoreboard;
    /* The finishing position the scoreboard states, resolved against the
     * OTHER rows of that same table.  A client demo carries no playerstate
     * for anyone else, so there is nothing else to rank against: the table
     * is the only place the opponents exist at all. */
    int         rank_result;            /* dli_result_t */
    int         rank_opponents;     /* others on the board; in team
                                     * mode, the other side of it */
} dli_part_t;

/* ------------------------------------------------------------------ */
/* Immutable published snapshot                                        */
/* ------------------------------------------------------------------ */

typedef struct dli_snapshot_s {
    unsigned            generation;
    int64_t             built_at;       /* wall clock of publication */

    const dli_root_t    *roots;
    int                  num_roots;
    const dli_file_t    *files;
    int                  num_files;
    const dli_segment_t *segments;
    int                  num_segments;
    const dli_epoch_t   *epochs;
    int                  num_epochs;
    const dli_track_t   *tracks;
    int                  num_tracks;
    const dli_part_t    *parts;
    int                  num_parts;

    const dli_board_row_t *board_rows;
    int                    num_board_rows;
    const dli_weapon_fact_t *weapon_facts;
    int                      num_weapon_facts;
    const dli_pickup_fact_t *pickup_facts;
    int                      num_pickup_facts;

    /* Honest accounting for the Data tab and `game_stats_info`. */
    int     stat_discovered;
    int     stat_eligible;
    int     stat_meta_reused;
    int     stat_meta_parsed;
    int     stat_facts_reused;
    int     stat_facts_parsed;
    int     stat_skipped_window;
    int     stat_broken;
    int     stat_duplicates_exact;
    int     stat_duplicates_maybe;
    int     stat_root_walks;
    int     stat_stats_extra_walks;     /* contractually 0 */
    int     stat_intro_extra_walks;     /* contractually 0 */
    int     stat_legacy_caches_seen;
    int     stat_legacy_caches_removed;
    int64_t stat_bytes_parsed;
    int64_t stat_cache_bytes;
    int64_t stat_memory_bytes;
    int64_t stat_cutoff;                /* local-calendar cutoff used */
    int     stat_window_days;

    /* Measured on the worker, not estimated.  Each figure covers only the
     * phase it names, so a rebuild that reuses everything reports its parse
     * phase as idle rather than as a number that looks like work. */
    int     stat_walk_ms;
    int     stat_parse_ms;
    int     stat_total_ms;

    /* Board rows an epoch claimed that its source array did not actually
     * hold.  Counted rather than assumed away: it means a cache or a builder
     * disagreed with itself, and silence there reads as "no opponents". */
    int     stat_board_rows_dropped;
} dli_snapshot_t;

/* ------------------------------------------------------------------ */
/* Worker progress (small copy, published at most 10 Hz)               */
/* ------------------------------------------------------------------ */

typedef enum {
    DLI_PHASE_IDLE = 0,
    DLI_PHASE_LOADING_CACHE,
    DLI_PHASE_DISCOVERING,
    DLI_PHASE_METADATA,
    DLI_PHASE_FACTS,
    DLI_PHASE_WRITING_CACHE,
    DLI_PHASE_DONE,
    DLI_PHASE_FAILED,
    DLI_PHASE_CANCELLED,
} dli_phase_t;

typedef struct {
    dli_phase_t phase;
    unsigned    generation;
    int         files_discovered;
    int         files_checked;
    int         meta_reused;
    int         meta_parsed;
    int         facts_reused;
    int         facts_parsed;
    int         facts_total;
    int64_t     bytes_processed;
    int         errors;
    bool        active;
    bool        cancel_requested;
    char        current[64];        /* display-safe, never a raw OS path */
} dli_progress_t;

/* ------------------------------------------------------------------ */
/* Refresh reasons                                                     */
/* ------------------------------------------------------------------ */

#define DLI_REASON_STARTUP          0x0001
#define DLI_REASON_USER             0x0002
#define DLI_REASON_BROWSER          0x0004
#define DLI_REASON_STATS            0x0008
#define DLI_REASON_REBUILD_FACTS    0x0010  /* drop facts in the current window */
#define DLI_REASON_REBUILD_ALL      0x0020  /* drop metadata and facts */

/* ------------------------------------------------------------------ */
/* Queries                                                             */
/* ------------------------------------------------------------------ */

/* Normalized alias set for exactly one tracked person. */
typedef struct {
    int     count;
    char    keys[DLI_MAX_ALIAS_QUERY][DLI_NAME_SIZE];
    bool    overflowed;             /* more than DLI_MAX_ALIAS_QUERY given */
} dli_alias_set_t;

typedef struct {
    dli_alias_set_t aliases;
    int64_t         cutoff;         /* inclusive local-calendar cutoff, mtime */
    int64_t         min_size;       /* bytes; smaller files are not a match */
    bool            intro_projection;   /* meaningful_personal_matches only */
} dli_stats_query_t;

typedef struct {
    int             count;
    int             wins, losses, draws;
    int             frags, deaths;
} dli_bucket_t;

typedef struct {
    int kills;
    int deaths;
} dli_weapon_stat_t;

typedef struct {
    char name[DLI_ITEM_NAME_SIZE];
    int  count;
} dli_pickup_stat_t;

#define DLI_MAX_LIST_ROWS   256

/* What a per-opponent win/loss/draw triple actually MEANS.
 *
 * Adding the whole match result to every opposing row is exact for a duel and
 * defensible for a team game, but in FFA it is simply false: losing a match
 * does not mean losing to each of the fifteen people you finished above.  The
 * triple therefore carries the relation that produced it, and the overlay is
 * required to label it accordingly. */
typedef enum {
    DLI_REL_UNKNOWN = 0,    /* mode or table not good enough to say anything */
    DLI_REL_HEAD_TO_HEAD,   /* duel: wins / losses / draws against them */
    DLI_REL_PLACED,         /* FFA: finished above / below / level with them */
    DLI_REL_TEAM,           /* TDM: team wins / losses / draws vs their side */
} dli_relation_t;

typedef struct {
    char        key[MAX_QPATH];
    /* Which mode this line belongs to, so the overlay can group by it.  A duel
     * against somebody and a TDM against the same person are different lines.
     * -1 means the bucket deliberately spans every mode. */
    int         mode;
    int64_t     last_date;      /* newest match in this bucket, file mtime */
    int64_t     first_date;     /* oldest match in this bucket */
    dli_bucket_t b;
    /* Opponent buckets only.  `unknown` counts meetings whose relation could
     * not be established — never guessed into the triple. */
    int         relation;       /* dli_relation_t */
    int         unknown;
} dli_named_bucket_t;

typedef struct {
    int64_t     date;               /* file mtime of the source demo */
    int         file_index;
    int         segment_index;
    int         epoch_index;
    dli_result_t result;
    dli_mode_t  mode;
    dli_conf_t  mode_conf;
    char        map[MAX_QPATH];
    /* Which mod ran the match.  The mode says what was played; the mod says
     * what it was played on, and they are not the same fact. */
    char        mod[MAX_QPATH];
    int         opponents;
    /* Size of the demo the match came from.  The date says when, the size
     * says how much of a match it was. */
    int64_t     file_size;
    int         frags;
    int         deaths;
    int         penalties;
    int         score_delta;
    dli_weapon_t favorite_kill_weapon;
    int          favorite_kill_weapon_count;
    dli_weapon_t deadliest_weapon;
    int          deadliest_weapon_count;
    int          pickups_total;
    dli_type_t  type;
    dli_conf_t  result_conf;
    dli_class_t cls;
    bool        duplicate_candidate;
} dli_match_row_t;

typedef struct {
    /* Match totals — real_match epochs only. */
    int     real_matches;
    int     wins, losses, draws;
    /* Scored/live candidates withheld from the match denominator because the
     * recording did not prove a final result.  A recognized match itself is
     * always exactly one of win/loss/draw. */
    int     unresolved_candidates;
    int     frags;
    int     penalties;
    int     score_delta;
    int     kills;
    int     deaths;
    dli_weapon_stat_t weapons[DLI_WEAPON_COUNT];
    int               num_pickups;
    dli_pickup_stat_t pickups[DLI_MAX_PICKUP_TYPES];
    int               pickups_total;
    int               pickup_types_truncated;

    /* Separately visible excluded categories. */
    int     spectated_epochs;
    int     warmup_epochs;
    int     active_play_epochs;
    int     unverifiable_epochs;
    int     ambiguous_epochs;

    /* Honest overall activity, never a match denominator. */
    int     observed_sessions;
    int     observed_sessions_dedup_dropped;

    /* Data quality. */
    int     excluded_too_small;     /* below the configured minimum size */
    int     partial_files;
    int     broken_files;
    int     duplicate_candidates;
    int     dm2_epochs;
    int     mvd2_epochs;
    int     reasons[DLI_RR_COUNT];      /* why candidates were not matches */
    int     results_from_scoreboard;

    /* Exact source-demo mtime bounds of the matches accepted by this query.
     * They are maintained before the bounded row list, so truncation cannot
     * turn either edge of the corpus into a UI guess. */
    int64_t first_match_date;
    int64_t last_match_date;

    /* Aggregates. */
    int                 num_maps;
    dli_named_bucket_t  maps[DLI_MAX_LIST_ROWS];
    int                 num_opponents;
    dli_named_bucket_t  opponents[DLI_MAX_LIST_ROWS];
    int                 num_mods;
    dli_named_bucket_t  mods[DLI_MAX_LIST_ROWS];
    dli_bucket_t        modes[DLI_MODE_COUNT];

    /* Match rows, newest first. */
    int                 num_rows;
    dli_match_row_t     rows[DLI_MAX_LIST_ROWS];
    int                 rows_truncated;

    unsigned    generation;
} dli_stats_result_t;

/* --------------------------------------------------------------------------
 * Detail projection
 *
 * ONE owner for "what does the pane say about the selected row".  The draw
 * function receives finished neutral facts; it never rescans snapshot arrays
 * and never reimplements result semantics — that is exactly how the overlay
 * and the tab it summarises drift apart.
 * ------------------------------------------------------------------------ */
#define DLI_DETAIL_RECENT_MAX   10
#define DLI_DETAIL_BOARD_MAX    16
#define DLI_DETAIL_NAMES_MAX    8

typedef enum {
    DLI_DETAIL_NONE = 0,
    DLI_DETAIL_MATCH,           /* one match row (Matches / Search) */
    DLI_DETAIL_OPPONENT,
    DLI_DETAIL_MAP,
    DLI_DETAIL_MODE,
} dli_detail_kind_t;

typedef struct {
    int64_t     date;
    char        map[MAX_QPATH];
    int         mode;
    dli_result_t result;        /* the match result */
    int         relation_outcome;   /* dli_result_t under the bucket relation */
    int         file_index;
    int         segment_index;
    int         epoch_index;
    int         row_index;      /* index into dli_stats_result_t.rows, or -1 */
} dli_detail_recent_t;

typedef struct {
    char        name[DLI_NAME_SIZE];
    int         slot;
    int         score;
    int         team;
} dli_detail_board_t;

typedef struct {
    dli_detail_kind_t kind;
    char        key[MAX_QPATH];     /* opponent name / map / mode label key */
    int         mode;               /* -1 = across every mode */

    dli_bucket_t agg;               /* count / W-L-D / frags / deaths */
    dli_weapon_stat_t weapons[DLI_WEAPON_COUNT];
    int               num_pickups;
    dli_pickup_stat_t pickups[DLI_MAX_PICKUP_TYPES];
    int               pickups_total;
    int               pickup_types_truncated;
    int         unknown;            /* results that stayed unknown */
    int         relation;           /* dli_relation_t, opponents only */
    int64_t     first_date, last_date;
    int         reasons[DLI_RR_COUNT];

    /* Maps shared (opponents) / most played maps (modes and maps).  Names, not
     * labels: localization belongs to the view, not to the projection. */
    int         num_names;
    char        names[DLI_DETAIL_NAMES_MAX][MAX_QPATH];
    int         name_count[DLI_DETAIL_NAMES_MAX];
    /* Modes this key was seen in, by mode index. */
    int         modes_seen[DLI_MODE_COUNT];

    /* Kind == MATCH only. */
    bool        have_match;
    dli_match_row_t match;
    int         duration_ms;
    int         num_board;
    dli_detail_board_t board[DLI_DETAIL_BOARD_MAX];
    bool        board_names_unknown;    /* rows exist but carry no identity */

    int         num_recent;
    dli_detail_recent_t recent[DLI_DETAIL_RECENT_MAX];
} dli_detail_t;

/* Builds the detail for the current corpus using the SAME walk, the SAME
 * classification and the SAME result semantics as DLI_QueryGameStats.
 * `row_index` selects a match row for DLI_DETAIL_MATCH; `key`/`mode` select an
 * opponent, map or mode.  Returns false when nothing could be projected. */
bool DLI_BuildDetail(const dli_snapshot_t *s, const dli_stats_query_t *q,
                     const dli_stats_result_t *res,
                     dli_detail_kind_t kind, const char *key, int mode,
                     int row_index, dli_detail_t *out);

/* The typed projection the Intro is allowed to see.  It is produced inside
 * the Module from the same immutable snapshot, never recomputed by UI code,
 * and contains no overall-session count and no other player's statistics. */
typedef struct {
    bool        valid;              /* a snapshot existed when this was taken */
    unsigned    generation;
    int         matches;            /* meaningful_personal_matches */
    int         wins, losses, draws;
    int         frags, deaths;
    dli_weapon_t favorite_kill_weapon;
    int          favorite_kill_weapon_count;
    dli_weapon_t deadliest_weapon;
    int          deadliest_weapon_count;
    int          pickups_total;
    int          num_pickup_highlights;
    /* Every strategic pickup found in the accepted matches, not a rotating
     * top-four sample.  Intro emits one complete scene for every entry. */
    dli_pickup_stat_t pickup_highlights[DLI_INTRO_MAX_PICKUPS];
    char        favorite_map[MAX_QPATH];
    char        favorite_mod[MAX_QPATH];
    dli_mode_t  favorite_mode;
    char        common_opponent[DLI_NAME_SIZE];
    /* When the most recent counted match was played, as the source demo's
     * mtime.  Formatted for display in the viewer's own timezone and language,
     * never as a raw number. */
    int64_t     last_match_date;
    /* Oldest source demo which passed the same strict meaningful-personal-
     * match projection as every Intro statistic.  A configured cutoff or an
     * excluded demo must never appear here. */
    int64_t     first_match_date;
    int64_t     period_end_date;
    /* The name the Intro is speaking to, taken from the live `name` at the
     * moment the projection was built. */
    /* Live userinfo value used only for the greeting.  Do not reuse the
     * 15-character recorded-player key: the local cvar may legitimately be
     * longer and the Intro must not amputate the player's chosen spelling. */
    char        player_name[MAX_INFO_VALUE];
} dli_intro_projection_t;

/* Result of a clear/refresh transaction, so callers consume a typed outcome
 * instead of assuming success. */
typedef enum {
    DLI_TXN_OK = 0,
    DLI_TXN_NOTHING_TO_DO,
    DLI_TXN_FAILED,
} dli_txn_result_t;

/* Test-only injection: force the next persistent cache commit to fail at the
 * named stage.  The production writer, path and open flags are untouched —
 * only the outcome of one stage is forced — so a failing commit still proves
 * the real transaction's rollback behaviour. */
typedef enum {
    DLI_FAULT_NONE = 0,
    DLI_FAULT_WRITE,        /* the candidate write reports short */
    DLI_FAULT_VALIDATE,     /* the re-read comparison reports mismatch */
    DLI_FAULT_REPLACE,      /* the atomic replace reports failure */
} dli_fault_t;

void        DLI_TestInjectCacheFault(dli_fault_t fault);

/* ------------------------------------------------------------------ */
/* Public Interface                                                    */
/* ------------------------------------------------------------------ */

/* Public settings owned by the Module.  Declared here so the query layer, the
 * overlay and the Intro consumer read the same objects rather than each
 * looking them up by name at draw time. */
extern cvar_t *cl_game_stats_auto;
extern cvar_t *cl_game_stats_days;
extern cvar_t *cl_game_stats_min_size_kb;
extern cvar_t *cl_game_stats_names;
extern cvar_t *cl_game_stats_intro;
extern cvar_t *cl_game_stats_intro_greeting;
extern cvar_t *cl_game_stats_intro_last_game;
extern cvar_t *cl_game_stats_intro_matches;
extern cvar_t *cl_game_stats_intro_results;
extern cvar_t *cl_game_stats_intro_combat;
extern cvar_t *cl_game_stats_intro_kill_weapon;
extern cvar_t *cl_game_stats_intro_death_weapon;
extern cvar_t *cl_game_stats_intro_items;
extern cvar_t *cl_game_stats_intro_map;
extern cvar_t *cl_game_stats_intro_mod;
extern cvar_t *cl_game_stats_intro_opponent;
extern cvar_t *cl_game_stats_debug;

/* The configured minimum in BYTES, clamped to the supported range.  One
 * place converts the setting so the overlay, the Intro and the console
 * diagnostics cannot disagree about what was excluded. */
int64_t     DLI_MinDemoSizeBytes(void);

/* --------------------------------------------------------------------------
 * Discovered names
 *
 * Names this machine has actually RECORDED under: the recorder POV identity of
 * local DM2 files, and nothing else.  Never an opponent, never a spectator,
 * never an MVD2 participant — those are other people, and silently adopting
 * them would quietly merge someone else's history into yours.
 *
 * Derived from the neutral cache already in memory: no second corpus parse and
 * no separate persistence.  Suggestions only; the user adds or removes.
 * ------------------------------------------------------------------------ */
#define DLI_MAX_DISCOVERED  32
typedef struct {
    char        name[DLI_NAME_SIZE];    /* as recorded, for display */
    int         count;                  /* demos this POV name appears in */
    int64_t     last_date;              /* newest such demo, file mtime */
} dli_discovered_name_t;

int         DLI_DiscoverPovNames(const dli_snapshot_t *snap,
                                 dli_discovered_name_t *out, int max_out);

void        DLI_Init(void);
void        DLI_Shutdown(void);

/* Main-thread pump.  Publishes a completed generation, retires the previous
 * snapshot and drains worker diagnostics.  Never blocks. */
void        DLI_Frame(void);

void        DLI_RequestRefresh(unsigned reasons, bool force);

/* Signal cancellation and return immediately.  Called from every map load,
 * connect, demo start and cinematic transition.  Never joins the worker. */
void        DLI_RequestCancelForMapLoad(void);

/* Immutable, main-thread only.  Never NULL after DLI_Init: an empty snapshot
 * is published at startup so every consumer has a valid object to read. */
const dli_snapshot_t *DLI_GetSnapshot(void);

dli_progress_t DLI_GetProgress(void);

/* Drop every neutral match fact from the ONE cache while keeping file
 * metadata and all user state.  Not a second file, not a second cache.
 *
 * Transactional: a fresh candidate generation is built, committed through
 * temp + revalidate + atomic replace, and published only after the commit
 * succeeded.  On any failure the live snapshot and the on-disk cache are both
 * left byte-for-byte as they were, and the typed result says so. */
dli_txn_result_t DLI_ClearMatchFacts(void);

/* Normalization is Module-owned so the overlay, the menu, the commands and
 * the Intro projection can never disagree about what an alias is. */
void        DLI_NormalizeAliasSet(const char *csv, const char *fallback_name,
                                  dli_alias_set_t *out);

/* Inclusive local-calendar cutoff: local midnight today minus N calendar
 * days, converted through the OS timezone/DST rules.  Never now - N*86400. */
int64_t     DLI_LocalCalendarCutoff(int days);

bool        DLI_QueryGameStats(const dli_snapshot_t *snap,
                               const dli_stats_query_t *q,
                               dli_stats_result_t *out);

/* Intro accessor.  O(1): copies a typed, generation-stamped value that was
 * prepared at a safe main-thread stage.  It never scans the corpus, sorts,
 * allocates or takes a worker lock, so it is legal in a draw path.  Fails
 * closed: a false return means draw nothing. */
bool        DLI_GetIntroProjection(dli_intro_projection_t *out);

/* The heavy half, called by the module when the snapshot generation, the
 * current name or the date window changes.  Never call this from a draw. */
void        DLI_BuildIntroProjection(dli_intro_projection_t *out);

/* Counted by the query layer so a test can prove the draw path is O(1). */
void        DLI_NoteQuery(void);

/* Browser projection: entries reachable through the CURRENT VFS, honoring
 * the accepted packed-demo policy.  Backing store for demo_index.c. */
int         DLI_BrowserEntryCount(void);
const dli_file_t *DLI_BrowserEntry(int index);

bool        DLI_IsBusy(void);

/* ------------------------------------------------------------------ */
/* Observable state for behavioral tests                               */
/* ------------------------------------------------------------------ */

/* Number of full corpus queries executed since process start.  The Intro
 * draw path must never advance this: it reads an already prepared typed
 * value.  Counting is one increment per query, and queries are rare. */
unsigned    DLI_QueryCount(void);

/* Number of times the worker was joined.  A map, connect, demo or cinematic
 * transition must never advance this; only true process shutdown may. */
unsigned    DLI_JoinCount(void);

/* The generation currently published, and whether a cancellation is pending.
 * Both are plain reads of committed state, not predictions. */
unsigned    DLI_CurrentGeneration(void);
bool        DLI_CancelPending(void);


/* Diagnostics text for `game_stats_info` and the Data tab.  Fills `out` with
 * a bounded, allocation-free report. */
void        DLI_FormatInfo(char *out, size_t out_size);

const char *DLI_TypeLabel(dli_type_t type);
dli_type_t  DLI_TypeFromPath(const char *path);
const char *DLI_ModeLabel(dli_mode_t mode, bool russian);
const char *DLI_ClassLabel(dli_class_t cls, bool russian);
const char *DLI_WeaponLabel(dli_weapon_t weapon, bool russian);

#endif /* Q2PROX_DEMO_LIBRARY_H */
