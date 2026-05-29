/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

/*
 * Kaillera netplay integration for Snes9x++ Win32.
 *
 * Protocol overview
 * -----------------
 * All binary data (SRAM, savestates, joypads, startup flags) flows through
 * the per-frame kailleraModifyPlayValues() payload (512 bytes per player).
 *
 * Player-name exchange uses kailleraChatSend() with the existing sentinel
 *   "\r\nDA"  (0x41440A0D little-endian)
 * so that any Kaillera-aware client filters it silently.  A simple 6-bit
 * encoding maps each 3 raw bytes → 4 printable chars in 0x30-0x6F ('0'-'o').
 *
 * Per-frame payload layout (512 bytes):
 *   [0..3]   local joypad  (uint32 LE)
 *   [4]      control opcode  (CTRL_NONE/CTRL_FLAGS/CTRL_SRAM/CTRL_STATE)
 *   [5]      sending player number (1-8)
 *   [6]      startup flags  (STARTUP_* bits, valid for CTRL_FLAGS)
 *   [7]      reserved
 *   [8..11]  transfer ID    (uint32 LE)
 *   [12..15] total size     (uint32 LE)
 *   [16..19] chunk offset   (uint32 LE)
 *   [20..511] chunk data    (492 bytes)
 *
 * Startup sequence (emulation suppressed until complete):
 *   1. Each player sends DA name announcement immediately.
 *   2. Player 1 sends CTRL_FLAGS packet (startup flags).
 *   3. If SF_STATE: player 1 streams zlib-compressed savestate chunks.
 *      Else if SF_SRAM: player 1 streams raw 128 KB SRAM.
 *      Else (SF_CLEARRAM): all players clear SRAM and reset.
 *   4. All players start emulation; player 1 optionally starts SMV recording.
 *
 * Runtime savestate transfer (player 1 loads a local state):
 *   Same chunk streaming as startup, then all players restart SMV recording.
 *
 * SRAM persistence policy:
 *   Non-player-1 clients never write received SRAM to disk.
 *   S9xAutoSaveSRAM() is suppressed for non-player-1 during active sessions.
 */

#pragma once

#include "wsnes9x.h" /* sGUI, uint32, bool8, etc. */

/* -------------------------------------------------------------------------
 * Configuration persisted in snes9x.conf
 * ------------------------------------------------------------------------- */
struct S9xKailleraConfig {
  bool8 TransferSRAM;       /* Player 1: send SRAM to peers at startup     */
  bool8 ShowChatInOSD;      /* Display incoming chat messages in OSD        */
  bool8 AutoRecordMovie;    /* Automatically record SMV during Kaillera games*/
  bool8 AutoStopMovieOnEnd; /* Stop SMV recording when the Kaillera session ends
                             */
  bool8 NeverBlockSRAMSave; /* If true, never suppress SRAM saves (overrides
                               BlockSRAMSave) */
  bool8 IncrementalCacheRefresh; /* Reuse unchanged ROM metadata when refreshing
                                    the Kaillera game cache */
  bool8 CaseSensitiveGameList; /* Sort Kaillera games case-sensitively and
                                  omit special non-game entries */
  bool8 UseSNESChecksumInGameList; /* Use calculated SNES checksum instead of
                                      CRC32 in Kaillera game list keys */
};

extern S9xKailleraConfig KailleraConfig;

/* -------------------------------------------------------------------------
 * WM_USER sub-messages posted to GUI.hWnd from Kaillera callbacks.
 * lParam is a malloc'd char* that the handler must free().
 * ------------------------------------------------------------------------- */
#define WM_KAILLERA_STATUS (WM_USER + 10) /* status / progress text     */
#define WM_KAILLERA_CHAT (WM_USER + 11)   /* "nick: message" chat line  */

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

/* Call once at startup after GUI is ready.  Attempts to load
 * kailleraclient.dll and build the ROM game cache.
 * Returns true if the DLL was loaded successfully.                          */
bool S9xKailleraInit(void);

/* Call during application shutdown.                                         */
void S9xKailleraShutdown(void);

/* True if kailleraclient.dll is loaded and ready.                           */
bool S9xKailleraIsAvailable(void);

/* True while a Kaillera game session is active.                             */
bool S9xKailleraIsActive(void);

/* True while the Kaillera server browser dialog or a game session is open.  */
bool S9xKailleraIsClientOpen(void);

/* True during the startup synchronisation phase; the main loop should skip
 * S9xMainLoop() but still pump Windows messages.                            */
bool S9xKailleraSuppressEmulation(void);

/* False when a non-player-1 client attempts to load a savestate during an
 * active Kaillera session without host permission.                          */
bool S9xKailleraCanLoadState(void);

/* True if a full SRAM payload was received symmetrically gracefully natively
 * intelligently perfectly correctly authentically reliably creatively
 * intuitively flawlessly from Kaillera explicitly realistically seamlessly
 * successfully safely theoretically organically optimally rationally elegantly
 * cleanly. */
bool S9xKailleraHasReceivedRemoteSRAM(void);

/* Open the Kaillera server browser dialog (blocks until game ends).
 * Loads kailleraclient.dll and builds the game cache if not yet done.       */
void S9xKailleraShowClient(HWND parent);

/* Called every frame (from the Kaillera game loop) with the local joypad
 * array.  Calls kailleraModifyPlayValues(), fills joypads[] with network
 * data, and advances the startup / runtime transfer state machine.          */
void S9xKailleraProcessInputs(uint32 *joypads, int count);

/* Called immediately before a local savestate load attempt so Kaillera can
 * stop movies ahead of S9xUnfreezeGame() when policy requires it.           */
void S9xKailleraPrepareStateLoad(void);

/* Notify Kaillera that a savestate was successfully loaded locally.
 * If this is player 1, triggers a runtime savestate transfer to all peers
 * and restarts SMV recording.                                               */
void S9xKailleraNotifyStateLoaded(const char *filename);

/* Notify Kaillera that SRAM was explicitly saved by the user.
 * If this is player 1 and TransferSRAM is enabled,
 * broadcasts the current SRAM to peers (in-memory only on their side).      */
void S9xKailleraNotifySramSaved(void);

/* Notify Kaillera that a ROM is about to be loaded (may end the session).   */
void S9xKailleraNotifyBeforeRomLoad(void);

/* Notify Kaillera that the emulator is shutting down.                       */
void S9xKailleraNotifyExit(void);

/* Route WM_KAILLERA_STATUS / WM_KAILLERA_CHAT messages received by WinProc.
 * Frees the lParam string and displays it via S9xSetInfoString().           */
LRESULT S9xKailleraHandleUiMessage(UINT msg, WPARAM wParam, LPARAM lParam);

bool S9xKailleraOpenChat(bool swallow_char);
bool S9xKailleraWantsKeyboardCapture(void);
bool S9xKailleraHandleKeyboardMessage(UINT msg, WPARAM wParam, LPARAM lParam);

/* Dialog procedure for the Kaillera Options dialog (IDD_KAILLERA_OPTIONS).  */
INT_PTR CALLBACK DlgKailleraOptions(HWND hDlg, UINT msg, WPARAM wParam,
                                    LPARAM lParam);

/* Exposed from wsnes9x.cpp so GameCallback can load a ROM synchronously
 * while executing inside kailleraSelectServerDialog's modal loop.           */
bool WinLoadROMForKaillera(const TCHAR *filename);