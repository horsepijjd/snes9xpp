#include "snes9x_imgui.h"
#include "imgui.h"

#include <cstdint>
#include <string>
#include <array>
#include <vector>
#include <algorithm>

#include "snes9x.h"
#include "port.h"
#include "controls.h"
#include "movie.h"
#include "gfx.h"
#include "ppu.h"
#include "../../font.h"
#include "../../pixform.h"

namespace
{
    S9xImGuiInitInfo settings;
    constexpr int bitmap_font_width = 8;
    constexpr int bitmap_font_height = 9;
    constexpr float bitmap_font_advance = 7.0f;

    static ImU32 bitmap_active_text_color()
    {
        uint32_t pixel = Settings.DisplayColor;
        int r = (pixel & FIRST_COLOR_MASK) >> RED_SHIFT_BITS;
        int g = (pixel >> GREEN_SHIFT_BITS) & 0x1f;
        int b = pixel & THIRD_COLOR_MASK;

        r = (r * 255) / MAX_RED;
        g = (g * 255) / 31;
        b = (b * 255) / MAX_BLUE;

        return IM_COL32(r, g, b, 255);
    }

    static float bitmap_scale()
    {
        int scale = (settings.font_size + (bitmap_font_height / 2)) / bitmap_font_height;
        if (scale < 1)
            scale = 1;
        return static_cast<float>(scale);
    }

    static uint32_t bitmap_utf8_decode(const char *&text)
    {
        unsigned char c = static_cast<unsigned char>(*text++);
        if (c < 0x80)
            return c;

        if ((c & 0xe0) == 0xc0)
        {
            uint32_t cp = (c & 0x1f) << 6;
            if (*text)
                cp |= (static_cast<unsigned char>(*text++) & 0x3f);
            return cp;
        }

        if ((c & 0xf0) == 0xe0)
        {
            uint32_t cp = (c & 0x0f) << 12;
            if (*text)
                cp |= (static_cast<unsigned char>(*text++) & 0x3f) << 6;
            if (*text)
                cp |= (static_cast<unsigned char>(*text++) & 0x3f);
            return cp;
        }

        if ((c & 0xf8) == 0xf0)
        {
            uint32_t cp = (c & 0x07) << 18;
            if (*text)
                cp |= (static_cast<unsigned char>(*text++) & 0x3f) << 12;
            if (*text)
                cp |= (static_cast<unsigned char>(*text++) & 0x3f) << 6;
            if (*text)
                cp |= (static_cast<unsigned char>(*text++) & 0x3f);
            return cp;
        }

        return '?';
    }

    static uint8_t bitmap_codepoint_to_char(uint32_t cp)
    {
        if (cp >= 32 && cp <= 255)
            return static_cast<uint8_t>(cp);

        switch (cp)
        {
        case 0x2190: return 224;
        case 0x2192: return 225;
        case 0x2191: return 226;
        case 0x2193: return 227;
        case 0xff61: return 0xa1;
        case 0xff62: return 0xa2;
        case 0xff63: return 0xa3;
        case 0xff64: return 0xa4;
        case 0xff65: return 0xa5;
        case 0xff66: return 0xa6;
        case 0xff67: return 0xa7;
        case 0xff68: return 0xa8;
        case 0xff69: return 0xa9;
        case 0xff6a: return 0xaa;
        case 0xff6b: return 0xab;
        case 0xff6c: return 0xac;
        case 0xff6d: return 0xad;
        case 0xff6e: return 0xae;
        case 0xff6f: return 0xaf;
        case 0xff70: return 0xb0;
        case 0xff71: return 0xb1;
        case 0xff72: return 0xb2;
        case 0xff73: return 0xb3;
        case 0xff74: return 0xb4;
        case 0xff75: return 0xb5;
        case 0xff76: return 0xb6;
        case 0xff77: return 0xb7;
        case 0xff78: return 0xb8;
        case 0xff79: return 0xb9;
        case 0xff7a: return 0xba;
        case 0xff7b: return 0xbb;
        case 0xff7c: return 0xbc;
        case 0xff7d: return 0xbd;
        case 0xff7e: return 0xbe;
        case 0xff7f: return 0xbf;
        case 0xff80: return 0xc0;
        case 0xff81: return 0xc1;
        case 0xff82: return 0xc2;
        case 0xff83: return 0xc3;
        case 0xff84: return 0xc4;
        case 0xff85: return 0xc5;
        case 0xff86: return 0xc6;
        case 0xff87: return 0xc7;
        case 0xff88: return 0xc8;
        case 0xff89: return 0xc9;
        case 0xff8a: return 0xca;
        case 0xff8b: return 0xcb;
        case 0xff8c: return 0xcc;
        case 0xff8d: return 0xcd;
        case 0xff8e: return 0xce;
        case 0xff8f: return 0xcf;
        case 0xff90: return 0xd0;
        case 0xff91: return 0xd1;
        case 0xff92: return 0xd2;
        case 0xff93: return 0xd3;
        case 0xff94: return 0xd4;
        case 0xff95: return 0xd5;
        case 0xff96: return 0xd6;
        case 0xff97: return 0xd7;
        case 0xff98: return 0xd8;
        case 0xff99: return 0xd9;
        case 0xff9a: return 0xda;
        case 0xff9b: return 0xdb;
        case 0xff9c: return 0xdc;
        case 0xff9d: return 0xdd;
        case 0xff9e: return 0xde;
        case 0xff9f: return 0xdf;
        default:     return '?';
        }
    }

    static std::vector<uint8_t> bitmap_text_to_bytes(const char *text)
    {
        std::vector<uint8_t> out;
        if (!text)
            return out;

        while (*text)
        {
            if (*text == '\n')
            {
                out.push_back('\n');
                text++;
                continue;
            }

            uint32_t cp = bitmap_utf8_decode(text);
            out.push_back(bitmap_codepoint_to_char(cp));
        }

        return out;
    }

    static ImVec2 bitmap_calc_text_size(const char *text, float scale, int wrap_at = 0)
    {
        auto bytes = bitmap_text_to_bytes(text);
        if (bytes.empty())
            return ImVec2(0.0f, 0.0f);

        const float line_height = bitmap_font_height * scale;

        float max_width = 0.0f;
        float current_width = 0.0f;
        int lines = 1;

        for (auto c : bytes)
        {
            if (c == '\n')
            {
                max_width = std::max(max_width, current_width);
                current_width = 0.0f;
                lines++;
                continue;
            }

            float next_width = current_width == 0.0f ? bitmap_font_width * scale : current_width + bitmap_font_advance * scale;

            if (wrap_at > 0 && current_width > 0.0f && next_width > wrap_at)
            {
                max_width = std::max(max_width, current_width);
                current_width = bitmap_font_width * scale;
                lines++;
            }
            else
            {
                current_width = next_width;
            }
        }

        max_width = std::max(max_width, current_width);
        return ImVec2(max_width, lines * line_height);
    }

    static void bitmap_draw_char(ImDrawList *draw_list, float x, float y, float scale, ImU32 color, uint8_t c)
    {
        if (c < 32 || c > 255)
            c = '?';

        const int line = ((c - 32) >> 4) * bitmap_font_height;
        const int offset = ((c - 32) & 15) * bitmap_font_width;
        const ImU32 black = IM_COL32(0, 0, 0, 255);

        for (int h = 0; h < bitmap_font_height; h++)
        {
            for (int w = 0; w < bitmap_font_width; w++)
            {
                char p = font[line + h][offset + w];
                if (p != '#' && p != '.')
                    continue;

                ImVec2 p0(x + w * scale, y + h * scale);
                ImVec2 p1(x + (w + 1) * scale, y + (h + 1) * scale);
                draw_list->AddRectFilled(p0, p1, p == '#' ? color : black);
            }
        }
    }

    static void bitmap_draw_text(ImDrawList *draw_list, float x, float y, float scale, ImU32 color, const char *text, int wrap_at = 0)
    {
        auto bytes = bitmap_text_to_bytes(text);
        const float line_height = bitmap_font_height * scale;
        float start_x = x;

        for (auto c : bytes)
        {
            if (c == '\n')
            {
                x = start_x;
                y += line_height;
                continue;
            }

            float next_width = (x == start_x) ? bitmap_font_width * scale : (x - start_x) + bitmap_font_width * scale;
            if (wrap_at > 0 && x > start_x && next_width > wrap_at)
            {
                x = start_x;
                y += line_height;
            }

            bitmap_draw_char(draw_list, x, y, scale, color, c);
            x += bitmap_font_advance * scale;
        }
    }
} // anonymous

static void ImGui_DrawPressedKeys(int spacing)
{
    const float scale = bitmap_scale();

    const std::array<const char *, 15> keynames =
        { " ", " ", " ", "R", "L", "X", "A", "→", "←", "↓", "↑", "S", "s", "Y", "B" };
    const std::array<int, 12> keyorder =
        { 10, 9, 8, 7, 6, 14, 13, 5, 4, 3, 11, 12 }; // < ^ > v   A B Y X  L R  S s

    enum controllers controller;
    int num_lines = 0;
    int cell_width = static_cast<int>(bitmap_calc_text_size("→ ", scale).x);
    int8_t ids[4];

    auto draw_list = ImGui::GetForegroundDrawList();

    for (int port = 0; port < 2; port++)
    {
        S9xGetController(port, &controller, &ids[0], &ids[1], &ids[2], &ids[3]);
        if (controller == CTL_JOYPAD || controller == CTL_MOUSE)
            num_lines++;
    }

    if (num_lines == 0)
        return;

    for (int port = 0; port < 2; port++)
    {
        S9xGetController(port, &controller, &ids[0], &ids[1], &ids[2], &ids[3]);

        switch (controller)
        {
        case CTL_MOUSE: {
            uint8_t buf[5];
            char string[256];
            if (!MovieGetMouse(port, buf))
                break;
            int16_t mouse_x = READ_WORD(buf);
            int16_t mouse_y = READ_WORD(buf + 2);
            uint8_t buttons = buf[4];
            sprintf(string, "#%d %d: (%03d,%03d) %c%c", port + 1, ids[0] + 1, mouse_x, mouse_y,
                    (buttons & 0x40) ? 'L' : ' ', (buttons & 0x80) ? 'R' : ' ');

            auto string_size = bitmap_calc_text_size(string, scale);
            int box_width = 2 * spacing + string_size.x;
            int box_height = 2 * spacing + string_size.y;
            int x = (ImGui::GetIO().DisplaySize.x - box_width) / 2;
            int y = ImGui::GetIO().DisplaySize.y - (spacing + box_height) * num_lines;

            draw_list->AddRectFilled(ImVec2(x, y),
                                     ImVec2(x + box_width, y + box_height),
                                     settings.box_color,
                                     spacing / 2.0f);

            bitmap_draw_text(draw_list, x + spacing, y + spacing, scale, bitmap_active_text_color(), string);

            break;
        }

        case CTL_JOYPAD: {
            std::string prefix = "#" + std::to_string(port + 1) + " ";
            auto prefix_size = bitmap_calc_text_size(prefix.c_str(), scale);
            int box_width = 2 * spacing + prefix_size.x + cell_width * keyorder.size();
            int box_height = 2 * spacing + prefix_size.y;
            int x = (ImGui::GetIO().DisplaySize.x - box_width) / 2;
            int y = ImGui::GetIO().DisplaySize.y - (spacing + box_height) * num_lines;

            draw_list->AddRectFilled(ImVec2(x, y),
                                     ImVec2(x + box_width, y + box_height),
                                     settings.box_color,
                                     spacing / 2.0f);
            x += spacing;
            y += spacing;

            bitmap_draw_text(draw_list, x, y, scale, bitmap_active_text_color(), prefix.c_str());
            x += prefix_size.x;

            uint16 pad = MovieGetJoypad(ids[0]);
            for (size_t i = 0; i < keyorder.size(); i++)
            {
                int j = keyorder[i];
                int mask = (1 << (j + 1));
                auto color = (pad & mask) ? settings.text_color : settings.inactive_text_color;
                bitmap_draw_text(draw_list, x, y, scale, color, keynames[j]);
                x += cell_width;
            }

            num_lines--;
            break;
        }

        default:
            break;
        }
    }
}

static void ImGui_DrawTextOverlay(const char *text,
                                  int x, int y,
                                  int padding,
                                  ImGui::DrawTextAlignment halign = ImGui::DrawTextAlignment::BEGIN,
                                  ImGui::DrawTextAlignment valign = ImGui::DrawTextAlignment::BEGIN,
                                  int wrap_at = 0)
{
    const float scale = bitmap_scale();
    auto text_size = bitmap_calc_text_size(text, scale, wrap_at);
    auto box_size = ImVec2(text_size.x + padding * 2, text_size.y + padding * 2);
    auto draw_list = ImGui::GetForegroundDrawList();
    if (halign == ImGui::DrawTextAlignment::END)
        x = x - box_size.x;
    else if (halign == ImGui::DrawTextAlignment::CENTER)
        x = x - box_size.x / 2;
    if (valign == ImGui::DrawTextAlignment::END)
        y = y - box_size.y;

    draw_list->AddRectFilled(ImVec2(x, y),
                             ImVec2(x + box_size.x, y + box_size.y),
                             settings.box_color,
                             settings.spacing / 2.0f);

    bitmap_draw_text(draw_list, x + padding, y + padding, scale, bitmap_active_text_color(), text, wrap_at);
}

static std::string sjis_to_utf8(std::string in)
{
    std::string out;
    for (const auto &i : in)
    {
        unsigned char c = i;
        if (c > 160 && c < 192)
		{
            out += "\357\275";
			out += c;
		}
        else if (c >= 192)
        {
            out += "\357\276";
            c -= 0x40;
			out += c;
        }
        else
            out += c;
    }

    return out;
}

bool S9xImGuiDraw(int width, int height)
{
    if (Memory.ROMFilename.empty())
        return false;

    if (!ImGui::GetCurrentContext())
        return false;

    ImGui::GetIO().DisplaySize.x = width;
    ImGui::GetIO().DisplaySize.y = height;
    ImGui::GetIO().DisplayFramebufferScale.x = 1.0;
    ImGui::GetIO().DisplayFramebufferScale.y = 1.0;
    ImGui::NewFrame();

    if (Settings.DisplayTime)
    {
        char string[256];

        time_t rawtime;
        struct tm *timeinfo;
        time(&rawtime);
        timeinfo = localtime(&rawtime);

        sprintf(string, "%02u:%02u", timeinfo->tm_hour, timeinfo->tm_min);
        ImGui_DrawTextOverlay(string,
                              width - settings.spacing,
                              height - settings.spacing,
                              settings.spacing,
                              ImGui::DrawTextAlignment::END,
                              ImGui::DrawTextAlignment::END);
    }

    if (Settings.DisplayFrameRate)
    {
        char string[256];
        static uint32 lastFrameCount = 0, calcFps = 0;
        static time_t lastTime = time(NULL);

        time_t currTime = time(NULL);
        if (lastTime != currTime)
        {
            if (lastFrameCount < IPPU.TotalEmulatedFrames)
            {
                calcFps = (IPPU.TotalEmulatedFrames - lastFrameCount) / (uint32)(currTime - lastTime);
            }
            lastTime = currTime;
            lastFrameCount = IPPU.TotalEmulatedFrames;
        }

        sprintf(string, "%u fps\n%02d/%02d", calcFps, (int)IPPU.DisplayedRenderedFrameCount, (int)Memory.ROMFramesPerSecond);

        ImGui_DrawTextOverlay(string,
                              width - settings.spacing,
                              settings.spacing,
                              settings.spacing,
                              ImGui::DrawTextAlignment::END,
                              ImGui::DrawTextAlignment::BEGIN);
    }

    if (Settings.DisplayPressedKeys)
    {
        ImGui_DrawPressedKeys(settings.spacing / 2);
    }

    if (Settings.DisplayIndicators)
    {
        if (Settings.Paused || Settings.ForcedPause)
        {
            ImGui_DrawTextOverlay("||",
                                  settings.spacing,
                                  settings.spacing,
                                  settings.spacing);
        }
        else if (Settings.TurboMode)
        {
            ImGui_DrawTextOverlay(">>",
                                  settings.spacing,
                                  settings.spacing,
                                  settings.spacing);
        }
    }

    if (!GFX.InfoString.empty())
    {
        auto utf8_message = sjis_to_utf8(GFX.InfoString);
        ImGui_DrawTextOverlay(utf8_message.c_str(),
                              settings.spacing,
                              height - settings.spacing,
                              settings.spacing,
                              ImGui::DrawTextAlignment::BEGIN,
                              ImGui::DrawTextAlignment::END,
                              width - settings.spacing * 4);
    }

    ImGui::Render();

    return true;
}

bool S9xImGuiRunning()
{
    if (ImGui::GetCurrentContext())
        return true;
    return false;
}

void S9xImGuiDeinit()
{
    if (S9xImGuiRunning())
        ImGui::DestroyContext();
}

S9xImGuiInitInfo S9xImGuiGetDefaults()
{
    return { 24, 10, 0x88000000, 0xffffffff, 0x44ffffff };
}

void S9xImGuiInit(S9xImGuiInitInfo *init_info)
{
    if (ImGui::GetCurrentContext())
        return;

    if (init_info)
    {
        ::settings = *init_info;
    }
    else
    {
        settings = S9xImGuiGetDefaults();
    }

    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouse;
    ImGui::StyleColorsLight();
    ImGui::GetIO().Fonts->AddFontDefault();
}
