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

//
// cmd.h -- command text buffering and command execution
//

/* Q2PRO-X 1.5 managed configs legitimately exceed the historical 64 KiB
 * ceiling (the PO's generated OpenTDM config compresses to ~70 KiB).  This is
 * both the main command-buffer capacity and Cmd_ExecuteFile's compressed-file
 * limit, so raising only one check would merely turn "File too large" into a
 * buffer-overflow refusal.  Keep one shared 512 KiB authority. */
#define CMD_BUFFER_SIZE     (1 << 19)

#define ALIAS_LOOP_COUNT    16

/* Q2PRO-X 2026-08-09 — causal origin of queued command text.
 *
 * Deliberately separate from `from_t`, which names the dispatch authority of a
 * single command.  This names where the BYTES came from and travels with them
 * as metadata, so a cfg cannot claim to be the console by writing a token. */
typedef enum {
    CMD_ORIGIN_CONSOLE,     // local console, keybinding, or engine default
    CMD_ORIGIN_CMDLINE,     // process command line (+set / +command)
    CMD_ORIGIN_MENU,        // menu action
    CMD_ORIGIN_CONFIG,      // any file lineage: cfg, nested exec, alias in it
    CMD_ORIGIN_REMOTE,      // server stufftext / rcon
} cmd_origin_t;

/* Nesting depth of provenance spans.  Deep enough for cfg -> exec -> alias ->
 * inserted text; on overflow a span merges into its neighbour under the more
 * RESTRICTIVE origin, so the failure direction is always safe. */
#define CMD_ORIGIN_MAX_SPANS 64

// where did current command come from?
typedef enum {
    FROM_STUFFTEXT,
    FROM_RCON,
    FROM_MENU,
    FROM_CONSOLE,
    FROM_CMDLINE,
    FROM_CODE
} from_t;

/* Q2PRO-X 2026-08-09 — true for every command whose lineage is a FILE: the
 * cfg itself, a nested `exec`, an alias expanded inside it, `if`/`text`
 * insertion, and anything surviving one or more `wait` frames.  Console or
 * menu text entered while a cfg waits keeps its own origin because it is
 * appended as a separate span.  No command can influence the answer. */
bool Cmd_FromConfig(void);
cmd_origin_t Cmd_CurrentOrigin(void);

/* Queue text with an explicit causal origin.  The plain `Cbuf_AddText` /
 * `Cbuf_InsertText` wrappers keep their stock signatures: appending defaults to
 * console authority, inserting INHERITS the executing origin (which is what
 * nested execs and aliases require). */

typedef struct cmdbuf_s {
    from_t      from;
    char        *text; // may not be NULL terminated
    size_t      cursize;
    size_t      maxsize;
    int         waitCount;
    int         aliasCount; // for detecting runaway loops

    /* Q2PRO-X 2026-08-09 — immutable provenance.  Spans are kept in buffer
     * order, so `spans[0]` always describes the text about to execute; the
     * latched `current_origin` describes the line executing right now. */
    struct {
        size_t          bytes;
        cmd_origin_t    origin;
    }           spans[CMD_ORIGIN_MAX_SPANS];
    int         span_count;
    cmd_origin_t current_origin;
    void        (*exec)(struct cmdbuf_s *, const char *);
} cmdbuf_t;

// generic console buffer
extern char         cmd_buffer_text[CMD_BUFFER_SIZE];
extern cmdbuf_t     cmd_buffer;

extern cmdbuf_t     *cmd_current;

/*
Any number of commands can be added in a frame, from several different sources.
Most commands come from either keybindings or console line input, but remote
servers can also send across commands and entire text files can be execed.
*/

void Cbuf_Init(void);
// allocates an initial text buffer that will grow as needed

void Cbuf_AddText(cmdbuf_t *buf, const char *text);
// as new commands are generated from the console or keybindings,
// the text is added to the end of the command buffer.

void Cbuf_InsertText(cmdbuf_t *buf, const char *text);
void Cbuf_AddTextFrom(cmdbuf_t *buf, const char *text, cmd_origin_t origin);
void Cbuf_InsertTextFrom(cmdbuf_t *buf, const char *text, cmd_origin_t origin);
// when a command wants to issue other commands immediately, the text is
// inserted at the beginning of the buffer, before any remaining unexecuted
// commands.

void Cbuf_Execute(cmdbuf_t *buf);
// Pulls off \n terminated lines of text from the command buffer and sends
// them through Cmd_ExecuteString.  Stops when the buffer is empty.
// Normally called once per frame, but may be explicitly invoked.
// Do not call inside a command function!

void Cbuf_Frame(cmdbuf_t *buf);
// Called once per frame. Decrements waitCount, resets aliasCount.

void Cbuf_Clear(cmdbuf_t *buf);
// Clears entire buffer text.

//===========================================================================

/*
Command execution takes a null terminated string, breaks it into tokens,
then searches for a command or variable that matches the first token.
*/

typedef struct genctx_s {
    const char  *partial;
    size_t length;
    int argnum;
    char **matches;
    int count, size;
    void *data;
    bool ignorecase;
    bool ignoredups;
    bool stripquotes;
} genctx_t;

typedef void (*xcommand_t)(void);
typedef size_t (*xmacro_t)(char *, size_t);
typedef void (*xcompleter_t)(struct genctx_s *, int);

typedef struct cmd_macro_s {
    struct cmd_macro_s  *next, *hashNext;
    const char          *name;
    xmacro_t            function;
} cmd_macro_t;

typedef struct {
    const char *sh, *lo, *help;
} cmd_option_t;

typedef struct {
    const char      *name;
    xcommand_t      function;
    xcompleter_t    completer;
} cmdreg_t;

void Cmd_Init(void);

bool Cmd_Exists(const char *cmd_name);
// used by the cvar code to check for cvar / command name overlap

void Cmd_ExecTrigger(const char *string);

xcommand_t Cmd_FindFunction(const char *name);
cmd_macro_t *Cmd_FindMacro(const char *name);
xcompleter_t Cmd_FindCompleter(const char *name);

char *Cmd_AliasCommand(const char *name);
void Cmd_AliasSet(const char *name, const char *cmd);
void Cmd_AliasRemove(const char *name);

void Cmd_Command_g(genctx_t *ctx);
void Cmd_Alias_g(genctx_t *ctx);
void Cmd_Macro_g(genctx_t *ctx);
void Cmd_Config_g(genctx_t *ctx);
void Cmd_Option_c(const cmd_option_t *opt, xgenerator_t g, genctx_t *ctx, int argnum);
// attempts to match a partial command for automatic command line completion
// returns NULL if nothing fits

void Cmd_TokenizeString(const char *text, bool macroExpand);
// Takes a null terminated string.  Does not need to be /n terminated.
// breaks the string up into arg tokens.

void Cmd_ExecuteCommand(cmdbuf_t *buf);
// execute already tokenized string

void Cmd_ExecuteString(cmdbuf_t *buf, const char *text);
// Parses a single line of text into arguments and tries to execute it
// as if it was typed at the console

int Cmd_ExecuteFile(const char *path, unsigned flags);
/* Same, but the caller names the destination buffer so a nested exec can
 * stay inside a tagged transaction segment.  NULL means the main buffer. */
int Cmd_ExecuteFileInto(cmdbuf_t *dst, const char *path, unsigned flags);
// execute a config file

char *Cmd_MacroExpandString(const char *text, bool aliasHack);

void Cmd_Register(const cmdreg_t *reg);
void Cmd_AddCommand(const char *cmd_name, xcommand_t function);
// called by the init functions of other parts of the program to
// register commands and functions to call for them.
// The cmd_name is referenced later, so it should not be in temp memory
// if function is NULL, the command will be forwarded to the server
// as a clc_stringcmd instead of executed locally
void Cmd_Deregister(const cmdreg_t *reg);

/* Q2PRO-X 1.5: opaque counter bumped by every command add/remove.  A cached
 * view of the registry (the Cvar Browser's native command catalog) stores it
 * and rebuilds when it changes.  Compare it; never interpret it. */
unsigned Cmd_RegistryGeneration(void);
void Cmd_RemoveCommand(const char *cmd_name);

void Cmd_AddMacro(const char *name, xmacro_t function);

from_t  Cmd_From(void);
int     Cmd_Argc(void);
char    *Cmd_Argv(int arg);
char    *Cmd_Args(void);
char    *Cmd_RawArgs(void);
char    *Cmd_ArgsFrom(int from);
char    *Cmd_RawArgsFrom(int from);
char    *Cmd_ArgsRange(int from, int to);
size_t  Cmd_ArgsBuffer(char *buffer, size_t size);
size_t  Cmd_ArgvBuffer(int arg, char *buffer, size_t size);
int     Cmd_ArgOffset(int arg);
int     Cmd_FindArgForOffset(int offset);
char    *Cmd_RawString(void);
void    Cmd_Shift(void);
// The functions that execute commands get their parameters with these
// functions. Cmd_Argv () will return an empty string, not a NULL
// if arg > argc, so string operations are always safe.

void Cmd_Alias_f(void);

void Cmd_WriteAliases(qhandle_t f);

#define EXEC_TRIGGER(var) \
    do { \
        if ((var)->string[0]) { \
            Cbuf_AddText(&cmd_buffer, (var)->string); \
            Cbuf_AddText(&cmd_buffer, "\n"); \
        } \
    } while(0)

extern int cmd_optind;
extern char *cmd_optarg;
extern char *cmd_optopt;

int Cmd_ParseOptions(const cmd_option_t *opt);
void Cmd_PrintHelp(const cmd_option_t *opt);
void Cmd_PrintUsage(const cmd_option_t *opt, const char *suffix);
void Cmd_PrintHint(void);
