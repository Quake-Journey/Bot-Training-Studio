/*
 * Q2PRO-X — shared structural obituary matcher.
 *
 * Extracted verbatim from the MVD analytics pre-scan so the live next-frag
 * director, the Demo Analytics heatmap and the offline Demo Library fact
 * decoder all accept exactly the same set of kill messages.  Three copies of
 * "does this print look like a kill" would drift the moment one of them met a
 * new mod; there is now one.
 *
 * Both entry points are PURE: no globals, no allocation, no logging.  That is
 * what makes them legal on the Demo Library worker thread.
 */
#ifndef Q2PROX_OBITUARY_H
#define Q2PROX_OBITUARY_H

#include "shared/shared.h"

/* True iff the print text structurally looks like a Quake II obituary rather
 * than chat, a team callout or a system message. */
bool Q2PROX_Obituary_IsObituaryText(const char *s);

/* Match an obituary against a player-name table.
 *
 * `names[i]` may be NULL or empty for an unused slot.  A confident match needs
 * EXACTLY TWO distinct names present in the text after strict-substring hits
 * are dropped ("pogo" inside "pogos").  In Quake II obituary word order the
 * later name is the killer.
 *
 * Returns true and writes both slot indices on a confident match, otherwise
 * returns false and leaves the out params untouched. */
bool Q2PROX_Obituary_Match(const char *text,
                           const char *const *names, int num_names,
                           int *out_killer_slot, int *out_victim_slot);

typedef enum {
    Q2PX_OBIT_WEAPON_UNKNOWN = 0,
    Q2PX_OBIT_WEAPON_BLASTER,
    Q2PX_OBIT_WEAPON_SHOTGUN,
    Q2PX_OBIT_WEAPON_SUPER_SHOTGUN,
    Q2PX_OBIT_WEAPON_MACHINEGUN,
    Q2PX_OBIT_WEAPON_CHAINGUN,
    Q2PX_OBIT_WEAPON_GRENADES,
    Q2PX_OBIT_WEAPON_GRENADE_LAUNCHER,
    Q2PX_OBIT_WEAPON_ROCKET_LAUNCHER,
    Q2PX_OBIT_WEAPON_HYPERBLASTER,
    Q2PX_OBIT_WEAPON_RAILGUN,
    Q2PX_OBIT_WEAPON_BFG10K,
} q2px_obit_weapon_t;

/* Pure phrase classifier.  Call only after the structural matcher accepted
 * the print as a two-player obituary. */
q2px_obit_weapon_t Q2PROX_Obituary_Weapon(const char *text);

#endif /* Q2PROX_OBITUARY_H */
