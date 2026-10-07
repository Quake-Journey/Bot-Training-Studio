/* Standalone evidence exporter. Wire decoding and match facts are Q2PRO-X's;
 * movement summaries are MAPGEN-1's. No renderer, server or second wire parser. */
#include "client/demo_match_facts.h"
#include "common/mapgen_demo.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdarg.h>
#include <time.h>

typedef struct {
    const dof_sink_t *facts;
    int segment;
    unsigned long long sequence, states, frames;
    clock_t began;
} export_t;

/* JSON strings preserve original eight-bit game text as Unicode codepoints. */
static void string(const char *s)
{
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') printf("\\%c", *p);
        else if (*p < 32 || *p >= 127) printf("\\u%04x", *p);
        else putchar(*p);
    }
    putchar('"');
}
static void begin(export_t *e, const char *kind, int time_ms)
{
    printf("{\"kind\":\"%s\",\"seq\":%llu,\"segment\":%d,\"time_ms\":%d",
           kind, e->sequence++, e->segment, time_ms);
}
static void vec(const char *key, const float *v)
{
    printf(",\"%s\":[", key);
    for (int i = 0; i < 3; i++) {
        if (i) putchar(',');
        if (isfinite(v[i])) printf("%.9g", (double)v[i]); else printf("null");
    }
    putchar(']');
}
#define END() puts("}")
#define INT(key, value) printf(",\"" key "\":%d", (int)(value))
#define STR(key, value) do { printf(",\"" key "\":"); string(value); } while (0)
#define FORWARD(e, name, ...) do { if ((e)->facts->name) \
    (e)->facts->name((e)->facts->ud, __VA_ARGS__); } while (0)

static void segment(void *ud, const dof_segment_info_t *s)
{
    export_t *e = ud; e->segment = s->index_in_file;
    FORWARD(e, segment_start, s);
    begin(e, "segment_start", 0);
    INT("mvd", s->mvd); INT("protocol", s->protocol);
    INT("recorder_slot", s->client_num); INT("max_clients", s->max_clients);
    INT("items_base", s->items_base); INT("extended", s->extended);
    STR("gamedir", s->gamedir); END();
}
static void config(void *ud, int t, int i, const char *text)
{
    export_t *e = ud; FORWARD(e, configstring, t, i, text);
    begin(e, "configstring", t); INT("index", i); STR("text", text); END();
}
static void state(void *ud, const dof_player_state_t *s)
{
    export_t *e = ud; FORWARD(e, player_state, s); e->states++;
    begin(e, "player_state", s->time_ms);
    INT("slot", s->slot); INT("pov", s->pov); INT("pm_type", s->pm_type);
    INT("pm_flags", s->pm_flags); INT("pm_time", s->pm_time);
    INT("gravity", s->gravity);INT("gunindex",s->gunindex);INT("gunframe",s->gunframe);
    vec("origin", s->origin); vec("velocity", s->velocity);
    vec("view_angles", s->view_angles); vec("delta_angles", s->delta_angles);
    vec("view_offset",s->view_offset);vec("kick_angles",s->kick_angles);
    printf(",\"fov\":%.9g",(double)s->fov);
    printf(",\"stats\":[");
    for (int i = 0; i < s->num_stats; i++) printf("%s%d", i ? "," : "", s->stats[i]);
    printf("]"); END();
}
static void print_event(void *ud, int t, int level, const char *text)
{
    export_t *e = ud; FORWARD(e, print, t, level, text);
    begin(e, "print", t); INT("level", level); STR("text", text); END();
}
static void layout(void *ud, int t, const char *text, size_t len, bool truncated)
{
    export_t *e = ud; FORWARD(e, layout, t, text, len, truncated);
    begin(e, "layout", t); INT("length", len); INT("truncated", truncated);
    STR("text", text); END();
}
static void frame(void *ud, int t)
{
    export_t *e = ud; FORWARD(e, frame, t); e->frames++;
    begin(e, "frame", t); END();
}
static void sound(void *ud,const dof_sound_t *s)
{
    export_t *e=ud;begin(e,"sound",s->time_ms);
    INT("mvd",s->mvd);INT("flags",s->flags);INT("index",s->index);
    INT("entity",s->entity);INT("channel",s->channel);INT("volume",s->volume);
    INT("attenuation",s->attenuation);INT("offset_ms",s->offset_ms);
    INT("scope",s->delivery.scope);INT("leaf",s->delivery.leaf);INT("target_slot",s->delivery.target_slot);
    INT("has_position",s->has_position);if(s->has_position)vec("position",s->position);
    END();
}
static void muzzleflash(void *ud,const dof_muzzleflash_t *f)
{
    export_t *e=ud;begin(e,"muzzleflash",f->time_ms);
    INT("mvd",f->mvd);INT("entity",f->entity);INT("weapon",f->weapon);INT("monster",f->monster);
    INT("scope",f->delivery.scope);INT("leaf",f->delivery.leaf);INT("target_slot",f->delivery.target_slot);END();
}
static void effect(void *ud,const dof_effect_t *f)
{
    export_t *e=ud;begin(e,"effect",f->time_ms);INT("type",f->type);INT("mvd",f->mvd);
    INT("has_position",f->has_position);INT("has_end",f->has_end);INT("direction",f->direction);
    INT("scope",f->delivery.scope);INT("leaf",f->delivery.leaf);INT("target_slot",f->delivery.target_slot);
    if(f->has_position)vec("position",f->position);if(f->has_end)vec("end",f->end);END();
}
static void entity_frame(void *ud,const dof_entity_frame_t *f)
{
    export_t *e=ud;begin(e,"entity_frame",f->time_ms);
    INT("frame_number",f->frame_number);INT("delta_frame",f->delta_frame);INT("mvd",f->mvd);
    printf(",\"entities\":[");int count=0;
    for(int i=1;i<f->max_entities;i++) {
        const dof_entity_state_t *v=&f->entities[i];const entity_state_t *s=&v->state;
        if(!v->present) continue;
        if(count++)putchar(',');
        printf("{\"number\":%d",s->number);
        vec("origin",s->origin);vec("old_origin",s->old_origin);vec("angles",s->angles);
        printf(",\"models\":[%d,%d,%d,%d]",s->modelindex,s->modelindex2,s->modelindex3,s->modelindex4);
        INT("frame",s->frame);INT("skin",s->skinnum);INT("sound",s->sound);INT("event",s->event);INT("solid",s->solid);
        printf(",\"effects\":%u,\"renderfx\":%u,\"morefx\":%u",(unsigned)s->effects,(unsigned)s->renderfx,(unsigned)v->extension.morefx);
        printf(",\"loop_volume\":%.9g,\"loop_attenuation\":%.9g}",(double)v->extension.loop_volume,(double)v->extension.loop_attenuation);
    }
    putchar(']');END();
}
static bool cancelled(void *ud)
{
    export_t *e = ud;
    return ferror(stdout) || e->states > 5000000 ||
           (clock() - e->began) / CLOCKS_PER_SEC > 180;
}
static int read_file(void *ud, void *buf, int len)
{
    size_t n = fread(buf, 1, len, ud);
    return ferror((FILE *)ud) ? -1 : (int)n;
}

/* Shared primitives require a host. Diagnostics never corrupt JSON stdout. */
void Com_LPrintf(print_type_t type, const char *fmt, ...)
{
    (void)type; va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
}
void Com_Error(error_type_t code, const char *fmt, ...)
{
    (void)code; va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap);
    va_end(ap); exit(3);
}

static void facts(export_t *e, dmf_builder_t *b)
{
    for (int i = 0; i < DMF_NumSegments(b); i++) {
        const dli_segment_t *s = &DMF_Segments(b)[i]; e->segment = i;
        begin(e, "segment_facts", s->start_ms);
        STR("map", s->map); STR("mod", s->mod); INT("end_ms", s->end_ms);
        INT("mode", s->mode); INT("mode_conf", s->mode_conf);
        INT("adapter", s->adapter); INT("quality", s->quality);
        printf(",\"fingerprint\":\"%016llx\"", (unsigned long long)s->fingerprint); END();
    }
    for (int i = 0; i < DMF_NumEpochs(b); i++) {
        const dli_epoch_t *s = &DMF_Epochs(b)[i]; e->segment = s->segment_index;
        begin(e, "epoch", s->start_ms); INT("epoch_id", i);
        INT("end_ms", s->end_ms); INT("start_conf", s->start_conf);
        INT("end_conf", s->end_conf); INT("closed", s->closed);
        INT("quality", s->quality); INT("final_scoreboard", s->have_final_scoreboard);
        INT("result_reason", s->result_reason);
        printf(",\"fingerprint\":\"%016llx\"", (unsigned long long)s->fingerprint); END();
    }
    for (int i = 0; i < DMF_NumTracks(b); i++) {
        const dli_track_t *s = &DMF_Tracks(b)[i]; e->segment = s->segment_index;
        begin(e, "track", s->first_seen_ms); INT("track_id", i); INT("slot", s->slot);
        INT("end_ms", s->last_seen_ms); INT("identity_end_ms", s->identity_end_ms);
        INT("team", s->team); INT("team_conf", s->team_conf);
        INT("active_seen", s->active_seen); INT("spectator_seen", s->spectator_seen);
        INT("chase_spectator", s->chase_spectator); INT("combat_seen", s->combat_seen);
        INT("is_recorder_pov", s->is_recorder_pov);
        printf(",\"aliases\":[");
        for (int j = 0; j < s->alias_count; j++) { if (j) putchar(','); string(s->aliases[j]); }
        printf("]"); END();
    }
    for (int i = 0; i < DMF_NumParts(b); i++) {
        const dli_part_t *s = &DMF_Parts(b)[i];
        e->segment = DMF_Epochs(b)[s->epoch_index].segment_index;
        begin(e, "participant", DMF_Epochs(b)[s->epoch_index].start_ms);
        INT("epoch_id", s->epoch_index); INT("track_id", s->track_index);
        INT("active", s->active_in_epoch); INT("spectator", s->spectator_in_epoch);
        INT("score_authoritative", s->score_authoritative);
        INT("baseline_score", s->baseline_score); INT("final_score", s->final_score);
        INT("frags_earned", s->frags_earned); INT("penalties", s->penalties);
        INT("kills", s->kills); INT("deaths", s->deaths);
        INT("rank_score", s->rank_score); INT("rank_conf", s->rank_conf);
        INT("rank_result", s->rank_result); END();
        for (int j = s->first_weapon_fact; j >= 0;) {
            const dli_weapon_fact_t *w = &DMF_WeaponFacts(b)[j];
            begin(e, "weapon_fact", 0); INT("epoch_id", s->epoch_index);
            INT("track_id", s->track_index); INT("weapon", w->weapon);
            INT("kills", w->kills); INT("deaths", w->deaths); END(); j = w->next;
        }
        for (int j = s->first_pickup_fact; j >= 0;) {
            const dli_pickup_fact_t *p = &DMF_PickupFacts(b)[j];
            begin(e, "pickup_fact", 0); INT("epoch_id", s->epoch_index);
            INT("track_id", s->track_index); STR("item", p->name);
            INT("count", p->count); END(); j = p->next;
        }
    }
}

/* Cheap recorded-map selection uses the same wire decoder and segment facts.
 * Scan the entire stream: a later segment may contain the requested map. */
static void map_segment(void *ud,const dof_segment_info_t *s)
{
    export_t *e=ud;FORWARD(e,segment_start,s);
}
static void map_config(void *ud,int t,int index,const char *text)
{
    export_t *e=ud;FORWARD(e,configstring,t,index,text);
}
static void diagnostic(void *ud, const char *code, int frame_number, int detail)
{
    (void)ud;
    fprintf(stderr, "DM2 %s: frame %d, detail %d\n", code, frame_number, detail);
}
int main(int argc, char **argv)
{
    int maps_only=argc==3 && !strcmp(argv[2],"--maps-only");
    if (argc != 2 && !maps_only) { fputs("usage: decoder <demo> [--maps-only]\n", stderr); return 2; }
    FILE *fp = fopen(argv[1], "rb"); if (!fp) return 2;
    dmf_builder_t *b = DMF_Create(); if (!b) { fclose(fp); return 3; }
    DMF_Begin(b, "", NULL, NULL);
    export_t e = { .facts = DMF_Sink(b), .segment = -1, .began = clock() };
    dof_reader_t reader = {fp, read_file};
    dof_sink_t sink = { .ud = &e, .segment_start = segment, .configstring = config,
        .player_state = state, .print = print_event, .layout = layout,
        .frame = frame, .cancelled = cancelled, .sound = sound, .muzzleflash = muzzleflash, .entity_frame = entity_frame, .effect = effect };
    if(maps_only)sink=(dof_sink_t){.ud=&e,.segment_start=map_segment,.configstring=map_config,.cancelled=cancelled};
    sink.diagnostic = diagnostic;
    unsigned quality = 0;
    int rc = DOF_Decode(&reader, &sink, &quality); fclose(fp);
    if(maps_only) {
        DMF_End(b,quality);
        printf("{\"return_code\":%d,\"quality\":%u,\"maps\":[",rc,quality);
        for(int i=0;i<DMF_NumSegments(b);i++) {
            if(i)putchar(',');string(DMF_Segments(b)[i].map);
        }
        puts("]}");DMF_Destroy(b);return ferror(stdout) ? 3 : 0;
    }
    DMF_End(b, quality); facts(&e, b);
    begin(&e, "decode_result", 0); INT("return_code", rc); INT("quality", quality);
    INT("facts_quality", DMF_Quality(b));
    printf(",\"scratch_bytes\":%zu,\"facts_only_scratch_bytes\":%zu",DOF_ScratchSizeForSink(&sink),DOF_ScratchSize());
    printf(",\"states\":%llu,\"frames\":%llu", e.states, e.frames); END();
    DMF_Destroy(b);

    /* Reuse MAPGEN's exact movement interpretation, including its rejection
     * rules. This second consumer pass is automatic, bounded and not a parser. */
    if (rc == DOF_OK && quality == 0) {
        mapgen_demo_t *d = NULL;
        mapgen_demo_result_t mr = MapGenDemo_Read(argv[1], NULL, "content", &d);
        e.segment = 0;
        begin(&e, "movement_result", 0); STR("result", MapGenDemo_ResultName(mr));
        const mapgen_demo_provenance_t *p = MapGenDemo_Provenance(d);
        if (p) { INT("duration_ms", p->duration_ms); INT("samples", p->samples); }
        END();
        if (mr == MAPGEN_DEMO_OK) {
            for (uint32_t i = 0; i < MapGenDemo_NumEvents(d); i++) {
                const mapgen_demo_event_t *v = MapGenDemo_Event(d, i);
                begin(&e, "movement_event", v->time_ms);
                STR("event", MapGenDemo_EventName(v->kind)); vec("origin", v->origin);
                printf(",\"magnitude\":%.9g", (double)v->magnitude); END();
            }
            for (uint32_t i = 0; i < MapGenDemo_NumCells(d); i++) {
                const mapgen_demo_cell_t *v = MapGenDemo_Cell(d, i);
                begin(&e, "movement_cell", 0); INT("cell_id", i);
                INT("x", v->cell[0]); INT("y", v->cell[1]); INT("z", v->cell[2]);
                INT("frames", v->frames); INT("contacts", v->contacts); END();
            }
            for (uint32_t i = 0; i < MapGenDemo_NumMoves(d); i++) {
                const mapgen_demo_move_t *v = MapGenDemo_Move(d, i);
                begin(&e, "movement_edge", 0); INT("from", v->from);
                INT("to", v->to); INT("seen", v->seen); END();
            }
        }
        MapGenDemo_Free(d);
    }
    return ferror(stdout) ? 3 : 0;
}
