/*
Copyright (C) 1997-2001 Id Software, Inc.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#pragma once

#include "common/cmd.h"
#include "common/utils.h"

//
// common.h -- definitions common between client and server, but not game.dll
//

#define PRODUCT         "Q2PRO-X"

#if USE_CLIENT
#define APPLICATION     "Q2PRO-X"
#else
#define APPLICATION     "Q2PRO-X dedicated"
#endif

#define COM_DEFAULT_CFG     "default.cfg"
#define COM_AUTOEXEC_CFG    "autoexec.cfg"
#define COM_POSTEXEC_CFG    "postexec.cfg"
#define COM_POSTINIT_CFG    "postinit.cfg"
#define COM_Q2PROX_DIR      "q2pro-x"
/* One compatibility epoch for every GPU shader-binary cache owned by
 * Q2PRO-X.  The renderer program cache (WGL/EGL) and ANGLE's opaque blob
 * cache MUST move together: otherwise ANGLE may accept an old D3D binary
 * after the GLSL/program contract has changed.  Bump this once whenever a
 * cached shader/program contract becomes incompatible. */
#define Q2PROX_SHADER_CACHE_EPOCH 205u
#define COM_Q2PROX_CFG      COM_Q2PROX_DIR "/q2pro-x.cfg"
/* Persistent user bindings owned by Q2PRO-X.  The legacy q2config.cfg is an
 * immutable import source in 1.5; automatic maintenance must never rewrite
 * it.  Keeping bindings in a separate managed file also prevents cvar
 * migration and mod-profile overlays from changing their ownership. */
#define COM_Q2PROX_BINDINGS_CFG COM_Q2PROX_DIR "/q2pro-x.bindings.cfg"
/* Q2PRO-X base LOCAL cfg — LOCAL-scoped cvars/binds when no mod is
 * active (fs_game == ""). Split out of the global cfg so that global
 * stays GLOBAL-only and demo-visual-polluted LOCAL state can't leak
 * into the user's global config via the old "LOCAL fallback" path. */
#define COM_Q2PROX_BASE_LOCAL_CFG  COM_Q2PROX_DIR "/q2pro-x.local.cfg"
#define COM_Q2PROX_MODHELP  COM_Q2PROX_DIR "/q2pro.modhelp"
#define COM_Q2PROX_MODHELP_LOCAL COM_Q2PROX_DIR "/q2pro.modhelp.local"
/* Q2PRO-X demo visual config — a third storage target that holds the
 * local visual cvar preset used while a .dm2 or .mvd2 demo is playing.
 * Lives under the existing q2pro-x root (NOT a parallel q2prox tree).
 * Applied as a final override after the normal base + mod-local stack;
 * the MVD game switch is preserved so assets still resolve. */
#define COM_Q2PROX_DEMO_VISUAL_DIR  COM_Q2PROX_DIR "/demo_visual"
#define COM_Q2PROX_DEMO_VISUAL_CFG  COM_Q2PROX_DEMO_VISUAL_DIR "/q2pro-x.demo.cfg"

#ifdef _WIN32
#define COM_CONFIG_CFG      "q2config.cfg"
#else
#define COM_CONFIG_CFG      "config.cfg"
#endif

// FIXME: rename these
#define COM_HISTORYFILE_NAME    ".conhistory"
#define COM_DEMOCACHE_NAME      ".democache"
#define SYS_HISTORYFILE_NAME    ".syshistory"

#define MAXPRINTMSG     4096
#define MAXERRORMSG     1024

#define CONST_STR_LEN(x) x, sizeof("" x) - 1

#define STRINGIFY2(x)   #x
#define STRINGIFY(x)    STRINGIFY2(x)

typedef struct {
    const char *name;
    void (*func)(void);
} ucmd_t;

static inline const ucmd_t *Com_Find(const ucmd_t *u, const char *c)
{
    for (; u->name; u++) {
        if (!strcmp(c, u->name)) {
            return u;
        }
    }
    return NULL;
}

typedef struct string_entry_s {
    struct string_entry_s *next;
    char string[1];
} string_entry_t;

typedef void (*rdflush_t)(int target, const char *buffer, size_t len);

void        Com_BeginRedirect(int target, char *buffer, size_t buffersize, rdflush_t flush);
void        Com_EndRedirect(void);

void        Com_AbortFunc(void (*func)(void *), void *arg);

q_cold
void        Com_SetLastError(const char *msg);

q_cold
const char  *Com_GetLastError(void);

q_noreturn
void        Com_Quit(const char *reason, error_type_t type);

void        Com_SetColor(color_index_t color);

void        Com_Address_g(genctx_t *ctx);
void        Com_Generic_c(genctx_t *ctx, int argnum);
#if USE_CLIENT
void        Com_Color_g(genctx_t *ctx);
#endif

size_t      Com_Time_m(char *buffer, size_t size);
size_t      Com_Uptime_m(char *buffer, size_t size);
size_t      Com_UptimeLong_m(char *buffer, size_t size);

#ifndef _WIN32
void        Com_FlushLogs(void);
#endif

void        Com_AddConfigFile(const char *name, unsigned flags);
bool        Com_CheckParm(const char *parm);
/* Runtime-only safety latch.  `+set q2prox_config_readonly 1` is accepted
 * from the process command line and cannot be changed by configs/console. */
bool        Com_ConfigWriteProtected(void);
/* True only after early +set processing has removed its argv triples and no
 * explicit late startup command remains.  Side-effect free; used by startup
 * UI/audio without duplicating OS command-line parsing. */
bool        Com_IsColdIdleStartup(void);
/* Claim the otherwise-idle startup for a typed client action. Once claimed,
 * default menu and Intro presentation remain suppressed for this process. */
void        Com_ClaimColdStartup(void);

#if USE_SYSCON
void        Sys_Printf(const char *fmt, ...) q_printf(1, 2);
#else
#define     Sys_Printf(...) (void)0
#endif

#if USE_CLIENT
#define COM_DEDICATED   (dedicated->integer != 0)
#else
#define COM_DEDICATED   1
#endif

#if USE_DEBUG
#define COM_DEVELOPER   (developer->integer)
#define Com_DPrintf(...) \
    do { if (developer && developer->integer >= 1) \
        Com_LPrintf(PRINT_DEVELOPER, __VA_ARGS__); } while (0)
#define Com_DDPrintf(...) \
    do { if (developer && developer->integer >= 2) \
        Com_LPrintf(PRINT_DEVELOPER, __VA_ARGS__); } while (0)
#define Com_DDDPrintf(...) \
    do { if (developer && developer->integer >= 3) \
        Com_LPrintf(PRINT_DEVELOPER, __VA_ARGS__); } while (0)
#define Com_DDDDPrintf(...) \
    do { if (developer && developer->integer >= 4) \
        Com_LPrintf(PRINT_DEVELOPER, __VA_ARGS__); } while (0)
#define Com_DWPrintf(...) \
    do { if (developer && developer->integer >= 1) \
        Com_LPrintf(PRINT_WARNING, __VA_ARGS__); } while (0)
#else
#define COM_DEVELOPER   0
#define Com_DPrintf(...) ((void)0)
#define Com_DDPrintf(...) ((void)0)
#define Com_DDDPrintf(...) ((void)0)
#define Com_DDDDPrintf(...) ((void)0)
#define Com_DWPrintf(...) ((void)0)
#endif

#if USE_TESTS
extern cvar_t   *z_perturb;
#endif

#if USE_DEBUG
extern cvar_t   *developer;
#endif
extern cvar_t   *dedicated;
#if USE_CLIENT
extern cvar_t   *host_speeds;
#endif
extern cvar_t   *com_version;

#if USE_CLIENT
extern cvar_t   *cl_running;
extern cvar_t   *cl_paused;
#endif
extern cvar_t   *sv_running;
extern cvar_t   *sv_paused;
extern cvar_t   *com_timedemo;
extern cvar_t   *com_sleep;

extern cvar_t   *allow_download;
extern cvar_t   *allow_download_players;
extern cvar_t   *allow_download_models;
extern cvar_t   *allow_download_sounds;
extern cvar_t   *allow_download_maps;
extern cvar_t   *allow_download_textures;
extern cvar_t   *allow_download_pics;
extern cvar_t   *allow_download_others;

extern cvar_t   *rcon_password;

extern cvar_t   *sys_forcegamelib;
extern cvar_t   *q2prox_config_readonly;

#if USE_SAVEGAMES
extern cvar_t   *sys_allow_unsafe_savegames;
#endif

#if USE_SYSCON
extern cvar_t   *sys_history;
#endif

#if USE_CLIENT
// host_speeds times
extern unsigned     time_before_game;
extern unsigned     time_after_game;
extern unsigned     time_before_ref;
extern unsigned     time_after_ref;
#endif

extern const char   com_version_string[];

extern unsigned     com_framenum;
extern unsigned     com_eventTime; // system time of the last event
extern unsigned     com_localTime; // milliseconds since Q2 startup
extern unsigned     com_localTime2; // milliseconds since Q2 startup, but doesn't run if paused
extern bool         com_initialized;
extern time_t       com_startTime;

void Qcommon_Init(int argc, char **argv);
void Qcommon_Frame(void);
