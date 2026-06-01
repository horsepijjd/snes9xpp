/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifndef NOMINMAX
#define NOMINMAX 1
#endif

#include "kaillera.h"
#include "wlanguage.h"
#include "rsrc/resource.h"
#include "../snes9x.h"
#include "../cheats.h"
#include "../cpuexec.h"
#include "../display.h"
#include "../apu/apu.h"
#include "../snapshot.h"
#include "../movie.h"
#include "../memmap.h"
#include "../fscompat.h"
#include "../stream.h"
#include "../kaillera/kailleraclient.h"
#include "win32_display.h"
#include <Shlwapi.h>
#ifdef UNZIP_SUPPORT
#include "../unzip/unzip.h"
#endif
#ifdef JMA_SUPPORT
#include "../jma/s9x-jma.h"
#endif
#include <zlib.h>
#include <io.h>
#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <vector>
#include <string>
#include <ctime>
#include <cassert>

/* --------------------------------------------------------------------------
 * External symbols from wsnes9x.cpp / win32.cpp
 * -------------------------------------------------------------------------- */
extern uint32 joypads[8];
extern void S9xWinScanJoypads(void);
void ControlPadFlagsToS9xReportButtons(int n, uint32 p);
void ControlPadFlagsToS9xPseudoPointer(uint32 p);
void ChangeInputDevice(void);

/* Forward declarations for functions defined in wsnes9x.cpp */
extern void S9xRestoreWindowTitle(void);
extern void WinSaveConfigFile(void);

/* Kaillera DLL function pointer types (decorated __stdcall names) */
typedef int(WINAPI *PFN_GetVersion)(char *);
typedef int(WINAPI *PFN_Init)(void);
typedef int(WINAPI *PFN_Shutdown)(void);
typedef int(WINAPI *PFN_SetInfos)(kailleraInfos *);
typedef int(WINAPI *PFN_SelectServerDialog)(HWND);
typedef int(WINAPI *PFN_ModifyPlayValues)(void *, int);
typedef int(WINAPI *PFN_ChatSend)(char *);
typedef int(WINAPI *PFN_EndGame)(void);

/* --------------------------------------------------------------------------
 * Constants
 * -------------------------------------------------------------------------- */
static const char *k_app_name = "Snes9x++ 0.1";
static const char *k_file_cache_hdr = "S9XKFILE1";
static const char *k_game_cache_hdr = "S9XKGAME2";
static const char *k_game_cache_snes_checksum_hdr = "S9XKGAME3SNES";

/* Special Kaillera game list entries - always shown but not startable */
static const char *k_away_entry = "*Away (leave messages)";
static const char *k_chat_entry = "*Chat (not game)";

/* DA chat sentinel: bytes \r\nDA in memory = 0x41440A0D little-endian */
static const uint32 k_da_sentinel = 0x41440A0Du;
static const uint8 k_cmd_name = 1u; /* DA sub-command: name      */
static const uint8 k_cmd_hacks = 2u;
static const uint8 k_cmd_sram_begin = 3u;
static const uint8 k_cmd_sram_chunk = 4u;
static const uint8 k_cmd_sram_ack = 5u;
static const uint8 k_cmd_sram_end = 6u;

static const uint8 k_chat_sram_flag_startup = 0x01u;
static const uint8 k_chat_sram_flag_state = 0x02u;

static const int k_payload_size = 5;
static const uint32 k_payload_sync_sram = 0x40000000u;
static const uint32 k_payload_sync_state = 0x20000000u;
static const uint32 k_payload_sync_clear = 0x10000000u;
static const uint32 k_payload_sync_reset = 0x08000000u;
static const uint32 k_payload_sync_mask =
    k_payload_sync_sram | k_payload_sync_state | k_payload_sync_clear |
    k_payload_sync_reset;

static const int k_name_wait_frames = 120; /* 2 s at 60 fps             */
static const int k_name_announce_lead_frames = 8;
static const int k_name_max = 48; /* max nick length to store  */
static const size_t k_hacks_packet_size = 34;
static const size_t k_chat_sram_chunk_size = 171;
static const DWORD k_chat_sram_retry_ms = 250;
static const size_t k_chat_input_max = 240;

static int controller_option_for_player_count(int numplayers) {
  if (numplayers >= 6)
    return SNES_MULTIPLAYER8;
  if (numplayers >= 3)
    return SNES_MULTIPLAYER5;
  return SNES_JOYPAD;
}

enum CtrlOp : uint8 {
  CTRL_NONE = 0,
  CTRL_FLAGS = 1,
  CTRL_SRAM = 2,
  CTRL_STATE = 3
};
enum StartupFlag : uint8 {
  SF_SRAM = 0x01,
  SF_STATE = 0x02,
  SF_CLEARRAM = 0x04
};
enum ChatSramPhase : uint8 {
  CHAT_SRAM_NONE = 0,
  CHAT_SRAM_BEGIN = 1,
  CHAT_SRAM_CHUNK = 2,
  CHAT_SRAM_END = 3
};

/* --------------------------------------------------------------------------
 * DLL wrapper
 * -------------------------------------------------------------------------- */
struct KailleraApi {
  HMODULE dll;
  PFN_GetVersion getVersion;
  PFN_Init init;
  PFN_Shutdown shutdown;
  PFN_SetInfos setInfos;
  PFN_SelectServerDialog selectServerDialog;
  PFN_ModifyPlayValues modifyPlayValues;
  PFN_ChatSend chatSend;
  PFN_EndGame endGame;
  bool initialised;

  KailleraApi()
      : dll(NULL), getVersion(NULL), init(NULL), shutdown(NULL), setInfos(NULL),
        selectServerDialog(NULL), modifyPlayValues(NULL), chatSend(NULL),
        endGame(NULL), initialised(false) {}
};

/* --------------------------------------------------------------------------
 * Cache structures
 * -------------------------------------------------------------------------- */
struct FileCacheEntry {
  std::string relative_path;
  uint64 size;
  uint64 write_time;

  bool operator==(const FileCacheEntry &o) const {
    return relative_path == o.relative_path && size == o.size &&
           write_time == o.write_time;
  }
};

struct GameCacheEntry {
  std::string key;           /* display string for game list */
  std::string relative_path; /* ROM path relative to ROM dir */
  uint64 size;
  uint64 write_time;
};

/* --------------------------------------------------------------------------
 * Per-session runtime state
 * -------------------------------------------------------------------------- */
struct KailleraRuntime {
  /* Session */
  bool active;
  bool suppress_emulation;
  bool startup_sync;
  bool runtime_state_sync;
  bool authoritative; /* true = local player == 1  */
  int local_player;   /* 1-8, 0 = spectator        */
  int runtime_sync_player;
  int num_players;          /* from last modifyPlayValues chunk count */
  int session_player_count; /* numplayers from gameCallback (room size) */
  uint8 pending_auto_movie; /* 0=none, 1=from reset, 2=from snapshot       */
  int wait_frames;
  int announce_frames;
  bool name_sent;
  bool flags_sent;
  bool flags_known;
  bool hacks_known;
  bool received_remote_sram;
  bool auto_movie_owned;
  uint8 startup_flags;
  uint8 remote_sync_flags;
  bool remote_startup_reset;
  bool startup_release_pending;
  bool runtime_release_pending;
  char player_names[8][k_name_max + 1];
  bool player_name_known[8];

  /* Outgoing transfer (player 1 only) */
  std::vector<uint8> outgoing;
  size_t outgoing_offset;
  uint8 outgoing_opcode;
  uint32 outgoing_id;

  /* Incoming transfer (non-player-1) */
  std::vector<uint8> incoming_sram;
  std::vector<uint8> incoming_state;
  size_t in_sram_received;
  size_t in_state_received;
  uint32 in_sram_total;
  uint32 in_state_total;
  uint32 in_sram_id;
  uint32 in_state_id;
  uint32 transfer_serial;

  /* ROM / game cache */
  bool cache_ready;
  std::vector<FileCacheEntry> file_cache;
  std::vector<GameCacheEntry> game_cache;
  std::map<std::string, size_t> game_lookup; /* key → index in game_cache */
  std::vector<char> game_list_blob;
  std::vector<std::string> valid_exts; /* lower-case, no dot        */

  kailleraInfos infos;

  bool chat_sram_active;
  bool chat_sram_sender;
  bool chat_sram_startup_sync;
  bool chat_sram_is_state;
  uint8 chat_sram_phase;
  uint8 chat_sram_expected_acks;
  uint8 chat_sram_received_acks;
  uint32 chat_sram_id;
  size_t chat_sram_offset;
  DWORD chat_sram_last_send_tick;
  std::vector<uint8> chat_sram_data;
  bool startup_sram_pending;
  std::vector<uint8> startup_sram_data;
  std::vector<uint8> pending_pv_state;
  bool pv_state_ready;
  bool chat_input_active;
  DWORD chat_input_swallow_until_tick;
  std::string chat_input_text;

  void reset_session() {
    active = false;
    suppress_emulation = false;
    startup_sync = false;
    runtime_state_sync = false;
    authoritative = false;
    local_player = 0;
    runtime_sync_player = 0;
    num_players = 0;
    session_player_count = 0;
    pending_auto_movie = 0;
    wait_frames = 0;
    announce_frames = 0;
    name_sent = false;
    flags_sent = false;
    flags_known = false;
    hacks_known = false;
    received_remote_sram = false;
    auto_movie_owned = false;
    startup_flags = 0;
    remote_sync_flags = 0;
    remote_startup_reset = false;
    startup_release_pending = false;
    runtime_release_pending = false;
    outgoing.clear();
    outgoing_offset = 0;
    outgoing_opcode = CTRL_NONE;
    outgoing_id = 0;
    incoming_sram.clear();
    incoming_state.clear();
    in_sram_received = 0;
    in_state_received = 0;
    in_sram_total = 0;
    in_state_total = 0;
    in_sram_id = 0;
    in_state_id = 0;
    transfer_serial = 0;
    chat_sram_active = false;
    chat_sram_sender = false;
    chat_sram_startup_sync = false;
    chat_sram_is_state = false;
    chat_sram_phase = CHAT_SRAM_NONE;
    chat_sram_expected_acks = 0;
    chat_sram_received_acks = 0;
    chat_sram_id = 0;
    chat_sram_offset = 0;
    chat_sram_last_send_tick = 0;
    chat_sram_data.clear();
    startup_sram_pending = false;
    startup_sram_data.clear();
    pending_pv_state.clear();
    pv_state_ready = false;
    chat_input_active = false;
    chat_input_swallow_until_tick = 0;
    chat_input_text.clear();
    memset(player_names, 0, sizeof(player_names));
    memset(player_name_known, 0, sizeof(player_name_known));
  }

  KailleraRuntime()
      : active(false), suppress_emulation(false), startup_sync(false),
        runtime_state_sync(false), authoritative(false), local_player(0),
        runtime_sync_player(0), num_players(0), session_player_count(0),
        pending_auto_movie(0), wait_frames(0), announce_frames(0),
        name_sent(false), flags_sent(false), flags_known(false),
        hacks_known(false), received_remote_sram(false),
        auto_movie_owned(false), startup_flags(0), remote_sync_flags(0),
        remote_startup_reset(false), startup_release_pending(false),
        runtime_release_pending(false), outgoing_offset(0),
        outgoing_opcode(CTRL_NONE), outgoing_id(0), in_sram_received(0),
        in_state_received(0), in_sram_total(0), in_state_total(0),
        in_sram_id(0), in_state_id(0), transfer_serial(0), cache_ready(false),
        chat_sram_active(false), chat_sram_sender(false),
        chat_sram_startup_sync(false), chat_sram_is_state(false),
        chat_sram_phase(CHAT_SRAM_NONE), chat_sram_expected_acks(0),
        chat_sram_received_acks(0), chat_sram_id(0), chat_sram_offset(0),
        chat_sram_last_send_tick(0), startup_sram_pending(false),
        pv_state_ready(false), chat_input_active(false),
        chat_input_swallow_until_tick(0) {
    memset(player_names, 0, sizeof(player_names));
    memset(player_name_known, 0, sizeof(player_name_known));
    memset(&infos, 0, sizeof(infos));
  }
};

struct KailleraHackSettings {
  uint32 superfx_clock_multiplier;
  int32 interpolation_method;
  int32 overclock_mode;
  int32 one_clock_cycle;
  int32 one_slow_clock_cycle;
  int32 two_clock_cycles;
  int32 max_sprite_tiles_per_line;
  bool8 block_invalid_vram_access_master;
  bool8 block_invalid_vram_access;
  bool8 separate_echo_buffer;
};

/* --------------------------------------------------------------------------
 * Module-level globals
 * -------------------------------------------------------------------------- */
S9xKailleraConfig KailleraConfig = {TRUE, TRUE, FALSE, FALSE,
                                      FALSE, TRUE, FALSE, FALSE};
static KailleraApi api;
static KailleraRuntime rt;
static bool s_client_dialog_open = false;
static std::basic_string<TCHAR> state_transfer_title_base;
static bool state_transfer_title_active = false;
static void clear_state_transfer_title(void);
static void update_state_transfer_title(bool sending, size_t completed,
                                        size_t total);

/* --------------------------------------------------------------------------
 * Helpers: string utilities
 * -------------------------------------------------------------------------- */
static std::string str_tolower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return (char)tolower(c); });
  return s;
}

static std::string str_trim(const std::string &s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] <= 32 || s[a] == 0))
    ++a;
  while (b > a && (s[b - 1] <= 32 || s[b - 1] == 0))
    --b;
  return s.substr(a, b - a);
}

/* --------------------------------------------------------------------------
 * Helpers: wide string
 * -------------------------------------------------------------------------- */
static std::wstring to_wide(const std::string &u8) {
  if (u8.empty())
    return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), -1, NULL, 0);
  std::wstring out(n ? n - 1 : 0, L'\0');
  if (n > 1)
    MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), -1, &out[0], n);
  return out;
}

/* --------------------------------------------------------------------------
 * DA encoding / decoding
 *
 * Encoding: 3 raw bytes → 4 printable chars in 0x30-3x6F ('0'-'o').
 * This is a simple 6-bit encoding; the only consumers are other Snes9x++
 * clients in the same Kaillera room.
 * -------------------------------------------------------------------------- */
static std::string da_encode(const uint8 *data, size_t len) {
  /* prefix: \r\nDA */
  std::string out("\r\nDA", 4);
  out.reserve(4 + ((len + 2) / 3) * 4);

  for (size_t i = 0; i < len;) {
    uint8 b0 = data[i++];
    uint8 b1 = (i < len) ? data[i++] : 0;
    uint8 b2 = (i < len) ? data[i++] : 0;
    out.push_back((char)((b0 >> 2) + 0x30));
    out.push_back((char)(((b0 & 3) << 4 | b1 >> 4) + 0x30));
    out.push_back((char)(((b1 & 0xF) << 2 | b2 >> 6) + 0x30));
    out.push_back((char)((b2 & 0x3F) + 0x30));
  }
  return out;
}

static std::vector<uint8> da_decode(const char *after_sentinel) {
  /* after_sentinel points to the first encoded char (after "\r\nDA") */
  size_t enc_len = strlen(after_sentinel);
  std::vector<uint8> out;
  out.reserve((enc_len / 4) * 3);

  for (size_t i = 0; i + 3 < enc_len; i += 4) {
    uint8 c0 = (uint8)(after_sentinel[i + 0] - 0x30);
    uint8 c1 = (uint8)(after_sentinel[i + 1] - 0x30);
    uint8 c2 = (uint8)(after_sentinel[i + 2] - 0x30);
    uint8 c3 = (uint8)(after_sentinel[i + 3] - 0x30);
    out.push_back((c0 << 2) | (c1 >> 4));
    out.push_back((c1 << 4) | (c2 >> 2));
    out.push_back((c2 << 6) | c3);
  }
  return out;
}

/* --------------------------------------------------------------------------
 * OSD helpers
 * -------------------------------------------------------------------------- */
static void post_status(const char *fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  buf[sizeof(buf) - 1] = '\0';
  char *copy = _strdup(buf);
  if (copy)
    PostMessage(GUI.hWnd, WM_KAILLERA_STATUS, 0, (LPARAM)copy);
}

static void show_status(const char *fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  buf[sizeof(buf) - 1] = '\0';
  S9xSetInfoString(buf);
}

static void close_chat_input(void) {
  rt.chat_input_active = false;
  rt.chat_input_swallow_until_tick = 0;
  rt.chat_input_text.clear();
  if (Settings.UseZSNESFont)
    S9xSetInfoStringChat(" ");
  else
    S9xSetInfoString(" ");
}

static void refresh_chat_input_osd(void) {
  if (!rt.chat_input_active)
    return;

  char buf[512];
  const char *cursor = ((GetTickCount() / 400) & 1) ? "_" : " ";
  snprintf(buf, sizeof(buf), ">%s%s", rt.chat_input_text.c_str(), cursor);
  buf[sizeof(buf) - 1] = '\0';
  if (Settings.UseZSNESFont)
    S9xSetInfoStringChat(buf);
  else
    S9xSetInfoString(buf);
}

static void backspace_chat_input(void) {
  if (!rt.chat_input_text.empty())
    rt.chat_input_text.erase(rt.chat_input_text.size() - 1);
}

static bool append_chat_input_char(WPARAM wParam) {
  if (wParam < 0x20 || wParam == 0x7f)
    return false;

#ifdef UNICODE
  wchar_t wide[2] = {(wchar_t)wParam, 0};
  char mb[8] = {};
  int count =
      WideCharToMultiByte(CP_ACP, 0, wide, 1, mb, sizeof(mb), NULL, NULL);
  if (count <= 0)
    return false;
  if (rt.chat_input_text.size() + (size_t)count > k_chat_input_max)
    return false;
  rt.chat_input_text.append(mb, (size_t)count);
  return true;
#else
  if (rt.chat_input_text.size() >= k_chat_input_max)
    return false;
  rt.chat_input_text.push_back((char)wParam);
  return true;
#endif
}

/* --------------------------------------------------------------------------
 * Movie helpers
 * -------------------------------------------------------------------------- */
static void stop_active_movie(void) {
  if (S9xMovieActive())
    S9xMovieStop(TRUE);
  rt.auto_movie_owned = false;
}

static void stop_auto_movie(void) {
  if (rt.auto_movie_owned)
    stop_active_movie();
}

static std::string build_player_names_str(void) {
  std::string s;
  for (int i = 0; i < rt.num_players && i < 8; i++) {
    if (!s.empty())
      s += ", ";
    if (rt.player_names[i][0])
      s += rt.player_names[i];
    else {
      char tmp[16];
      snprintf(tmp, sizeof(tmp), "Player %d", i + 1);
      s += tmp;
    }
  }
  return s;
}

static std::string make_unique_movie_path(void) {
  /* Resolve movie directory exactly like the movie dialog in wsnes9x.cpp:
   * set CWD to DEFAULT_DIR first so _tfullpath resolves relative MovieDir
   * correctly, using a separate output buffer (not in-place). */
  TCHAR saved_cwd[MAX_PATH] = {};
  GetCurrentDirectory(MAX_PATH, saved_cwd);
  SetCurrentDirectory(S9xGetDirectoryT(DEFAULT_DIR));
  TCHAR movie_dir_w[MAX_PATH] = {};
  _tfullpath(movie_dir_w, GUI.MovieDir, MAX_PATH);
  SetCurrentDirectory(saved_cwd);
  _tmkdir(movie_dir_w);
  std::string movie_dir = (char *)WideToCP(movie_dir_w, CP_ACP);

  /* Extract stem exactly like S9xRestoreWindowTitle (wsnes9x.cpp line 720):
   * _splitpath on the narrow ROMFilename string directly, no wide conversion.
   */
  char stem[_MAX_FNAME + 1] = "kaillera";
  if (!Memory.ROMFilename.empty())
    _splitpath(Memory.ROMFilename.c_str(), NULL, NULL, stem, NULL);
  if (stem[0] == '\0')
    strcpy(stem, "kaillera");

  /* Build and test candidates in narrow strings throughout */
  time_t now = time(NULL);
  tm *lt = localtime(&now);
  char ts[32] = "00000000000000";
  if (lt)
    strftime(ts, sizeof(ts), "%Y%m%d%H%M%S", lt);
  for (int i = 0; i < 1000; i++) {
    char candidate[MAX_PATH];
    snprintf(candidate, MAX_PATH, "%s\\%s-kaillera-%s-%03d.smv",
             movie_dir.c_str(), stem, ts, i);
    if (_access(candidate, 0) != 0)
      return candidate;
  }
  char fallback[MAX_PATH];
  snprintf(fallback, MAX_PATH, "%s\\%s-kaillera.smv", movie_dir.c_str(), stem);
  return fallback;
}

static void start_auto_movie(bool from_reset) {
  if (!KailleraConfig.AutoRecordMovie)
    return;
  stop_auto_movie();

  std::string meta_u8 = "Kaillera players: " + build_player_names_str();
  std::wstring meta = to_wide(meta_u8);

  /* Bits for joypads 0..N-1 matching the Kaillera room size (gameCallback). */
  int n = rt.session_player_count;
  if (n < 1)
    n = rt.num_players;
  if (n < 1)
    n = 1;
  if (n > 8)
    n = 8;
  uint8 controllers = (uint8)(((1u << n) - 1u) & 0xFFu);
  if (!controllers)
    controllers = 1;

  uint8 opts = from_reset ? MOVIE_OPT_FROM_RESET : MOVIE_OPT_FROM_SNAPSHOT;
  std::string path = make_unique_movie_path();

  int result = S9xMovieCreate(path.c_str(), controllers, opts, meta.c_str(),
                              (int)meta.size());
  if (result == SUCCESS) {
    rt.auto_movie_owned = true;
    post_status(KAILLERA_MOVIE_STARTED);
  } else {
    post_status(
        "Kaillera: auto movie start failed (%d) romfn=%s path=%s errno=%d",
        result, Memory.ROMFilename.c_str(), path.c_str(), errno);
  }
}

/* --------------------------------------------------------------------------
 * Transfer helpers
 * -------------------------------------------------------------------------- */
static void begin_outgoing(uint8 opcode, std::vector<uint8> data) {
  rt.outgoing_opcode = opcode;
  rt.outgoing = std::move(data);
  rt.outgoing_offset = 0;
  rt.outgoing_id = ++rt.transfer_serial;
}

static void reset_transfer_state(void) {
  rt.outgoing.clear();
  rt.outgoing_offset = 0;
  rt.outgoing_opcode = CTRL_NONE;
  rt.outgoing_id = 0;
  rt.incoming_sram.clear();
  rt.incoming_state.clear();
  rt.in_sram_received = 0;
  rt.in_state_received = 0;
  rt.in_sram_total = 0;
  rt.in_state_total = 0;
  rt.in_sram_id = 0;
  rt.in_state_id = 0;
}

/* Compress savestate bytes with zlib; prepends 4-byte uncompressed size. */
/* Returns the number of bytes this game actually uses for battery-backed RAM.
 * 0 means the game has no SRAM (nothing to transfer).                       */
static size_t get_game_sram_size(void) {
  return Memory.SRAMMask ? (size_t)(Memory.SRAMMask + 1) : 0;
}

/* Generic zlib compress: prepends 4-byte LE uncompressed size, then data.
 * Used for both savestates and SRAM.  Returns empty vector on failure.      */
static std::vector<uint8> compress_data(const uint8 *raw, uint32 raw_size) {
  uLong bound = compressBound(raw_size);
  std::vector<uint8> buf(4 + (size_t)bound);
  WRITE_DWORD(&buf[0], raw_size);
  uLong out_size = bound;
  if (compress2(&buf[4], &out_size, raw, raw_size, Z_DEFAULT_COMPRESSION) !=
      Z_OK)
    return {};
  buf.resize(4 + out_size);
  return buf;
}

/* Convenience wrapper for savestates */
static inline std::vector<uint8> compress_state(const uint8 *raw,
                                                uint32 raw_size) {
  return compress_data(raw, raw_size);
}

static uint32 max_chat_state_size(void) {
  uLong bound = compressBound(S9xFreezeSize());
  if (bound >= 0xffffffffUL - 4)
    return 0xffffffffu;
  return (uint32)(4 + bound);
}

/* Read file content into byte vector (binary mode) */
static std::vector<uint8> read_file_bytes(const char *path) {
  std::vector<uint8> result;
  if (!path || !path[0])
    return result;
  FILE *f = fopen(path, "rb");
  if (!f)
    return result;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  if (sz <= 0 || sz > 64 * 1024 * 1024) {
    fclose(f);
    return result;
  }
  fseek(f, 0, SEEK_SET);
  result.resize((size_t)sz);
  size_t got = fread(&result[0], 1, (size_t)sz, f);
  fclose(f);
  if (got != (size_t)sz)
    result.clear();
  return result;
}

/* Check if data starts with gzip magic bytes */
static bool is_gzip(const std::vector<uint8> &data) {
  return data.size() >= 2 && data[0] == 0x1f && data[1] == 0x8b;
}

/* Decompress gzip data in memory.  Returns decompressed bytes, or empty on
 * failure. */
static std::vector<uint8> gunzip_buffer(const uint8 *data, size_t len) {
  if (len < 2 || data[0] != 0x1f || data[1] != 0x8b)
    return {};

  z_stream strm = {};
  if (inflateInit2(&strm, MAX_WBITS + 16) != Z_OK)
    return {};

  strm.next_in = const_cast<Bytef *>(data);
  strm.avail_in = (uInt)len;

  std::vector<uint8> out;
  uint8 chunk[32768];
  int ret;
  do {
    strm.next_out = chunk;
    strm.avail_out = sizeof(chunk);
    ret = inflate(&strm, Z_NO_FLUSH);
    if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) {
      inflateEnd(&strm);
      return {};
    }
    size_t have = sizeof(chunk) - strm.avail_out;
    out.insert(out.end(), chunk, chunk + have);
  } while (ret != Z_STREAM_END);

  inflateEnd(&strm);
  return out;
}

/* Apply received state bytes.  Handles both gzip (.frz file format)
 * and our legacy [4-byte LE raw_size][zlib-data] format.            */
static void apply_state_from_pv_transfer(const std::vector<uint8> &data) {
  if (data.empty())
    return;

  if (is_gzip(data)) {
    /* Gzip format (raw .frz file bytes) — gunzip then apply */
    auto raw = gunzip_buffer(&data[0], data.size());
    if (raw.empty()) {
      show_status("Kaillera: state gunzip failed.");
      return;
    }
    int result = S9xUnfreezeGameMem(&raw[0], (uint32)raw.size());
    if (result != SUCCESS)
      show_status("Kaillera: state apply failed (%d).", result);
  } else {
    /* Legacy zlib format with 4-byte raw_size header */
    if (data.size() < 4)
      return;
    uint32 raw_size = READ_DWORD(&data[0]);
    if (raw_size == 0 || raw_size > 64u * 1024u * 1024u)
      return;
    std::vector<uint8> raw(raw_size);
    uLong rl = raw_size;
    if (uncompress(&raw[0], &rl, &data[4], (uLong)(data.size() - 4)) != Z_OK) {
      show_status("Kaillera: state decompress failed.");
      return;
    }
    S9xUnfreezeGameMem(&raw[0], (uint32)rl);
  }
}

/* Send N zero-padding frames via modifyPlayValues, pumping messages.
 * Both sides call this in lockstep so they remain synchronized.
 *
 * On non-LAN connections the library uses frame delay > 1, meaning
 * modifyPlayValues may return 0 ("still buffering") for several calls
 * before actual data arrives.  Padding frames tolerate this — there's
 * no meaningful data to extract from the return buffer.              */
static void do_playvalues_padding(int count) {
  uint8 frame[40];
  for (int i = 0; i < count; i++) {
    memset(frame, 0, k_payload_size);
    int ret = api.modifyPlayValues(frame, k_payload_size);
    if (ret < 0) {
      rt.active = false;
      return;
    }
    MSG msg;
    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) {
        rt.active = false;
        PostQuitMessage((int)msg.wParam);
        return;
      }
      TranslateMessage(&msg);
      DispatchMessage(&msg);
    }
  }
}

/* Exchange a data blob via modifyPlayValues in lockstep.
 *
 * sender_data : bytes to send (empty for receivers)
 * numplayers  : number of players in the session
 * show_pct    : if true, update window title + OSD with transfer %
 * is_sending  : used with show_pct to label "send" vs "receive"
 *
 * Returns: per-player received data (vector of vectors).
 *
 * Protocol (identical to the Mupen64k/startup-SRAM exchange):
 *   30 zero-padding → 1 size frame (0x05) → 30 padding → N data (0x06) → 30
 * padding
 *
 * IMPORTANT: On non-LAN connections, modifyPlayValues uses frame delay > 1.
 * The library returns 0 ("buffering") when it hasn't collected enough frames
 * yet from all players.  When return == 0 the output buffer does NOT contain
 * valid data, so we must skip opcode processing for those calls.  Data from
 * delayed frames arrives in later calls, so every opcode eventually reaches
 * the receiver — but potentially offset by several calls from when it was
 * sent.  The size-propagation and drain padding windows absorb this delay.
 */
static std::vector<std::vector<uint8>>
do_playvalues_transfer(const std::vector<uint8> &sender_data, int numplayers,
                       bool show_pct = false, bool is_sending = true) {
  uint8 frame[40];
  uint32 local_size = (uint32)sender_data.size();

  /* 1. Prime-padding */
  do_playvalues_padding(30);
  if (!rt.active)
    return {};

  /* 2. Size frame (opcode 0x05) */
  memset(frame, 0, k_payload_size);
  frame[0] = 5;
  frame[1] = local_size & 0xFF;
  frame[2] = (local_size >> 8) & 0xFF;
  frame[3] = (local_size >> 16) & 0xFF;
  frame[4] = (local_size >> 24) & 0xFF;
  int ret = api.modifyPlayValues(frame, k_payload_size);
  if (ret < 0) {
    rt.active = false;
    return {};
  }

  uint32 sync_size = 0;
  if (ret > 0) {
    for (int p = 0; p < numplayers && p < 8; p++) {
      if (frame[p * k_payload_size] == 5) {
        uint32 psz = frame[p * k_payload_size + 1] |
                     (frame[p * k_payload_size + 2] << 8) |
                     (frame[p * k_payload_size + 3] << 16) |
                     (frame[p * k_payload_size + 4] << 24);
        if (psz > sync_size)
          sync_size = psz;
      }
    }
  }

  /* 3. Size-propagation padding.
   * On non-LAN connections, the 0x05 frame we sent may not have reached
   * the other side yet (buffered by the library).  These 30 extra calls
   * give enough room for delayed 0x05 frames to arrive from any player. */
  for (int i = 0; i < 30; i++) {
    memset(frame, 0, k_payload_size);
    ret = api.modifyPlayValues(frame, k_payload_size);
    if (ret < 0) {
      rt.active = false;
      return {};
    }
    if (ret > 0) {
      for (int p = 0; p < numplayers && p < 8; p++) {
        if (frame[p * k_payload_size] == 5) {
          uint32 psz = frame[p * k_payload_size + 1] |
                       (frame[p * k_payload_size + 2] << 8) |
                       (frame[p * k_payload_size + 3] << 16) |
                       (frame[p * k_payload_size + 4] << 24);
          if (psz > sync_size)
            sync_size = psz;
        }
      }
    }
    MSG msg;
    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) {
        rt.active = false;
        PostQuitMessage((int)msg.wParam);
        return {};
      }
      TranslateMessage(&msg);
      DispatchMessage(&msg);
    }
  }
  if (!rt.active)
    return {};

  /* 4. Data frames (opcode 0x06, 4 payload bytes each) */
  uint32 num_chunks = (sync_size + 3) / 4;
  std::vector<std::vector<uint8>> recv_data(numplayers);

  for (uint32 chunk = 0; chunk < num_chunks; chunk++) {
    memset(frame, 0, k_payload_size);
    uint32 offset = chunk * 4;
    if (offset < local_size) {
      frame[0] = 6;
      for (int b = 0; b < 4; b++)
        frame[1 + b] = (offset + b < local_size) ? sender_data[offset + b] : 0;
    }
    ret = api.modifyPlayValues(frame, k_payload_size);
    if (ret < 0) {
      rt.active = false;
      return recv_data;
    }

    if (ret > 0) {
      for (int p = 0; p < numplayers && p < 8; p++) {
        if (frame[p * k_payload_size] == 6) {
          for (int b = 0; b < 4; b++)
            recv_data[p].push_back(frame[p * k_payload_size + 1 + b]);
        }
      }
    }

    /* Progress update every ~10 KB (2500 chunks × 4 bytes) */
    if (show_pct && num_chunks > 100 &&
        (chunk % 2500 == 0 || chunk == num_chunks - 1)) {
      int pct = (int)((uint64)(chunk + 1) * 100 / num_chunks);
      update_state_transfer_title(is_sending, (size_t)(chunk * 4),
                                  (size_t)sync_size);
      show_status(is_sending ? "Kaillera: sending state %d%%"
                             : "Kaillera: receiving state %d%%",
                  pct);
    }
  }

  /* 5. Drain-padding */
  do_playvalues_padding(30);

  return recv_data;
}

/* Decompress and apply incoming state buffer */
static void apply_incoming_state(void) {
  if (rt.incoming_state.size() < 4)
    return;
  uint32 raw_size = READ_DWORD(&rt.incoming_state[0]);
  std::vector<uint8> raw(raw_size);
  uLong rl = raw_size;
  if (uncompress(&raw[0], &rl, &rt.incoming_state[4],
                 (uLong)(rt.incoming_state.size() - 4)) != Z_OK) {
    show_status("Kaillera: state decompress failed.");
    return;
  }
  S9xUnfreezeGameMem(&raw[0], (uint32)rl);
}

static void apply_incoming_sram(void) {
  if (rt.incoming_sram.size() < 4)
    return;
  uint32 raw_size = READ_DWORD(&rt.incoming_sram[0]);
  if (raw_size == 0 || raw_size > 0x80000)
    return;
  std::vector<uint8> raw(raw_size);
  uLong rl = raw_size;
  if (uncompress(&raw[0], &rl, &rt.incoming_sram[4],
                 (uLong)(rt.incoming_sram.size() - 4)) != Z_OK) {
    show_status("Kaillera: SRAM decompress failed.");
    return;
  }
  /* Write only as many bytes as the game actually uses; zero the rest */
  size_t game_sz = get_game_sram_size();
  size_t copy_sz = std::min((size_t)rl, game_sz);
  if (copy_sz)
    memcpy(Memory.SRAM, &raw[0], copy_sz);
  if (copy_sz < game_sz)
    memset(Memory.SRAM + copy_sz, 0, game_sz - copy_sz);
  CPU.SRAMModified = FALSE;
  rt.received_remote_sram = true;
}

/* --------------------------------------------------------------------------
 * Name announcement via DA chat
 * -------------------------------------------------------------------------- */
static void send_da_packet(const std::vector<uint8> &payload) {
  if (!api.chatSend || payload.empty())
    return;
  std::string enc = da_encode(&payload[0], payload.size());
  api.chatSend(const_cast<char *>(enc.c_str()));
}

static KailleraHackSettings capture_hack_settings(void) {
  KailleraHackSettings hacks;
  hacks.superfx_clock_multiplier = Settings.SuperFXClockMultiplier;
  hacks.interpolation_method = Settings.InterpolationMethod;
  hacks.overclock_mode = Settings.OverclockMode;
  hacks.one_clock_cycle = Settings.OneClockCycle;
  hacks.one_slow_clock_cycle = Settings.OneSlowClockCycle;
  hacks.two_clock_cycles = Settings.TwoClockCycles;
  hacks.max_sprite_tiles_per_line = Settings.MaxSpriteTilesPerLine;
  hacks.block_invalid_vram_access_master =
      Settings.BlockInvalidVRAMAccessMaster;
  hacks.block_invalid_vram_access = Settings.BlockInvalidVRAMAccess;
  hacks.separate_echo_buffer = Settings.SeparateEchoBuffer;
  return hacks;
}

static void apply_hack_settings(const KailleraHackSettings &hacks) {
  Settings.SuperFXClockMultiplier = hacks.superfx_clock_multiplier;
  Settings.InterpolationMethod = hacks.interpolation_method;
  Settings.OverclockMode = hacks.overclock_mode;
  Settings.OneClockCycle = hacks.one_clock_cycle;
  Settings.OneSlowClockCycle = hacks.one_slow_clock_cycle;
  Settings.TwoClockCycles = hacks.two_clock_cycles;
  Settings.MaxSpriteTilesPerLine = hacks.max_sprite_tiles_per_line;
  Settings.BlockInvalidVRAMAccessMaster =
      hacks.block_invalid_vram_access_master;
  Settings.BlockInvalidVRAMAccess = hacks.block_invalid_vram_access;
  Settings.SeparateEchoBuffer = hacks.separate_echo_buffer;
}

static uint8 chat_sram_remote_mask(void) {
  int n = rt.session_player_count;
  if (n < 1)
    n = rt.num_players;
  if (n > 8)
    n = 8;
  uint8 mask = 0;
  for (int p = 1; p <= n; p++)
    if (p != rt.local_player)
      mask |= (uint8)(1u << (p - 1));
  return mask;
}

static uint32 current_chat_sram_chunk_index(void) {
  return (uint32)(rt.chat_sram_offset / k_chat_sram_chunk_size);
}

static uint32 build_playdata_sync_flags(void) {
  if (rt.startup_sync) {
    if (!rt.authoritative)
      return 0;
    if (rt.startup_flags & SF_STATE)
      return rt.startup_release_pending ? 0 : k_payload_sync_state;
    if (rt.startup_flags & SF_SRAM)
      return k_payload_sync_sram |
             (rt.startup_release_pending ? k_payload_sync_reset : 0);
    if (rt.startup_flags & SF_CLEARRAM)
      return k_payload_sync_clear |
             (rt.startup_release_pending ? k_payload_sync_reset : 0);
    return 0;
  }

  if (rt.runtime_state_sync && rt.runtime_sync_player == rt.local_player &&
      !rt.runtime_release_pending)
    return k_payload_sync_state;

  return 0;
}

static uint8 decode_playdata_startup_flags(uint32 payload) {
  uint32 sync = payload & k_payload_sync_mask;
  if (sync & k_payload_sync_state)
    return SF_STATE;
  if (sync & k_payload_sync_sram)
    return SF_SRAM;
  if (sync & k_payload_sync_clear)
    return SF_CLEARRAM;
  return 0;
}

static bool begin_chat_transfer(std::vector<uint8> data, bool startup_sync,
                                bool is_state) {
  if (rt.chat_sram_active || data.empty())
    return false;

  if (startup_sync || !is_state) {
    if (!rt.authoritative)
      return false;
  } else if (rt.local_player < 1 || rt.local_player > 8 || !rt.authoritative)
    return false;

  rt.chat_sram_active = true;
  rt.chat_sram_sender = true;
  rt.chat_sram_startup_sync = startup_sync;
  rt.chat_sram_is_state = is_state;
  rt.chat_sram_phase = CHAT_SRAM_BEGIN;
  rt.chat_sram_expected_acks = chat_sram_remote_mask();
  rt.chat_sram_received_acks = 0;
  rt.chat_sram_id = ++rt.transfer_serial;
  rt.chat_sram_offset = 0;
  rt.chat_sram_last_send_tick = 0;
  rt.chat_sram_data = std::move(data);
  return true;
}

static bool can_receive_chat_transfer(int sender, bool startup_sync,
                                      bool is_state) {
  if (sender < 1 || sender > 8 || sender == rt.local_player)
    return false;
  if (startup_sync)
    return !rt.authoritative && sender == 1;
  if (is_state)
    return sender == 1;
  return !rt.authoritative && sender == 1;
}

static void send_chat_sram_ack(uint32 id, uint8 phase, uint32 chunk_index) {
  if (rt.local_player < 1 || rt.local_player > 8)
    return;
  std::vector<uint8> payload(11);
  payload[0] = k_cmd_sram_ack;
  payload[1] = (uint8)rt.local_player;
  WRITE_DWORD(&payload[2], id);
  payload[6] = phase;
  WRITE_DWORD(&payload[7], chunk_index);
  send_da_packet(payload);
}

static void finish_chat_sram_sender(void) {
  bool startup_sync = rt.chat_sram_startup_sync;
  bool is_state = rt.chat_sram_is_state;
  rt.chat_sram_active = false;
  rt.chat_sram_sender = false;
  rt.chat_sram_startup_sync = false;
  rt.chat_sram_is_state = false;
  rt.chat_sram_phase = CHAT_SRAM_END;
  rt.chat_sram_expected_acks = 0;
  rt.chat_sram_received_acks = 0;
  rt.chat_sram_offset = 0;
  rt.chat_sram_last_send_tick = 0;
  rt.chat_sram_data.clear();
  if (is_state)
    clear_state_transfer_title();
  if (!startup_sync && !is_state)
    post_status(KAILLERA_TRANSFER_DONE);
}

static void finish_chat_sram_receiver(void) {
  bool startup_sync = rt.chat_sram_startup_sync;
  bool is_state = rt.chat_sram_is_state;
  if (!startup_sync && !is_state)
    apply_incoming_sram();
  rt.chat_sram_active = false;
  rt.chat_sram_sender = false;
  rt.chat_sram_startup_sync = false;
  rt.chat_sram_is_state = false;
  rt.chat_sram_phase = CHAT_SRAM_END;
  rt.chat_sram_expected_acks = 0;
  rt.chat_sram_received_acks = 0;
  rt.chat_sram_offset = 0;
  rt.chat_sram_last_send_tick = 0;
  rt.chat_sram_data.clear();
  if (!startup_sync && !is_state)
    post_status(KAILLERA_TRANSFER_DONE);
}

static void handle_chat_sram_ack(int player, uint32 id, uint8 phase,
                                 uint32 chunk_index) {
  if (!rt.chat_sram_active || !rt.chat_sram_sender)
    return;
  if (id != rt.chat_sram_id || phase != rt.chat_sram_phase)
    return;
  if (phase == CHAT_SRAM_CHUNK &&
      chunk_index != current_chat_sram_chunk_index())
    return;
  if (player < 1 || player > 8 || player == rt.local_player)
    return;
  uint8 bit = (uint8)(1u << (player - 1));
  if (!(rt.chat_sram_expected_acks & bit))
    return;
  rt.chat_sram_received_acks |= bit;
  if ((rt.chat_sram_received_acks & rt.chat_sram_expected_acks) !=
      rt.chat_sram_expected_acks)
    return;
  rt.chat_sram_received_acks = 0;
  rt.chat_sram_last_send_tick = 0;
  if (rt.chat_sram_phase == CHAT_SRAM_BEGIN) {
    rt.chat_sram_phase =
        rt.chat_sram_data.empty() ? CHAT_SRAM_END : CHAT_SRAM_CHUNK;
    return;
  }
  if (rt.chat_sram_phase == CHAT_SRAM_CHUNK) {
    size_t chunk = std::min(k_chat_sram_chunk_size,
                            rt.chat_sram_data.size() - rt.chat_sram_offset);
    rt.chat_sram_offset += chunk;
    rt.chat_sram_phase = (rt.chat_sram_offset < rt.chat_sram_data.size())
                             ? CHAT_SRAM_CHUNK
                             : CHAT_SRAM_END;
    return;
  }
  if (rt.chat_sram_phase == CHAT_SRAM_END)
    finish_chat_sram_sender();
}

static void service_chat_sram_transfer(void) {
  if (!rt.chat_sram_active)
    return;
  if (!rt.chat_sram_sender) {
    if (rt.chat_sram_is_state && rt.in_state_total > 0) {
      update_state_transfer_title(false, rt.in_state_received,
                                  rt.in_state_total);
      show_status(KAILLERA_RECEIVING_STATE,
                  (int)(rt.in_state_received * 100 /
                        std::max((size_t)1, (size_t)rt.in_state_total)));
    } else if (rt.in_sram_total > 0)
      show_status(KAILLERA_RECEIVING_SRAM,
                  (int)(rt.in_sram_received * 100 /
                        std::max((size_t)1, (size_t)rt.in_sram_total)));
    return;
  }
  if (rt.chat_sram_is_state) {
    size_t completed = rt.chat_sram_phase == CHAT_SRAM_END
                           ? rt.chat_sram_data.size()
                           : rt.chat_sram_offset;
    update_state_transfer_title(true, completed, rt.chat_sram_data.size());
  }
  show_status(rt.chat_sram_is_state ? KAILLERA_SENDING_STATE
                                    : KAILLERA_SENDING_SRAM);
  if (rt.chat_sram_expected_acks == 0) {
    finish_chat_sram_sender();
    return;
  }
  DWORD now = GetTickCount();
  if (rt.chat_sram_last_send_tick != 0 &&
      now - rt.chat_sram_last_send_tick < k_chat_sram_retry_ms)
    return;
  std::vector<uint8> payload;
  if (rt.chat_sram_phase == CHAT_SRAM_BEGIN) {
    payload.resize(11);
    payload[0] = k_cmd_sram_begin;
    payload[1] = (uint8)rt.local_player;
    WRITE_DWORD(&payload[2], rt.chat_sram_id);
    WRITE_DWORD(&payload[6], (uint32)rt.chat_sram_data.size());
    payload[10] = (rt.chat_sram_startup_sync ? k_chat_sram_flag_startup : 0) |
                  (rt.chat_sram_is_state ? k_chat_sram_flag_state : 0);
  } else if (rt.chat_sram_phase == CHAT_SRAM_CHUNK) {
    size_t chunk = std::min(k_chat_sram_chunk_size,
                            rt.chat_sram_data.size() - rt.chat_sram_offset);
    payload.resize(15 + chunk);
    payload[0] = k_cmd_sram_chunk;
    payload[1] = (uint8)rt.local_player;
    WRITE_DWORD(&payload[2], rt.chat_sram_id);
    WRITE_DWORD(&payload[6], (uint32)rt.chat_sram_offset);
    WRITE_DWORD(&payload[10], current_chat_sram_chunk_index());
    payload[14] = (uint8)chunk;
    memcpy(&payload[15], &rt.chat_sram_data[rt.chat_sram_offset], chunk);
  } else if (rt.chat_sram_phase == CHAT_SRAM_END) {
    payload.resize(6);
    payload[0] = k_cmd_sram_end;
    payload[1] = (uint8)rt.local_player;
    WRITE_DWORD(&payload[2], rt.chat_sram_id);
  } else
    return;
  rt.chat_sram_received_acks = 0;
  rt.chat_sram_last_send_tick = now;
  send_da_packet(payload);
}

static void clear_state_transfer_title(void) {
  if (!state_transfer_title_active)
    return;
  if (GUI.hWnd)
    SetWindowText(GUI.hWnd, state_transfer_title_base.c_str());
  state_transfer_title_base.clear();
  state_transfer_title_active = false;
}

static void update_state_transfer_title(bool sending, size_t completed,
                                        size_t total) {
  if (!GUI.hWnd)
    return;

  if (!state_transfer_title_active) {
    TCHAR current_title[1024];
    GetWindowText(GUI.hWnd, current_title,
                  (int)(sizeof(current_title) / sizeof(current_title[0])));
    state_transfer_title_base = current_title;
    state_transfer_title_active = true;
  }

  size_t clamped_completed = total > 0 ? std::min(completed, total) : 0;
  int percent = total > 0 ? (int)((clamped_completed * 100) / total) : 0;

  TCHAR title[1200];
  _stprintf(title, TEXT("%s - Kaillera savestate %s %3d%%"),
            state_transfer_title_base.c_str(),
            sending ? TEXT("send") : TEXT("receive"), percent);
  SetWindowText(GUI.hWnd, title);
}

/* --------------------------------------------------------------------------
 * ROM metadata extraction
 * -------------------------------------------------------------------------- */
static inline bool name_byte_ok(unsigned char c) { return c >= 0x20; }

static inline bool all_ascii(const char *b, int n) {
  for (int i = 0; i < n; i++)
    if (b[i] < 32 || b[i] > 126)
      return false;
  return true;
}

static inline int info_score(const char *buf) {
  int s = 0;
  if (buf[28] + (buf[29] << 8) + buf[30] + (buf[31] << 8) == 0xFFFF)
    s += 3;
  if (buf[26] == 0x33)
    s += 2;
  if ((buf[21] & 0xF) < 4)
    s += 2;
  if (!(buf[61] & 0x80))
    s -= 4;
  if ((1 << (buf[23] - 7)) > 48)
    s -= 1;
  if (buf[25] < 14)
    s += 1;
  if (!all_ascii(buf, 20))
    s -= 1;
  return s;
}

static int detect_header(const std::vector<uint8> &d) {
  if (d.size() < 0x8000)
    return 0;
  int has = 0, none = 0;
  int remain = (int)d.size() & 0x7FFF;
  if (remain == 0)
    none += 3;
  else if (remain == 512)
    has += 2;
  if (d.size() >= 512) {
    unsigned short s = 0;
    for (int i = 0; i < 512; i++)
      s += d[i];
    if (s < 2500)
      has += 2;
    if (d[8] == 0xAA && d[9] == 0xBB && d[10] == 4)
      has += 3;
    else if (memcmp(&d[0], "GAME DOCTOR SF 3", 16) == 0)
      has += 5;
    else if ((d[4] == 0x77 && d[5] == 0x83) || (d[4] == 0xDD && d[5] == 0x82) ||
             (d[4] == 0xDD && d[5] == 2) || (d[4] == 0xF7 && d[5] == 0x83) ||
             (d[4] == 0xFD && d[5] == 0x82) || (d[4] == 0x00 && d[5] == 0x80) ||
             (d[4] == 0x47 && d[5] == 0x83) || (d[4] == 0x11 && d[5] == 2))
      has += 2;
  }
  return has > none ? 512 : 0;
}

static const char *region_code(uint8 r) {
  switch (r) {
  case 0:
    return "JPN";
  case 1:
    return "USA";
  case 2:
    return "EUR";
  case 3:
    return "SWE";
  case 4:
    return "FIN";
  case 5:
    return "DNK";
  case 6:
    return "FRA";
  case 7:
    return "NLD";
  case 8:
    return "ESP";
  case 9:
    return "DEU";
  case 10:
    return "ITA";
  case 11:
    return "HKG";
  case 12:
    return "IDN";
  case 13:
    return "KOR";
  default:
    return "UNK";
  }
}

static bool load_rom_bytes(const std::string &path, std::vector<uint8> &out) {
  std::string lext = str_tolower(splitpath(path).ext);
#ifdef JMA_SUPPORT
  if (lext == ".jma") {
    std::vector<uint8> tmp(CMemory::MAX_ROM_SIZE + 512);
    size_t n = load_jma_file(path.c_str(), &tmp[0]);
    if (!n)
      return false;
    out.assign(tmp.begin(), tmp.begin() + n);
    return true;
  }
#endif
#ifdef UNZIP_SUPPORT
  if (lext == ".zip" || lext == ".msu1") {
    out.clear();
    unzFile uf = unzOpen(path.c_str());
    if (!uf)
      return false;

    char fname[132] = {};
    uint32 best = 0;
    unz_file_info info;
    int rc = unzGoToFirstFile(uf);
    while (rc == UNZ_OK) {
      char nm[132] = {};
      unzGetCurrentFileInfo(uf, &info, nm, 128, NULL, 0, NULL, 0);
      if (info.uncompressed_size <= (unsigned)(CMemory::MAX_ROM_SIZE + 512) &&
          info.uncompressed_size > best) {
        strncpy(fname, nm, 131);
        best = info.uncompressed_size;
      }
      rc = unzGoToNextFile(uf);
    }
    if (!best || unzLocateFile(uf, fname, 0) != UNZ_OK ||
        unzOpenCurrentFile(uf) != UNZ_OK) {
      unzClose(uf);
      return false;
    }
    out.resize(best);
    int got = unzReadCurrentFile(uf, &out[0], best);
    bool ok = (got == (int)best && unzCloseCurrentFile(uf) == UNZ_OK);
    unzClose(uf);
    if (!ok) {
      out.clear();
      return false;
    }
    return true;
  }
#endif
  /* Raw / gzip */
  Stream *s = openStreamFromFSTREAM(path.c_str(), "rb");
  if (!s)
    return false;
  out.clear();
  uint8 buf[8192];
  for (;;) {
    size_t n = s->read(buf, sizeof(buf));
    if (!n)
      break;
    out.insert(out.end(), buf, buf + n);
  }
  s->closeStream();
  return !out.empty();
}

static uint16 snes_checksum_calc_sum(const uint8 *data, uint32 length) {
  uint16 sum = 0;
  for (uint32 i = 0; i < length; i++)
    sum += data[i];
  return sum;
}

static uint16 snes_checksum_mirror_sum(const uint8 *start, uint32 &length,
                                       uint32 mask = 0x800000) {
  while (!(length & mask) && mask)
    mask >>= 1;

  uint16 part1 = snes_checksum_calc_sum(start, mask);
  uint16 part2 = 0;

  uint32 next_length = length - mask;
  if (next_length) {
    part2 = snes_checksum_mirror_sum(start + mask, next_length, mask >> 1);

    while (next_length < mask) {
      next_length += next_length;
      part2 += part2;
    }

    length = mask + mask;
  }

  return part1 + part2;
}

static uint16 calculate_snes_rom_checksum(const uint8 *rom, uint32 length) {
  if (length & 0x7fff)
    return snes_checksum_calc_sum(rom, length);

  uint32 mirrored_length = length;
  return snes_checksum_mirror_sum(rom, mirrored_length);
}

static bool extract_rom_metadata(const std::string &fullpath,
                                 std::string &game_key) {
  std::vector<uint8> d;
  if (!load_rom_bytes(fullpath, d) || d.size() < 0x8000)
    return false;

  int hdr_sz = detect_header(d);
  size_t usable = (d.size() > (size_t)hdr_sz) ? d.size() - hdr_sz : 0;
  if (usable < 0x8000)
    return false;

  size_t hbase = 0;
  bool found = false;

  if (usable >= 0x500000) {
    size_t c = (size_t)hdr_sz + 0x40FFC0;
    if (c + 0x40 <= d.size() && info_score((char *)&d[c]) > 1) {
      hbase = (size_t)hdr_sz + 0x40FFB0;
      found = true;
    }
  }
  if (!found && usable >= 0x10000) {
    size_t lo = (size_t)hdr_sz + 0x7FC0;
    size_t hi = (size_t)hdr_sz + 0xFFC0;
    if (hi + 0x40 <= d.size()) {
      int ls = info_score((char *)&d[lo]);
      int hs = info_score((char *)&d[hi]);
      hbase = (ls > hs) ? (size_t)hdr_sz + 0x7FB0 : (size_t)hdr_sz + 0xFFB0;
      found = true;
      if (usable >= 0x20000) {
        size_t il = usable / 2 + (size_t)hdr_sz + 0x7FC0;
        if (il + 0x40 <= d.size()) {
          int is2 = info_score((char *)&d[il]) / 2;
          if (is2 > ls && is2 > hs)
            hbase = usable / 2 + (size_t)hdr_sz + 0x7FB0;
        }
      }
    }
  }
  if (!found || hbase + 0x2C >= d.size())
    return false;

  /* Name: 21 raw bytes at header+0x10, no charset conversion */
  std::string name;
  for (size_t i = 0; i < 21 && hbase + 0x10 + i < d.size(); i++) {
    unsigned char c = d[hbase + 0x10 + i];
    if (!c)
      break;
    name.push_back(name_byte_ok(c) ? (char)c : '_');
  }
  name = str_trim(name);
  if (name.empty())
    return false;

  uint8 region = d[hbase + 0x29];
  uint8 revision = d[hbase + 0x2B];

  char buf[256];
  if (KailleraConfig.UseSNESChecksumInGameList) {
    uint16 checksum =
        calculate_snes_rom_checksum(&d[hdr_sz], (uint32)usable);
    snprintf(buf, sizeof(buf), "%s (%s) v1.%d [%04X]", name.c_str(),
             region_code(region), revision, checksum);
  } else {
    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, &d[hdr_sz], (uInt)usable);
    snprintf(buf, sizeof(buf), "%s (%s) v1.%d [%08X]", name.c_str(),
             region_code(region), revision, (uint32)crc);
  }
  game_key = buf;
  return true;
}

/* --------------------------------------------------------------------------
 * Valid extension list (from Valid.Ext file or hard-coded fallback)
 * -------------------------------------------------------------------------- */
static void load_valid_exts(void) {
  rt.valid_exts.clear();
  /* Try to read from the exe directory */
  std::string path = S9xGetDirectory(DEFAULT_DIR) + "\\Valid.Ext";
  std::ifstream f(to_wide(path).c_str());
  if (f.good()) {
    std::string line;
    while (std::getline(f, line)) {
      line = str_trim(line);
      if (line.size() < 2)
        continue;
      /* Last char is Y/N for compressed; strip it */
      std::string ext = str_tolower(line.substr(0, line.size() - 1));
      rt.valid_exts.push_back(ext);
    }
  }
  if (rt.valid_exts.empty()) {
    for (auto *e :
         {"smc", "sfc", "swc", "fig", "bs", "st", "zip", "gz", "jma", "msu1"})
      rt.valid_exts.push_back(e);
  }
}

static bool is_supported(const std::string &filename) {
  std::string ext = str_tolower(splitpath(filename).ext);
  if (!ext.empty() && ext[0] == '.')
    ext = ext.substr(1);
  for (auto &v : rt.valid_exts)
    if (ext == v)
      return true;
  return false;
}

/* --------------------------------------------------------------------------
 * File / game cache I/O
 * -------------------------------------------------------------------------- */
static std::string cache_path(const char *leaf) {
  return S9xGetDirectory(DEFAULT_DIR) + "\\" + leaf;
}

static const char *game_cache_header(void) {
  return KailleraConfig.UseSNESChecksumInGameList
             ? k_game_cache_snes_checksum_hdr
             : k_game_cache_hdr;
}

static void enumerate_roms(const TCHAR *base, const TCHAR *rel,
                           std::vector<FileCacheEntry> &out) {
  TCHAR cur[MAX_PATH], pat[MAX_PATH];
  if (rel && *rel)
    _sntprintf(cur, MAX_PATH, TEXT("%s\\%s"), base, rel);
  else
    _sntprintf(cur, MAX_PATH, TEXT("%s"), base);
  cur[MAX_PATH - 1] = TEXT('\0');
  _sntprintf(pat, MAX_PATH, TEXT("%s\\*"), cur);
  pat[MAX_PATH - 1] = TEXT('\0');

  WIN32_FIND_DATA fd;
  HANDLE h = FindFirstFile(pat, &fd);
  if (h == INVALID_HANDLE_VALUE)
    return;

  do {
    if (!lstrcmp(fd.cFileName, TEXT(".")) || !lstrcmp(fd.cFileName, TEXT("..")))
      continue;

    TCHAR child[MAX_PATH];
    if (rel && *rel)
      _sntprintf(child, MAX_PATH, TEXT("%s\\%s"), rel, fd.cFileName);
    else
      _sntprintf(child, MAX_PATH, TEXT("%s"), fd.cFileName);
    child[MAX_PATH - 1] = TEXT('\0');

    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      enumerate_roms(base, child, out);
    } else {
      std::string u8rel = _tToChar(child);
      if (!is_supported(u8rel))
        continue;

      ULARGE_INTEGER wt;
      wt.LowPart = fd.ftLastWriteTime.dwLowDateTime;
      wt.HighPart = fd.ftLastWriteTime.dwHighDateTime;

      FileCacheEntry e;
      e.relative_path = u8rel;
      e.size = ((uint64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
      e.write_time = wt.QuadPart;
      out.push_back(e);
    }
  } while (FindNextFile(h, &fd));
  FindClose(h);
}

static bool load_file_cache(std::vector<FileCacheEntry> &out) {
  out.clear();
  std::ifstream f(to_wide(cache_path("kaillera_filecache.txt")).c_str());
  if (!f.good())
    return false;
  std::string hdr;
  std::getline(f, hdr);
  if (hdr != k_file_cache_hdr)
    return false;
  std::string line;
  while (std::getline(f, line)) {
    size_t a = line.find('\t');
    if (a == std::string::npos)
      continue;
    size_t b = line.find('\t', a + 1);
    if (b == std::string::npos)
      continue;
    FileCacheEntry e;
    e.relative_path = line.substr(0, a);
    e.size = (uint64)strtoull(line.substr(a + 1, b - a - 1).c_str(), NULL, 10);
    e.write_time = (uint64)strtoull(line.substr(b + 1).c_str(), NULL, 10);
    out.push_back(e);
  }
  return true;
}

static void save_file_cache(const std::vector<FileCacheEntry> &v) {
  std::ofstream f(to_wide(cache_path("kaillera_filecache.txt")).c_str(),
                  std::ios::trunc);
  if (!f.good())
    return;
  f << k_file_cache_hdr << '\n';
  for (auto &e : v)
    f << e.relative_path << '\t' << e.size << '\t' << e.write_time << '\n';
}

static bool load_game_cache_entries(std::vector<GameCacheEntry> &out) {
  out.clear();
  std::ifstream f(to_wide(cache_path("kaillera_gamecache.txt")).c_str());
  if (!f.good())
    return false;
  std::string hdr;
  std::getline(f, hdr);
  if (hdr != game_cache_header())
    return false;

  std::string line;
  while (std::getline(f, line)) {
    size_t tab = line.find('\t');
    if (tab == std::string::npos)
      continue;
    GameCacheEntry g;
    g.key = line.substr(0, tab);
    g.relative_path = line.substr(tab + 1);
    g.size = 0;
    g.write_time = 0;
    out.push_back(g);
  }
  return true;
}

static void save_game_cache(const std::vector<GameCacheEntry> &v) {
  std::ofstream f(to_wide(cache_path("kaillera_gamecache.txt")).c_str(),
                  std::ios::trunc);
  if (!f.good())
    return;
  f << game_cache_header() << '\n';
  for (auto &g : v)
    f << g.key << '\t' << g.relative_path << '\n';
}

static std::string make_unique_game_key(const std::string &key,
                                        std::set<std::string> &used_keys) {
  std::string unique = key;
  for (int dup = 2; used_keys.count(unique); dup++) {
    char extra[16];
    snprintf(extra, sizeof(extra), " #%d", dup);
    unique = key + extra;
  }
  used_keys.insert(unique);
  return unique;
}

static bool file_identity_matches(const FileCacheEntry &a,
                                  const FileCacheEntry &b) {
  return a.relative_path == b.relative_path && a.size == b.size &&
         a.write_time == b.write_time;
}

static void add_cached_game_entry(const GameCacheEntry &cached_game,
                                  const FileCacheEntry &current_file,
                                  std::set<std::string> &used_keys,
                                  std::vector<GameCacheEntry> &out) {
  if (cached_game.key.empty())
    return;

  GameCacheEntry g;
  g.key = make_unique_game_key(cached_game.key, used_keys);
  g.relative_path = current_file.relative_path;
  g.size = current_file.size;
  g.write_time = current_file.write_time;
  out.push_back(g);
}

static void add_scanned_game_entry(const FileCacheEntry &current_file,
                                   std::set<std::string> &used_keys,
                                   std::vector<GameCacheEntry> &out) {
  std::string full =
      S9xGetDirectory(ROM_DIR) + "\\" + current_file.relative_path;
  std::string key;
  if (!extract_rom_metadata(full, key))
    return;

  GameCacheEntry g;
  g.key = make_unique_game_key(key, used_keys);
  g.relative_path = current_file.relative_path;
  g.size = current_file.size;
  g.write_time = current_file.write_time;
  out.push_back(g);
}

static bool load_current_game_cache(
    const std::vector<FileCacheEntry> &current,
    const std::vector<GameCacheEntry> &cached_games,
    std::vector<GameCacheEntry> &out) {
  out.clear();
  std::map<std::string, const FileCacheEntry *> cur_map;
  for (auto &e : current)
    cur_map[e.relative_path] = &e;

  std::set<std::string> used_keys;
  for (auto &cached_game : cached_games) {
    auto it = cur_map.find(cached_game.relative_path);
    if (it == cur_map.end())
      return false; /* stale cache */
    add_cached_game_entry(cached_game, *it->second, used_keys, out);
  }

  /* Every game-cache entry must reference a current file (checked above).
   * ROMs that fail metadata extraction legitimately have no game-cache
   * entry, so out.size() may be smaller than current.size(). */
  return true;
}

static void rebuild_game_cache_from_files(
    const std::vector<FileCacheEntry> &current) {
  rt.file_cache = current;
  rt.game_cache.clear();
  std::set<std::string> used_keys;
  for (auto &fe : current)
    add_scanned_game_entry(fe, used_keys, rt.game_cache);
  save_file_cache(rt.file_cache);
  save_game_cache(rt.game_cache);
}

static void rebuild_game_lookup(void) {
  rt.game_lookup.clear();
  rt.game_list_blob.clear();

  /* Add special non-startable entries at the top of the list unless the
   * hidden strict game-list mode is enabled. */
  if (!KailleraConfig.CaseSensitiveGameList) {
    for (const char *special : {k_away_entry, k_chat_entry}) {
      for (char c : std::string(special))
        rt.game_list_blob.push_back(c);
      rt.game_list_blob.push_back('\0');
    }
  }

  if (KailleraConfig.CaseSensitiveGameList) {
    std::sort(rt.game_cache.begin(), rt.game_cache.end(),
              [](const GameCacheEntry &a, const GameCacheEntry &b) {
                return a.key < b.key;
              });
  } else {
    std::sort(rt.game_cache.begin(), rt.game_cache.end(),
              [](const GameCacheEntry &a, const GameCacheEntry &b) {
                return str_tolower(a.key) < str_tolower(b.key);
              });
  }

  for (size_t i = 0; i < rt.game_cache.size(); i++) {
    rt.game_lookup[rt.game_cache[i].key] = i;
    for (char c : rt.game_cache[i].key)
      rt.game_list_blob.push_back(c);
    rt.game_list_blob.push_back('\0');
  }
  rt.game_list_blob.push_back('\0'); /* double-null terminator */
  rt.infos.gameList = rt.game_list_blob.empty() ? const_cast<char *>("\0")
                                                : &rt.game_list_blob[0];
}

static void ensure_game_cache(void) {
  if (rt.cache_ready)
    return;
  load_valid_exts();

  /* 1. Enumerate current files */
  std::vector<FileCacheEntry> current;
  enumerate_roms(S9xGetDirectoryT(ROM_DIR), TEXT(""), current);
  std::sort(current.begin(), current.end(),
            [](const FileCacheEntry &a, const FileCacheEntry &b) {
              return a.relative_path < b.relative_path;
            });

  /* 2. Try to load cached data */
  std::vector<FileCacheEntry> cached_files;
  std::vector<GameCacheEntry> cached_games;
  bool file_cache_loaded = load_file_cache(cached_files);
  bool game_cache_loaded = load_game_cache_entries(cached_games);
  bool same = file_cache_loaded && cached_files == current;
  bool ok = false;

  if (same && game_cache_loaded) {
    rt.file_cache = cached_files;
    ok = load_current_game_cache(current, cached_games, rt.game_cache);
  }

  if (!ok && !same && KailleraConfig.IncrementalCacheRefresh &&
      file_cache_loaded && game_cache_loaded) {
    rt.file_cache = current;
    rt.game_cache.clear();

    std::map<std::string, FileCacheEntry> old_files;
    for (auto &fe : cached_files)
      old_files[fe.relative_path] = fe;

    std::map<std::string, GameCacheEntry> old_games;
    for (auto &g : cached_games)
      old_games[g.relative_path] = g;

    std::set<std::string> used_keys;
    for (auto &fe : current) {
      auto old_file = old_files.find(fe.relative_path);
      bool unchanged = old_file != old_files.end() &&
                       file_identity_matches(old_file->second, fe);
      if (unchanged) {
        auto old_game = old_games.find(fe.relative_path);
        if (old_game != old_games.end())
          add_cached_game_entry(old_game->second, fe, used_keys, rt.game_cache);
        continue;
      }

      add_scanned_game_entry(fe, used_keys, rt.game_cache);
    }
    save_file_cache(rt.file_cache);
    save_game_cache(rt.game_cache);
    ok = true;
  }

  if (!ok)
    rebuild_game_cache_from_files(current);

  rebuild_game_lookup();
  rt.cache_ready = true;
}

/* --------------------------------------------------------------------------
 * Per-frame payload building
 * -------------------------------------------------------------------------- */
static void build_local_payload(uint8 *payload, uint32 local_joypad) {
  memset(payload, 0, k_payload_size);
  uint32 value = local_joypad & ~k_payload_sync_mask;
  value |= build_playdata_sync_flags();
  WRITE_DWORD(payload + 0, value);
}

static void consume_player_payload(int pidx, const uint8 *payload) {
  int player = pidx + 1;
  uint32 value = READ_DWORD(payload + 0);
  uint8 sync_flags = decode_playdata_startup_flags(value);

  if (rt.startup_sync) {
    if (pidx == 0) {
      rt.remote_sync_flags = sync_flags;
      rt.remote_startup_reset = (value & k_payload_sync_reset) != 0;
      if (sync_flags != 0) {
        rt.flags_known = true;
        rt.startup_flags = sync_flags;
      }
    }
    return;
  }

  if (rt.runtime_state_sync) {
    if (player == rt.runtime_sync_player) {
      rt.remote_sync_flags = sync_flags;
      return;
    }
    return;
  }

  if ((sync_flags & SF_STATE) && player != rt.local_player && player == 1) {
    rt.runtime_state_sync = true;
    rt.runtime_sync_player = player;
    rt.remote_sync_flags = sync_flags;
    rt.suppress_emulation = true;
    stop_active_movie();
  }
}

/* --------------------------------------------------------------------------
 * Startup synchronisation state machine
 * -------------------------------------------------------------------------- */
static bool names_ready(void) {
  for (int i = 0; i < rt.num_players && i < 8; i++)
    if (!rt.player_name_known[i])
      return false;
  return true;
}

static void fill_missing_names(void) {
  for (int i = 0; i < rt.num_players && i < 8; i++) {
    if (!rt.player_name_known[i]) {
      snprintf(rt.player_names[i], k_name_max + 1, "Player %d", i + 1);
      rt.player_names[i][k_name_max] = '\0';
      rt.player_name_known[i] = true;
    }
  }
}

static void finalize_startup(void) {
  if (!rt.startup_sync)
    return;

  if (rt.startup_flags & SF_STATE) {
    if (rt.authoritative) {
      if (rt.chat_sram_active && rt.chat_sram_startup_sync &&
          rt.chat_sram_is_state)
        return;
      if (!rt.startup_release_pending) {
        rt.startup_release_pending = true;
        return;
      }
      if (rt.remote_sync_flags & SF_STATE)
        return;
    } else {
      if (rt.chat_sram_active && rt.chat_sram_startup_sync &&
          rt.chat_sram_is_state)
        return;
      if (rt.remote_sync_flags & SF_STATE)
        return;
      if (rt.incoming_state.empty() || rt.in_state_received < rt.in_state_total)
        return;
      show_status(KAILLERA_RECEIVING_STATE, 100);
      apply_incoming_state();
    }
    rt.startup_sync = false;
    rt.suppress_emulation = false;
    rt.pending_auto_movie = 2; /* snapshot */
    post_status(KAILLERA_TRANSFER_DONE);
    clear_state_transfer_title();
    return;
  }
}

static void finalize_runtime_sync(void) {
  if (!rt.runtime_state_sync)
    return;

  /* Gate: wait one regular modifyPlayValues frame so both sender and
   * receiver are synchronized in sync mode before starting the
   * blocking play-values transfer.  On the first call here both sides
   * set pv_state_ready=true and return; on the NEXT ProcessInputs
   * call they both proceed past this check and enter the transfer
   * on the exact same modifyPlayValues frame. */
  if (!rt.pv_state_ready) {
    rt.pv_state_ready = true;
    return;
  }

  bool is_sender = (rt.runtime_sync_player == rt.local_player);
  int sender_idx = rt.runtime_sync_player - 1;

  /* Perform the lockstep data exchange via modifyPlayValues.
   * Sender sends rt.pending_pv_state; receiver sends nothing. */
  auto received = do_playvalues_transfer(is_sender ? rt.pending_pv_state
                                                   : std::vector<uint8>(),
                                         rt.num_players, true, is_sender);

  /* Receiver: decompress and apply the received state */
  if (!is_sender && sender_idx >= 0 && sender_idx < (int)received.size() &&
      !received[sender_idx].empty()) {
    apply_state_from_pv_transfer(received[sender_idx]);
  }

  /* Resume-sync: 30 lockstep padding frames guarantee both sides
   * start emulation on the exact same frame, AFTER the receiver
   * has finished decompressing and loading the state. */
  do_playvalues_padding(30);

  /* Resume emulation */
  rt.runtime_state_sync = false;
  rt.runtime_sync_player = 0;
  rt.runtime_release_pending = false;
  rt.remote_sync_flags = 0;
  rt.suppress_emulation = false;
  rt.pending_pv_state.clear();
  rt.pv_state_ready = false;
  rt.pending_auto_movie = 2;
  post_status(KAILLERA_TRANSFER_DONE);
  clear_state_transfer_title();
}

/* --------------------------------------------------------------------------
 * Kaillera callbacks (all run on the game-loop thread)
 * -------------------------------------------------------------------------- */
static void WINAPI chat_callback(char *nick, char *text) {
  if (!nick || !text)
    return;

  /* Check for our DA sentinel */
  if (*(const uint32 *)text == k_da_sentinel) {
    auto data = da_decode(text + 4);

    if (!data.empty() && data[0] == k_cmd_name && data.size() >= 3) {
      int pnum = data[1];
      size_t nlen = std::min((size_t)data[2], data.size() - 3);
      if (pnum >= 1 && pnum <= 8) {
        if (nick[0]) {
          strncpy(rt.player_names[pnum - 1], nick, k_name_max);
          rt.player_names[pnum - 1][k_name_max] = '\0';
          rt.player_name_known[pnum - 1] = true;
        } else if (nlen <= (size_t)k_name_max) {
          memcpy(rt.player_names[pnum - 1], &data[3], nlen);
          rt.player_names[pnum - 1][nlen] = '\0';
          rt.player_name_known[pnum - 1] = true;
        }
      }
      return;
    }

    if (!data.empty() && data[0] == k_cmd_sram_begin && data.size() >= 10) {
      int sender = data[1];
      uint32 id = READ_DWORD(&data[2]);
      uint32 total = READ_DWORD(&data[6]);
      uint8 flags = data.size() >= 11 ? data[10] : k_chat_sram_flag_startup;
      bool startup_sync = (flags & k_chat_sram_flag_startup) != 0;
      bool is_state = (flags & k_chat_sram_flag_state) != 0;
      if (can_receive_chat_transfer(sender, startup_sync, is_state)) {
        if (rt.chat_sram_phase == CHAT_SRAM_END && !rt.chat_sram_active &&
            id == rt.chat_sram_id) {
          send_chat_sram_ack(id, CHAT_SRAM_BEGIN, 0);
          return;
        }
        if (rt.chat_sram_active && rt.chat_sram_phase != CHAT_SRAM_NONE &&
            id != rt.chat_sram_id)
          return;
        if (total > 0 &&
            total <= (is_state ? max_chat_state_size() : 0x100000)) {
          reset_transfer_state();
          if (startup_sync) {
            rt.flags_known = true;
            rt.startup_flags = is_state ? SF_STATE : SF_SRAM;
          } else if (is_state) {
            rt.runtime_state_sync = true;
            rt.runtime_sync_player = sender;
            rt.remote_sync_flags = 0;
            rt.runtime_release_pending = false;
            rt.suppress_emulation = true;
            stop_active_movie();
          }
          if (is_state) {
            rt.in_state_id = id;
            rt.in_state_total = total;
            rt.in_state_received = 0;
            rt.incoming_state.assign(total, 0);
            update_state_transfer_title(false, 0, total);
          } else {
            rt.in_sram_id = id;
            rt.in_sram_total = total;
            rt.in_sram_received = 0;
            rt.incoming_sram.assign(total, 0);
          }
          rt.chat_sram_active = true;
          rt.chat_sram_sender = false;
          rt.chat_sram_startup_sync = startup_sync;
          rt.chat_sram_is_state = is_state;
          rt.chat_sram_id = id;
          rt.chat_sram_phase = CHAT_SRAM_BEGIN;
          rt.chat_sram_expected_acks = 0;
          rt.chat_sram_received_acks = 0;
          rt.chat_sram_offset = 0;
          rt.chat_sram_last_send_tick = 0;
          send_chat_sram_ack(id, CHAT_SRAM_BEGIN, 0);
        }
      }
      return;
    }
    if (!data.empty() && data[0] == k_cmd_sram_chunk && data.size() >= 15) {
      int sender = data[1];
      if (rt.chat_sram_active &&
          can_receive_chat_transfer(sender, rt.chat_sram_startup_sync,
                                    rt.chat_sram_is_state)) {
        uint32 id = READ_DWORD(&data[2]);
        uint32 offset = READ_DWORD(&data[6]);
        uint32 chunk_index = READ_DWORD(&data[10]);
        size_t expected_offset = (size_t)chunk_index * k_chat_sram_chunk_size;
        size_t chunk = std::min((size_t)data[14], data.size() - 15);
        std::vector<uint8> &incoming =
            rt.chat_sram_is_state ? rt.incoming_state : rt.incoming_sram;
        uint32 active_id =
            rt.chat_sram_is_state ? rt.in_state_id : rt.in_sram_id;
        size_t &received =
            rt.chat_sram_is_state ? rt.in_state_received : rt.in_sram_received;
        if (id == active_id && offset == expected_offset &&
            offset < incoming.size() && offset + chunk <= incoming.size()) {
          memcpy(&incoming[offset], &data[15], chunk);
          received = std::max(received, (size_t)(offset + chunk));
          rt.chat_sram_phase = CHAT_SRAM_CHUNK;
          send_chat_sram_ack(id, CHAT_SRAM_CHUNK, chunk_index);
        }
      }
      return;
    }
    if (!data.empty() && data[0] == k_cmd_sram_end && data.size() >= 6) {
      int sender = data[1];
      uint32 id = READ_DWORD(&data[2]);
      if (can_receive_chat_transfer(sender, rt.chat_sram_startup_sync,
                                    rt.chat_sram_is_state)) {
        if (!rt.chat_sram_active && rt.chat_sram_phase == CHAT_SRAM_END &&
            id == rt.chat_sram_id) {
          send_chat_sram_ack(id, CHAT_SRAM_END, 0);
          return;
        }
        size_t received =
            rt.chat_sram_is_state ? rt.in_state_received : rt.in_sram_received;
        size_t total = rt.chat_sram_is_state ? rt.incoming_state.size()
                                             : rt.incoming_sram.size();
        uint32 active_id =
            rt.chat_sram_is_state ? rt.in_state_id : rt.in_sram_id;
        if (rt.chat_sram_active && id == active_id && received >= total) {
          send_chat_sram_ack(id, CHAT_SRAM_END, 0);
          finish_chat_sram_receiver();
        }
      }
      return;
    }
    if (!data.empty() && data[0] == k_cmd_sram_ack && data.size() >= 11) {
      handle_chat_sram_ack(data[1], READ_DWORD(&data[2]), data[6],
                           READ_DWORD(&data[7]));
      return;
    }
    return; /* never display DA packets */
  }

  /* Regular chat */
  if (KailleraConfig.ShowChatInOSD) {
    char buf[512];
    if (Settings.UseZSNESFont)
      snprintf(buf, sizeof(buf), "%s>%s", nick, text);
    else
      snprintf(buf, sizeof(buf), "%s: %s", nick, text);
    buf[sizeof(buf) - 1] = '\0';
    if (Settings.UseZSNESFont)
      S9xSetInfoStringChat(buf);
    else
      post_status("%s", buf);
  }
}

static void WINAPI drop_callback(char *nick, int playernb) {
  post_status(KAILLERA_PLAYER_DROPPED, nick ? nick : "?");
  if (playernb >= 1 && playernb <= 8) {
    snprintf(rt.player_names[playernb - 1], k_name_max + 1, "%s (dropped)",
             nick ? nick : "?");
    rt.player_name_known[playernb - 1] = true;
  }
}

static void WINAPI infos_callback(char *game) {
  if (!game)
    return;
  ensure_game_cache();
  auto it = rt.game_lookup.find(game ? game : "");
  if (it == rt.game_lookup.end())
    return;

  const GameCacheEntry &e = rt.game_cache[it->second];
  char msg[1024];
  snprintf(msg, sizeof(msg), "%s\n\nFile: %s\nSize: %llu bytes", game,
           e.relative_path.c_str(), (unsigned long long)e.size);
  MessageBoxA(GUI.hWnd, msg, "Kaillera Game Info", MB_OK | MB_ICONINFORMATION);
}

/* --------------------------------------------------------------------------
 * Kaillera game loop (runs synchronously inside gameCallback)
 * -------------------------------------------------------------------------- */
static void run_kaillera_game_loop(void) {
  while (rt.active) {
    /* Process Windows messages so the UI stays responsive */
    MSG msg;
    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) {
        rt.active = false;
        PostQuitMessage((int)msg.wParam);
        return;
      }
      TranslateMessage(&msg);
      DispatchMessage(&msg);
    }
    if (!rt.active)
      break;

    /* S9xSetPause mutes sound but S9xClearPause never unmutes; re-evaluate
     * the mute state here so sound resumes when a menu/pause is dismissed. */
    S9xSetSoundMute(GUI.Mute || Settings.ForcedPause != 0);

    /* Scan local input hardware */
    if (!rt.suppress_emulation)
      S9xWinScanJoypads();

    /* Exchange inputs with Kaillera; may drive transfer state machine */
    S9xKailleraProcessInputs(joypads, 8);
    service_chat_sram_transfer();
    refresh_chat_input_osd();

    /* After ProcessInputs returns: start SMV (not inside Kaillera DLL call
     * chain). */
    if (rt.pending_auto_movie != 0) {
      uint8 kind = rt.pending_auto_movie;
      rt.pending_auto_movie = 0;
      start_auto_movie(kind == 1);
    }

    if (!rt.suppress_emulation) {
      /* Report all players' pads to the SNES control layer */
      for (int i = 0; i < rt.num_players && i < 8; i++)
        ControlPadFlagsToS9xReportButtons(i, joypads[i]);
      if (GUI.ControllerOption == SNES_JUSTIFIER_2)
        ControlPadFlagsToS9xPseudoPointer(joypads[1]);

      S9xMainLoop();
    } else {
      Sleep(8);
    }
  }
}

/* --------------------------------------------------------------------------
 * gameCallback: called by kailleraSelectServerDialog when a game starts
 * -------------------------------------------------------------------------- */
#include <cctype>

static std::string ExtractNickFromIni(const std::string &path) {
  std::ifstream file(path.c_str());
  if (!file.is_open())
    return "";

  std::string line;
  while (std::getline(file, line)) {
    std::string lower_line = line;
    for (size_t i = 0; i < lower_line.size(); ++i)
      lower_line[i] = (char)tolower((unsigned char)lower_line[i]);

    size_t pos = lower_line.find("nick=");
    if (pos != std::string::npos) {
      std::string nick = line.substr(pos + 5);
      size_t start = nick.find_first_not_of(" \r\n\t");
      if (start != std::string::npos)
        nick = nick.substr(start);
      else
        nick = "";
      size_t end = nick.find_last_not_of(" \r\n\t");
      if (end != std::string::npos)
        nick = nick.substr(0, end + 1);
      if (!nick.empty())
        return nick;
    }
  }
  return "";
}

static std::string ExtractKailleraNick(void) {
  std::string dirs[3];
  dirs[0] = ".";

#ifdef UNICODE
  char tmp[_MAX_PATH];
  WideCharToMultiByte(CP_UTF8, 0, GUI.RomDir, -1, tmp, _MAX_PATH, NULL, NULL);
  dirs[1] = tmp;
  WideCharToMultiByte(CP_UTF8, 0, GUI.MovieDir, -1, tmp, _MAX_PATH, NULL, NULL);
  dirs[2] = tmp;
#else
  dirs[1] = GUI.RomDir;
  dirs[2] = GUI.MovieDir;
#endif

  const char *inis[] = {"n02.ini", "supraclient.ini", "supraslinkclient.ini"};

  for (int i = 0; i < 3; i++) {
    for (int d = 0; d < 3; d++) {
      std::string path = dirs[d] + "\\" + inis[i];
      std::string nick = ExtractNickFromIni(path);
      if (!nick.empty())
        return nick;
    }
  }

  return Settings.NetplayUsername;
}

static int WINAPI game_callback(char *game, int player, int numplayers) {
  ensure_game_cache();

  /* Check for special non-startable entries */
  if (game &&
      (strcmp(game, k_away_entry) == 0 || strcmp(game, k_chat_entry) == 0))
    return 0;

  auto it = rt.game_lookup.find(game ? game : "");
  if (it == rt.game_lookup.end()) {
    MessageBoxA(GUI.hWnd, "Selected game not found in cache.", "Kaillera",
                MB_OK | MB_ICONERROR);
    return 0;
  }

  const GameCacheEntry &entry = rt.game_cache[it->second];
  std::string full = S9xGetDirectory(ROM_DIR) + "\\" + entry.relative_path;

  /* Load ROM if it isn't already loaded (rt.active is false here) */
  bool need_load = Memory.ROMFilename.empty() ||
                   _tcsicmp(_tFromChar(Memory.ROMFilename.c_str()),
                            _tFromChar(full.c_str())) != 0;
  if (need_load && !WinLoadROMForKaillera(_tFromChar(full.c_str()))) {
    MessageBoxA(GUI.hWnd, "Failed to load ROM for Kaillera session.",
                "Kaillera", MB_OK | MB_ICONERROR);
    return 0;
  }

  if (Settings.ApplyCheats) {
    Settings.ApplyCheats = FALSE;
    S9xCheatsDisable();
  }

  int previous_controller_option = GUI.ControllerOption;
  int kaillera_controller_option =
      controller_option_for_player_count(numplayers);
  if (GUI.ControllerOption != kaillera_controller_option) {
    GUI.ControllerOption = kaillera_controller_option;
    ChangeInputDevice();
  }

  /* Block SRAM saves to disk for sessions where the in-memory SRAM is not
   * the player's own authoritative save: non-P1 received someone else's SRAM,
   * and when TransferSRAM is disabled SRAM was cleared at startup for all.
   * Conversely, clear any prior block when P1 starts with TransferSRAM on,
   * since their own save is the authoritative source for the session. */
  if (numplayers > 1 && (player != 1 || !KailleraConfig.TransferSRAM))
    GUI.BlockSRAMSave = !KailleraConfig.NeverBlockSRAMSave;
  else if (player == 1 && KailleraConfig.TransferSRAM)
    GUI.BlockSRAMSave = false;

  /* Initialise session state */
  rt.reset_session();
  reset_transfer_state();
  rt.active = true;
  rt.suppress_emulation = (numplayers > 1);
  rt.startup_sync = (numplayers > 1);
  rt.authoritative = (player == 1);
  rt.local_player = player;
  rt.runtime_sync_player = 0;
  rt.num_players = numplayers;
  rt.session_player_count = numplayers;
  rt.flags_known = rt.authoritative || !rt.startup_sync;
  rt.startup_flags = rt.startup_sync ? 0 : SF_CLEARRAM;
  rt.remote_sync_flags = 0;
  rt.remote_startup_reset = false;
  rt.startup_release_pending = false;
  rt.runtime_release_pending = false;
  rt.startup_sram_pending = false;
  rt.startup_sram_data.clear();

  rt.announce_frames = 0;
  stop_auto_movie();

  if (numplayers > 1) {
    std::vector<uint8> startup_data;
    if (rt.authoritative && KailleraConfig.TransferSRAM &&
        get_game_sram_size() > 0)
      rt.startup_flags = SF_SRAM;
    else if (rt.authoritative && rt.startup_sync)
      rt.startup_flags = SF_CLEARRAM;

    startup_data.push_back((rt.local_player == 1) ? rt.startup_flags : 0);

    KailleraHackSettings hacks;
    if (rt.authoritative)
      hacks = capture_hack_settings();
    else
      memset(&hacks, 0, sizeof(hacks));
    startup_data.push_back(rt.authoritative ? 1 : 0);

    uint8 hack_buf[34] = {0};
    if (rt.authoritative) {
      hack_buf[0] = hacks.block_invalid_vram_access_master ? 1 : 0;
      hack_buf[1] = hacks.block_invalid_vram_access ? 1 : 0;
      hack_buf[2] = hacks.separate_echo_buffer ? 1 : 0;
      hack_buf[3] = 0; // reserved (formerly allow_all_players_state_load)
      WRITE_DWORD(&hack_buf[4], hacks.superfx_clock_multiplier);
      WRITE_DWORD(&hack_buf[8], (uint32)hacks.interpolation_method);
      WRITE_DWORD(&hack_buf[12], (uint32)hacks.overclock_mode);
      WRITE_DWORD(&hack_buf[16], (uint32)hacks.one_clock_cycle);
      WRITE_DWORD(&hack_buf[20], (uint32)hacks.one_slow_clock_cycle);
      WRITE_DWORD(&hack_buf[24], (uint32)hacks.two_clock_cycles);
      WRITE_DWORD(&hack_buf[28], (uint32)hacks.max_sprite_tiles_per_line);
    }
    startup_data.insert(startup_data.end(), hack_buf, hack_buf + 34);

    // Extract local nickname and bundle directly into the startup sequence
    // payload natively
    std::string nick = ExtractKailleraNick();
    uint8 nlen = (uint8)nick.length();
    if (nlen > k_name_max)
      nlen = k_name_max;
    startup_data.push_back(nlen);
    startup_data.insert(startup_data.end(), nick.begin(), nick.begin() + nlen);
    if (rt.authoritative && rt.startup_flags == SF_SRAM) {
      size_t sram_sz = get_game_sram_size();
      if (sram_sz > 0) {
        std::vector<uint8> comp = compress_data(Memory.SRAM, (uint32)sram_sz);
        if (!comp.empty()) {
          /* comp = [4-byte raw_size][zlib_data]
           * Startup payload: [4-byte raw_size][4-byte zlib_len][zlib_data] */
          uint32 zlib_len = (uint32)(comp.size() - 4);
          uint8 sz_buf[8];
          WRITE_DWORD(&sz_buf[0], (uint32)sram_sz);
          WRITE_DWORD(&sz_buf[4], zlib_len);
          startup_data.insert(startup_data.end(), sz_buf, sz_buf + 8);
          startup_data.insert(startup_data.end(), comp.begin() + 4, comp.end());
        }
      }
    }
    /* Pad to 4-byte boundary + 4 extra to prevent chunking truncation */
    while (startup_data.size() % 4 != 0)
      startup_data.push_back(0);
    for (int pad = 0; pad < 4; pad++)
      startup_data.push_back(0);

    /* Extra initial padding for startup (beyond the 30 inside
     * do_playvalues_transfer) */
    do_playvalues_padding(30);

    /* Exchange startup data via play-values */
    auto recv_data = do_playvalues_transfer(startup_data, numplayers);

    for (int p = 0; p < numplayers; p++) {
      if (recv_data[p].size() >= 36) {
        uint32 offset = 0;
        uint8 flags = recv_data[p][offset++];
        if (p == 0 && rt.startup_sync) {
          rt.startup_flags = flags;
          rt.flags_known = true;
        }

        uint8 hacks_valid = recv_data[p][offset++];
        if (hacks_valid && p == 0 && rt.startup_sync && !rt.authoritative) {
          uint32 hack_base = offset;
          KailleraHackSettings hs;
          hs.block_invalid_vram_access_master = recv_data[p][offset++];
          hs.block_invalid_vram_access = recv_data[p][offset++];
          hs.separate_echo_buffer = recv_data[p][offset++];
          offset++; // reserved (formerly allow_all_players_state_load)
          hs.superfx_clock_multiplier = READ_DWORD(&recv_data[p][offset]);
          offset += 4;
          hs.interpolation_method = (int32)READ_DWORD(&recv_data[p][offset]);
          offset += 4;
          hs.overclock_mode = (int32)READ_DWORD(&recv_data[p][offset]);
          offset += 4;
          hs.one_clock_cycle = (int32)READ_DWORD(&recv_data[p][offset]);
          offset += 4;
          hs.one_slow_clock_cycle = (int32)READ_DWORD(&recv_data[p][offset]);
          offset += 4;
          hs.two_clock_cycles = (int32)READ_DWORD(&recv_data[p][offset]);
          offset += 4;
          hs.max_sprite_tiles_per_line =
              (int32)READ_DWORD(&recv_data[p][offset]);
          offset += 4;
          apply_hack_settings(hs);
          rt.hacks_known = true;
          offset = hack_base + 34; // Ensure perfectly aligned advancement past
                                   // hack padding block
        } else {
          offset += 34; // skip hacks
        }

        uint8 nlen = recv_data[p][offset++];
        if (offset + nlen <= recv_data[p].size()) {
          memcpy(rt.player_names[p], &recv_data[p][offset], nlen);
          rt.player_names[p][nlen] = '\0';
          rt.player_name_known[p] = true;
          offset += nlen;
        }

        if (p == 0 && rt.startup_sync && !rt.authoritative) {
          if (rt.startup_flags == SF_CLEARRAM) {
            Memory.ClearSRAM(FALSE);
            CPU.SRAMModified = FALSE;
          } else if (rt.startup_flags == SF_SRAM) {
            size_t rem_len = recv_data[p].size() - offset;
            if (rem_len >= 8) {
              uint32 raw_size = READ_DWORD(&recv_data[p][offset]);
              uint32 zlib_len = READ_DWORD(&recv_data[p][offset + 4]);
              offset += 8;
              if (raw_size > 0 && raw_size <= 0x80000 &&
                  zlib_len <= rem_len - 8) {
                size_t game_sz = get_game_sram_size();
                size_t copy_sz = std::min((size_t)raw_size, game_sz);
                if (copy_sz) {
                  std::vector<uint8> raw(raw_size);
                  uLong rl = raw_size;
                  if (uncompress(&raw[0], &rl, &recv_data[p][offset],
                                 (uLong)zlib_len) == Z_OK) {
                    memcpy(Memory.SRAM, &raw[0], std::min((size_t)rl, copy_sz));
                    post_status("Kaillera: received %u bytes of SRAM from P1.",
                                (uint32)copy_sz);
                  } else {
                    post_status("Kaillera: SRAM zlib decompress failed.");
                  }
                }
                if (copy_sz < game_sz)
                  memset(Memory.SRAM + copy_sz, 0, game_sz - copy_sz);
                CPU.SRAMModified = FALSE;
                rt.received_remote_sram = true;
              }
            }
          }
        }
      }
    }

    fill_missing_names();
    if (rt.authoritative && rt.startup_flags == SF_CLEARRAM &&
        rt.startup_sync) {
      Memory.ClearSRAM(FALSE);
      CPU.SRAMModified = FALSE;
    }
    rt.startup_sync = false;
    rt.suppress_emulation = false;
  } else {
    Memory.ClearSRAM(FALSE);
    CPU.SRAMModified = FALSE;
    rt.startup_sync = false;
    rt.suppress_emulation = false;
  }

  S9xReset();
  rt.pending_auto_movie = 1;

  /* Update window title */
  TCHAR title[256];
  _stprintf(title, TEXT("%S - Kaillera P%d"), k_app_name, player);
  SetWindowText(GUI.hWnd, title);

  post_status(KAILLERA_GAME_STARTING);

  /* Run the game loop (blocks until session ends) */
  run_kaillera_game_loop();

  /* Must notify Kaillera even if rt.active was cleared inside the loop;
   * otherwise the client stays out of sync and the next session can hang. */
  if (api.endGame)
    api.endGame();
  if (GUI.ControllerOption != previous_controller_option) {
    GUI.ControllerOption = previous_controller_option;
    ChangeInputDevice();
  }
  rt.reset_session();
  if (KailleraConfig.AutoStopMovieOnEnd)
    stop_active_movie();
  clear_state_transfer_title();
  S9xRestoreWindowTitle();
  post_status(KAILLERA_GAME_ENDED);
  return 0;
}

/* --------------------------------------------------------------------------
 * DLL loading
 * -------------------------------------------------------------------------- */
static bool load_dll(void) {
  if (api.dll)
    return true;

  api.dll = LoadLibrary(TEXT("kailleraclient.dll"));
  if (!api.dll)
    return false;

  /* On x86, __stdcall exports are decorated: _kailleraInit@0, etc.
   * On x64, there is no __stdcall decoration: exports are just kailleraInit.
   * Try the decorated name first (x86), then fall back to undecorated (x64). */
#define RESOLVE(member, decorated, undecorated)                                \
  api.member = (decltype(api.member))GetProcAddress(api.dll, decorated);       \
  if (!api.member)                                                             \
  api.member = (decltype(api.member))GetProcAddress(api.dll, undecorated)
  RESOLVE(getVersion, "_kailleraGetVersion@4", "kailleraGetVersion");
  RESOLVE(init, "_kailleraInit@0", "kailleraInit");
  RESOLVE(shutdown, "_kailleraShutdown@0", "kailleraShutdown");
  RESOLVE(setInfos, "_kailleraSetInfos@4", "kailleraSetInfos");
  RESOLVE(selectServerDialog, "_kailleraSelectServerDialog@4",
          "kailleraSelectServerDialog");
  RESOLVE(modifyPlayValues, "_kailleraModifyPlayValues@8",
          "kailleraModifyPlayValues");
  RESOLVE(chatSend, "_kailleraChatSend@4", "kailleraChatSend");
  RESOLVE(endGame, "_kailleraEndGame@0", "kailleraEndGame");
#undef RESOLVE

  if (!api.init || !api.shutdown || !api.setInfos || !api.selectServerDialog ||
      !api.modifyPlayValues || !api.chatSend || !api.endGame) {
    FreeLibrary(api.dll);
    memset(&api, 0, sizeof(api));
    return false;
  }
  return true;
}

/* --------------------------------------------------------------------------
 * Public API — Init / Shutdown / Status
 * -------------------------------------------------------------------------- */
bool S9xKailleraInit(void) {
  if (!load_dll()) {
    rt.cache_ready = false;
    return false;
  }

  if (!api.initialised) {
    api.init();
    api.initialised = true;
  }

  ensure_game_cache();

  rt.infos.appName = const_cast<char *>(k_app_name);
  rt.infos.gameCallback = game_callback;
  rt.infos.chatReceivedCallback = chat_callback;
  rt.infos.clientDroppedCallback = drop_callback;
  rt.infos.moreInfosCallback = infos_callback;
  api.setInfos(&rt.infos);
  return true;
}

void S9xKailleraShowClient(HWND parent) {
  if (!S9xKailleraInit()) {
    MessageBox(parent ? parent : GUI.hWnd,
               TEXT("Could not load kailleraclient.dll."), TEXT("Kaillera"),
               MB_OK | MB_ICONERROR);
    return;
  }

  if (api.selectServerDialog) {
    s_client_dialog_open = true;
    api.selectServerDialog(parent ? parent : GUI.hWnd);
    s_client_dialog_open = false;
  }
}

void S9xKailleraShutdown(void) {
  S9xKailleraNotifyExit();
  if (api.initialised && api.shutdown) {
    api.shutdown();
    api.initialised = false;
  }
  if (api.dll) {
    FreeLibrary(api.dll);
    memset(&api, 0, sizeof(api));
  }
}

bool S9xKailleraIsAvailable(void) { return api.dll != NULL; }
bool S9xKailleraIsActive(void) { return rt.active; }
bool S9xKailleraIsClientOpen(void) { return s_client_dialog_open || rt.active; }
bool S9xKailleraSuppressEmulation(void) {
  return rt.active && rt.suppress_emulation;
}

bool S9xKailleraCanLoadState(void) {
  if (!rt.active)
    return true;
  if (rt.local_player == 1)
    return true;
  post_status("Kaillera: only player 1 can load savestates in this game.");
  return false;
}

bool S9xKailleraHasReceivedRemoteSRAM(void) { return rt.received_remote_sram; }

/* --------------------------------------------------------------------------
 * S9xKailleraProcessInputs — called each frame from the game loop
 * -------------------------------------------------------------------------- */
void S9xKailleraProcessInputs(uint32 *joypads_io, int count) {
  if (!rt.active || !api.modifyPlayValues)
    return;

  /* Determine which local joypad index to submit */
  int lidx = GUI.NetplayUseJoypad1 ? 0 : std::max(0, rt.local_player - 1);
  uint32 local_joypad = (lidx >= 0 && lidx < count) ? joypads_io[lidx] : 0;

  /* Build our payload */
  uint8 frame[8 * k_payload_size];
  memset(frame, 0, sizeof(frame));
  build_local_payload(frame, local_joypad);

  /* Progress display during data transfer */
  if (rt.suppress_emulation) {
    if (rt.chat_sram_active && rt.chat_sram_sender && rt.chat_sram_is_state)
      show_status(KAILLERA_SENDING_STATE);
    else if (rt.chat_sram_active && rt.chat_sram_sender)
      show_status(KAILLERA_SENDING_SRAM);
    else if (rt.in_state_total > 0)
      show_status(KAILLERA_RECEIVING_STATE,
                  (int)(rt.in_state_received * 100 /
                        std::max((size_t)1, (size_t)rt.in_state_total)));
    else if (rt.in_sram_total > 0)
      show_status(KAILLERA_RECEIVING_SRAM,
                  (int)(rt.in_sram_received * 100 /
                        std::max((size_t)1, (size_t)rt.in_sram_total)));
  }

  /* Exchange values with the Kaillera library */
  int received = api.modifyPlayValues(frame, k_payload_size);
  if (received < 0) {
    rt.active = false; /* SDK: -1 = error / no longer in game */
    return;
  }

  int players = std::min(received / k_payload_size, 8);
  if (players > 0)
    rt.num_players = players;

  /* Zero out all pads, then fill from received payloads */
  for (int i = 0; i < count; i++)
    joypads_io[i] = 0;

  for (int i = 0; i < players; i++) {
    const uint8 *pl = frame + i * k_payload_size;
    if (i < count)
      joypads_io[i] = (READ_DWORD(pl + 0) & ~k_payload_sync_mask) | 0x80000000u;
    consume_player_payload(i, pl);
  }

  finalize_startup();
  finalize_runtime_sync();
}

/* --------------------------------------------------------------------------
 * Notify hooks called from the rest of the Win32 port
 * -------------------------------------------------------------------------- */
void S9xKailleraPrepareStateLoad(void) {
  if (!rt.active)
    return;
  stop_active_movie();
}

void S9xKailleraNotifyStateLoaded(const char *filename) {
  if (!rt.active || rt.local_player < 1 || rt.local_player > 8 ||
      !rt.authoritative)
    return;

  /* Try reading the .frz file directly from disk.
   * Snes9x writes savestates through gzopen, so .frz files are
   * already gzip-compressed — we can send them as-is and avoid
   * the expensive freeze-to-memory + recompression cycle.       */
  std::vector<uint8> state_data;
  if (filename && filename[0])
    state_data = read_file_bytes(filename);

  if (!state_data.empty() && is_gzip(state_data)) {
    /* File is already gzip-compressed — use as-is */
  } else {
    /* File not available or not gzipped — freeze to memory and compress */
    uint32 raw_size = S9xFreezeSize();
    std::vector<uint8> raw(raw_size);
    S9xFreezeGameMem(&raw[0], raw_size);
    state_data = compress_state(&raw[0], raw_size);
    if (state_data.empty())
      return;
  }

  rt.pending_pv_state = std::move(state_data);
  rt.pv_state_ready = false;
  rt.runtime_state_sync = true;
  rt.runtime_sync_player = rt.local_player;
  rt.remote_sync_flags = 0;
  rt.runtime_release_pending = false;
  rt.suppress_emulation = true;
  stop_active_movie();
  update_state_transfer_title(true, 0, rt.pending_pv_state.size());
  post_status(KAILLERA_SENDING_STATE);
}

void S9xKailleraNotifySramSaved(void) {
  if (!rt.active || !rt.authoritative || !KailleraConfig.TransferSRAM ||
      rt.chat_sram_active)
    return;

  size_t sram_sz = get_game_sram_size();
  if (!sram_sz)
    return; /* game has no SRAM — nothing to send */
  auto comp = compress_data(Memory.SRAM, (uint32)sram_sz);
  if (comp.empty())
    return;
  if (!begin_chat_transfer(std::move(comp), false, false))
    return;
  post_status(KAILLERA_SENDING_SRAM);
}

void S9xKailleraNotifyBeforeRomLoad(void) {
  if (!rt.active)
    return;
  if (api.endGame)
    api.endGame();
  rt.reset_session();
  if (KailleraConfig.AutoStopMovieOnEnd)
    stop_active_movie();
  post_status(KAILLERA_GAME_CLOSED);
  clear_state_transfer_title();
}

void S9xKailleraNotifyExit(void) {
  if (!rt.active)
    return;
  if (api.endGame)
    api.endGame();
  rt.reset_session();
  if (KailleraConfig.AutoStopMovieOnEnd)
    stop_active_movie();
  clear_state_transfer_title();
}

bool S9xKailleraOpenChat(bool swallow_char) {
  if (!rt.active || !api.chatSend)
    return false;

  rt.chat_input_active = true;
  rt.chat_input_text.clear();
  rt.chat_input_swallow_until_tick = swallow_char ? 1 : 0;
  refresh_chat_input_osd();
  return true;
}

bool S9xKailleraWantsKeyboardCapture(void) {
  return rt.active && rt.chat_input_active;
}

bool S9xKailleraHandleKeyboardMessage(UINT msg, WPARAM wParam,
                                      LPARAM /*lParam*/) {
  if (!rt.active || !rt.chat_input_active)
    return false;

  switch (msg) {
  case WM_KEYDOWN:
  case WM_SYSKEYDOWN:
    if (wParam == VK_RETURN) {
      std::string message = str_trim(rt.chat_input_text);
      close_chat_input();
      if (!message.empty() && api.chatSend &&
          api.chatSend(const_cast<char *>(message.c_str())) < 0)
        post_status("Kaillera: chat send failed.");
      return true;
    }
    if (wParam == VK_ESCAPE) {
      close_chat_input();
      return true;
    }
    if (wParam == VK_BACK) {
      backspace_chat_input();
      refresh_chat_input_osd();
      return true;
    }
    if (wParam == VK_DELETE) {
      rt.chat_input_text.clear();
      refresh_chat_input_osd();
      return true;
    }
    return true;

  case WM_KEYUP:
  case WM_SYSKEYUP:
    return true;

  case WM_CHAR:
  case WM_SYSCHAR:
    if (rt.chat_input_swallow_until_tick != 0) {
      rt.chat_input_swallow_until_tick = 0;
      return true;
    }
    rt.chat_input_swallow_until_tick = 0;
    if (wParam == '\r' || wParam == '\b' || wParam == 27)
      return true;
    if (append_chat_input_char(wParam))
      refresh_chat_input_osd();
    return true;
  }

  return false;
}

/* --------------------------------------------------------------------------
 * UI message handler — called from WinProc for WM_KAILLERA_* messages
 * -------------------------------------------------------------------------- */
LRESULT S9xKailleraHandleUiMessage(UINT msg, WPARAM /*wParam*/, LPARAM lParam) {
  if (msg == WM_KAILLERA_STATUS || msg == WM_KAILLERA_CHAT) {
    char *text = (char *)lParam;
    if (text) {
      S9xSetInfoString(text);
      free(text);
    }
    return 0;
  }
  return 0;
}

/* --------------------------------------------------------------------------
 * Options dialog
 * -------------------------------------------------------------------------- */
INT_PTR CALLBACK DlgKailleraOptions(HWND hDlg, UINT msg, WPARAM wParam,
                                    LPARAM /*lParam*/) {
  switch (msg) {
  case WM_INITDIALOG:
    WinRefreshDisplay();
    SendDlgItemMessage(
        hDlg, IDC_KAILLERA_TRANSFER_SRAM, BM_SETCHECK,
        KailleraConfig.TransferSRAM ? BST_CHECKED : BST_UNCHECKED, 0);
    SendDlgItemMessage(
        hDlg, IDC_KAILLERA_CHAT_OSD, BM_SETCHECK,
        KailleraConfig.ShowChatInOSD ? BST_CHECKED : BST_UNCHECKED, 0);
    SendDlgItemMessage(
        hDlg, IDC_KAILLERA_AUTO_MOVIE, BM_SETCHECK,
        KailleraConfig.AutoRecordMovie ? BST_CHECKED : BST_UNCHECKED, 0);
    SendDlgItemMessage(
        hDlg, IDC_KAILLERA_STOP_MOVIE, BM_SETCHECK,
        KailleraConfig.AutoStopMovieOnEnd ? BST_CHECKED : BST_UNCHECKED, 0);
    SendDlgItemMessage(
        hDlg, IDC_KAILLERA_NEVER_BLOCK_SRAM, BM_SETCHECK,
        KailleraConfig.NeverBlockSRAMSave ? BST_CHECKED : BST_UNCHECKED, 0);
    SendDlgItemMessage(hDlg, IDC_KAILLERA_INCREMENTAL_CACHE_REFRESH,
                       BM_SETCHECK,
                       KailleraConfig.IncrementalCacheRefresh ? BST_CHECKED
                                                              : BST_UNCHECKED,
                       0);
    SendDlgItemMessage(
        hDlg, IDC_KAILLERA_CLIENT_COMPAT, BM_SETCHECK,
        KailleraConfig.CaseSensitiveGameList ? BST_CHECKED : BST_UNCHECKED, 0);
    return TRUE;

  case WM_COMMAND:
    switch (LOWORD(wParam)) {
    case IDOK:
      KailleraConfig.TransferSRAM =
          IsDlgButtonChecked(hDlg, IDC_KAILLERA_TRANSFER_SRAM) == BST_CHECKED;
      KailleraConfig.ShowChatInOSD =
          IsDlgButtonChecked(hDlg, IDC_KAILLERA_CHAT_OSD) == BST_CHECKED;
      KailleraConfig.AutoRecordMovie =
          IsDlgButtonChecked(hDlg, IDC_KAILLERA_AUTO_MOVIE) == BST_CHECKED;
      KailleraConfig.AutoStopMovieOnEnd =
          IsDlgButtonChecked(hDlg, IDC_KAILLERA_STOP_MOVIE) == BST_CHECKED;
      KailleraConfig.NeverBlockSRAMSave =
          IsDlgButtonChecked(hDlg, IDC_KAILLERA_NEVER_BLOCK_SRAM) ==
          BST_CHECKED;
      KailleraConfig.IncrementalCacheRefresh =
          IsDlgButtonChecked(hDlg, IDC_KAILLERA_INCREMENTAL_CACHE_REFRESH) ==
          BST_CHECKED;
      KailleraConfig.CaseSensitiveGameList =
          IsDlgButtonChecked(hDlg, IDC_KAILLERA_CLIENT_COMPAT) == BST_CHECKED;
      WinSaveConfigFile();
      EndDialog(hDlg, 1);
      return TRUE;
    case IDCANCEL:
      EndDialog(hDlg, 0);
      return TRUE;
    }
    break;
  }
  return FALSE;
}