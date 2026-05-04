/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifdef NETPLAY_SUPPORT

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <memory.h>
#include <sys/types.h>

#include "snes9x.h"
#include "controls.h"

#ifdef __WIN32__
#include <winsock.h>
#include <process.h>
#include "win32/wsnes9x.h"
#include "win32/kaillera.h"

#define ioctl ioctlsocket
#define close(h)                                                               \
  if (h) {                                                                     \
    closesocket(h);                                                            \
  }
#define read(a, b, c) recv(a, b, c, 0)
#define write(a, b, c) send(a, b, c, 0)
#else
#include <unistd.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <netdb.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/param.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#ifdef __SVR4
#include <sys/stropts.h>
#endif
#endif

#ifdef USE_THREADS
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#endif

#include "memmap.h"
#include "netplay.h"
#include "snapshot.h"
#include "display.h"
#include "movie.h"
#include <string>
#include <vector>

void S9xNPClientLoop(void *);
bool8 S9xNPLoadROM(uint32 len);
bool8 S9xNPLoadROMDialog(const char *);
bool8 S9xNPGetROMImage(uint32 len);
bool8 S9xNPGetSRAMData(uint32 len);
void S9xNPGetFreezeFile(uint32 len);
static const char *S9xNPClientDisplayName(int player);
static void S9xNPFormatChatLine(char *buffer, size_t size, int player,
                                const char *message);

#ifdef __WIN32__
static void S9xNPCloseChatInput(void);
static void S9xNPRefreshChatInputOSD(void);
static void S9xNPBackspaceChatInput(void);
static bool S9xNPAppendChatInputChar(WPARAM wParam);
static bool S9xNPMessageHasContent(const std::string &message);

static int S9xNPClientPlayerCount(void) {
  int count = 0;

  for (int i = 0; i < NP_MAX_CLIENTS; i++) {
    if (NetPlay.ClientNames[i][0])
      count++;
  }

  return count;
}

static uint8 S9xNPClientControllerMask(void) {
  uint8 mask = 0;

  for (int i = 0; i < NP_MAX_CLIENTS; i++) {
    if (NetPlay.ClientNames[i][0])
      mask |= (uint8)(1u << i);
  }

  return mask;
}

static std::string S9xNPClientMovieMetadataText(void) {
  std::string metadata = "Netplay players: ";
  bool first = true;

  for (int i = 0; i < NP_MAX_CLIENTS; i++) {
    if (!NetPlay.ClientNames[i][0])
      continue;

    if (!first)
      metadata += ", ";
    metadata += NetPlay.ClientNames[i];
    first = false;
  }

  if (first)
    metadata += S9xNPClientDisplayName(NetPlay.Player ? NetPlay.Player : 1);

  return metadata;
}

static void S9xNPApplyHackSettingsFromServer(
    uint32 superfx_clock_multiplier, int overclock_mode,
    int interpolation_method, bool8 block_invalid_vram_access_master,
    bool8 separate_echo_buffer, int max_sprite_tiles_per_line) {
  Settings.SuperFXClockMultiplier = superfx_clock_multiplier;
  Settings.OverclockMode = overclock_mode;
  Settings.InterpolationMethod = interpolation_method;
  Settings.BlockInvalidVRAMAccessMaster = block_invalid_vram_access_master;
  Settings.SeparateEchoBuffer = separate_echo_buffer;
  Settings.MaxSpriteTilesPerLine = max_sprite_tiles_per_line;

  switch (Settings.OverclockMode) {
  default:
  case 0:
    Settings.OneClockCycle = 6;
    Settings.OneSlowClockCycle = 8;
    Settings.TwoClockCycles = 12;
    break;
  case 1:
    Settings.OneClockCycle = 6;
    Settings.OneSlowClockCycle = 6;
    Settings.TwoClockCycles = 12;
    break;
  case 2:
    Settings.OneClockCycle = 4;
    Settings.OneSlowClockCycle = 6;
    Settings.TwoClockCycles = 8;
    break;
  case 3:
    Settings.OneClockCycle = 3;
    Settings.OneSlowClockCycle = 4;
    Settings.TwoClockCycles = 6;
    break;
  }
}

static std::wstring S9xNPWideFromAnsi(const std::string &text) {
  if (text.empty())
    return std::wstring();

  int wide_length = MultiByteToWideChar(CP_ACP, 0, text.c_str(), -1, NULL, 0);
  if (wide_length <= 0)
    return std::wstring(text.begin(), text.end());

  std::wstring wide((size_t)wide_length, L'\0');
  MultiByteToWideChar(CP_ACP, 0, text.c_str(), -1, &wide[0], wide_length);
  if (!wide.empty() && wide.back() == L'\0')
    wide.pop_back();
  return wide;
}

static bool S9xNPHasLoadedROM(void) {
  return Memory.ROM != NULL && Memory.CalculatedSize > 0;
}

static bool S9xNPShouldAutoRecordMovie(void) {
  return Settings.NetPlay && NetPlay.Connected && S9xNPHasLoadedROM() &&
         KailleraConfig.AutoRecordMovie && S9xNPClientPlayerCount() >= 2;
}

static void S9xNPStopMovie(void) {
  if (S9xMovieActive()) {
    PostMessage(GUI.hWnd, WM_NETPLAY_STOP_MOVIE, 0, 0);
    Sleep(0);
  }
}

static void S9xNPStartAutoMovie(uint8 opts) {
  if (!S9xNPShouldAutoRecordMovie())
    return;

  uint8 controller_mask = S9xNPClientControllerMask();
  if (!controller_mask)
    return;

  std::wstring metadata = S9xNPWideFromAnsi(S9xNPClientMovieMetadataText());
  WinStartAutoMovieRecordingWithMetadata(
      controller_mask, opts, metadata.c_str(), (int)metadata.size());
}
#endif

unsigned long START = 0;

static bool8 S9xNPSendFreezeFileToServer(void) {
  uint32 freeze_size = S9xFreezeSize();
  if (freeze_size == 0)
    return FALSE;

  std::vector<uint8> freeze_data(freeze_size);
  if (!S9xFreezeGameMem(&freeze_data[0], freeze_size))
    return FALSE;

  int len = 7 + (int)freeze_size;
  std::vector<uint8> packet((size_t)len);
  uint8 *ptr = &packet[0];
  *ptr++ = NP_CLNT_MAGIC;
  *ptr++ = NetPlay.MySequenceNum++;
  *ptr++ = NP_CLNT_FREEZE_FILE;
  WRITE_LONG(ptr, len);
  ptr += 4;
  memcpy(ptr, &freeze_data[0], freeze_size);

  if (!S9xNPSendData(NetPlay.Socket, &packet[0], len)) {
    S9xNPSetError("Sending 'FREEZE_FILE' message failed.");
    S9xNPDisconnect();
    return FALSE;
  }

  return TRUE;
}

bool8 S9xNPCanLoadState(void) {
#ifdef __WIN32__
  if (Settings.NetPlay && NetPlay.Connected && !Settings.NetPlayServer) {
    // Only Player 1 can load states in TCP netplay
    if (NetPlay.Player != 1) {
      S9xNPSetWarning("Netplay: Only the server can load savestates.");
      return FALSE;
    }
    return FALSE;
  }
#endif

  return TRUE;
}

void S9xNPPrepareStateLoad(void) {
#ifdef __WIN32__
  if (Settings.NetPlay && NetPlay.Connected)
    S9xNPStopMovie();
#endif
}

void S9xNPNotifyStateLoaded(const char *filename) {
  if (!Settings.NetPlay || !NetPlay.Connected)
    return;

  if (Settings.NetPlayServer) {
    S9xNPServerQueueSendingFreezeFile(filename);
    return;
  }

  // Only Player 1 can send savestates in TCP netplay
  if (NetPlay.Player != 1)
    return;

  if (!S9xNPCanLoadState())
    return;

  if (!S9xNPSendFreezeFileToServer())
    S9xSetInfoString("Netplay: savestate send failed.");
}

bool8 S9xNPConnect();

bool8 S9xNPConnectToServer(const char *hostname, int port,
                           const char *rom_name) {
  if (!S9xNPInitialise())
    return (FALSE);

  S9xNPDisconnect();

  NetPlay.MySequenceNum = 0;
  NetPlay.ServerSequenceNum = 0;
  NetPlay.Connected = FALSE;
  NetPlay.Abort = FALSE;
  NetPlay.Player = 0;
  NetPlay.Paused = FALSE;
  NetPlay.PercentageComplete = 0;
  NetPlay.Socket = 0;
  if (NetPlay.ServerHostName)
    free((char *)NetPlay.ServerHostName);
  NetPlay.ServerHostName = strdup(hostname);
  if (NetPlay.ROMName)
    free((char *)NetPlay.ROMName);
  NetPlay.ROMName = strdup(rom_name);
  NetPlay.Port = port;
  NetPlay.PendingWait4Sync = FALSE;
  memset((void *)NetPlay.ClientNames, 0, sizeof(NetPlay.ClientNames));

#ifdef __WIN32__
  if (GUI.ClientSemaphore == NULL)
    GUI.ClientSemaphore = CreateSemaphore(NULL, 0, NP_JOYPAD_HIST_SIZE, NULL);

  if (NetPlay.ReplyEvent == NULL)
    NetPlay.ReplyEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

  _beginthread(S9xNPClientLoop, 0, NULL);

  return (TRUE);
#endif

  return S9xNPConnect();
}

bool8 S9xNPConnect() {
  struct sockaddr_in address;
  struct hostent *hostinfo;
  unsigned int addr;

  address.sin_family = AF_INET;
  address.sin_port = htons(NetPlay.Port);
#ifdef NP_DEBUG
  printf("CLIENT: Looking up server's hostname (%s) @%ld\n",
         NetPlay.ServerHostName, S9xGetMilliTime() - START);
#endif
  S9xNPSetAction("Looking up server's hostname...");
  if ((int)(addr = inet_addr(NetPlay.ServerHostName)) == -1) {
    if ((hostinfo = gethostbyname(NetPlay.ServerHostName))) {
      memcpy((char *)&address.sin_addr, hostinfo->h_addr, hostinfo->h_length);
    } else {
      S9xNPSetError("\
Unable to look up server's IP address from hostname.\n\n\
Unknown hostname or may be your nameserver isn't set\n\
up correctly?");
      return (FALSE);
    }
  } else {
    memcpy((char *)&address.sin_addr, &addr, sizeof(addr));
  }

#ifdef NP_DEBUG
  printf("CLIENT: Creating socket @%ld\n", S9xGetMilliTime() - START);
#endif
  S9xNPSetAction("Creating network socket...");
  if ((NetPlay.Socket = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    S9xNPSetError("Creating network socket failed.");
    return (FALSE);
  }

#ifdef NP_DEBUG
  printf("CLIENT: Trying to connect to server @%ld...\n",
         S9xGetMilliTime() - START);
#endif
  S9xNPSetAction("Trying to connect to Snes9x server...");

  if (connect(NetPlay.Socket, (struct sockaddr *)&address, sizeof(address)) <
      0) {
    char buf[100];
#ifdef __WIN32__
    if (WSAGetLastError() == WSAECONNREFUSED)
#else
    if (errno == ECONNREFUSED)
#endif
    {
      S9xNPSetError("\
Connection to remote server socket refused:\n\n\
Is there actually a Snes9x Netplay server running\n\
on the remote machine on this port?");
    } else {
      sprintf(buf, "Connection to server failed with error number %d",
#ifdef __WIN32__
              WSAGetLastError()
#else
              errno
#endif
      );
      S9xNPSetError(buf);
      S9xNPDisconnect();
    }
    return (FALSE);
  }
  NetPlay.Connected = TRUE;

#ifdef NP_DEBUG
  printf("CLIENT: Sending 'HELLO' message @%ld...\n",
         S9xGetMilliTime() - START);
#endif
  S9xNPSetAction("Sending 'HELLO' message...");
  /* Send the server a HELLO packet*/
  const char *local_username =
      Settings.NetplayUsername[0] ? Settings.NetplayUsername : "Player";
  int len = 7 + 4 + (int)strlen(local_username) + 1 +
            (int)strlen(NetPlay.ROMName) + 1;
  uint8 *tmp = new uint8[len];
  uint8 *ptr = tmp;

  *ptr++ = NP_CLNT_MAGIC;
  *ptr++ = NetPlay.MySequenceNum++;
  *ptr++ = NP_CLNT_HELLO;
  WRITE_LONG(ptr, len);
  ptr += 4;
#ifdef __WIN32__
  uint32 ft = Settings.FrameTime;

  WRITE_LONG(ptr, ft);
#else
  WRITE_LONG(ptr, Settings.FrameTime);
#endif
  ptr += 4;
  strcpy((char *)ptr, local_username);
  ptr += strlen(local_username) + 1;
  strcpy((char *)ptr, NetPlay.ROMName);

  if (!S9xNPSendData(NetPlay.Socket, tmp, len)) {
    S9xNPSetError("Sending 'HELLO' message failed.");
    S9xNPDisconnect();
    delete[] tmp;
    return (FALSE);
  }
  delete[] tmp;

#ifdef NP_DEBUG
  printf("CLIENT: Waiting for 'WELCOME' reply from server @%ld...\n",
         S9xGetMilliTime() - START);
#endif
  S9xNPSetAction("Waiting for 'HELLO' reply from server...");

  uint8 header[7];

  if (!S9xNPGetData(NetPlay.Socket, header, 7) || header[0] != NP_SERV_MAGIC ||
      header[1] != 0 || (header[2] & 0x1f) != NP_SERV_HELLO) {
    S9xNPSetError("Error in 'HELLO' reply packet received from server.");
    S9xNPDisconnect();
    return (FALSE);
  }
#ifdef NP_DEBUG
  printf("CLIENT: Got 'WELCOME' reply @%ld\n", S9xGetMilliTime() - START);
#endif
  len = READ_LONG(&header[3]);
  if (len > 512 || len < 7 + 1 + 1 + 4 + 1 + 1) {
    S9xNPSetError("Error in 'HELLO' reply packet received from server.");
    S9xNPDisconnect();
    return (FALSE);
  }
  uint8 *data = new uint8[len - 7];
  if (!S9xNPGetData(NetPlay.Socket, data, len - 7)) {
    S9xNPSetError("Error in 'HELLO' reply packet received from server.");
    delete[] data;
    S9xNPDisconnect();
    return (FALSE);
  }

  if (data[0] != NP_VERSION) {
    S9xNPSetError("\
The Snes9x Netplay server implements a different\n\
version of the protocol. Disconnecting.");
    delete[] data;
    S9xNPDisconnect();
    return (FALSE);
  }

  NetPlay.FrameCount = READ_LONG(&data[2]);

  char *rom_name = (char *)data + 6;
  uint32 remaining = len - 7 - 6;
  char *rom_name_end = (char *)memchr(rom_name, 0, remaining);

  if (!rom_name_end) {
    S9xNPSetError("Error in 'HELLO' reply packet received from server.");
    delete[] data;
    S9xNPDisconnect();
    return (FALSE);
  }

  char *server_username = rom_name_end + 1;
  remaining -= (uint32)(rom_name_end - rom_name + 1);
  if (!memchr(server_username, 0, remaining)) {
    S9xNPSetError("Error in 'HELLO' reply packet received from server.");
    delete[] data;
    S9xNPDisconnect();
    return (FALSE);
  }

  if (!(header[2] & 0x80) && strcmp(rom_name, NetPlay.ROMName) != 0) {
    if (!S9xNPLoadROMDialog(rom_name)) {
      delete[] data;
      S9xNPDisconnect();
      return (FALSE);
    }
  }
  NetPlay.Player = data[1];
  if (NetPlay.Player < 1 || NetPlay.Player > NP_MAX_CLIENTS) {
    S9xNPSetError("Error in 'HELLO' reply packet received from server.");
    delete[] data;
    S9xNPDisconnect();
    return (FALSE);
  }
  strncpy(NetPlay.ClientNames[NetPlay.Player - 1], local_username,
          sizeof(NetPlay.ClientNames[0]) - 1);
  NetPlay.ClientNames[NetPlay.Player - 1][sizeof(NetPlay.ClientNames[0]) - 1] =
      0;
  if (NetPlay.Player > 1) {
    /* Non-P1 clients receive the server's SRAM; block disk saves so the
     * player's own save file is not overwritten by the session data. */
    GUI.BlockSRAMSave = !KailleraConfig.NeverBlockSRAMSave;
    strncpy(NetPlay.ClientNames[0], server_username,
            sizeof(NetPlay.ClientNames[0]) - 1);
    NetPlay.ClientNames[0][sizeof(NetPlay.ClientNames[0]) - 1] = 0;
  } else {
    /* P1 (server) uses their own SRAM; clear any block left from a prior
     * session. */
    GUI.BlockSRAMSave = false;
  }
  delete[] data;

  NetPlay.PendingWait4Sync = TRUE;
  Settings.NetPlay = TRUE;
  S9xNPResetJoypadReadPos();
  NetPlay.ServerSequenceNum = 1;

#ifdef NP_DEBUG
  printf("CLIENT: Sending 'READY' to server @%ld...\n",
         S9xGetMilliTime() - START);
#endif
  S9xNPSetAction("Sending 'READY' to the server...");

  return (S9xNPSendReady((header[2] & 0x80) ? NP_CLNT_WAITING_FOR_ROM_IMAGE
                                            : NP_CLNT_READY));
}

bool8 S9xNPSendReady(uint8 op) {
  uint8 ready[7];
  uint8 *ptr = ready;
  *ptr++ = NP_CLNT_MAGIC;
  *ptr++ = NetPlay.MySequenceNum++;
  *ptr++ = op;
  WRITE_LONG(ptr, 7);
  ptr += 4;

  if (!S9xNPSendData(NetPlay.Socket, ready, 7)) {
    S9xNPDisconnect();
    S9xNPSetError("Sending 'READY' message failed.");
    return (FALSE);
  }

  return (TRUE);
}

bool8 S9xNPSendPause(bool8 paused) {
#ifdef NP_DEBUG
  printf("CLIENT: Pause - %s @%ld\n", paused ? "YES" : "NO",
         S9xGetMilliTime() - START);
#endif
  uint8 pause[7];
  uint8 *ptr = pause;
  *ptr++ = NP_CLNT_MAGIC;
  *ptr++ = NetPlay.MySequenceNum++;
  *ptr++ = NP_CLNT_PAUSE | (paused ? 0x80 : 0);
  WRITE_LONG(ptr, 7);
  ptr += 4;

  if (!S9xNPSendData(NetPlay.Socket, pause, 7)) {
    S9xNPSetError("Sending 'PAUSE' message failed.");
    S9xNPDisconnect();
    return (FALSE);
  }

  return (TRUE);
}

bool8 S9xNPSendChat(const char *message) {
  if (!NetPlay.Connected || !message)
    return FALSE;

  size_t message_len = strlen(message);
  if (message_len > NP_MAX_CHAT_MESSAGE_LEN)
    message_len = NP_MAX_CHAT_MESSAGE_LEN;

  int len = 7 + (int)message_len + 1;
  uint8 *data = new uint8[len];
  uint8 *ptr = data;
  *ptr++ = NP_CLNT_MAGIC;
  *ptr++ = NetPlay.MySequenceNum++;
  *ptr++ = NP_CLNT_CHAT;
  WRITE_LONG(ptr, len);
  ptr += 4;
  memcpy(ptr, message, message_len);
  ptr[message_len] = 0;

  if (!S9xNPSendData(NetPlay.Socket, data, len)) {
    delete[] data;
    S9xNPSetError("Sending 'CHAT' message failed.");
    S9xNPDisconnect();
    return FALSE;
  }

  delete[] data;
  return TRUE;
}

bool8 S9xNPSendControllerType(int port, uint8 controller_type) {
  if (!NetPlay.Connected || !Settings.NetPlay)
    return FALSE;

  // Only Player 1 can change controller type in TCP netplay
  if (NetPlay.Player != 1) {
    S9xNPSetWarning("Netplay: Only Player 1 can change controller type.");
    return FALSE;
  }

  int len = 7 + 1 + 1; // header + port + controller_type
  uint8 *data = new uint8[len];
  uint8 *ptr = data;
  *ptr++ = NP_CLNT_MAGIC;
  *ptr++ = NetPlay.MySequenceNum++;
  *ptr++ = NP_CLNT_CONTROLLER_TYPE;
  WRITE_LONG(ptr, len);
  ptr += 4;
  *ptr++ = (uint8)port;
  *ptr++ = controller_type;

  if (!S9xNPSendData(NetPlay.Socket, data, len)) {
    delete[] data;
    S9xNPSetError("Sending 'CONTROLLER_TYPE' message failed.");
    S9xNPDisconnect();
    return FALSE;
  }

  delete[] data;
  return TRUE;
}

#ifdef __WIN32__
void S9xNPClientLoop(void *) {
  NetPlay.Waiting4EmulationThread = FALSE;

  if (S9xNPConnect()) {
    S9xClearPause(PAUSE_NETPLAY_CONNECT);
    while (NetPlay.Connected) {
      if (S9xNPWaitForHeartBeat()) {
        LONG prev;
        if (!ReleaseSemaphore(GUI.ClientSemaphore, 1, &prev)) {
#ifdef NP_DEBUG
          printf("CLIENT: ReleaseSemaphore failed - already hit max count (%d) "
                 "%ld\n",
                 NP_JOYPAD_HIST_SIZE, S9xGetMilliTime() - START);
#endif
          S9xNPSetWarning("Netplay: Client may be out of sync with server.");
        } else {
          if (!NetPlay.Waiting4EmulationThread &&
              prev == (int)NetPlay.MaxBehindFrameCount) {
            NetPlay.Waiting4EmulationThread = TRUE;
            S9xNPSendPause(TRUE);
          }
        }
      } else
        S9xNPDisconnect();
    }
  } else {
    S9xClearPause(PAUSE_NETPLAY_CONNECT);
  }
#ifdef NP_DEBUG
  printf("CLIENT: Client thread exiting @%ld\n", S9xGetMilliTime() - START);
#endif
}
#endif

bool8 S9xNPCheckForHeartBeat(uint32 time_msec) {
  fd_set read_fds;
  struct timeval timeout;
  int res;
  int i;

  int max_fd = NetPlay.Socket;

  FD_ZERO(&read_fds);
  FD_SET(NetPlay.Socket, &read_fds);

  timeout.tv_sec = 0;
  timeout.tv_usec = time_msec * 1000;
  res = select(max_fd + 1, &read_fds, NULL, NULL, &timeout);

  i = (res > 0 && FD_ISSET(NetPlay.Socket, &read_fds));

#if defined(NP_DEBUG) && NP_DEBUG >= 4
  printf("CLIENT: S9xCheckForHeartBeat %s @%ld\n",
         (i ? "successful" : "still waiting"), S9xGetMilliTime() - START);
#endif

  return i;
}

bool8 S9xNPWaitForHeartBeatDelay(uint32 time_msec) {
  if (!S9xNPCheckForHeartBeat(time_msec))
    return FALSE;

  if (!S9xNPWaitForHeartBeat()) {
    S9xNPDisconnect();
    return FALSE;
  }

  return TRUE;
}

bool8 S9xNPWaitForHeartBeat() {
  uint8 header[3 + 4 + 4 * 5];

  while (S9xNPGetData(NetPlay.Socket, header, 3 + 4)) {
    if (header[0] != NP_SERV_MAGIC) {
      S9xNPSetError(
          "Bad magic value from server while waiting for heart-beat message\n");
      S9xNPDisconnect();
      return (FALSE);
    }
    if (header[1] != NetPlay.ServerSequenceNum) {
      char buf[200];
      sprintf(buf,
              "Unexpected message sequence number from server, expected %d, "
              "got %d\n",
              NetPlay.ServerSequenceNum, header[1]);
      S9xNPSetWarning(buf);
      NetPlay.ServerSequenceNum = header[1] + 1;
    } else
      NetPlay.ServerSequenceNum++;

    if ((header[2] & 0x1f) == NP_SERV_JOYPAD) {
      // Top 2 bits + 1 of opcode is joypad data count.
      int num = (header[2] >> 6) + 1;

      if (num) {
        if (!S9xNPGetData(NetPlay.Socket, header + 3 + 4, num * 4)) {
          S9xNPSetError("Error while receiving 'JOYPAD' message.");
          S9xNPDisconnect();
          return (FALSE);
        }
      }
      NetPlay.Frame[NetPlay.JoypadWriteInd] = READ_LONG(&header[3]);

      int i;

      for (i = 0; i < NP_MAX_CLIENTS; i++)
        NetPlay.Joypads[NetPlay.JoypadWriteInd][i] =
            READ_LONG(&header[3 + 4 + i * sizeof(uint32)]);

      for (i = 0; i < NP_MAX_CLIENTS; i++)
        NetPlay.JoypadsReady[NetPlay.JoypadWriteInd][i] = TRUE;

      NetPlay.Paused = (header[2] & 0x20) != 0;

      NetPlay.JoypadWriteInd =
          (NetPlay.JoypadWriteInd + 1) % NP_JOYPAD_HIST_SIZE;

      if (NetPlay.JoypadWriteInd !=
          (NetPlay.JoypadReadInd + 1) % NP_JOYPAD_HIST_SIZE) {
        // printf ("(%d)", (NetPlay.JoypadWriteInd - NetPlay.JoypadReadInd) %
        // NP_JOYPAD_HIST_SIZE); fflush (stdout);
      }
      // printf ("CLIENT: HB: @%d\n", S9xGetMilliTime () - START);
      return (TRUE);
    } else {
      uint32 len = READ_LONG(&header[3]);
      switch (header[2] & 0x1f) {
      case NP_SERV_RESET:
#ifdef NP_DEBUG
        printf("CLIENT: RESET received @%ld\n", S9xGetMilliTime() - START);
#endif
#ifdef __WIN32__
        S9xNPStopMovie();
#endif
        S9xNPDiscardHeartbeats();
        S9xReset();
        NetPlay.FrameCount = READ_LONG(&header[3]);
        S9xNPResetJoypadReadPos();
        S9xNPSendReady();
#ifdef __WIN32__
        S9xNPStartAutoMovie(MOVIE_OPT_FROM_RESET);
#endif
        break;
      case NP_SERV_PAUSE:
        NetPlay.Paused = (header[2] & 0x20) != 0;
        break;
      case NP_SERV_JOYPAD_SWAP:
#ifdef NP_DEBUG
        printf("CLIENT: Joypad Swap received @%ld\n",
               S9xGetMilliTime() - START);
#endif
        S9xApplyCommand(S9xGetCommandT("SwapJoypads"), 1, 1);
        break;
      case NP_SERV_PLAYER_INFO: {
        if (len < 7 + 1 + 1 + 1) {
          S9xNPSetError("Error while receiving 'PLAYER_INFO' message.");
          S9xNPDisconnect();
          return (FALSE);
        }

        uint32 info_len = len - 7;

        uint8 *info = new uint8[info_len];
        if (!S9xNPGetData(NetPlay.Socket, info, info_len)) {
          delete[] info;
          S9xNPSetError("Error while receiving 'PLAYER_INFO' message.");
          S9xNPDisconnect();
          return (FALSE);
        }

        char *name = (char *)&info[2];
        if (!memchr(name, 0, info_len - 2)) {
          delete[] info;
          S9xNPSetError("Error while receiving 'PLAYER_INFO' message.");
          S9xNPDisconnect();
          return (FALSE);
        }

        int player = info[0];
        bool8 joined = info[1] != 0;
        char old_name[sizeof(NetPlay.ClientNames[0])];
        old_name[0] = 0;

        if (player < 1 || player > NP_MAX_CLIENTS) {
          delete[] info;
          S9xNPSetError("Error while receiving 'PLAYER_INFO' message.");
          S9xNPDisconnect();
          return (FALSE);
        }

        if (player >= 1 && player <= NP_MAX_CLIENTS) {
          strncpy(old_name, NetPlay.ClientNames[player - 1],
                  sizeof(old_name) - 1);
          old_name[sizeof(old_name) - 1] = 0;

          if (joined) {
            strncpy(NetPlay.ClientNames[player - 1], name,
                    sizeof(NetPlay.ClientNames[0]) - 1);
            NetPlay
                .ClientNames[player - 1][sizeof(NetPlay.ClientNames[0]) - 1] =
                0;
          } else {
            NetPlay.ClientNames[player - 1][0] = 0;
          }
        }

#ifdef __WIN32__
        // Restart movie recording for existing clients when a new player joins
        // (but not for the joining player themselves, and only if ROM is
        // loaded)
        if (joined && S9xNPHasLoadedROM() && player != NetPlay.Player &&
            NetPlay.ClientNames[player - 1][0] != 0) {
          S9xNPStopMovie();
          S9xNPStartAutoMovie(MOVIE_OPT_FROM_SNAPSHOT);
        }
#endif

        sprintf(NetPlay.WarningMsg,
                joined ? "Netplay: Client %d (%s) joined."
                       : "Netplay: Client %d (%s) left.",
                player, joined ? name : (old_name[0] ? old_name : name));
        S9xNPSetWarning(NetPlay.WarningMsg);
        delete[] info;
        break;
      }
      case NP_SERV_CHAT: {
        if (len < 7 + 1 + 1) {
          S9xNPSetError("Error while receiving 'CHAT' message.");
          S9xNPDisconnect();
          return FALSE;
        }

        uint32 chat_len = len - 7;
        uint8 *chat = new uint8[chat_len];
        if (!S9xNPGetData(NetPlay.Socket, chat, chat_len)) {
          delete[] chat;
          S9xNPSetError("Error while receiving 'CHAT' message.");
          S9xNPDisconnect();
          return FALSE;
        }

        int player = chat[0];
        char *message = (char *)&chat[1];
        if (player < 1 || player > NP_MAX_CLIENTS ||
            !memchr(message, 0, chat_len - 1)) {
          delete[] chat;
          S9xNPSetError("Error while receiving 'CHAT' message.");
          S9xNPDisconnect();
          return FALSE;
        }

        char line[NP_MAX_CHAT_MESSAGE_LEN + sizeof(NetPlay.ClientNames[0]) + 4];
        S9xNPFormatChatLine(line, sizeof(line), player, message);
        S9xNPDisplayChatMessage(line);
        delete[] chat;
        break;
      }
      case NP_SERV_CONTROLLER_TYPE: {
        if (len < 7 + 1 + 1) {
          S9xNPSetError("Error while receiving 'CONTROLLER_TYPE' message.");
          S9xNPDisconnect();
          return FALSE;
        }

        uint32 data_len = len - 7;
        uint8 *data = new uint8[data_len];
        if (!S9xNPGetData(NetPlay.Socket, data, data_len)) {
          delete[] data;
          S9xNPSetError("Error while receiving 'CONTROLLER_TYPE' message.");
          S9xNPDisconnect();
          return FALSE;
        }

        uint8 controller_type = data[1];

#ifdef NP_DEBUG
        printf("CLIENT: CONTROLLER_TYPE received - type %d @%ld\n",
               controller_type, S9xGetMilliTime() - START);
#endif

#ifdef __WIN32__
        // Set the controller option and apply it
        // ControllerOption encodes the configuration for both ports
        extern struct sGUI GUI;
        if (controller_type < SNES_MAX_CONTROLLER_OPTIONS &&
            GUI.ControllerOption != controller_type) {
          GUI.ControllerOption = controller_type;
          extern void ChangeInputDevice(void);
          ChangeInputDevice();
          // Reset joypad read position to ensure clean input state
          S9xNPResetJoypadReadPos();
          // Rotate movie recording to reflect controller change
          S9xNPStopMovie();
          S9xNPStartAutoMovie(MOVIE_OPT_FROM_SNAPSHOT);
        }
#endif

        delete[] data;
        break;
      }
      case NP_SERV_HACK_SETTINGS: {
        if (len < 7 + 4 + 4 + 4 + 1 + 1 + 4) {
          S9xNPSetError("Error while receiving 'HACK_SETTINGS' message.");
          S9xNPDisconnect();
          return FALSE;
        }

        uint8 data[4 + 4 + 4 + 1 + 1 + 4];
        if (!S9xNPGetData(NetPlay.Socket, data, sizeof(data))) {
          S9xNPSetError("Error while receiving 'HACK_SETTINGS' message.");
          S9xNPDisconnect();
          return FALSE;
        }

        uint32 superfx_clock_multiplier = READ_LONG(&data[0]);
        int overclock_mode = (int)READ_LONG(&data[4]);
        int interpolation_method = (int)READ_LONG(&data[8]);
        bool8 block_invalid_vram_access_master = data[12] ? TRUE : FALSE;
        bool8 separate_echo_buffer = data[13] ? TRUE : FALSE;
        int max_sprite_tiles_per_line = (int)READ_LONG(&data[14]);

        S9xNPApplyHackSettingsFromServer(
            superfx_clock_multiplier, overclock_mode, interpolation_method,
            block_invalid_vram_access_master, separate_echo_buffer,
            max_sprite_tiles_per_line);
        break;
      }
      case NP_SERV_LOAD_ROM:
#ifdef NP_DEBUG
        printf("CLIENT: LOAD_ROM received @%ld\n", S9xGetMilliTime() - START);
#endif
        S9xNPDiscardHeartbeats();
        if (S9xNPLoadROM(len - 7))
          S9xNPSendReady(NP_CLNT_LOADED_ROM);
        break;
      case NP_SERV_ROM_IMAGE:
#ifdef NP_DEBUG
        printf("CLIENT: ROM_IMAGE received @%ld\n", S9xGetMilliTime() - START);
#endif
        S9xNPDiscardHeartbeats();
        if (S9xNPGetROMImage(len - 7))
          S9xNPSendReady(NP_CLNT_RECEIVED_ROM_IMAGE);
        break;
      case NP_SERV_SRAM_DATA:
#ifdef NP_DEBUG
        printf("CLIENT: SRAM_DATA received @%ld\n", S9xGetMilliTime() - START);
#endif
        S9xNPDiscardHeartbeats();
        S9xNPGetSRAMData(len - 7);
        break;
      case NP_SERV_FREEZE_FILE:
#ifdef NP_DEBUG
        printf("CLIENT: FREEZE_FILE received @%ld\n",
               S9xGetMilliTime() - START);
#endif
        S9xNPDiscardHeartbeats();
#ifdef __WIN32__
        S9xNPStopMovie();
#endif
        S9xNPGetFreezeFile(len - 7);
        S9xNPResetJoypadReadPos();
        S9xNPSendReady();
        break;
      default:
#ifdef NP_DEBUG
        printf("CLIENT: UNKNOWN received @%ld\n", S9xGetMilliTime() - START);
#endif
        S9xNPDisconnect();
        return (FALSE);
      }
    }
  }

  S9xNPDisconnect();
  return (FALSE);
}

bool8 S9xNPLoadROMDialog(const char *rom_name) {
  NetPlay.Answer = FALSE;

#ifdef __WIN32__
  ResetEvent(NetPlay.ReplyEvent);

#ifdef NP_DEBUG
  printf("CLIENT: Asking GUI thread to open ROM load dialog...\n");
#endif

  PostMessage(GUI.hWnd, WM_USER + 3, (WPARAM)rom_name, (LPARAM)rom_name);

#ifdef NP_DEBUG
  printf("CLIENT: Waiting for reply from GUI thread...\n");
#endif

  WaitForSingleObject(NetPlay.ReplyEvent, INFINITE);

#ifdef NP_DEBUG
  printf("CLIENT: Got reply from GUI thread (%d)\n", NetPlay.Answer);
#endif

#else
  NetPlay.Answer = TRUE;
#endif

  return (NetPlay.Answer);
}

bool8 S9xNPLoadROM(uint32 len) {
  uint8 *data = new uint8[len];

  S9xNPSetAction("Receiving ROM name...");
  if (!S9xNPGetData(NetPlay.Socket, data, len)) {
    S9xNPSetError("Error while receiving ROM name.");
    delete[] data;
    S9xNPDisconnect();
    return (FALSE);
  }

  S9xNPSetAction("Opening LoadROM dialog...");
  if (!S9xNPLoadROMDialog((char *)data)) {
    S9xNPSetError("Disconnected from Netplay server because you are playing a "
                  "different game!");
    delete[] data;
    S9xNPDisconnect();
    return (FALSE);
  }
  delete[] data;
  return (TRUE);
}

bool8 S9xNPGetROMImage(uint32 len) {
  uint8 rom_info[5];

  S9xNPSetAction("Receiving ROM information...");
  if (!S9xNPGetData(NetPlay.Socket, rom_info, 5)) {
    S9xNPSetError("Error while receiving ROM information.");
    S9xNPDisconnect();
    return (FALSE);
  }
  uint32 CalculatedSize = READ_LONG(&rom_info[1]);
#ifdef NP_DEBUG
  printf("CLIENT: Hi-ROM: %s, Size: %04x\n", rom_info[0] ? "Y" : "N",
         CalculatedSize);
#endif
  if (CalculatedSize + 5 >= len || CalculatedSize >= CMemory::MAX_ROM_SIZE) {
    S9xNPSetError("Size error in ROM image data received from server.");
    S9xNPDisconnect();
    return (FALSE);
  }

  // Load up ROM image
#ifdef NP_DEBUG
  printf("CLIENT: Receiving ROM image @%ld...\n", S9xGetMilliTime() - START);
#endif
  S9xNPSetAction("Receiving ROM image...");
  uint8 *rom_data = new uint8[CalculatedSize];
  if (!S9xNPGetData(NetPlay.Socket, rom_data, CalculatedSize)) {
    S9xNPSetError("Error while receiving ROM image from server.");
    Settings.StopEmulation = TRUE;
    delete[] rom_data;
    S9xNPDisconnect();
    return (FALSE);
  }
#ifdef NP_DEBUG
  printf("CLIENT: Receiving ROM filename @%ld...\n", S9xGetMilliTime() - START);
#endif
  S9xNPSetAction("Receiving ROM filename...");
  uint32 filename_len = len - CalculatedSize - 5;
  uint8 *filename = NULL;
  if (filename_len > PATH_MAX) {
    S9xNPSetError("Error while receiving ROM filename from server.");
    delete[] rom_data;
    S9xNPDisconnect();
    Settings.StopEmulation = TRUE;
    return (FALSE);
  }
  filename = new uint8[filename_len + 1];
  if (!S9xNPGetData(NetPlay.Socket, filename, filename_len)) {
    S9xNPSetError("Error while receiving ROM filename from server.");
    delete[] filename;
    delete[] rom_data;
    S9xNPDisconnect();
    Settings.StopEmulation = TRUE;
    return (FALSE);
  }
  filename[filename_len] = 0;
  if (!Memory.LoadROMMem(rom_data, CalculatedSize, (char *)filename)) {
    S9xNPSetError("Error while loading ROM image from server.");
    delete[] filename;
    delete[] rom_data;
    S9xNPDisconnect();
    Settings.StopEmulation = TRUE;
    return (FALSE);
  }
  delete[] filename;
  delete[] rom_data;
  S9xReset();
  S9xNPResetJoypadReadPos();
  Settings.StopEmulation = FALSE;

#ifdef __WIN32__
  PostMessage(GUI.hWnd, WM_NULL, 0, 0);
#endif

  return (TRUE);
}

bool8 S9xNPGetSRAMData(uint32 len) {
  if (len > 0x70000) {
    S9xNPSetError("Length error in S-RAM data received from server.");
    S9xNPDisconnect();
    return (FALSE);
  }
  S9xNPSetAction("Receiving S-RAM data...");
  if (len > 0) {
    uint8 *sram_data = new uint8[len];
    if (!S9xNPGetData(NetPlay.Socket, sram_data, len)) {
      S9xNPSetError("Error while receiving S-RAM data from server.");
      delete[] sram_data;
      S9xNPDisconnect();
      return (FALSE);
    }
    memcpy(Memory.SRAM, sram_data, len);
    delete[] sram_data;
  }
  S9xNPSetAction("", TRUE);
  return (TRUE);
}

void S9xNPGetFreezeFile(uint32 len) {
  uint8 frame_count[4];

#ifdef NP_DEBUG
  printf("CLIENT: Receiving freeze file information @%ld...\n",
         S9xGetMilliTime() - START);
#endif
#ifdef __WIN32__
  S9xNPStopMovie();
#endif
  S9xNPSetAction("Receiving freeze file information...");
  if (!S9xNPGetData(NetPlay.Socket, frame_count, 4)) {
    S9xNPSetError("Error while receiving freeze file information from server.");
    S9xNPDisconnect();
    return;
  }
  NetPlay.FrameCount = READ_LONG(frame_count);

  uint32 freeze_len = len - 4;

#ifdef NP_DEBUG
  printf("CLIENT: Receiving freeze file @%ld...\n", S9xGetMilliTime() - START);
#endif
  S9xNPSetAction("Receiving freeze file...");
  uint8 *data = new uint8[freeze_len];
  if (!S9xNPGetData(NetPlay.Socket, data, freeze_len)) {
    S9xNPSetError("Error while receiving freeze file from server.");
    S9xNPDisconnect();
    delete[] data;
    return;
  }
  S9xNPSetAction("", TRUE);

  FILE *file;
#ifdef HAVE_MKSTEMP
  int fd;
  char fname[] = "/tmp/snes9x_fztmpXXXXXX";
  if ((fd = mkstemp(fname)) >= 0) {
    if ((file = fdopen(fd, "wb")))
#else
  char fname[L_tmpnam];
  if (tmpnam(fname)) {
    if ((file = fopen(fname, "wb")))
#endif
    {
      if (fwrite(data, 1, freeze_len, file) == freeze_len) {
        fclose(file);
#ifndef __WIN32__
        char buf[PATH_MAX + 1];

        strncpy(buf, fname, PATH_MAX);
        strcat(buf, ".s96");

        rename(fname, buf);

        if (!S9xUnfreezeGame(buf))
#else
        if (!S9xUnfreezeGame(fname))
#endif
          S9xNPSetError("Unable to load freeze file just received.");
#ifdef __WIN32__
        else
          S9xNPStartAutoMovie(MOVIE_OPT_FROM_SNAPSHOT);
#endif
      } else {
        S9xNPSetError("Failed to write to temporary freeze file.");
        fclose(file);
      }
    } else
      S9xNPSetError("Failed to create temporary freeze file.");
    remove(fname);
  } else
    S9xNPSetError("Unable to get name for temporary freeze file.");
  delete[] data;
}

uint32 S9xNPGetJoypad(int which1) {
  if (Settings.NetPlay && which1 < 8) {
    uint32 joypad = NetPlay.Joypads[NetPlay.JoypadReadInd][which1];
    bool8 ready = NetPlay.JoypadsReady[NetPlay.JoypadReadInd][which1];

    // Check if we have valid data (bit 31 should be set for valid joypad data)
    // If not ready and no valid data marker, return 0
    if (!ready && !(joypad & 0x80000000)) {
      return 0;
    }

#ifdef NP_DEBUG
    if (!ready && !NetPlay.Paused && !NetPlay.PendingWait4Sync &&
        !NetPlay.Waiting4EmulationThread) {
      S9xNPSetWarning("Missing input from server!");
    }
#endif
    NetPlay.JoypadsReady[NetPlay.JoypadReadInd][which1] = FALSE;

    return joypad;
  }

  return (0);
}

void S9xNPStepJoypadHistory() {
  if ((NetPlay.JoypadReadInd + 1) % NP_JOYPAD_HIST_SIZE !=
      NetPlay.JoypadWriteInd) {
    NetPlay.JoypadReadInd = (NetPlay.JoypadReadInd + 1) % NP_JOYPAD_HIST_SIZE;
    if (NetPlay.FrameCount != NetPlay.Frame[NetPlay.JoypadReadInd]) {
      S9xNPSetWarning(
          "This Snes9x session may be out of sync with the server.");
#ifdef NP_DEBUG
      printf("*** CLIENT: client out of sync with server (%d, %d) @%ld\n",
             NetPlay.FrameCount, NetPlay.Frame[NetPlay.JoypadReadInd],
             S9xGetMilliTime() - START);
#endif
    }
  } else {
#ifdef NP_DEBUG
    printf("*** CLIENT: S9xNPStepJoypadHistory NOT OK@%ld\n",
           S9xGetMilliTime() - START);
#endif
  }
}

static const char *S9xNPClientDisplayName(int player) {
  static char fallback[32];

  if (player >= 1 && player <= NP_MAX_CLIENTS &&
      NetPlay.ClientNames[player - 1][0])
    return NetPlay.ClientNames[player - 1];

  snprintf(fallback, sizeof(fallback), "Player %d", player);
  fallback[sizeof(fallback) - 1] = '\0';
  return fallback;
}

static void S9xNPFormatChatLine(char *buffer, size_t size, int player,
                                const char *message) {
  const char *name = S9xNPClientDisplayName(player);
  snprintf(buffer, size, "%s: %s", name, message ? message : "");
  buffer[size - 1] = '\0';
}

void S9xNPDisplayChatMessage(const char *text) {
  if (!text)
    return;

#ifdef __WIN32__
  if (!KailleraConfig.ShowChatInOSD)
    return;
  char *copy = _strdup(text);
  if (copy) {
    PostMessage(GUI.hWnd, WM_NETPLAY_CHAT, 0, (LPARAM)copy);
    Sleep(0);
    return;
  }
#endif

  S9xMessage(S9X_INFO, 0, text);
}

void S9xNPResetJoypadReadPos() {
#ifdef NP_DEBUG
  printf("CLIENT: ResetJoyReadPos @%ld\n", S9xGetMilliTime() - START);
  fflush(stdout);
#endif
  NetPlay.JoypadWriteInd = 0;
  NetPlay.JoypadReadInd = NP_JOYPAD_HIST_SIZE - 1;
  for (int h = 0; h < NP_JOYPAD_HIST_SIZE; h++)
    memset((void *)&NetPlay.Joypads[h], 0, sizeof(NetPlay.Joypads[0]));
  for (int h = 0; h < NP_JOYPAD_HIST_SIZE; h++)
    memset((void *)&NetPlay.JoypadsReady[h], 0,
           sizeof(NetPlay.JoypadsReady[0]));
}

bool8 S9xNPSendJoypadUpdate(uint32 joypad) {
  uint8 data[7];
  uint8 *ptr = data;

  *ptr++ = NP_CLNT_MAGIC;
  *ptr++ = NetPlay.MySequenceNum++;
  *ptr++ = NP_CLNT_JOYPAD;

  joypad |= 0x80000000;

  WRITE_LONG(ptr, joypad);
  if (!S9xNPSendData(NetPlay.Socket, data, 7)) {
    S9xNPSetError("Error while sending joypad data server.");
    S9xNPDisconnect();
    return (FALSE);
  }
  return (TRUE);
}

void S9xNPDisconnect() {
  bool8 was_connected = NetPlay.Connected;
  close(NetPlay.Socket);
  NetPlay.Socket = -1;
  NetPlay.Connected = FALSE;
  Settings.NetPlay = FALSE;
  memset((void *)NetPlay.ClientNames, 0, sizeof(NetPlay.ClientNames));
#ifdef __WIN32__
  if (was_connected && KailleraConfig.AutoStopMovieOnEnd)
    S9xNPStopMovie();
  S9xNPCloseChatInput();
#endif
}

bool8 S9xNPSendData(int socket, const uint8 *data, int length) {
  int len = length;
  const uint8 *ptr = data;

  NetPlay.PercentageComplete = 0;

  do {
    if (NetPlay.Abort)
      return (FALSE);

    int num_bytes = len;

    // Write the data in small chunks, allowing this thread to spot an
    // abort request from another thread.
    if (num_bytes > 512)
      num_bytes = 512;

    int sent = write(socket, (char *)ptr, num_bytes);
    if (sent < 0) {
      if (errno == EINTR
#ifdef EAGAIN
          || errno == EAGAIN
#endif
#ifdef EWOULDBLOCK
          || errno == EWOULDBLOCK
#endif
      ) {
#ifdef NP_DEBUG
        printf("CLIENT: EINTR, EAGAIN or EWOULDBLOCK while sending data @%ld\n",
               S9xGetMilliTime() - START);
#endif
        continue;
      }
      return (FALSE);
    } else if (sent == 0)
      return (FALSE);
    len -= sent;
    ptr += sent;

    NetPlay.PercentageComplete = (uint8)(((length - len) * 100) / length);
  } while (len > 0);

  return (TRUE);
}

bool8 S9xNPGetData(int socket, uint8 *data, int length) {
  int len = length;
  uint8 *ptr = data;
  int chunk = length / 50;

  if (chunk < 1024)
    chunk = 1024;

  NetPlay.PercentageComplete = 0;
  do {
    if (NetPlay.Abort)
      return (FALSE);

    int num_bytes = len;

    // Read the data in small chunks, allowing this thread to spot an
    // abort request from another thread.
    if (num_bytes > chunk)
      num_bytes = chunk;

    int got = read(socket, (char *)ptr, num_bytes);
    if (got < 0) {
      if (errno == EINTR
#ifdef EAGAIN
          || errno == EAGAIN
#endif
#ifdef EWOULDBLOCK
          || errno == EWOULDBLOCK
#endif
#ifdef WSAEWOULDBLOCK
          || errno == WSAEWOULDBLOCK
#endif
      ) {
#ifdef NP_DEBUG
        printf(
            "CLIENT: EINTR, EAGAIN or EWOULDBLOCK while receiving data @%ld\n",
            S9xGetMilliTime() - START);
#endif
        continue;
      }
#ifdef WSAEMSGSIZE
      if (errno != WSAEMSGSIZE)
        return (FALSE);
      else {
        got = num_bytes;
#ifdef NP_DEBUG
        printf(
            "CLIENT: WSAEMSGSIZE, actual bytes %d while receiving data @%ld\n",
            got, S9xGetMilliTime() - START);
#endif
      }
#else
      return (FALSE);
#endif
    } else if (got == 0)
      return (FALSE);

    len -= got;
    ptr += got;

    if (!Settings.NetPlayServer && length > 1024) {
      NetPlay.PercentageComplete = (uint8)(((length - len) * 100) / length);
#ifdef __WIN32__
      PostMessage(GUI.hWnd, WM_USER, NetPlay.PercentageComplete,
                  NetPlay.PercentageComplete);
      Sleep(0);
#endif
    }

  } while (len > 0);

  return (TRUE);
}

bool8 S9xNPInitialise() {
#ifdef __WIN32__
  static bool8 initialised = FALSE;

  if (!initialised) {
    initialised = TRUE;
    WSADATA data;

#ifdef NP_DEBUG
    START = S9xGetMilliTime();

    printf("CLIENT/SERVER: Initialising WinSock @%ld\n",
           S9xGetMilliTime() - START);
#endif
    S9xNPSetAction("Initialising Windows sockets interface...");
    if (WSAStartup(MAKEWORD(1, 0), &data) != 0) {
      S9xNPSetError("Call to init Windows sockets failed. Do you have WinSock2 "
                    "installed?");
      return (FALSE);
    }
  }
#endif
  return (TRUE);
}

void S9xNPDiscardHeartbeats() {
  // Discard any pending heartbeats and wait for any frame that is currently
  // being emulated to complete.
#ifdef NP_DEBUG
  printf("CLIENT: DiscardHeartbeats @%ld, finished @",
         S9xGetMilliTime() - START);
  fflush(stdout);
#endif

#ifdef __WIN32__
  while (WaitForSingleObject(GUI.ClientSemaphore, 200) == WAIT_OBJECT_0)
    ;
#endif

#ifdef NP_DEBUG
  printf("%ld\n", S9xGetMilliTime() - START);
#endif
  NetPlay.Waiting4EmulationThread = FALSE;
}

void S9xNPSetAction(const char *action, bool8 force) {
#ifdef NP_DEBUG
  printf("NPSetAction: %s, forced = %d %ld\n", action, force,
         S9xGetMilliTime() - START);
#endif
  if (force || !Settings.NetPlayServer) {
    strncpy(NetPlay.ActionMsg, action, NP_MAX_ACTION_LEN - 1);
    NetPlay.ActionMsg[NP_MAX_ACTION_LEN - 1] = 0;
#ifdef __WIN32__
    PostMessage(GUI.hWnd, WM_USER, 0, 0);
    Sleep(0);
#endif
  }
}

void S9xNPSetError(const char *error) {
#if defined(NP_DEBUG) && NP_DEBUG == 2
  printf("ERROR: %s\n", error);
  fflush(stdout);
#endif
  strncpy(NetPlay.ErrorMsg, error, NP_MAX_ACTION_LEN - 1);
  NetPlay.ErrorMsg[NP_MAX_ACTION_LEN - 1] = 0;
#ifdef __WIN32__
  PostMessage(GUI.hWnd, WM_USER + 1, 0, 0);
  Sleep(0);
#endif
}

void S9xNPSetWarning(const char *warning) {
#if defined(NP_DEBUG) && NP_DEBUG == 3
  printf("Warning: %s\n", warning);
  fflush(stdout);
#endif
  strncpy(NetPlay.WarningMsg, warning, NP_MAX_ACTION_LEN - 1);
  NetPlay.WarningMsg[NP_MAX_ACTION_LEN - 1] = 0;
#ifdef __WIN32__
  PostMessage(GUI.hWnd, WM_USER + 2, 0, 0);
  Sleep(0);
#endif
}

#ifdef __WIN32__
static bool np_chat_input_active = false;
static DWORD np_chat_input_swallow_until_tick = 0;
static std::string np_chat_input_text;

static void S9xNPCloseChatInput(void) {
  np_chat_input_active = false;
  np_chat_input_swallow_until_tick = 0;
  np_chat_input_text.clear();
  S9xSetInfoString(" ");
}

static void S9xNPRefreshChatInputOSD(void) {
  if (!np_chat_input_active)
    return;

  char buf[512];
  const char *cursor = ((GetTickCount() / 400) & 1) ? "_" : " ";
  snprintf(buf, sizeof(buf), ">%s%s", np_chat_input_text.c_str(), cursor);
  buf[sizeof(buf) - 1] = '\0';
  S9xSetInfoString(buf);
}

static void S9xNPBackspaceChatInput(void) {
  if (!np_chat_input_text.empty())
    np_chat_input_text.erase(np_chat_input_text.size() - 1);
}

static bool S9xNPAppendChatInputChar(WPARAM wParam) {
  if (wParam < 0x20 || wParam == 0x7f)
    return false;

#ifdef UNICODE
  wchar_t wide[2] = {(wchar_t)wParam, 0};
  char mb[8] = {};

  int count =
      WideCharToMultiByte(CP_ACP, 0, wide, 1, mb, sizeof(mb), NULL, NULL);
  if (count <= 0)
    return false;
  if (np_chat_input_text.size() + (size_t)count > NP_MAX_CHAT_MESSAGE_LEN)
    return false;
  np_chat_input_text.append(mb, (size_t)count);
  return true;
#else
  if (np_chat_input_text.size() >= NP_MAX_CHAT_MESSAGE_LEN)
    return false;
  np_chat_input_text.push_back((char)wParam);
  return true;
#endif
}

static bool S9xNPMessageHasContent(const std::string &message) {
  for (size_t i = 0; i < message.size(); i++) {
    unsigned char ch = (unsigned char)message[i];
    if (ch > ' ')
      return true;
  }
  return false;
}

LRESULT S9xNPHandleUiMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
  if (msg == WM_NETPLAY_CHAT) {
    char *text = (char *)lParam;
    if (text) {
      S9xSetInfoString(text);
      free(text);
    }
    return 0;
  }
  if (msg == WM_NETPLAY_STOP_MOVIE) {
    if (S9xMovieActive())
      S9xMovieStop(TRUE);
    return 0;
  }
  return 0;
}

bool S9xNPChatOpen(bool swallow_char) {
  if (!Settings.NetPlay || !NetPlay.Connected)
    return false;

  np_chat_input_active = true;
  np_chat_input_text.clear();
  np_chat_input_swallow_until_tick = swallow_char ? 1 : 0;
  S9xNPRefreshChatInputOSD();
  return true;
}

bool S9xNPChatWantsKeyboardCapture(void) {
  return Settings.NetPlay && NetPlay.Connected && np_chat_input_active;
}

bool S9xNPChatHandleKeyboardMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
  if (!S9xNPChatWantsKeyboardCapture())
    return false;

  switch (msg) {
  case WM_KEYDOWN:
  case WM_SYSKEYDOWN:
    if (wParam == VK_RETURN) {
      std::string message = np_chat_input_text;
      S9xNPCloseChatInput();
      if (S9xNPMessageHasContent(message) && !S9xNPSendChat(message.c_str()))
        S9xSetInfoString("Netplay: chat send failed.");
      return true;
    }
    if (wParam == VK_ESCAPE) {
      S9xNPCloseChatInput();
      return true;
    }
    if (wParam == VK_BACK) {
      S9xNPBackspaceChatInput();
      S9xNPRefreshChatInputOSD();
      return true;
    }
    if (wParam == VK_DELETE) {
      np_chat_input_text.clear();
      S9xNPRefreshChatInputOSD();
      return true;
    }
    return true;

  case WM_KEYUP:
  case WM_SYSKEYUP:
    return true;

  case WM_CHAR:
  case WM_SYSCHAR:
    if (np_chat_input_swallow_until_tick != 0) {
      np_chat_input_swallow_until_tick = 0;
      return true;
    }
    np_chat_input_swallow_until_tick = 0;
    if (wParam == '\r' || wParam == '\b' || wParam == 27)
      return true;
    if (S9xNPAppendChatInputChar(wParam))
      S9xNPRefreshChatInputOSD();
    return true;
  }

  return false;
}
#endif
#endif
