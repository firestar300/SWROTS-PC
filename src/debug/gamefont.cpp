// The game's HUD font for the port's overlays (see gamefont.h).
//
// interfc\fonts\font_large_white.stx: an STX texture (header: "STX", ..., width at +0xC, height at +0x10, format 0x52
// at +0x3C; the pixels last), 512 x 512 DXT5 whose alpha holds the glyphs (white). font_large_white.txt: the sheet's
// size, then "<cell width> <line height>", "<space> <kerning>", then a line a character: the character and its box
// (x0 y0 x1 y1) in the sheet.

#include "debug/gamefont.h"

#include <d3d9.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>
#include <vector>

#include "core/log.h"
#include "game/resources.h"
#include "imgui.h"

namespace swrots::debug {

namespace {

constexpr const char* kSheet = "interfc\\fonts\\font_large_white.stx";
constexpr const char* kBoxes = "interfc\\fonts\\font_large_white.txt";

struct Glyph {
    float x0, y0, x1, y1; // in the sheet, pixels
    bool present = false;
};

IDirect3DDevice9* g_Device = nullptr;
IDirect3DTexture9* g_Texture = nullptr;
bool g_Tried = false;
float g_SheetW = 0, g_SheetH = 0, g_LineHeight = 37;
Glyph g_Glyphs[256];

bool Load()
{
    if (g_Texture)
        return true;
    if (g_Tried || !g_Device)
        return false;
    g_Tried = true;
    std::vector<uint8_t> sheet, boxes;
    if (!game::ReadDiscResource(kSheet, sheet) || !game::ReadDiscResource(kBoxes, boxes) || sheet.size() < 0x40 ||
        std::memcmp(sheet.data(), "STX", 3) != 0) {
        LOG_WARN("Game font: %s not found; overlays use the menu's font", kSheet);
        return false;
    }
    uint32_t w = 0, h = 0;
    std::memcpy(&w, sheet.data() + 0xC, 4);
    std::memcpy(&h, sheet.data() + 0x10, 4);
    const uint8_t format = sheet[0x3C]; // 0x52: DXT5 (docs/modding/file-formats.md)
    const size_t bytes = size_t(w) * h; // DXT5: 16 bytes a 4 x 4 block
    if (format != 0x52 || w == 0 || h == 0 || w % 4 || h % 4 || bytes > sheet.size()) {
        LOG_WARN("Game font: unexpected sheet (%ux%u, format %u)", w, h, format);
        return false;
    }
    IDirect3DTexture9* texture = nullptr;
    if (FAILED(g_Device->CreateTexture(w, h, 1, 0, D3DFMT_DXT5, D3DPOOL_MANAGED, &texture, nullptr)))
        return false;
    D3DLOCKED_RECT lr;
    if (FAILED(texture->LockRect(0, &lr, nullptr, 0))) {
        texture->Release();
        return false;
    }
    const uint8_t* pixels = sheet.data() + sheet.size() - bytes;
    const size_t rowBytes = size_t(w) / 4 * 16;
    for (uint32_t row = 0; row < h / 4; ++row)
        std::memcpy(static_cast<uint8_t*>(lr.pBits) + size_t(row) * lr.Pitch, pixels + row * rowBytes, rowBytes);
    texture->UnlockRect(0);
    g_Texture = texture;
    g_SheetW = float(w);
    g_SheetH = float(h);

    std::istringstream text(std::string(boxes.begin(), boxes.end()));
    std::string line;
    int number = 0;
    while (std::getline(text, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        ++number;
        if (number == 2) {
            float cell = 0;
            std::istringstream(line) >> cell >> g_LineHeight;
            continue;
        }
        if (number <= 3 || line.size() < 3)
            continue;
        const unsigned char c = static_cast<unsigned char>(line[0]);
        Glyph g;
        std::istringstream(line.substr(1)) >> g.x0 >> g.y0 >> g.x1 >> g.y1;
        g.present = g.x1 > g.x0;
        g_Glyphs[c] = g;
    }
    LOG_INFO("Game font: %s loaded (%ux%u)", kSheet, w, h);
    return true;
}

const Glyph* GlyphOf(char c)
{
    const unsigned char u = static_cast<unsigned char>(toupper(static_cast<unsigned char>(c)));
    return g_Glyphs[u].present ? &g_Glyphs[u] : nullptr;
}

constexpr float kGap = 2.0f; // between letters, in sheet pixels

} // namespace

bool GameFontReady() { return Load(); }

float GameFontWidth(float height, const std::string& text)
{
    if (!Load())
        return 0;
    const float scale = height / g_LineHeight;
    float width = 0;
    for (char c : text) {
        const Glyph* g = GlyphOf(c);
        const Glyph* space = GlyphOf(' ');
        width += ((g ? g->x1 - g->x0 : space ? space->x1 - space->x0 : g_LineHeight / 3) + kGap) * scale;
    }
    return width;
}

bool GameFontText(ImDrawList* draw, const ImVec2& at, float height, unsigned colour, const std::string& text,
    bool shadow)
{
    if (!Load())
        return false;
    const float scale = height / g_LineHeight;
    const ImTextureID id = reinterpret_cast<ImTextureID>(g_Texture);
    for (int pass = shadow ? 0 : 1; pass < 2; ++pass) {
        const float offset = pass == 0 ? std::max(1.0f, height / 16) : 0;
        const ImU32 tint = pass == 0 ? IM_COL32(0, 0, 0, (colour >> 24) * 170 / 255) : colour;
        float x = at.x + offset;
        for (char c : text) {
            const Glyph* g = GlyphOf(c);
            if (!g) {
                const Glyph* space = GlyphOf(' ');
                x += ((space ? space->x1 - space->x0 : g_LineHeight / 3) + kGap) * scale;
                continue;
            }
            const float w = (g->x1 - g->x0) * scale, hgt = (g->y1 - g->y0) * scale;
            if (c != ' ')
                draw->AddImage(id, ImVec2(x, at.y + offset), ImVec2(x + w, at.y + offset + hgt),
                    ImVec2(g->x0 / g_SheetW, g->y0 / g_SheetH), ImVec2(g->x1 / g_SheetW, g->y1 / g_SheetH), tint);
            x += w + kGap * scale;
        }
    }
    return true;
}

void GameFontDeviceCreated(IDirect3DDevice9* device)
{
    g_Device = device;
    g_Tried = false;
}

void GameFontDeviceReleasing()
{
    if (g_Texture)
        g_Texture->Release();
    g_Texture = nullptr;
    g_Device = nullptr;
}

} // namespace swrots::debug
