/*
 * Q2PRO-X 1.5 — neutral match-fact builder.
 *
 * Consumes the offline decoder's event stream for exactly ONE demo file and
 * produces the neutral records the Demo Library persists: segments (one per
 * serverdata/connection epoch), match epochs (bounded scoring intervals),
 * participant tracks (a slot's lifetime) and per (epoch, track) scoring
 * records.
 *
 * "Neutral" is the load-bearing word.  Nothing produced here depends on which
 * aliases the user happens to be tracking, so changing `cl_game_stats_names`
 * is a query over memory and never a reparse.  Nothing here decides whether a
 * given person "played a match" either — that classification is per alias set
 * and lives in the query layer.
 *
 * What this file DOES decide, because it is mod truth rather than user
 * preference, is the phase of the server (warmup / countdown / live / ended)
 * and the participation state of each slot (active / spectator).  Those come
 * from mod-authoritative recorded signals through per-mod Adapters, never from
 * "score changed, therefore played" or "zero frags, therefore spectator".
 *
 * Worker-safe: malloc/free only, no zone, no VFS, no cvars, no logging.
 */
#ifndef Q2PROX_DEMO_MATCH_FACTS_H
#define Q2PROX_DEMO_MATCH_FACTS_H

#include "shared/shared.h"
#include "client/demo_library.h"
#include "client/demo_offline_decoder.h"

typedef struct dmf_builder_s dmf_builder_t;

dmf_builder_t *DMF_Create(void);
void           DMF_Destroy(dmf_builder_t *b);

/* Start a new file.  `gamedir_hint` is the physical game directory the file
 * was found under; it is only a HINT used to pick an adapter when the stream
 * itself is ambiguous, never as mode evidence on its own. */
void DMF_Begin(dmf_builder_t *b, const char *gamedir_hint,
               bool (*cancelled)(void *ud), void *cancel_ud);

/* The sink to hand to DOF_Decode(). */
const dof_sink_t *DMF_Sink(dmf_builder_t *b);

/* Close the last open segment/epoch and finalize fingerprints. */
void DMF_End(dmf_builder_t *b, unsigned decoder_quality);

int  DMF_NumSegments(const dmf_builder_t *b);
int  DMF_NumEpochs(const dmf_builder_t *b);
int  DMF_NumTracks(const dmf_builder_t *b);
int  DMF_NumParts(const dmf_builder_t *b);
int  DMF_NumBoardRows(const dmf_builder_t *b);
int  DMF_NumWeaponFacts(const dmf_builder_t *b);
int  DMF_NumPickupFacts(const dmf_builder_t *b);

const dli_segment_t *DMF_Segments(const dmf_builder_t *b);
const dli_epoch_t   *DMF_Epochs(const dmf_builder_t *b);
const dli_track_t   *DMF_Tracks(const dmf_builder_t *b);
const dli_part_t    *DMF_Parts(const dmf_builder_t *b);
const dli_board_row_t *DMF_BoardRows(const dmf_builder_t *b);
const dli_weapon_fact_t *DMF_WeaponFacts(const dmf_builder_t *b);
const dli_pickup_fact_t *DMF_PickupFacts(const dmf_builder_t *b);

unsigned DMF_Quality(const dmf_builder_t *b);

/* Case/high-bit normalization used for every alias comparison in the project.
 * Exposed so the query layer, the overlay and the Intro projection cannot
 * disagree about what makes two recorded names the same person. */
void DMF_NormalizeName(char *dst, size_t dst_size, const char *src);

#endif /* Q2PROX_DEMO_MATCH_FACTS_H */
