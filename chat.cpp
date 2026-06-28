/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "chat.h"
#include "snes9x.h"
#include "movie.h"
#include "display.h"

#ifdef _WIN32
#include "win32/kaillera.h"
#endif

static bool8 S9xChatDisplayEnabled(void)
{
#ifdef _WIN32
	return KailleraConfig.ShowChatInOSD;
#else
	return TRUE;
#endif
}

#include <stdio.h>
#include <string.h>
#include <string>

#define CHAT_VERSION 1
#define CHAT_MESSAGE_HEADER_SIZE 8

static const uint8 ChatMagic[8] = { 'S', '9', 'X', 'C', 'H', 'A', 'T', 0 };

enum ChatMode
{
	CHAT_MODE_NONE = 0,
	CHAT_MODE_RECORD,
	CHAT_MODE_PLAY
};

struct PendingChatMessage
{
	uint32		frame;
	uint8		playerNum;
	std::string	playerName;
	std::string	message;
};

static FILE			*ChatFile = NULL;
static ChatMode		 ChatModeState = CHAT_MODE_NONE;
static char			 ChatFilename[PATH_MAX + 1];
static bool8		 ChatHavePending = FALSE;
static PendingChatMessage PendingMessage;

static bool8 S9xChatWriteFileHeader(FILE *file)
{
	uint8 version[4];
	WRITE_DWORD(version, CHAT_VERSION);

	return (fwrite(ChatMagic, 1, sizeof(ChatMagic), file) == sizeof(ChatMagic) &&
			fwrite(version, 1, sizeof(version), file) == sizeof(version));
}

static bool8 S9xChatReadFileHeader(FILE *file)
{
	uint8 magic[8];
	uint8 version[4];

	if (fread(magic, 1, sizeof(magic), file) != sizeof(magic))
		return (FALSE);
	if (memcmp(magic, ChatMagic, sizeof(ChatMagic)) != 0)
		return (FALSE);
	if (fread(version, 1, sizeof(version), file) != sizeof(version))
		return (FALSE);

	return (READ_DWORD(version) == CHAT_VERSION);
}

static bool8 S9xChatEnsureWriteFile(void)
{
	if (ChatFile)
		return (TRUE);
	if (ChatModeState != CHAT_MODE_RECORD || !ChatFilename[0])
		return (FALSE);

	FILE *existing = fopen(ChatFilename, "rb");
	if (existing)
	{
		bool8 valid = S9xChatReadFileHeader(existing);
		fclose(existing);
		if (!valid)
			return (FALSE);

		ChatFile = fopen(ChatFilename, "ab");
		return (ChatFile != NULL);
	}

	ChatFile = fopen(ChatFilename, "wb");
	if (!ChatFile)
		return (FALSE);

	if (!S9xChatWriteFileHeader(ChatFile))
	{
		fclose(ChatFile);
		ChatFile = NULL;
		return (FALSE);
	}

	fflush(ChatFile);
	return (TRUE);
}

bool8 S9xChatGetFilename(const char *movie_filename, char *chat_filename, size_t chat_filename_size)
{
	if (!movie_filename || !movie_filename[0] || !chat_filename || chat_filename_size == 0)
		return (FALSE);

	size_t len = strlen(movie_filename);
	const char *base = movie_filename;
	const char *slash = strrchr(movie_filename, '/');
	const char *backslash = strrchr(movie_filename, '\\');
	if (slash && slash + 1 > base)
		base = slash + 1;
	if (backslash && backslash + 1 > base)
		base = backslash + 1;

	const char *dot = strrchr(base, '.');
	size_t stem_len = dot ? (size_t)(dot - movie_filename) : len;

	if (stem_len + 5 + 1 > chat_filename_size)
		return (FALSE);

	memcpy(chat_filename, movie_filename, stem_len);
	memcpy(chat_filename + stem_len, ".chat", 6);
	return (TRUE);
}

bool8 S9xChatOpenForRecord(const char *movie_filename)
{
	S9xChatClose();

	if (!Settings.ChatRecordEnabled)
		return (FALSE);
	if (!S9xChatGetFilename(movie_filename, ChatFilename, sizeof(ChatFilename)))
		return (FALSE);

	ChatModeState = CHAT_MODE_RECORD;
	return (TRUE);
}

bool8 S9xChatOpenForPlayback(const char *movie_filename)
{
	S9xChatClose();

	if (!Settings.ChatPlaybackEnabled)
		return (FALSE);
	if (!S9xChatGetFilename(movie_filename, ChatFilename, sizeof(ChatFilename)))
		return (FALSE);

	ChatFile = fopen(ChatFilename, "rb");
	if (!ChatFile)
	{
		ChatFilename[0] = 0;
		return (FALSE);
	}

	if (!S9xChatReadFileHeader(ChatFile))
	{
		S9xChatClose();
		return (FALSE);
	}

	ChatModeState = CHAT_MODE_PLAY;
	S9xChatRead();
	return (TRUE);
}

void S9xChatClose(void)
{
	if (ChatFile)
	{
		fclose(ChatFile);
		ChatFile = NULL;
	}

	ChatModeState = CHAT_MODE_NONE;
	ChatFilename[0] = 0;
	ChatHavePending = FALSE;
	PendingMessage.playerName.clear();
	PendingMessage.message.clear();
}

bool8 S9xChatWrite(uint8 player_num, const char *player_name, const char *message)
{
	if (!Settings.ChatRecordEnabled || !S9xMovieRecording() || !message)
		return (FALSE);
	if (!S9xChatEnsureWriteFile())
		return (FALSE);

	size_t player_len = player_name ? strlen(player_name) : 0;
	size_t message_len = strlen(message);
	if (player_len > 255)
		player_len = 255;
	if (message_len > 65535)
		message_len = 65535;

	uint8 header[CHAT_MESSAGE_HEADER_SIZE];
	uint8 *ptr = header;
	WRITE_DWORD(ptr, S9xMovieGetFrameCounter());
	ptr += 4;
	*ptr++ = player_num;
	*ptr++ = (uint8)player_len;
	WRITE_WORD(ptr, (uint16)message_len);

	if (fwrite(header, 1, sizeof(header), ChatFile) != sizeof(header))
		return (FALSE);
	if (player_len && fwrite(player_name, 1, player_len, ChatFile) != player_len)
		return (FALSE);
	if (message_len && fwrite(message, 1, message_len, ChatFile) != message_len)
		return (FALSE);

	fflush(ChatFile);
	return (TRUE);
}

bool8 S9xChatRead(void)
{
	ChatHavePending = FALSE;
	PendingMessage.playerName.clear();
	PendingMessage.message.clear();

	if (ChatModeState != CHAT_MODE_PLAY || !ChatFile)
		return (FALSE);

	uint8 header[CHAT_MESSAGE_HEADER_SIZE];
	size_t got = fread(header, 1, sizeof(header), ChatFile);
	if (got == 0 && feof(ChatFile))
		return (FALSE);
	if (got != sizeof(header))
	{
		S9xChatClose();
		return (FALSE);
	}

	uint8 *ptr = header;
	PendingMessage.frame = READ_DWORD(ptr);
	ptr += 4;
	PendingMessage.playerNum = *ptr++;
	uint8 player_len = *ptr++;
	uint16 message_len = READ_WORD(ptr);

	if (player_len)
	{
		PendingMessage.playerName.resize(player_len);
		if (fread(&PendingMessage.playerName[0], 1, player_len, ChatFile) != player_len)
		{
			S9xChatClose();
			return (FALSE);
		}
	}

	if (message_len)
	{
		PendingMessage.message.resize(message_len);
		if (fread(&PendingMessage.message[0], 1, message_len, ChatFile) != message_len)
		{
			S9xChatClose();
			return (FALSE);
		}
	}

	ChatHavePending = TRUE;
	return (TRUE);
}

void S9xChatUpdate(void)
{
	if (!Settings.ChatPlaybackEnabled || ChatModeState != CHAT_MODE_PLAY || !ChatHavePending)
		return;

	uint32 frame = S9xMovieGetFrameCounter();
	if (PendingMessage.frame > frame)
		return;

	char line[65536 + 256 + 4];
	if (!PendingMessage.playerName.empty())
	{
		const char *separator = Settings.UseZSNESFont ? ">" : ": ";
		snprintf(line, sizeof(line), "%s%s%s", PendingMessage.playerName.c_str(), separator, PendingMessage.message.c_str());
		line[sizeof(line) - 1] = 0;
	}
	else
	{
		snprintf(line, sizeof(line), "%s", PendingMessage.message.c_str());
		line[sizeof(line) - 1] = 0;
	}

	if (S9xChatDisplayEnabled())
	{
		if (Settings.UseZSNESFont)
			S9xSetInfoStringChat(line);
		else
			S9xSetInfoString(line);
	}

	S9xChatRead();
}
