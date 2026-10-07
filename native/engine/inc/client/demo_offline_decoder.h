/*
 * Q2PRO-X 1.5 — offline DM2/MVD2 decoder core (private cursor)
 *
 * This is the re-entrant Seam the Demo Library Module owns.  It walks a demo
 * byte stream with a `sizebuf_t` it allocates itself and emits NEUTRAL events
 * into a caller-supplied sink.  It exists because the two pre-existing demo
 * readers cannot be used off the main thread:
 *
 *   CL_GetDemoInfo()       reads through the process-global `msg_read` and Q2
 *                          VFS handles, and deliberately never parses frames.
 *   MVD_PreScanAnalytics() saves/restores global `msg_read` and global
 *                          `mvd_jmpbuf` — valid only on the calling thread —
 *                          and allocates through the shared zone.
 *
 * Hard contract for everything in demo_offline_decoder.c:
 *
 *   - no `msg_read`, no `mvd_jmpbuf`, no `cl`, `cls`, `sv`, no MVD channel
 *     lists, no renderer, no cvars, no Q2 VFS, no `Com_Printf`/`Com_Error`,
 *     no `Z_Malloc` family;
 *   - input arrives through `dof_reader_t`, which the Module implements with
 *     raw stdio on the worker thread;
 *   - all allocation is one bounded block owned by the job;
 *   - every length, index and count coming out of the file is validated
 *     before it is used for allocation or pointer arithmetic;
 *   - a malformed stream aborts THIS file with a quality flag and never the
 *     process.
 *
 * The protocol primitives themselves are NOT re-implemented here: the decoder
 * calls the shared `MSG_*_From()` entry points in `src/common/msg.c` with its
 * own cursor, so there is exactly one delta-entity / delta-playerstate
 * implementation in the engine and it cannot drift.
 */
#ifndef Q2PROX_DEMO_OFFLINE_DECODER_H
#define Q2PROX_DEMO_OFFLINE_DECODER_H

#include "shared/shared.h"

/* Quality flags reported for the decoded file / segment. */
#define DOF_Q_TRUNCATED         0x0001  /* stream ended mid-message */
#define DOF_Q_DESYNC            0x0002  /* a message did not consume exactly */
#define DOF_Q_LIMIT_HIT         0x0004  /* a bounded cap stopped collection */
#define DOF_Q_UNSUPPORTED       0x0008  /* protocol variant not decodable */
#define DOF_Q_CANCELLED         0x0010

/* Decoder return codes. */
#define DOF_OK                   0
#define DOF_ERR_OPEN            (-1)
#define DOF_ERR_FORMAT          (-2)
#define DOF_ERR_TRUNCATED       (-3)
#define DOF_ERR_CANCELLED       (-4)
#define DOF_ERR_NOMEM           (-5)

typedef struct {
    void   *ud;
    /* Fill up to `len` bytes.  Returns the number actually read (0 at EOF)
     * or a negative value on a hard I/O error.  Short reads are legal. */
    int   (*read)(void *ud, void *dst, int len);
} dof_reader_t;

typedef struct {
    bool        mvd;
    int         protocol;       /* raw protocol long from the stream */
    bool        extended;       /* extended configstring remap in use */
    int         client_num;     /* recorder POV for DM2, dummy slot for MVD2 */
    int         max_clients;
    int         items_base;     /* protocol-remapped CS_ITEMS */
    char        gamedir[MAX_QPATH];
    int         index_in_file;
} dof_segment_info_t;

/* One decoded playerstate observation.  `stats` is the FULL stat array for
 * the slot after the delta has been applied, so the consumer can read
 * STAT_FRAGS, STAT_SPECTATOR, STAT_CHASE, STAT_FRAGS_STRING and any
 * mod-specific slot without knowing the wire encoding. */
typedef struct {
    int             time_ms;
    int             slot;
    bool            pov;            /* DM2: this is the recorder's own state */
    int             pm_type;
    int             num_stats;
    const int16_t  *stats;

    /*
     * Where he was and what he was doing, for consumers that read recorded
     * play as movement evidence (MAPGEN-1 contract 6.4).
     *
     * Unpacked from the wire's fixed point into world units here, because
     * that scaling is a property of the protocol and belongs with the thing
     * that knows the protocol. `pm_flags` and `pm_time` are widened to the
     * new API's width so one struct serves both; `pm_time` keeps the units
     * the stream used - eight milliseconds a unit in the old protocol, one in
     * the new - and `protocol` in the segment info is what tells them apart.
     *
     * A demo carries about ten of these a second. It is an observation of
     * where a player WAS, never the input that put him there.
     */
    float           origin[3];
    float           velocity[3];
    uint16_t        pm_flags;
    uint16_t        pm_time;
    int16_t         gravity;
    float           view_angles[3];   /* clamped, as the server sent them   */
    float           delta_angles[3];  /* spawns, teleports, rotating movers */
    int             gunindex, gunframe; /* recorded view weapon, not an item ID */
    float           view_offset[3], kick_angles[3], fov;
} dof_player_state_t;

/* Neutral recorded cues. time_ms is the parser's current frame clock; seq/order
 * and frame boundaries must be retained by consumers. An MVD cue is a server
 * observation and does not establish that every participant could hear it. */
typedef struct {
    int scope; /* -1 DM2 recorder, -2 MVD unicast; otherwise multicast_t */
    int leaf, target_slot; /* -1 when absent from the recording */
} dof_delivery_t;

typedef struct {
    int time_ms, flags, index, entity, channel;
    int volume, attenuation, offset_ms; /* raw volume/attenuation bytes */
    bool mvd, has_position;
    float position[3];
    dof_delivery_t delivery;
} dof_sound_t;
typedef struct {
    int time_ms, entity, weapon;
    bool monster, mvd;
    dof_delivery_t delivery;
} dof_muzzleflash_t;
typedef struct {
    int time_ms, type, has_position, has_end, direction;
    bool mvd;
    float position[3], end[3];
    dof_delivery_t delivery;
} dof_effect_t;

/* Optional reconstructed packet world, indexed by entity number. Absence
 * means absent from this POV packet, never proof of death. MVD is a server view.
 * Callbacks are synchronous; these borrowed pointers must not escape them. */
typedef struct {
    entity_state_t state;
    entity_state_extension_t extension;
    bool present;
} dof_entity_state_t;
typedef struct {
    int time_ms, frame_number, delta_frame, max_entities;
    bool mvd;
    const dof_entity_state_t *entities;
} dof_entity_frame_t;

typedef struct {
    void   *ud;

    /* A new serverdata block: a fresh connection/map segment. */
    void  (*segment_start)(void *ud, const dof_segment_info_t *info);

    /* Any configstring the decoder was asked to observe (player names, the
     * map name and the bounded general band used by mod adapters). */
    void  (*configstring)(void *ud, int time_ms, int index, const char *text);

    /* One player's state after a frame delta. */
    void  (*player_state)(void *ud, const dof_player_state_t *ps);

    /* svc_print / mvd_print, including prints nested inside an MVD unicast. */
    void  (*print)(void *ud, int time_ms, int level, const char *text);

    /* svc_layout — a scoreboard candidate.  Never trusted by itself.
     *
     * `length` is what the stream actually carried and `truncated` says the
     * text was longer than the decoder could store.  Both are part of the
     * event because a valid PREFIX of a scoreboard must never be parsed as a
     * complete one: OpenFFA deliberately emits layouts up to MAX_NET_STRING,
     * and a 1024-byte buffer silently cut real scoreboards in half. */
    void  (*layout)(void *ud, int time_ms, const char *text,
                    size_t length, bool truncated);

    /* Called once per decoded frame, after every player_state of that frame. */
    void  (*frame)(void *ud, int time_ms);

    /* Cooperative cancellation, polled between messages and inside long
     * files.  Returning true aborts the decode with DOF_ERR_CANCELLED. */
    bool  (*cancelled)(void *ud);

    /* Optional BOTDUEL consumers; existing fact sinks need not subscribe. */
    void  (*sound)(void *ud, const dof_sound_t *sound);
    void  (*muzzleflash)(void *ud, const dof_muzzleflash_t *flash);
    void  (*entity_frame)(void *ud, const dof_entity_frame_t *frame);
    void  (*effect)(void *ud, const dof_effect_t *effect);
    /* Optional bounded diagnostic; decoder itself never prints globally. */
    void  (*diagnostic)(void *ud, const char *code, int frame, int detail);
} dof_sink_t;

/* Decode one demo stream.  `out_quality` receives the DOF_Q_* flags and is
 * always written.  Returns DOF_OK or a negative DOF_ERR_*. */
int DOF_Decode(const dof_reader_t *reader, const dof_sink_t *sink,
               unsigned *out_quality);

/* Size of the scratch block DOF_Decode allocates internally, exposed so the
 * Module can report bounded memory honestly. */
size_t DOF_ScratchSize(void);
/* Entity history is only allocated for a subscribed consumer, in the same
 * bounded job block. Existing fact-only workers retain the smaller size. */
size_t DOF_ScratchSizeForSink(const dof_sink_t *sink);

#endif /* Q2PROX_DEMO_OFFLINE_DECODER_H */
