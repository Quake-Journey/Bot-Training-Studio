/*
 * Q2PRO-X 1.5 — offline DM2/MVD2 decoder core (private cursor).
 *
 * See inc/client/demo_offline_decoder.h for the hard worker-safety contract.
 * Nothing in this file may touch process-global parser state, the Q2 virtual
 * filesystem, the zone allocator, cvars, the renderer or the console.
 *
 * Why a full message walk and not the old header-only scan:
 *
 *   The existing metadata scan abandons a DM2 message as soon as it meets a
 *   byte it cannot advance past, which is every frame and every temporary
 *   entity.  That is fine for "what map is this", and useless for statistics:
 *   an obituary that happens to sit behind a rocket explosion in the same
 *   demo message would simply never be seen, and player scores live inside
 *   the delta playerstate the old scan explicitly refuses to decode.  So this
 *   decoder consumes every command exactly, and any message that does not end
 *   on its own boundary is reported as a desync instead of being silently
 *   trusted.
 */

#include "shared/shared.h"
#include "common/msg.h"
#include "common/protocol.h"
#include "common/sizebuf.h"
#include "common/cmodel.h"
#include "server/mvd/protocol.h"
#include "client/demo_offline_decoder.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Decoder state                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    dof_entity_state_t baseline[MAX_EDICTS];
    dof_entity_state_t frames[UPDATE_BACKUP][MAX_EDICTS];
    dof_entity_state_t work[MAX_EDICTS];
} dof_scene_t;

typedef struct {
    const dof_reader_t *rd;
    const dof_sink_t   *sink;

    sizebuf_t           msg;
    byte                buf[MAX_MSGLEN];

    bool                mvd;
    bool                have_segment;
    bool                dm2_disconnected;
    const cs_remap_t   *csr;
    int                 protocol;        /* raw long from the stream */
    int                 eff_protocol;    /* DM2: 26..34 after the extended fold */
    int                 mvd_version;
    int                 mvd_flags;
    msgEsFlags_t        esFlags;
    msgPsFlags_t        psFlags;
    int                 client_num;
    int                 max_clients;
    int                 num_stats;

    unsigned            quality;
    int                 segment_index;

    /* Monotonic clock.  DM2 frame numbers run for the whole file; MVD frame
     * numbers restart at every serverdata, so the base carries the offset. */
    int                 time_base_ms;
    int                 seg_time_ms;
    int                 framenum;

    /* DM2 deltas reference an explicit earlier frame, not necessarily the
     * preceding packet. Keep tagged history, like the ordinary client. */
    player_state_t      dm2_ps[UPDATE_BACKUP];
    int                 dm2_frame[UPDATE_BACKUP];
    bool                dm2_valid[UPDATE_BACKUP];

    /* MVD per-slot playerstate.  Allocated once with the decoder block. */
    player_state_t      mvd_ps[MAX_CLIENTS];
    bool                mvd_inuse[MAX_CLIENTS];

    /* Throwaway targets so the shared delta parsers advance the cursor with
     * production-identical byte consumption without us keeping the payload. */
    entity_state_t              ent;
    entity_state_extension_t    ext;
    dof_scene_t                 *scene; /* optional, same job block */
    dof_delivery_t              delivery;

    char                strbuf[MAX_STRING_CHARS];
    /* Layouts have their own, larger buffer: they are the only
     * message whose full length is load-bearing. */
    char                layoutbuf[MAX_NET_STRING];
} dof_t;

static void dof_diagnostic(dof_t *d, const char *code, int frame, int detail)
{
    if (d->sink->diagnostic)
        d->sink->diagnostic(d->sink->ud, code, frame, detail);
}

size_t DOF_ScratchSize(void)
{
    return sizeof(dof_t);
}
size_t DOF_ScratchSizeForSink(const dof_sink_t *sink)
{
    return sizeof(dof_t)+(sink && sink->entity_frame ? sizeof(dof_scene_t) : 0);
}

/* ------------------------------------------------------------------ */
/* Raw input                                                           */
/* ------------------------------------------------------------------ */

static bool dof_read_exact(dof_t *d, void *dst, int len)
{
    byte *p = dst;
    while (len > 0) {
        int got = d->rd->read(d->rd->ud, p, len);
        if (got <= 0)
            return false;
        p   += got;
        len -= got;
    }
    return true;
}

/* Returns 1 on a message, 0 at a clean end of stream, -1 on a broken one. */
static int dof_next_dm2_message(dof_t *d)
{
    uint32_t msglen;

    int got = d->rd->read(d->rd->ud, &msglen, 1);
    if (got == 0)
        return 0;                       /* clean EOF without the -1 marker */
    if (got != 1 || !dof_read_exact(d, (byte *)&msglen + 1, 3)) {
        d->quality |= DOF_Q_TRUNCATED;
        return -1;                      /* a partial length is not EOF */
    }
    if (msglen == (uint32_t)-1)
        return 0;

    msglen = LittleLong(msglen);
    if (msglen == 0 || msglen > sizeof(d->buf)) {
        d->quality |= DOF_Q_TRUNCATED;
        return -1;
    }
    if (!dof_read_exact(d, d->buf, msglen)) {
        d->quality |= DOF_Q_TRUNCATED;
        return -1;
    }

    SZ_InitRead(&d->msg, d->buf, msglen);
    return 1;
}

static int dof_next_mvd_message(dof_t *d)
{
    uint16_t us;

    if (!dof_read_exact(d, &us, 2))
        return 0;
    if (us == 0)
        return 0;                       /* MVD end marker */

    int msglen = LittleShort(us);
    if (msglen <= 0 || msglen > (int)sizeof(d->buf)) {
        d->quality |= DOF_Q_TRUNCATED;
        return -1;
    }
    if (!dof_read_exact(d, d->buf, msglen)) {
        d->quality |= DOF_Q_TRUNCATED;
        return -1;
    }

    SZ_InitRead(&d->msg, d->buf, msglen);
    return 1;
}

static inline bool dof_overrun(const dof_t *d)
{
    return d->msg.readcount > d->msg.cursize;
}

static inline int dof_now(const dof_t *d)
{
    return d->time_base_ms + d->seg_time_ms;
}

/* ------------------------------------------------------------------ */
/* Shared helpers                                                      */
/* ------------------------------------------------------------------ */

static void dof_emit_configstring(dof_t *d, int index)
{
    /* MSG_ReadString always consumes up to the terminating NUL regardless of
     * how much it stores, so a bounded destination can never desync us. */
    MSG_ReadString_From(&d->msg, d->strbuf, sizeof(d->strbuf));
    if (dof_overrun(d))
        return;

    /* CS_MAXCLIENTS bounds every later slot check, so the decoder consumes it
     * itself instead of trusting a consumer to feed it back. */
    if (index == (int)d->csr->maxclients) {
        int n = atoi(d->strbuf);
        if (n >= 1 && n <= MAX_CLIENTS)
            d->max_clients = n;
    }

    if (d->sink->configstring)
        d->sink->configstring(d->sink->ud, dof_now(d), index, d->strbuf);
}

/* Consume one delta entity exactly the way the live parsers do. */
static bool dof_skip_delta_entity(dof_t *d, bool mvd_semantics)
{
    uint64_t bits;
    int number = MSG_ParseEntityBits_From(&d->msg, &bits, d->esFlags);

    if (dof_overrun(d))
        return false;
    if (number < 0 || number >= (int)d->csr->max_edicts || number >= MAX_EDICTS)
        return false;
    if (number == 0)
        return true;                    /* end of the packet-entities list */

    /* The DM2 client skips the field decode for a removal; the MVD client
     * calls it with a bits value that carries no field bits.  Both consume
     * the same bytes — mirroring each one keeps that provable. */
    if (!mvd_semantics && (bits & U_REMOVE))
        return true;

    if(d->scene && !mvd_semantics) {
        dof_entity_state_t *e=&d->scene->baseline[number];
        memset(e,0,sizeof(*e));
        MSG_ParseDeltaEntity_From(&d->msg,&e->state,&e->extension,number,bits,d->esFlags);
        e->present=true;
    } else MSG_ParseDeltaEntity_From(&d->msg, &d->ent, &d->ext, number, bits, d->esFlags);
    return !dof_overrun(d);
}

static bool dof_skip_packet_entities(dof_t *d, bool mvd_semantics)
{
    for (int guard = 0; guard < MAX_EDICTS + 2; guard++) {
        uint32_t before = d->msg.readcount;
        uint64_t bits;
        int number = MSG_ParseEntityBits_From(&d->msg, &bits, d->esFlags);

        if (dof_overrun(d))
            return false;
        if (number < 0 || number >= (int)d->csr->max_edicts || number >= MAX_EDICTS)
            return false;
        if (number == 0)
            return true;
        if (!mvd_semantics && (bits & U_REMOVE))
            continue;

        MSG_ParseDeltaEntity_From(&d->msg, &d->ent, &d->ext, number, bits, d->esFlags);
        if (dof_overrun(d) || d->msg.readcount == before)
            return false;
    }
    d->quality |= DOF_Q_LIMIT_HIT;
    return false;
}

static bool dof_entity_indices_valid(dof_t *d,const dof_entity_state_t *e)
{
    const entity_state_t *s=&e->state;
    return s->modelindex>=0 && s->modelindex2>=0 && s->modelindex3>=0 && s->modelindex4>=0 &&
        (unsigned)(s->modelindex|s->modelindex2|s->modelindex3|s->modelindex4)<d->csr->max_models &&
        s->sound>=0 && (unsigned)s->sound<d->csr->max_sounds;
}
static bool dof_packet_world(dof_t *d,bool mvd,int deltaframe)
{
    if(!d->scene) return dof_skip_packet_entities(d,mvd);
    dof_entity_state_t *world=d->scene->work;
    size_t bytes=d->csr->max_edicts*sizeof(*world);
    if(!mvd) {
        if(deltaframe>0) memcpy(world,d->scene->frames[deltaframe&UPDATE_MASK],bytes);
        else memset(world,0,bytes);
    }
    /* Same event/old-origin reset as the live DM2/MVD consumers. In MVD a
     * removed entity retains its delta base for when it returns. */
    for(unsigned i=1;i<d->csr->max_edicts;i++) if(world[i].present) {
        world[i].state.event=0;
        if(!(world[i].state.renderfx&RF_BEAM)) VectorCopy(world[i].state.origin,world[i].state.old_origin);
    }
    int previous=0;
    for(int guard=0;guard<MAX_EDICTS+2;guard++) {
        uint64_t bits;int number=MSG_ParseEntityBits_From(&d->msg,&bits,d->esFlags);
        if(dof_overrun(d) || number<0 || (unsigned)number>=d->csr->max_edicts || number>=MAX_EDICTS) return false;
        if(!number) {
            if(!mvd) memcpy(d->scene->frames[d->framenum&UPDATE_MASK],world,bytes);
            return true;
        }
        if(number<=previous) return false;
        previous=number;
        dof_entity_state_t *e=&world[number];
        if(!mvd && (bits&U_REMOVE)) {
            if(deltaframe<=0 || !e->present) return false;
            e->present=false;continue;
        }
        if(!mvd && !e->present) *e=d->scene->baseline[number];
        vec3_t old_origin;VectorCopy(e->state.origin,old_origin);
        MSG_ParseDeltaEntity_From(&d->msg,&e->state,&e->extension,number,bits,d->esFlags);
        if(dof_overrun(d) || !dof_entity_indices_valid(d,e)) return false;
        if(!mvd && !(bits&U_OLDORIGIN) && !(e->state.renderfx&RF_BEAM)) VectorCopy(old_origin,e->state.old_origin);
        if(!d->csr->extended) {
            e->state.renderfx&=RF_SHELL_LITE_GREEN-1;
            if(e->state.renderfx&RF_BEAM) e->state.renderfx&=~RF_GLOW;
        }
        e->present=!(bits&U_REMOVE);
        if(mvd && !e->present && !(e->state.renderfx&RF_BEAM)) VectorCopy(e->state.origin,e->state.old_origin);
    }
    d->quality|=DOF_Q_LIMIT_HIT;return false;
}
static void dof_publish_world(dof_t *d,int deltaframe)
{
    if(!d->scene) return;
    const dof_entity_frame_t out={.time_ms=dof_now(d),.frame_number=d->framenum,
        .delta_frame=deltaframe,.max_entities=d->csr->max_edicts,.mvd=d->mvd,.entities=d->scene->work};
    d->sink->entity_frame(d->sink->ud,&out);
}

static void dof_publish_player(dof_t *d, int slot, const player_state_t *ps,
                               bool pov)
{
    if (!d->sink->player_state)
        return;

    /* Zeroed first: this struct grows as consumers need more of the frame,
       and a field added to the header and forgotten here would be read as
       whatever was on the stack. */
    dof_player_state_t out;
    memset(&out, 0, sizeof(out));
    out.time_ms   = dof_now(d);
    out.slot      = slot;
    out.pov       = pov;
    out.pm_type   = ps->pmove.pm_type;
    out.num_stats = d->num_stats;
    out.stats     = ps->stats;

    for (int i = 0; i < 3; i++) {
        out.origin[i]       = ps->pmove.origin[i] * 0.125f;
        out.velocity[i]     = ps->pmove.velocity[i] * 0.125f;
        out.view_angles[i]  = ps->viewangles[i];
        out.delta_angles[i] = SHORT2ANGLE(ps->pmove.delta_angles[i]);
        out.view_offset[i] = ps->viewoffset[i];
        out.kick_angles[i] = ps->kick_angles[i];
    }
    out.pm_flags = ps->pmove.pm_flags;
    out.pm_time  = ps->pmove.pm_time;
    out.gravity  = ps->pmove.gravity;
    out.gunindex = ps->gunindex;out.gunframe = ps->gunframe;
    out.fov = ps->fov;

    d->sink->player_state(d->sink->ud, &out);
}

/* ------------------------------------------------------------------ */
/* DM2                                                                 */
/* ------------------------------------------------------------------ */

static bool dof_dm2_serverdata(dof_t *d)
{
    d->protocol = MSG_ReadLong_From(&d->msg);
    MSG_ReadLong_From(&d->msg);                 /* servercount */
    MSG_ReadByte_From(&d->msg);                 /* attractloop */

    d->csr     = &cs_remap_old;
    d->esFlags = 0;
    d->psFlags = 0;

    if (EXTENDED_SUPPORTED(d->protocol)) {
        /* Same fold the live client applies for demo playback: an extended
         * demo speaks the DEFAULT frame layout with the new remap. */
        d->csr          = &cs_remap_new;
        d->eff_protocol = PROTOCOL_VERSION_DEFAULT;
        d->esFlags     |= MSG_ES_LONGSOLID | MSG_ES_UMASK | MSG_ES_BEAMORIGIN |
                          MSG_ES_SHORTANGLES | MSG_ES_EXTENSIONS;
        d->psFlags     |= MSG_PS_EXTENSIONS;
        if (d->protocol >= PROTOCOL_VERSION_EXTENDED_LIMITS_2) {
            d->esFlags |= MSG_ES_EXTENSIONS_2;
            d->psFlags |= MSG_PS_EXTENSIONS_2;
        }
        if (d->protocol >= PROTOCOL_VERSION_EXTENDED_PLAYERFOG)
            d->psFlags |= MSG_PS_MOREBITS;
    } else if (d->protocol < PROTOCOL_VERSION_OLD ||
               d->protocol > PROTOCOL_VERSION_DEFAULT) {
        /* A recorded demo is always min(live, DEFAULT) or an extended
         * variant, so anything else is a format we must not guess at. */
        d->quality |= DOF_Q_UNSUPPORTED;
        return false;
    } else {
        d->eff_protocol = d->protocol;
    }

    d->num_stats = (d->psFlags & MSG_PS_EXTENSIONS_2) ? MAX_STATS_NEW : MAX_STATS_OLD;

    dof_segment_info_t info;
    memset(&info, 0, sizeof(info));
    info.mvd      = false;
    info.protocol = d->protocol;
    info.extended = d->csr->extended;

    MSG_ReadString_From(&d->msg, info.gamedir, sizeof(info.gamedir));
    d->client_num  = MSG_ReadShort_From(&d->msg);
    MSG_ReadString_From(&d->msg, d->strbuf, sizeof(d->strbuf));  /* level name */

    if (dof_overrun(d)) {
        d->quality |= DOF_Q_TRUNCATED;
        return false;
    }

    /* `csr->maxclients` is the CS_MAXCLIENTS *index*, not a count.  The real
     * value arrives as a configstring a few bytes later and is picked up in
     * dof_emit_configstring(); until then assume the protocol maximum. */
    d->max_clients   = MAX_CLIENTS;
    info.client_num  = d->client_num;
    info.max_clients = d->max_clients;
    info.items_base  = d->csr->items;
    info.index_in_file = d->segment_index;

    /* A DM2 keeps one continuous frame counter across mid-stream serverdata
     * blocks, so the segment clock keeps running rather than restarting. */
    memset(d->dm2_valid, 0, sizeof(d->dm2_valid));
    if(d->scene) memset(d->scene,0,sizeof(*d->scene));

    d->have_segment = true;
    if (d->sink->segment_start)
        d->sink->segment_start(d->sink->ud, &info);
    d->segment_index++;
    return true;
}

static bool dof_dm2_frame(dof_t *d)
{
    int currentframe = MSG_ReadLong_From(&d->msg);
    int deltaframe = MSG_ReadLong_From(&d->msg);
    const player_state_t *base = NULL;
    player_state_t *ps;
    int slot;
    if (currentframe < 0 || currentframe > INT_MAX / BASE_FRAMETIME ||
        deltaframe > currentframe) {
        d->quality |= DOF_Q_DESYNC;
        return false;
    }
    slot = currentframe & UPDATE_MASK;
    if (deltaframe > 0) {
        int previous = deltaframe & UPDATE_MASK;
        /* Some real recordings update an already recorded frame number
         * during a pause. The matching tagged state remains a valid base;
         * an unseen same-frame reference is still rejected below. */
        if (!d->dm2_valid[previous] || d->dm2_frame[previous] != deltaframe) {
            dof_diagnostic(d, "missing_delta_base", currentframe, deltaframe);
            d->quality |= DOF_Q_DESYNC;
            return false;
        }
        base = &d->dm2_ps[previous];
    }
    ps = &d->dm2_ps[slot];

    if (d->eff_protocol != PROTOCOL_VERSION_OLD)
        MSG_ReadByte_From(&d->msg);             /* suppress count */

    int length = MSG_ReadByte_From(&d->msg);
    if (length < 0 || length > MAX_MAP_AREA_BYTES) {
        d->quality |= DOF_Q_DESYNC;
        return false;
    }
    if (length && !MSG_ReadData_From(&d->msg, length)) {
        d->quality |= DOF_Q_TRUNCATED;
        return false;
    }

    if (MSG_ReadByte_From(&d->msg) != svc_playerinfo) {
        d->quality |= DOF_Q_DESYNC;
        return false;
    }

    uint32_t bits = MSG_ReadWord_From(&d->msg);
    if ((d->psFlags & MSG_PS_MOREBITS) && (bits & PS_MOREBITS))
        bits |= (uint32_t)MSG_ReadByte_From(&d->msg) << 16;

    /* A demo's playerstate always uses the Default encoding: recording folds
     * R1Q2/Q2PRO down to protocol 34 (or an extended variant of it). */
    MSG_ParseDeltaPlayerstate_Default_From(&d->msg,
                                           base, ps, bits, d->psFlags);
    if (dof_overrun(d)) {
        d->quality |= DOF_Q_TRUNCATED;
        return false;
    }
    d->dm2_valid[slot] = true;
    d->dm2_frame[slot] = currentframe;

    if (currentframe > 0)
        d->seg_time_ms = (currentframe - 1) * BASE_FRAMETIME;
    d->framenum = currentframe;

    if (MSG_ReadByte_From(&d->msg) != svc_packetentities) {
        d->quality |= DOF_Q_DESYNC;
        return false;
    }
    if (!dof_packet_world(d, false, deltaframe)) {
        dof_diagnostic(d, "invalid_entities", currentframe, deltaframe);
        d->quality |= DOF_Q_DESYNC;
        return false;
    }

    /* Publish only after the complete frame has been validated. */
    dof_publish_world(d,deltaframe);
    if (d->client_num >= 0)
        dof_publish_player(d, d->client_num, ps, true);
    if (d->sink->frame)
        d->sink->frame(d->sink->ud, dof_now(d));
    return true;
}

/* Consume one svc_temp_entity payload.  Byte-for-byte mirror of
 * CL_ParseTEntPacket; positions go through the shared reader so the
 * extended coordinate encoding stays correct. */
static bool dof_dm2_temp_entity(dof_t *d)
{
    vec3_t scratch;
    bool   ext2 = (d->esFlags & MSG_ES_EXTENSIONS_2) != 0;
    int    type = MSG_ReadByte_From(&d->msg);
    int    ent1;
    dof_effect_t cue={.time_ms=dof_now(d),.type=type,.mvd=d->mvd,
        .direction=-1,.delivery=d->delivery};

    switch (type) {
    case TE_BLOOD: case TE_GUNSHOT: case TE_SPARKS: case TE_BULLET_SPARKS:
    case TE_SCREEN_SPARKS: case TE_SHIELD_SPARKS: case TE_SHOTGUN:
    case TE_BLASTER: case TE_GREENBLOOD: case TE_BLASTER2: case TE_FLECHETTE:
    case TE_HEATBEAM_SPARKS: case TE_HEATBEAM_STEAM: case TE_MOREBLOOD:
    case TE_ELECTRIC_SPARKS: case TE_BLUEHYPERBLASTER_2: case TE_BERSERK_SLAM:
        MSG_ReadPos_From(&d->msg, cue.position, ext2);cue.has_position=1;
        cue.direction=MSG_ReadByte_From(&d->msg);
        break;

    case TE_SPLASH: case TE_LASER_SPARKS: case TE_WELDING_SPARKS:
    case TE_TUNNEL_SPARKS:
        MSG_ReadByte_From(&d->msg);             /* count */
        MSG_ReadPos_From(&d->msg, scratch, ext2);
        MSG_ReadByte_From(&d->msg);             /* dir index */
        MSG_ReadByte_From(&d->msg);             /* color */
        break;

    case TE_BLUEHYPERBLASTER: case TE_RAILTRAIL: case TE_RAILTRAIL2:
    case TE_BUBBLETRAIL: case TE_DEBUGTRAIL: case TE_BUBBLETRAIL2:
    case TE_BFG_LASER: case TE_BFG_ZAP:
        MSG_ReadPos_From(&d->msg, cue.position, ext2);cue.has_position=1;
        MSG_ReadPos_From(&d->msg, cue.end, ext2);cue.has_end=1;
        break;

    case TE_GRENADE_EXPLOSION: case TE_GRENADE_EXPLOSION_WATER:
    case TE_EXPLOSION2: case TE_PLASMA_EXPLOSION: case TE_ROCKET_EXPLOSION:
    case TE_ROCKET_EXPLOSION_WATER: case TE_EXPLOSION1: case TE_EXPLOSION1_NP:
    case TE_EXPLOSION1_BIG: case TE_BFG_EXPLOSION: case TE_BFG_BIGEXPLOSION:
    case TE_BOSSTPORT: case TE_PLAIN_EXPLOSION: case TE_CHAINFIST_SMOKE:
    case TE_TRACKER_EXPLOSION: case TE_TELEPORT_EFFECT: case TE_DBALL_GOAL:
    case TE_WIDOWSPLASH: case TE_NUKEBLAST: case TE_EXPLOSION1_NL:
    case TE_EXPLOSION2_NL:
        MSG_ReadPos_From(&d->msg, cue.position, ext2);cue.has_position=1;
        break;

    case TE_PARASITE_ATTACK: case TE_MEDIC_CABLE_ATTACK: case TE_HEATBEAM:
    case TE_MONSTER_HEATBEAM: case TE_GRAPPLE_CABLE_2: case TE_LIGHTNING_BEAM:
        MSG_ReadShort_From(&d->msg);
        MSG_ReadPos_From(&d->msg, scratch, ext2);
        MSG_ReadPos_From(&d->msg, scratch, ext2);
        break;

    case TE_GRAPPLE_CABLE:
        MSG_ReadShort_From(&d->msg);
        MSG_ReadPos_From(&d->msg, scratch, ext2);
        MSG_ReadPos_From(&d->msg, scratch, ext2);
        MSG_ReadPos_From(&d->msg, scratch, ext2);
        break;

    case TE_LIGHTNING:
        MSG_ReadShort_From(&d->msg);
        MSG_ReadShort_From(&d->msg);
        MSG_ReadPos_From(&d->msg, scratch, ext2);
        MSG_ReadPos_From(&d->msg, scratch, ext2);
        break;

    case TE_FLASHLIGHT:
        MSG_ReadPos_From(&d->msg, scratch, ext2);
        MSG_ReadShort_From(&d->msg);
        break;

    case TE_FORCEWALL:
        MSG_ReadPos_From(&d->msg, scratch, ext2);
        MSG_ReadPos_From(&d->msg, scratch, ext2);
        MSG_ReadByte_From(&d->msg);
        break;

    case TE_STEAM:
        ent1 = MSG_ReadShort_From(&d->msg);
        MSG_ReadByte_From(&d->msg);             /* count */
        MSG_ReadPos_From(&d->msg, scratch, ext2);
        MSG_ReadByte_From(&d->msg);             /* dir index */
        MSG_ReadByte_From(&d->msg);             /* color */
        MSG_ReadShort_From(&d->msg);            /* entity2 */
        if (ent1 != -1)
            MSG_ReadLong_From(&d->msg);         /* time */
        break;

    case TE_WIDOWBEAMOUT:
        MSG_ReadShort_From(&d->msg);
        MSG_ReadPos_From(&d->msg, scratch, ext2);
        break;

    case TE_POWER_SPLASH:
        MSG_ReadShort_From(&d->msg);
        MSG_ReadByte_From(&d->msg);
        break;

    case TE_DAMAGE_DEALT:
        MSG_ReadShort_From(&d->msg);
        break;

    default:
        dof_diagnostic(d, "unsupported_temp_entity", d->framenum, type);
        d->quality |= DOF_Q_DESYNC;
        return false;
    }

    if(dof_overrun(d)) return false;
    if(d->sink->effect)d->sink->effect(d->sink->ud,&cue);
    return true;
}

static bool dof_dm2_sound(dof_t *d)
{
    int flags = MSG_ReadByte_From(&d->msg);
    dof_sound_t cue = { .time_ms=dof_now(d), .flags=flags, .mvd=d->mvd,
        .entity=-1, .channel=-1, .volume=255, .attenuation=64, .delivery=d->delivery };

    if (d->csr->extended && (flags & SND_INDEX16))
        cue.index=MSG_ReadWord_From(&d->msg);
    else
        cue.index=MSG_ReadByte_From(&d->msg);

    if (flags & SND_VOLUME)
        cue.volume=MSG_ReadByte_From(&d->msg);
    if (flags & SND_ATTENUATION)
        cue.attenuation=MSG_ReadByte_From(&d->msg);
    if (flags & SND_OFFSET)
        cue.offset_ms=MSG_ReadByte_From(&d->msg);
    if (flags & SND_ENT) {
        int sendchan=MSG_ReadWord_From(&d->msg);
        cue.entity=sendchan>>3;cue.channel=sendchan&7;
    }
    if (flags & SND_POS) {
        cue.has_position=true;
        MSG_ReadPos_From(&d->msg, cue.position, (d->esFlags & MSG_ES_EXTENSIONS_2) != 0);
    }

    if(dof_overrun(d))return false;
    if(d->sink->sound)d->sink->sound(d->sink->ud,&cue);
    return true;
}

static bool dof_muzzleflash(dof_t *d,int cmd)
{
    dof_muzzleflash_t flash={.time_ms=dof_now(d),.mvd=d->mvd,
        .monster=cmd==svc_muzzleflash2,.delivery=d->delivery};
    flash.entity=MSG_ReadWord_From(&d->msg);flash.weapon=MSG_ReadByte_From(&d->msg);
    if(dof_overrun(d))return false;
    if(d->sink->muzzleflash)d->sink->muzzleflash(d->sink->ud,&flash);
    return true;
}
static int dof_cue_command(dof_t *d,int cmd)
{
    switch(cmd) {
    case svc_nop:return 1;
    case svc_sound:return dof_dm2_sound(d) ? 1 : -1;
    case svc_temp_entity:return dof_dm2_temp_entity(d) ? 1 : -1;
    case svc_muzzleflash:case svc_muzzleflash2:return dof_muzzleflash(d,cmd) ? 1 : -1;
    default:return 0;
    }
}

static bool dof_dm2_message(dof_t *d)
{
    while (1) {
        if (dof_overrun(d))
            return false;
        if (d->msg.readcount == d->msg.cursize)
            return true;                        /* clean end of message */

        uint32_t before = d->msg.readcount;
        int cmd = MSG_ReadByte_From(&d->msg);

        /* Demos never carry the R1Q2/Q2PRO extra-bits packing. */
        if (cmd < 0 || (cmd & ~SVCMD_MASK)) {
            dof_diagnostic(d, "invalid_command", d->framenum, cmd);
            d->quality |= DOF_Q_DESYNC;
            return false;
        }

        switch (cmd) {
        case svc_nop:
            break;

        case svc_disconnect:
        case svc_reconnect:
            /* Both terminate ordinary client demo playback. A trailing
             * packet is not a continuation of this connection and must not
             * supply future observations or inherit its delta state. */
            if (d->msg.readcount != d->msg.cursize) {
                d->quality |= DOF_Q_DESYNC;
                return false;
            }
            d->dm2_disconnected = true;
            return true;

        case svc_serverdata:
            if (!dof_dm2_serverdata(d))
                return false;
            break;

        case svc_configstring: {
            int index = MSG_ReadWord_From(&d->msg);
            if (index < 0 || index >= (int)d->csr->end) {
                d->quality |= DOF_Q_DESYNC;
                return false;
            }
            dof_emit_configstring(d, index);
            break;
        }

        case svc_print: {
            int level = MSG_ReadByte_From(&d->msg);
            MSG_ReadString_From(&d->msg, d->strbuf, sizeof(d->strbuf));
            if (dof_overrun(d))
                return false;
            if (d->sink->print)
                d->sink->print(d->sink->ud, dof_now(d), level, d->strbuf);
            break;
        }

        case svc_centerprint:
        case svc_stufftext:
            MSG_ReadString_From(&d->msg, d->strbuf, sizeof(d->strbuf));
            break;

        case svc_layout: {
            size_t n = MSG_ReadString_From(&d->msg, d->layoutbuf,
                                           sizeof(d->layoutbuf));
            if (dof_overrun(d))
                return false;
            if (d->sink->layout)
                d->sink->layout(d->sink->ud, dof_now(d), d->layoutbuf, n,
                                n >= sizeof(d->layoutbuf));
            break;
        }

        case svc_inventory:
            for (int i = 0; i < MAX_ITEMS; i++)
                MSG_ReadShort_From(&d->msg);
            break;

        case svc_sound:
            if (!dof_dm2_sound(d))
                return false;
            break;

        case svc_temp_entity:
            if (!dof_dm2_temp_entity(d))
                return false;
            break;

        case svc_muzzleflash:
        case svc_muzzleflash2:
            if(!dof_muzzleflash(d,cmd))return false;
            break;

        case svc_spawnbaseline:
            if (!dof_skip_delta_entity(d, false)) {
                d->quality |= DOF_Q_DESYNC;
                return false;
            }
            break;

        case svc_frame:
            if (!dof_dm2_frame(d))
                return false;
            break;

        default:
            /* Everything else (download, zpacket, gamestate, setting) is
             * never copied into a recorded demo.  Seeing one means we lost
             * the byte stream; say so instead of guessing. */
            dof_diagnostic(d, "unsupported_command", d->framenum, cmd);
            d->quality |= DOF_Q_DESYNC;
            return false;
        }

        if (d->msg.readcount == before) {
            d->quality |= DOF_Q_DESYNC;
            return false;
        }
    }
}

/* ------------------------------------------------------------------ */
/* MVD2                                                                */
/* ------------------------------------------------------------------ */

static bool dof_mvd_serverdata(dof_t *d, int extrabits)
{
    d->protocol = MSG_ReadLong_From(&d->msg);
    if (d->protocol != PROTOCOL_VERSION_MVD) {
        d->quality |= DOF_Q_UNSUPPORTED;
        return false;
    }

    d->mvd_version = MSG_ReadWord_From(&d->msg);
    if (!MVD_SUPPORTED(d->mvd_version)) {
        d->quality |= DOF_Q_UNSUPPORTED;
        return false;
    }

    if (d->mvd_version >= PROTOCOL_VERSION_MVD_EXTENDED_LIMITS_2)
        d->mvd_flags = MSG_ReadWord_From(&d->msg);
    else
        d->mvd_flags = extrabits;

    MSG_ReadLong_From(&d->msg);                 /* servercount */

    dof_segment_info_t info;
    memset(&info, 0, sizeof(info));
    info.mvd      = true;
    info.protocol = d->protocol;

    MSG_ReadString_From(&d->msg, info.gamedir, sizeof(info.gamedir));
    d->client_num = MSG_ReadShort_From(&d->msg);

    d->esFlags = MSG_ES_UMASK | MSG_ES_BEAMORIGIN;
    d->psFlags = 0;
    d->csr     = &cs_remap_old;

    if (d->mvd_version >= PROTOCOL_VERSION_MVD_EXTENDED_LIMITS &&
        (d->mvd_flags & MVF_EXTLIMITS)) {
        d->esFlags |= MSG_ES_LONGSOLID | MSG_ES_SHORTANGLES | MSG_ES_EXTENSIONS;
        d->psFlags |= MSG_PS_EXTENSIONS;
        d->csr      = &cs_remap_new;
    }
    if (d->mvd_version >= PROTOCOL_VERSION_MVD_EXTENDED_LIMITS_2 &&
        (d->mvd_flags & MVF_EXTLIMITS_2)) {
        if (!(d->mvd_flags & MVF_EXTLIMITS)) {
            d->quality |= DOF_Q_UNSUPPORTED;
            return false;
        }
        d->esFlags |= MSG_ES_EXTENSIONS_2;
        d->psFlags |= MSG_PS_EXTENSIONS_2;
        if (d->mvd_version >= PROTOCOL_VERSION_MVD_PLAYERFOG)
            d->psFlags |= MSG_PS_MOREBITS;
    }

    d->num_stats = (d->psFlags & MSG_PS_EXTENSIONS_2) ? MAX_STATS_NEW : MAX_STATS_OLD;

    /* An MVD restarts its frame counter at every serverdata, so fold the
     * segment that just ended into the base and start the new one at zero. */
    d->time_base_ms += d->seg_time_ms;
    d->seg_time_ms   = 0;
    d->framenum      = 0;
    memset(d->mvd_inuse, 0, sizeof(d->mvd_inuse));
    memset(d->mvd_ps, 0, sizeof(d->mvd_ps));
    if(d->scene) memset(d->scene,0,sizeof(*d->scene));

    info.extended    = d->csr->extended;
    info.client_num  = d->client_num;
    info.items_base  = d->csr->items;
    info.index_in_file = d->segment_index;

    /* Announce the segment before its inline configstring run.  The previous
     * order called configstring first even though its own comment promised
     * the opposite; the fact builder correctly ignored those ownerless
     * events, losing every initial player and item name in an MVD2.  The exact
     * maxclients value is still inside the run, so begin conservatively at the
     * protocol cap; unused slots remain empty. */
    info.max_clients = MAX_CLIENTS;
    d->have_segment = true;
    if (d->sink->segment_start)
        d->sink->segment_start(d->sink->ud, &info);

    /* Configstring run terminated by the end-of-list index. */
    int maxclients = 0;
    while (1) {
        int index = MSG_ReadWord_From(&d->msg);
        if (index == (int)d->csr->end)
            break;
        if (index < 0 || index >= (int)d->csr->end || dof_overrun(d)) {
            d->quality |= DOF_Q_DESYNC;
            return false;
        }
        MSG_ReadString_From(&d->msg, d->strbuf, sizeof(d->strbuf));
        if (dof_overrun(d)) {
            d->quality |= DOF_Q_TRUNCATED;
            return false;
        }
        if (index == (int)d->csr->maxclients)
            maxclients = atoi(d->strbuf);
        if (d->sink->configstring)
            d->sink->configstring(d->sink->ud, dof_now(d), index, d->strbuf);
    }

    if (maxclients <= 0 || maxclients > MAX_CLIENTS)
        maxclients = MAX_CLIENTS;
    d->max_clients   = maxclients;
    info.max_clients = maxclients;

    d->segment_index++;
    return !dof_overrun(d);
}

static bool dof_mvd_packet_players(dof_t *d)
{
    for (int guard = 0; guard <= MAX_CLIENTS + 1; guard++) {
        if (dof_overrun(d))
            return false;

        int number = MSG_ReadByte_From(&d->msg);
        if (number == CLIENTNUM_NONE)
            return true;
        if (number < 0 || number >= d->max_clients)
            return false;

        int bits = MSG_ReadWord_From(&d->msg);
        if (bits & PPS_MOREBITS) {
            if (d->psFlags & MSG_PS_MOREBITS)
                bits |= (uint32_t)MSG_ReadByte_From(&d->msg) << 16;
            else
                bits |= PPS_REMOVE;
        }

        MSG_ParseDeltaPlayerstate_Packet_From(&d->msg, &d->mvd_ps[number],
                                              bits, d->psFlags);
        if (dof_overrun(d))
            return false;

        if (bits & PPS_REMOVE) {
            d->mvd_inuse[number] = false;
            continue;
        }
        d->mvd_inuse[number] = true;
    }
    d->quality |= DOF_Q_LIMIT_HIT;
    return false;
}

static bool dof_mvd_frame(dof_t *d)
{
    int length = MSG_ReadByte_From(&d->msg);
    if (length < 0 || !MSG_ReadData_From(&d->msg, length)) {
        d->quality |= DOF_Q_TRUNCATED;
        return false;
    }

    if (!dof_mvd_packet_players(d)) {
        d->quality |= DOF_Q_DESYNC;
        return false;
    }
    if (!dof_packet_world(d, true, -1)) {
        d->quality |= DOF_Q_DESYNC;
        return false;
    }

    d->seg_time_ms = d->framenum * BASE_FRAMETIME;
    dof_publish_world(d,-1);

    for (int i = 0; i < d->max_clients; i++)
        if (d->mvd_inuse[i])
            dof_publish_player(d, i, &d->mvd_ps[i], i == d->client_num);

    if (d->sink->frame)
        d->sink->frame(d->sink->ud, dof_now(d));

    d->framenum++;
    return true;
}

static bool dof_mvd_unicast(dof_t *d, int extrabits)
{
    uint32_t length = MSG_ReadByte_From(&d->msg);
    length |= (uint32_t)extrabits << 8;
    int target_slot=MSG_ReadByte_From(&d->msg);

    if(dof_overrun(d) || length>d->msg.cursize-d->msg.readcount) {
        d->quality|=DOF_Q_TRUNCATED;return false;
    }
    uint32_t last = d->msg.readcount + length;
    if (last > d->msg.cursize) {
        d->quality |= DOF_Q_TRUNCATED;
        return false;
    }

    while (d->msg.readcount < last) {
        int cmd = MSG_ReadByte_From(&d->msg);
        switch (cmd) {
        case svc_layout: {
            size_t n = MSG_ReadString_From(&d->msg, d->layoutbuf,
                                           sizeof(d->layoutbuf));
            if (d->sink->layout && !dof_overrun(d))
                d->sink->layout(d->sink->ud, dof_now(d), d->layoutbuf, n,
                                n >= sizeof(d->layoutbuf));
            break;
        }
        case svc_configstring: {
            int index = MSG_ReadWord_From(&d->msg);
            if (index < 0 || index >= (int)d->csr->end) {
                d->msg.readcount = last;
                return true;
            }
            dof_emit_configstring(d, index);
            break;
        }
        case svc_print: {
            int level = MSG_ReadByte_From(&d->msg);
            MSG_ReadString_From(&d->msg, d->strbuf, sizeof(d->strbuf));
            if (d->sink->print && !dof_overrun(d))
                d->sink->print(d->sink->ud, dof_now(d), level, d->strbuf);
            break;
        }
        case svc_sound:case svc_temp_entity:case svc_muzzleflash:case svc_muzzleflash2: {
            if(!d->sink->sound && !d->sink->muzzleflash && !d->sink->effect) {
                d->msg.readcount=last;return true; /* retain existing fact-only behavior */
            }
            uint32_t outer_size=d->msg.cursize;dof_delivery_t previous=d->delivery;
            d->msg.cursize=last;d->delivery=(dof_delivery_t){-2,-1,target_slot};
            int valid=dof_cue_command(d,cmd);
            d->msg.cursize=outer_size;d->delivery=previous;
            if(valid<0)return false;
            break;
        }
        case svc_stufftext:
            MSG_ReadString_From(&d->msg, d->strbuf, sizeof(d->strbuf));
            break;
        default:
            /* The live parser forwards the remainder verbatim; we simply
             * jump to the end of the unicast block, exactly as it does. */
            d->msg.readcount = last;
            return true;
        }
        if (dof_overrun(d))
            return false;
    }

    return d->msg.readcount == last;
}

static bool dof_mvd_multicast(dof_t *d, int to, int extrabits)
{
    uint32_t length=MSG_ReadByte_From(&d->msg);length|=(uint32_t)extrabits<<8;
    int leaf=-1;
    if(to%MULTICAST_ALL_R)leaf=MSG_ReadWord_From(&d->msg);
    if(dof_overrun(d) || length>d->msg.cursize-d->msg.readcount) {
        d->quality|=DOF_Q_TRUNCATED;return false;
    }
    const byte *payload=MSG_ReadData_From(&d->msg,length);
    if(!payload || dof_overrun(d)) { d->quality|=DOF_Q_TRUNCATED;return false; }
    if(!d->sink->sound && !d->sink->muzzleflash && !d->sink->effect) return true;
    sizebuf_t outer=d->msg;dof_delivery_t previous=d->delivery;bool valid=true;
    SZ_InitRead(&d->msg,payload,length);d->delivery=(dof_delivery_t){to,leaf,-1};
    while(d->msg.readcount<d->msg.cursize) {
        int r=dof_cue_command(d,MSG_ReadByte_From(&d->msg));
        if(r<0) { valid=false;break; }
        if(!r)break; /* Other mod-specific multicast payloads are opaque. */
    }
    d->msg=outer;d->delivery=previous;return valid;
}

static bool dof_mvd_sound(dof_t *d, int extrabits)
{
    int flags = MSG_ReadByte_From(&d->msg);
    dof_sound_t cue={ .time_ms=dof_now(d), .flags=flags, .mvd=true,
        .volume=255, .attenuation=64,
        .delivery={ (extrabits&1 ? MULTICAST_ALL : MULTICAST_PHS)+(extrabits&2 ? MULTICAST_ALL_R : 0),-1,-1 } };

    if (d->csr->extended && (flags & SND_INDEX16))
        cue.index=MSG_ReadWord_From(&d->msg);
    else
        cue.index=MSG_ReadByte_From(&d->msg);

    if (flags & SND_VOLUME)
        cue.volume=MSG_ReadByte_From(&d->msg);
    if (flags & SND_ATTENUATION)
        cue.attenuation=MSG_ReadByte_From(&d->msg);
    if (flags & SND_OFFSET)
        cue.offset_ms=MSG_ReadByte_From(&d->msg);

    int sendchan=MSG_ReadWord_From(&d->msg);
    cue.entity=sendchan>>3;cue.channel=sendchan&7;
    if(dof_overrun(d))return false;
    if(d->sink->sound)d->sink->sound(d->sink->ud,&cue);
    return true;
}

static bool dof_mvd_message(dof_t *d)
{
    while (1) {
        if (dof_overrun(d))
            return false;
        if (d->msg.readcount == d->msg.cursize)
            return true;

        uint32_t before = d->msg.readcount;
        int raw = MSG_ReadByte_From(&d->msg);
        if (raw < 0)
            return false;

        int extrabits = raw >> SVCMD_BITS;
        int cmd       = raw & SVCMD_MASK;

        switch (cmd) {
        case mvd_serverdata:
            /* Serverdata ends with an inline baseline frame, without an
             * mvd_frame command byte (see MVD_ParseServerData). */
            if (!dof_mvd_serverdata(d, extrabits) || !dof_mvd_frame(d))
                return false;
            break;

        case mvd_multicast_all:
        case mvd_multicast_pvs:
        case mvd_multicast_phs:
        case mvd_multicast_all_r:
        case mvd_multicast_pvs_r:
        case mvd_multicast_phs_r:
            if (!dof_mvd_multicast(d, cmd - mvd_multicast_all, extrabits))
                return false;
            break;

        case mvd_unicast:
        case mvd_unicast_r:
            if (!dof_mvd_unicast(d, extrabits))
                return false;
            break;

        case mvd_configstring: {
            int index = MSG_ReadWord_From(&d->msg);
            if (index < 0 || index >= (int)d->csr->end) {
                d->quality |= DOF_Q_DESYNC;
                return false;
            }
            dof_emit_configstring(d, index);
            break;
        }

        case mvd_frame:
            if (!dof_mvd_frame(d))
                return false;
            break;

        case mvd_sound:
            if (!dof_mvd_sound(d, extrabits))
                return false;
            break;

        case mvd_print: {
            int level = MSG_ReadByte_From(&d->msg);
            MSG_ReadString_From(&d->msg, d->strbuf, sizeof(d->strbuf));
            if (dof_overrun(d))
                return false;
            if (d->sink->print)
                d->sink->print(d->sink->ud, dof_now(d), level, d->strbuf);
            break;
        }

        case mvd_nop:
            break;

        default:
            d->quality |= DOF_Q_DESYNC;
            return false;
        }

        if (d->msg.readcount == before) {
            d->quality |= DOF_Q_DESYNC;
            return false;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

int DOF_Decode(const dof_reader_t *reader, const dof_sink_t *sink,
               unsigned *out_quality)
{
    if (out_quality)
        *out_quality = 0;
    if (!reader || !reader->read || !sink)
        return DOF_ERR_OPEN;

    dof_t *d = calloc(1, DOF_ScratchSizeForSink(sink));
    if (!d)
        return DOF_ERR_NOMEM;

    d->rd   = reader;
    d->sink = sink;
    d->delivery=(dof_delivery_t){-1,-1,-1};
    if(sink->entity_frame) d->scene=(dof_scene_t *)(d+1);
    d->csr  = &cs_remap_old;
    d->num_stats = MAX_STATS_OLD;
    d->client_num = -1;
    d->max_clients = MAX_CLIENTS;
    d->eff_protocol = PROTOCOL_VERSION_DEFAULT;

    int ret = DOF_OK;

    /* The first four bytes are either the MVD magic or a DM2 message length,
     * exactly as read_first_message() classifies them. */
    uint32_t head;
    if (!dof_read_exact(d, &head, 4)) {
        ret = DOF_ERR_FORMAT;
        goto done;
    }

    if (head == MVD_MAGIC) {
        d->mvd = true;
        while (1) {
            if (sink->cancelled && sink->cancelled(sink->ud)) {
                d->quality |= DOF_Q_CANCELLED;
                ret = DOF_ERR_CANCELLED;
                goto done;
            }
            int r = dof_next_mvd_message(d);
            if (r == 0)
                break;
            if (r < 0) {
                ret = DOF_ERR_TRUNCATED;
                goto done;
            }
            if (!dof_mvd_message(d)) {
                if (!d->quality)
                    d->quality |= DOF_Q_DESYNC;
                ret = DOF_ERR_FORMAT;
                goto done;
            }
        }
    } else {
        uint32_t msglen = LittleLong(head);
        if (msglen == (uint32_t)-1 || msglen == 0 || msglen > sizeof(d->buf)) {
            ret = DOF_ERR_FORMAT;
            goto done;
        }
        if (!dof_read_exact(d, d->buf, msglen)) {
            d->quality |= DOF_Q_TRUNCATED;
            ret = DOF_ERR_TRUNCATED;
            goto done;
        }
        SZ_InitRead(&d->msg, d->buf, msglen);

        while (1) {
            if (sink->cancelled && sink->cancelled(sink->ud)) {
                d->quality |= DOF_Q_CANCELLED;
                ret = DOF_ERR_CANCELLED;
                goto done;
            }
            if (!dof_dm2_message(d)) {
                if (!d->quality)
                    d->quality |= DOF_Q_DESYNC;
                ret = DOF_ERR_FORMAT;
                goto done;
            }
            if (d->dm2_disconnected)
                break;
            int r = dof_next_dm2_message(d);
            if (r == 0)
                break;
            if (r < 0) {
                ret = DOF_ERR_TRUNCATED;
                goto done;
            }
        }
    }

    if (!d->have_segment && ret == DOF_OK)
        ret = DOF_ERR_FORMAT;

done:
    if (out_quality)
        *out_quality = d->quality;
    free(d);
    return ret;
}
