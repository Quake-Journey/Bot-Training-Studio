/*
 * Q2PRO-X — shared structural obituary matcher.
 *
 * The rules below are the ones that were already proven in production by the
 * MVD next-frag director and the Demo Analytics pre-scan; this file only moves
 * them somewhere all three consumers can reach, including the Demo Library
 * worker thread, which may not touch `mvd_t` or any other global state.
 *
 * Why obituary text and not STAT_FRAGS deltas: in modern mods (OpenTDM, CTF,
 * TDM variants) STAT_FRAGS is updated asynchronously relative to the kill —
 * round-score aggregates, warmup freezes, team-score sums — so a score delta
 * drifts seconds away from the wire event.  Obituaries are emitted by game
 * code on the exact tick of the kill and name the killer explicitly.
 */

#include "shared/shared.h"
#include "common/q2prox_obituary.h"

bool Q2PROX_Obituary_IsObituaryText(const char *s)
{
    if (!s || !*s)
        return false;

    /* Team chat opens with `(` — bail fast. */
    if (s[0] == '(')
        return false;

    /* Regular chat is `<name>: <text>` — a colon-space within the first ~24
     * bytes is a strong chat signal (standard Q2 colors don't introduce
     * colons that early). */
    const char *cs = strstr(s, ": ");
    if (cs && (cs - s) < 24)
        return false;

    /* Kill-phrase signals.  `" by "` covers most direct-fire obituaries
     * (railed by, blasted by, cut in half by, ...).  The rest cover the
     * splash / indirect means-of-death variants. */
    static const char *const kill_phrases[] = {
        " by ",
        " ate ",                    /* "X ate Y's rocket" */
        " almost dodged ",          /* MOD_R_SPLASH */
        " couldn't hide from ",     /* MOD_BFG_EFFECT */
        " saw the pretty lights",   /* MOD_BFG_LASER */
        " caught ",                 /* MOD_HELD_GRENADE variant */
        " didn't see ",             /* MOD_HG_SPLASH */
        " feels ",                  /* MOD_HELD_GRENADE */
        " tried to invade ",        /* MOD_TELEFRAG */
    };
    for (size_t i = 0; i < q_countof(kill_phrases); i++)
        if (strstr(s, kill_phrases[i]))
            return true;

    return false;
}

bool Q2PROX_Obituary_Match(const char *text,
                           const char *const *names, int num_names,
                           int *out_killer_slot, int *out_victim_slot)
{
    if (!text || !*text || !names || num_names <= 0)
        return false;

    struct {
        int         slot;
        const char *first_occ;
        int         name_len;
    } hits[4];
    int nhits = 0;

    for (int i = 0; i < num_names; i++) {
        const char *name = names[i];
        if (!name || !name[0])
            continue;
        size_t nlen = strlen(name);
        if (nlen < 3)
            continue;   /* under three characters: too many false positives
                         * against ordinary words in the message */
        const char *hit = strstr(text, name);
        if (!hit)
            continue;
        if (nhits >= (int)q_countof(hits)) {
            nhits++;    /* overflow sentinel — rejected by the count check */
            break;
        }
        hits[nhits].slot      = i;
        hits[nhits].first_occ = hit;
        hits[nhits].name_len  = (int)nlen;
        nhits++;
    }

    /* Drop strict-substring hits: "pogo" inside "pogos". */
    for (int a = 0; a < nhits && nhits <= 3; a++) {
        for (int b = 0; b < nhits; b++) {
            if (a == b)
                continue;
            if (hits[a].first_occ <= hits[b].first_occ &&
                hits[a].first_occ + hits[a].name_len >=
                    hits[b].first_occ + hits[b].name_len &&
                hits[a].name_len > hits[b].name_len) {
                for (int k = b; k < nhits - 1; k++)
                    hits[k] = hits[k + 1];
                nhits--;
                if (b <= a) a--;
                b--;
            }
        }
    }

    if (nhits != 2)
        return false;

    /* Quake II obituary layout is "<victim> <action> <killer>", so whichever
     * hit appears later in the string is the killer. */
    int killer_idx = (hits[0].first_occ > hits[1].first_occ) ? 0 : 1;
    int victim_idx = killer_idx ^ 1;

    if (out_killer_slot) *out_killer_slot = hits[killer_idx].slot;
    if (out_victim_slot) *out_victim_slot = hits[victim_idx].slot;
    return true;
}

q2px_obit_weapon_t Q2PROX_Obituary_Weapon(const char *text)
{
    if (!text || !text[0])
        return Q2PX_OBIT_WEAPON_UNKNOWN;

    /* Test the explicit weapon suffixes first: several mods preserve the
     * stock suffix while changing a verb or punctuation. */
    if (strstr(text, "'s super shotgun")) return Q2PX_OBIT_WEAPON_SUPER_SHOTGUN;
    if (strstr(text, "'s chaingun"))      return Q2PX_OBIT_WEAPON_CHAINGUN;
    if (strstr(text, "'s hyperblaster"))  return Q2PX_OBIT_WEAPON_HYPERBLASTER;
    if (strstr(text, "'s handgrenade") || strstr(text, "'s pain"))
        return Q2PX_OBIT_WEAPON_GRENADES;
    if (strstr(text, "'s grenade") || strstr(text, "'s shrapnel"))
        return Q2PX_OBIT_WEAPON_GRENADE_LAUNCHER;
    if (strstr(text, "'s rocket"))        return Q2PX_OBIT_WEAPON_ROCKET_LAUNCHER;
    if (strstr(text, "'s BFG"))           return Q2PX_OBIT_WEAPON_BFG10K;

    if (strstr(text, " was blasted by "))       return Q2PX_OBIT_WEAPON_BLASTER;
    if (strstr(text, " was gunned down by "))   return Q2PX_OBIT_WEAPON_SHOTGUN;
    if (strstr(text, " was machinegunned by ")) return Q2PX_OBIT_WEAPON_MACHINEGUN;
    if (strstr(text, " was railed by "))        return Q2PX_OBIT_WEAPON_RAILGUN;
    return Q2PX_OBIT_WEAPON_UNKNOWN;
}
