/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifndef _CHAT_H_
#define _CHAT_H_

#include <stddef.h>
#include "port.h"

bool8 S9xChatOpenForRecord(const char *movie_filename);
bool8 S9xChatOpenForPlayback(const char *movie_filename);
void S9xChatClose(void);
bool8 S9xChatWrite(uint8 player_num, const char *player_name, const char *message);
bool8 S9xChatRead(void);
void S9xChatUpdate(void);
bool8 S9xChatGetFilename(const char *movie_filename, char *chat_filename, size_t chat_filename_size);

#define CHAT_INPUT_MAX_LEN 240

#ifdef __WIN32__
#include <windows.h>

bool S9xChatInputOpen(bool swallow_char);
void S9xChatInputClose(void);
bool S9xChatInputWantsKeyboardCapture(void);
bool S9xChatInputShouldSuppressEnter(void);
bool S9xChatInputHandleKeyboardMessage(UINT msg, WPARAM wParam, LPARAM lParam);
#endif

#endif
