// The launcher's window (see launcher.h): a Direct3D 9 window drawn with Dear ImGui, in the
// colours of the port's own overlays (debug/hvvhud.cpp) and, once the game files are there, with
// the game's HUD font (interfc\fonts\font_large_white, see debug/gamefont.cpp) and the title
// emblem of its executable (core/icon.h).

#include "launcher/launcher.h"

#include <windows.h>
#include <d3d9.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "backends/imgui_impl_dx9.h"
#include "backends/imgui_impl_win32.h"
#include "core/icon.h"
#include "core/install.h"
#include "core/log.h"
#include "core/settings.h"
#include "core/xbe.h"
#include "game/game.h"
#include "game/pak.h"
#include "imgui.h"
#include "input/controls.h"
#include "version.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace swrots {

namespace {

const wchar_t* kTitle = L"Star Wars: Episode III - Revenge of the Sith";
const wchar_t* kClass = L"SWROTSLauncher";

// The window's layout, in 96-DPI units (scaled by g.scale).
constexpr float kWidth = 1040, kHeight = 640;
constexpr float kMargin = 28, kNavWidth = 210, kHeaderBottom = 104;

// The port's overlay colours (debug/hvvhud.cpp): the heroes' blue, the villains' red, gold for
// what is chosen, on a near-black blue.
constexpr ImU32 kGold = IM_COL32(255, 196, 90, 255);
constexpr ImU32 kWhite = IM_COL32(235, 235, 235, 255);
constexpr ImU32 kDim = IM_COL32(150, 156, 172, 255);
constexpr ImU32 kBlue = IM_COL32(110, 175, 255, 255);
constexpr ImU32 kRed = IM_COL32(255, 85, 70, 255);
constexpr ImU32 kGreen = IM_COL32(120, 215, 140, 255);
constexpr ImU32 kPanel = IM_COL32(10, 12, 18, 215);
constexpr ImU32 kEdge = IM_COL32(200, 200, 210, 70);

constexpr const char* kFontSheet = "interfc\\fonts\\font_large_white.stx";
constexpr const char* kFontBoxes = "interfc\\fonts\\font_large_white.txt";

enum class Page { Home, Settings, Controls, Installation };
enum class GameState {
    NotInstalled,
    Installed, // the supported release's default.xbe is there
    Damaged,   // GameData has a default.xbe that is not the supported one
};

// The game's HUD font: its glyph sheet as a texture, and the glyphs' boxes in it.
struct GameFont {
    IDirect3DTexture9* texture = nullptr;
    float sheetW = 0, sheetH = 0, lineHeight = 37;
    struct Glyph {
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        bool present = false;
    } glyphs[256];
};

struct Star {
    float x, y, size, phase;
};

// [Debug] and [Mods] switches of settings.ini the Settings struct does not hold, and [Launcher].
struct ExtraSettings {
    bool skipLauncher = false;
    bool debugMenu = false, debugDisplays = false, console = false, logResources = false, flightRecorder = false;
    bool dumpResources = false;
};

struct State {
    std::wstring exeDir, gameData, ini, controlsIni;
    HWND hwnd = nullptr;
    IDirect3D9* d3d = nullptr;
    IDirect3DDevice9* device = nullptr;
    D3DPRESENT_PARAMETERS pp = {};
    bool deviceLost = false;
    float scale = 1.0f;
    ImFont* body = nullptr;
    ImFont* small = nullptr;
    ImFont* head = nullptr; // headings until the game's font is there
    GameFont font;
    IDirect3DTexture9* emblem = nullptr;
    std::vector<Star> stars;

    Page page = Page::Home;
    GameState game = GameState::NotInstalled;
    bool gameFolder = false; // GameData\ (or what an interrupted setup left) exists
    bool quit = false, play = false, closeRequested = false;

    // The game's font, found in the disc's PAKs on a thread of its own (a PAK is indexed by reading it whole).
    std::thread fontThread;
    std::atomic<int> fontStage = 0; // 0 none, 1 looking, 2 found (fontSheet, fontBoxes), 3 in use, 4 not there
    std::atomic<bool> fontStop = false;
    std::vector<uint8_t> fontSheet, fontBoxes;

    // Installing.
    std::unique_ptr<InstallProgress> job;
    std::thread worker;
    std::wstring jobImage, lastImage;
    InstallError installError = InstallError::None;
    std::wstring installMessage;
    bool justInstalled = false, desktopShortcut = false, chooseImage = false, openStop = false;
    std::wstring droppedFile;

    // Uninstalling.
    UninstallOptions uninstall;
    std::atomic<bool> uninstallDone = false;
    bool uninstalling = false, uninstallOk = false, openUninstall = false;
    std::wstring uninstallError;

    // Settings and controls.
    ExtraSettings extra;
    bool customSize = false, openResetSettings = false, openResetControls = false;
    std::vector<input::ActionInfo> actions;
    std::vector<std::vector<int>> binds;
    int listenAction = -1, listenSlot = 0, capturedKey = 0;
    std::string controlsNote;
    bool mouseStick = true;
    float sensitivity = 1.0f, walkSpeed = 0.5f;
};
State g;

float S(float v) { return v * g.scale; }
ImVec2 P(float x, float y) { return ImVec2(x * g.scale, y * g.scale); }
ImVec4 Colour(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }
ImU32 Faded(ImU32 c, float alpha) { return (c & 0x00FFFFFF) | (ImU32(float(c >> 24) * alpha) << 24); }

std::string Utf8(const std::wstring& w)
{
    if (w.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(std::max(n, 0)), '\0');
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

bool ReadWholeFile(const std::wstring& path, std::vector<uint8_t>& out)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size = {};
    GetFileSizeEx(file, &size);
    DWORD read = 0;
    bool ok = size.QuadPart > 0 && size.QuadPart < (64 << 20);
    if (ok) {
        out.resize(size_t(size.QuadPart));
        ok = ReadFile(file, out.data(), DWORD(out.size()), &read, nullptr) && read == out.size();
    }
    CloseHandle(file);
    return ok;
}

void WriteWholeFile(const std::wstring& path, const std::vector<uint8_t>& data)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0;
    WriteFile(file, data.data(), DWORD(data.size()), &written, nullptr);
    CloseHandle(file);
}

// --- settings.ini and controls.ini ----------------------------------------------------------------

bool IniFlag(const std::wstring& file, const wchar_t* section, const wchar_t* key, bool def)
{
    return GetPrivateProfileIntW(section, key, def ? 1 : 0, file.c_str()) != 0;
}

void SetIniFlag(const std::wstring& file, const wchar_t* section, const wchar_t* key, bool value)
{
    WritePrivateProfileStringW(section, key, value ? L"1" : L"0", file.c_str());
}

float IniFloat(const std::wstring& file, const wchar_t* section, const wchar_t* key, float def)
{
    wchar_t value[32] = {};
    GetPrivateProfileStringW(section, key, L"", value, 32, file.c_str());
    return value[0] ? float(_wtof(value)) : def;
}

void SetIniFloat(const std::wstring& file, const wchar_t* section, const wchar_t* key, float value)
{
    wchar_t text[32];
    swprintf_s(text, L"%.2f", value);
    WritePrivateProfileStringW(section, key, text, file.c_str());
}

void LoadExtraSettings()
{
    ExtraSettings& e = g.extra;
    e.skipLauncher = IniFlag(g.ini, L"Launcher", L"Skip", false);
    e.debugMenu = IniFlag(g.ini, L"Debug", L"DebugMenu", false);
    e.debugDisplays = IniFlag(g.ini, L"Debug", L"DebugDisplays", false);
    e.console = IniFlag(g.ini, L"Debug", L"Console", false);
    e.logResources = IniFlag(g.ini, L"Debug", L"LogResources", false);
    e.flightRecorder = IniFlag(g.ini, L"Debug", L"FlightRecorder", false);
    e.dumpResources = IniFlag(g.ini, L"Mods", L"DumpResources", false);
}

void WriteBinding(size_t action)
{
    std::string value;
    for (int vk : g.binds[action]) {
        const std::string name = input::KeyName(vk);
        if (name.empty())
            continue;
        if (!value.empty())
            value += ", ";
        value += name;
    }
    WritePrivateProfileStringW(L"Keyboard", g.actions[action].name, std::wstring(value.begin(), value.end()).c_str(),
        g.controlsIni.c_str());
}

void LoadControlsPage()
{
    g.actions = input::ActionList();
    g.binds.assign(g.actions.size(), {});
    for (size_t i = 0; i < g.actions.size(); ++i) {
        wchar_t value[256] = {};
        const std::string def = g.actions[i].defaults;
        GetPrivateProfileStringW(L"Keyboard", g.actions[i].name, std::wstring(def.begin(), def.end()).c_str(), value, 256,
            g.controlsIni.c_str());
        std::string narrow;
        for (const wchar_t* c = value; *c; ++c)
            narrow += char(*c < 128 ? *c : '?');
        g.binds[i] = input::ParseKeyList(narrow);
    }
    g.mouseStick = IniFlag(g.controlsIni, L"Mouse", L"RightStick", true);
    g.sensitivity = std::clamp(IniFloat(g.controlsIni, L"Mouse", L"Sensitivity", 1.0f), 0.05f, 20.0f);
    g.walkSpeed = std::clamp(IniFloat(g.controlsIni, L"Keyboard", L"WalkSpeed", 0.5f), 0.1f, 1.0f);
}

void ResetControls()
{
    for (size_t i = 0; i < g.actions.size(); ++i) {
        g.binds[i] = input::ParseKeyList(g.actions[i].defaults);
        WriteBinding(i);
    }
    g.mouseStick = true;
    g.sensitivity = 1.0f;
    g.walkSpeed = 0.5f;
    SetIniFlag(g.controlsIni, L"Mouse", L"RightStick", true);
    SetIniFloat(g.controlsIni, L"Mouse", L"Sensitivity", g.sensitivity);
    SetIniFloat(g.controlsIni, L"Keyboard", L"WalkSpeed", g.walkSpeed);
    g.controlsNote = "The default controls are back.";
}

void ResetSettings()
{
    EditSettings() = Settings();
    SaveSettings();
    for (const wchar_t* key : { L"DebugMenu", L"DebugDisplays", L"Console", L"LogResources", L"FlightRecorder" })
        SetIniFlag(g.ini, L"Debug", key, false);
    SetIniFlag(g.ini, L"Mods", L"DumpResources", false);
    SetIniFlag(g.ini, L"Launcher", L"Skip", false);
    LoadExtraSettings();
    g.customSize = false;
}

// --- Textures -------------------------------------------------------------------------------------

// A texture of the launcher's device from top-down A8R8G8B8 pixels (managed: it survives a device reset).
IDirect3DTexture9* MakeTexture(const uint32_t* pixels, int width, int height)
{
    IDirect3DTexture9* texture = nullptr;
    if (!g.device || FAILED(g.device->CreateTexture(UINT(width), UINT(height), 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
            &texture, nullptr)))
        return nullptr;
    D3DLOCKED_RECT lr;
    if (FAILED(texture->LockRect(0, &lr, nullptr, 0))) {
        texture->Release();
        return nullptr;
    }
    for (int y = 0; y < height; ++y)
        std::memcpy(static_cast<uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch, pixels + size_t(y) * width, size_t(width) * 4);
    texture->UnlockRect(0);
    return texture;
}

void ReleaseGameArt()
{
    if (g.font.texture)
        g.font.texture->Release();
    g.font = GameFont();
    if (g.emblem)
        g.emblem->Release();
    g.emblem = nullptr;
}

// The glyph sheet (an STX texture: width at +0xC, height at +0x10, format 0x52 = DXT5 at +0x3C, the
// pixels last) holds the glyphs in its alpha: white pixels with that alpha.
bool BuildGameFont(const std::vector<uint8_t>& sheet, const std::vector<uint8_t>& boxes)
{
    if (sheet.size() < 0x40 || std::memcmp(sheet.data(), "STX", 3) != 0)
        return false;
    uint32_t w = 0, h = 0;
    std::memcpy(&w, sheet.data() + 0xC, 4);
    std::memcpy(&h, sheet.data() + 0x10, 4);
    const size_t bytes = size_t(w) * h; // DXT5: 16 bytes a 4 x 4 block
    if (sheet[0x3C] != 0x52 || w == 0 || h == 0 || w % 4 || h % 4 || w > 4096 || h > 4096 || bytes > sheet.size())
        return false;
    const uint8_t* blocks = sheet.data() + sheet.size() - bytes;
    std::vector<uint32_t> pixels(size_t(w) * h);
    for (uint32_t by = 0; by < h / 4; ++by) {
        for (uint32_t bx = 0; bx < w / 4; ++bx) {
            const uint8_t* b = blocks + (size_t(by) * (w / 4) + bx) * 16;
            uint32_t alpha[8] = { b[0], b[1] };
            if (alpha[0] > alpha[1]) {
                for (uint32_t i = 2; i < 8; ++i)
                    alpha[i] = ((8 - i) * alpha[0] + (i - 1) * alpha[1]) / 7;
            } else {
                for (uint32_t i = 2; i < 6; ++i)
                    alpha[i] = ((6 - i) * alpha[0] + (i - 1) * alpha[1]) / 5;
                alpha[6] = 0;
                alpha[7] = 255;
            }
            uint64_t bits = 0;
            for (int i = 0; i < 6; ++i)
                bits |= uint64_t(b[2 + i]) << (8 * i);
            for (uint32_t i = 0; i < 16; ++i) {
                const uint32_t a = alpha[(bits >> (3 * i)) & 7];
                pixels[size_t(by * 4 + i / 4) * w + bx * 4 + i % 4] = (a << 24) | 0x00FFFFFF;
            }
        }
    }
    GameFont font;
    font.sheetW = float(w);
    font.sheetH = float(h);
    // The boxes: the sheet's size, "<cell width> <line height>", "<space> <kerning>", then a line a
    // character: the character and its box (x0 y0 x1 y1).
    std::istringstream text(std::string(boxes.begin(), boxes.end()));
    std::string line;
    int number = 0, count = 0;
    while (std::getline(text, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        ++number;
        if (number == 2) {
            float cell = 0, height = 0;
            std::istringstream(line) >> cell >> height;
            if (height >= 8 && height <= 256)
                font.lineHeight = height;
            continue;
        }
        if (number <= 3 || line.size() < 3)
            continue;
        GameFont::Glyph glyph;
        std::istringstream(line.substr(1)) >> glyph.x0 >> glyph.y0 >> glyph.x1 >> glyph.y1;
        glyph.present = glyph.x1 > glyph.x0 && glyph.y1 > glyph.y0 && glyph.x1 <= font.sheetW && glyph.y1 <= font.sheetH;
        if (glyph.present)
            ++count;
        font.glyphs[static_cast<unsigned char>(line[0])] = glyph;
    }
    if (count < 26)
        return false;
    font.texture = MakeTexture(pixels.data(), int(w), int(h));
    if (!font.texture)
        return false;
    if (g.font.texture)
        g.font.texture->Release();
    g.font = font;
    return true;
}

// Finds the font in the installed game files. The first time, the disc's PAKs are searched (the
// smallest first) and the two files kept in cache\launcher; later runs read them from there.
void FindGameFont()
{
    const std::wstring cache = g.exeDir + L"\\cache\\launcher";
    const std::wstring sheetFile = cache + L"\\font_large_white.stx", boxesFile = cache + L"\\font_large_white.txt";
    std::vector<uint8_t> sheet, boxes;
    bool found = ReadWholeFile(sheetFile, sheet) && ReadWholeFile(boxesFile, boxes);
    if (!found) {
        struct Pak {
            std::wstring path;
            uint64_t size;
        };
        std::vector<Pak> paks;
        WIN32_FIND_DATAW data;
        HANDLE find = FindFirstFileW((g.gameData + L"\\pak\\res_*.pak").c_str(), &data);
        if (find != INVALID_HANDLE_VALUE) {
            do {
                const std::wstring name = data.cFileName;
                if (name.find(L"_seg") == std::wstring::npos)
                    paks.push_back({ g.gameData + L"\\pak\\" + name, (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow });
            } while (FindNextFileW(find, &data));
            FindClose(find);
        }
        std::sort(paks.begin(), paks.end(), [](const Pak& a, const Pak& b) { return a.size < b.size; });
        for (const Pak& pak : paks) {
            if (g.fontStop)
                break;
            game::PakIndex index;
            if (!index.Load(pak.path))
                continue;
            const game::PakIndex::Entry* s = index.FindWithData(kFontSheet);
            const game::PakIndex::Entry* b = index.FindWithData(kFontBoxes);
            if (!s || !b || s->imageSize > 0 || b->imageSize > 0)
                continue;
            if (index.ReadData(*s, sheet) && index.ReadData(*b, boxes) && !sheet.empty() && !boxes.empty()) {
                found = true;
                CreateDirectoryW((g.exeDir + L"\\cache").c_str(), nullptr);
                CreateDirectoryW(cache.c_str(), nullptr);
                WriteWholeFile(sheetFile, sheet);
                WriteWholeFile(boxesFile, boxes);
                LOG_INFO("Launcher: the game's font found in %ls", pak.path.c_str());
                break;
            }
        }
    }
    if (found) {
        g.fontSheet = std::move(sheet);
        g.fontBoxes = std::move(boxes);
    }
    g.fontStage = found ? 2 : 4;
}

void StopFontThread()
{
    g.fontStop = true;
    if (g.fontThread.joinable())
        g.fontThread.join();
    g.fontStop = false;
}

void StartFontThread()
{
    StopFontThread();
    g.fontStage = 1;
    g.fontThread = std::thread(FindGameFont);
}

// The window thread's part: the found sheet becomes the texture.
void PollFont()
{
    if (g.fontStage != 2)
        return;
    const bool ok = BuildGameFont(g.fontSheet, g.fontBoxes);
    g.fontSheet.clear();
    g.fontBoxes.clear();
    g.fontStage = ok ? 3 : 4;
    if (!ok)
        LOG_WARN("Launcher: the game's font is not as expected; headings use a system font");
}

// --- Headings: the game's font (capitals), or a system font until it is there ---------------------

constexpr float kGlyphGap = 2.0f; // between letters, in sheet pixels

const GameFont::Glyph* GlyphOf(char c)
{
    const unsigned char u = static_cast<unsigned char>(toupper(static_cast<unsigned char>(c)));
    return g.font.glyphs[u].present ? &g.font.glyphs[u] : nullptr;
}

// Whether the game's font has every character of `text` (it has capitals, digits and little else).
bool GameFontCovers(const char* text)
{
    if (!g.font.texture)
        return false;
    for (const char* c = text; *c; ++c)
        if (*c != ' ' && !GlyphOf(*c))
            return false;
    return true;
}

std::string Upper(const char* text)
{
    std::string upper = text;
    for (char& c : upper)
        c = char(toupper(static_cast<unsigned char>(c)));
    return upper;
}

float HeadingWidth(float height, const char* text)
{
    if (!GameFontCovers(text))
        return g.head->CalcTextSizeA(height * 1.08f, FLT_MAX, 0.0f, Upper(text).c_str()).x;
    const float scale = height / g.font.lineHeight;
    const GameFont::Glyph* space = GlyphOf(' ');
    float width = 0;
    for (const char* c = text; *c; ++c) {
        const GameFont::Glyph* glyph = GlyphOf(*c);
        width += ((glyph ? glyph->x1 - glyph->x0 : space ? space->x1 - space->x0 : g.font.lineHeight / 3) + kGlyphGap) * scale;
    }
    return width;
}

// `text` in capitals, `height` pixels tall, its left top at `at`.
void Heading(ImDrawList* draw, ImVec2 at, float height, ImU32 colour, const char* text)
{
    if (!GameFontCovers(text)) {
        draw->AddText(g.head, height * 1.08f, ImVec2(at.x, at.y - height * 0.16f), colour, Upper(text).c_str());
        return;
    }
    const float scale = height / g.font.lineHeight;
    const ImTextureID id = reinterpret_cast<ImTextureID>(g.font.texture);
    const GameFont::Glyph* space = GlyphOf(' ');
    for (int pass = 0; pass < 2; ++pass) { // a dark shadow, then the letters
        const float offset = pass == 0 ? std::max(1.0f, height / 16) : 0;
        const ImU32 tint = pass == 0 ? IM_COL32(0, 0, 0, (colour >> 24) * 170 / 255) : colour;
        float x = at.x + offset;
        for (const char* c = text; *c; ++c) {
            const GameFont::Glyph* glyph = GlyphOf(*c);
            if (!glyph || *c == ' ') {
                x += ((space ? space->x1 - space->x0 : g.font.lineHeight / 3) + kGlyphGap) * scale;
                continue;
            }
            const float w = (glyph->x1 - glyph->x0) * scale, h = (glyph->y1 - glyph->y0) * scale;
            draw->AddImage(id, ImVec2(x, at.y + offset), ImVec2(x + w, at.y + offset + h),
                ImVec2(glyph->x0 / g.font.sheetW, glyph->y0 / g.font.sheetH),
                ImVec2(glyph->x1 / g.font.sheetW, glyph->y1 / g.font.sheetH), tint);
            x += w + kGlyphGap * scale;
        }
    }
}

// --- The game files' state ------------------------------------------------------------------------

void RefreshGameState()
{
    g.gameFolder = Exists(g.gameData) || Exists(g.gameData + L".partial");
    g.game = GameState::NotInstalled;
    if (!GameDataInstalled(g.gameData))
        return;
    XbeFile xbe;
    std::string error;
    if (!xbe.Load(g.gameData + L"\\default.xbe", error) || xbe.Md5Hex() != game::kRetailMd5) {
        g.game = GameState::Damaged;
        return;
    }
    g.game = GameState::Installed;
    SetWindowIconFromXbe(g.hwnd, xbe);
    if (!g.emblem) {
        int w = 0, h = 0;
        const std::vector<uint32_t> pixels = DecodeTitleImage(xbe, w, h);
        if (!pixels.empty())
            g.emblem = MakeTexture(pixels.data(), w, h);
    }
}

// --- Installing and uninstalling ------------------------------------------------------------------

void StartInstall(const std::wstring& image)
{
    if (g.job || g.uninstalling || g.game == GameState::Installed)
        return;
    g.installError = InstallError::None;
    g.installMessage.clear();
    g.justInstalled = false;
    g.jobImage = image;
    g.job = std::make_unique<InstallProgress>();
    g.job->SetStatus(L"Starting...");
    g.worker = std::thread([image] {
        InstallGameFiles(g.exeDir, g.gameData, image, *g.job);
        g.job->finished = true;
    });
}

void PollInstall()
{
    if (!g.job || !g.job->finished)
        return;
    g.worker.join();
    if (g.job->ok) {
        CreateGameShortcuts(g.exeDir, g.gameData, g.desktopShortcut);
        g.justInstalled = true;
    } else if (g.job->code != InstallError::Cancelled) {
        g.installError = g.job->code;
        g.installMessage = g.job->error;
        g.lastImage = g.jobImage;
    }
    g.job.reset();
    RefreshGameState();
    if (g.game == GameState::Installed)
        StartFontThread();
}

void StartUninstall()
{
    if (g.job || g.uninstalling)
        return;
    StopFontThread();
    ReleaseGameArt();
    g.fontStage = 0;
    g.uninstalling = true;
    g.game = GameState::NotInstalled; // from now on; what is left is seen when it is done
    g.gameFolder = true;
    g.uninstallDone = false;
    g.uninstallError.clear();
    g.justInstalled = false;
    g.worker = std::thread([options = g.uninstall] {
        g.uninstallOk = UninstallGame(g.exeDir, options, g.uninstallError);
        g.uninstallDone = true;
    });
}

void PollUninstall()
{
    if (!g.uninstalling || !g.uninstallDone)
        return;
    g.worker.join();
    g.uninstalling = false;
    if (g.uninstall.settings) {
        // The files are gone: make them again with the defaults, as a first start does.
        LoadSettings(g.ini);
        input::LoadControls(g.controlsIni);
        LoadExtraSettings();
        LoadControlsPage();
        g.customSize = false;
    }
    g.uninstall = UninstallOptions();
    RefreshGameState();
}

// --- Widgets --------------------------------------------------------------------------------------

void TextW(const std::wstring& text) { ImGui::TextWrapped("%s", Utf8(text).c_str()); }

void DimText(const char* text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, Colour(kDim));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

void SmallText(const char* text, ImU32 colour = kDim)
{
    ImGui::PushFont(g.small);
    ImGui::PushStyleColor(ImGuiCol_Text, Colour(colour));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void Space(float height) { ImGui::Dummy(ImVec2(1, S(height))); }

// A page's title, in the game's font.
void PageTitle(const char* text)
{
    const ImVec2 at = ImGui::GetCursorScreenPos();
    Heading(ImGui::GetWindowDrawList(), at, S(24), kGold, text);
    ImGui::Dummy(ImVec2(HeadingWidth(S(24), text), S(24)));
    Space(8);
}

void SectionTitle(const char* text)
{
    Space(6);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    Heading(ImGui::GetWindowDrawList(), at, S(15), kBlue, text);
    ImGui::Dummy(ImVec2(HeadingWidth(S(15), text), S(15)));
    Space(2);
}

// The launcher's large buttons, lettered in the game's font: the primary one filled gold, the
// others outlined.
bool BigButton(const char* label, ImVec2 size, bool primary, bool enabled = true)
{
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::InvisibleButton(label, size);
    const bool hovered = enabled && ImGui::IsItemHovered(), held = enabled && ImGui::IsItemActive();
    const bool focused = enabled && ImGui::IsItemFocused();
    ImGui::EndDisabled();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 end(at.x + size.x, at.y + size.y);
    const float textHeight = std::min(size.y * 0.42f, S(24));
    ImU32 text = kWhite;
    if (primary) {
        const ImU32 fill = held ? IM_COL32(225, 165, 60, 255) : hovered ? IM_COL32(255, 214, 130, 255) : kGold;
        draw->AddRectFilled(at, end, enabled ? fill : IM_COL32(90, 84, 70, 255));
        text = IM_COL32(18, 14, 6, 255);
    } else {
        draw->AddRectFilled(at, end, IM_COL32(255, 255, 255, held ? 44 : hovered ? 28 : 12));
        draw->AddRect(at, end, hovered ? kGold : kEdge);
        text = enabled ? (hovered ? kGold : kWhite) : kDim;
    }
    if (focused && !hovered)
        draw->AddRect(ImVec2(at.x - 2, at.y - 2), ImVec2(end.x + 2, end.y + 2), kBlue);
    const float width = HeadingWidth(textHeight, label);
    const ImVec2 textAt(at.x + (size.x - width) / 2, at.y + (size.y - textHeight) / 2);
    if (primary && GameFontCovers(label)) {
        // No shadow under dark letters on gold: drawn as tinted glyphs alone.
        const float scale = textHeight / g.font.lineHeight;
        const GameFont::Glyph* space = GlyphOf(' ');
        float x = textAt.x;
        for (const char* c = label; *c; ++c) {
            const GameFont::Glyph* glyph = GlyphOf(*c);
            if (!glyph || *c == ' ') {
                x += ((space ? space->x1 - space->x0 : g.font.lineHeight / 3) + kGlyphGap) * scale;
                continue;
            }
            const float w = (glyph->x1 - glyph->x0) * scale, h = (glyph->y1 - glyph->y0) * scale;
            draw->AddImage(reinterpret_cast<ImTextureID>(g.font.texture), ImVec2(x, textAt.y), ImVec2(x + w, textAt.y + h),
                ImVec2(glyph->x0 / g.font.sheetW, glyph->y0 / g.font.sheetH),
                ImVec2(glyph->x1 / g.font.sheetW, glyph->y1 / g.font.sheetH), text);
            x += w + kGlyphGap * scale;
        }
    } else {
        Heading(draw, textAt, textHeight, text, label);
    }
    return clicked && enabled;
}

// A tinted box with a coloured edge around a title and a message (an error, a notice).
void MessageBoxWidget(ImU32 colour, const std::string& title, const std::string& message)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    draw->ChannelsSplit(2);
    draw->ChannelsSetCurrent(1);
    ImGui::BeginGroup();
    ImGui::Dummy(ImVec2(width, S(8)));
    ImGui::Indent(S(18));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width - S(36));
    ImGui::PushStyleColor(ImGuiCol_Text, Colour(colour));
    ImGui::TextUnformatted(title.c_str());
    ImGui::PopStyleColor();
    if (!message.empty())
        ImGui::TextUnformatted(message.c_str());
    ImGui::PopTextWrapPos();
    ImGui::Unindent(S(18));
    ImGui::Dummy(ImVec2(width, S(8)));
    ImGui::EndGroup();
    const ImVec2 end(start.x + width, ImGui::GetItemRectMax().y);
    draw->ChannelsSetCurrent(0);
    draw->AddRectFilled(start, end, Faded(colour, 0.11f));
    draw->AddRectFilled(start, ImVec2(start.x + S(4), end.y), colour);
    draw->ChannelsMerge();
    Space(6);
}

// Two columns: what a setting is (and a line about it) on the left, its control on the right.
bool BeginRows(const char* id)
{
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_None))
        return false;
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthFixed, S(250));
    return true;
}

void RowLabel(const char* label, const char* help)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (help)
        SmallText(help);
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::PushID(label);
}

bool CheckRow(const char* label, bool* value, const char* help = nullptr)
{
    RowLabel(label, help);
    const bool changed = ImGui::Checkbox("##value", value);
    ImGui::PopID();
    return changed;
}

bool ComboRow(const char* label, int* value, const char* const* items, int count, const char* help = nullptr)
{
    RowLabel(label, help);
    const bool changed = ImGui::Combo("##value", value, items, count);
    ImGui::PopID();
    return changed;
}

// --- Pages: installing ----------------------------------------------------------------------------

void InstallFlow()
{
    if (g.job) {
        TextW(L"Installing from " + g.jobImage.substr(g.jobImage.find_last_of(L"\\/") + 1));
        Space(6);
        TextW(g.job->Status());
        Space(4);
        const uint64_t total = g.job->total, done = std::min<uint64_t>(g.job->done, total);
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const ImVec2 size(ImGui::GetContentRegionAvail().x, S(22));
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(at, ImVec2(at.x + size.x, at.y + size.y), IM_COL32(255, 255, 255, 14));
        draw->AddRect(at, ImVec2(at.x + size.x, at.y + size.y), kEdge);
        if (total) {
            const float part = float(double(done) / double(total));
            draw->AddRectFilled(ImVec2(at.x + 2, at.y + 2), ImVec2(at.x + 2 + (size.x - 4) * part, at.y + size.y - 2), kGold);
        } else {
            // Nothing to measure yet: a block going back and forth.
            const float t = 0.5f + 0.5f * std::sin(float(ImGui::GetTime()) * 2.5f), w = size.x * 0.2f;
            draw->AddRectFilled(ImVec2(at.x + 2 + (size.x - 4 - w) * t, at.y + 2),
                ImVec2(at.x + 2 + (size.x - 4 - w) * t + w, at.y + size.y - 2), Faded(kGold, 0.6f));
        }
        ImGui::Dummy(size);
        if (total) {
            char text[96];
            snprintf(text, sizeof(text), "%.0f%%  (%.2f of %.2f GB)", 100.0 * double(done) / double(total), double(done) / 1e9,
                double(total) / 1e9);
            SmallText(text);
        }
        Space(10);
        if (BigButton(g.job->cancel ? "STOPPING" : "CANCEL", P(180, 42), false, !g.job->cancel))
            g.job->cancel = true;
        return;
    }
    if (g.installError != InstallError::None) {
        std::wstring message = g.installMessage;
        if (!g.lastImage.empty())
            message += L"\n\nFile: " + g.lastImage;
        MessageBoxWidget(kRed, Utf8(InstallErrorTitle(g.installError)), Utf8(message));
    }
    ImGui::TextWrapped("The port needs the game's files from your own disc of Star Wars: Episode III - Revenge of the Sith "
                       "for the Xbox (North American release). Choose its disc image: an .iso / .xiso file (an XISO or a "
                       "full Redump-style image), or a .7z / .zip / .rar archive containing one.");
    if (g.installError == InstallError::None) {
        Space(2);
        DimText("The image is checked first, then the game files are copied into the GameData folder next to swrots.exe "
                "(about 2.5 GB). Your image is not changed.");
    }
    Space(8);
    ImGui::Checkbox("Add a shortcut to the desktop (the Start menu gets one in any case)", &g.desktopShortcut);
    Space(10);
    if (BigButton(g.installError != InstallError::None ? "CHOOSE ANOTHER FILE" : "CHOOSE DISC IMAGE", P(330, 52), true,
            !g.uninstalling))
        g.chooseImage = true;
    Space(4);
    SmallText("Or drop the file onto this window.");
}

void HomePage()
{
    if (g.game == GameState::Installed) {
        PageTitle(g.justInstalled ? "INSTALLATION COMPLETE" : "READY TO PLAY");
        if (g.justInstalled)
            MessageBoxWidget(kGreen, "The game files are installed.",
                g.desktopShortcut ? "Shortcuts were added to the Start menu and the desktop."
                                  : "A shortcut was added to the Start menu.");
        ImGui::TextWrapped("Star Wars: Episode III - Revenge of the Sith is installed (North American release).");
        DimText("Settings and Controls on the left change how the game looks and plays; both can also be edited as "
                "text (settings.ini, controls.ini).");
        Space(18);
        if (BigButton("PLAY", P(300, 68), true))
            g.play = true;
        Space(18);
        if (ImGui::Checkbox("Start the game directly from now on (skip this launcher)", &g.extra.skipLauncher))
            SetIniFlag(g.ini, L"Launcher", L"Skip", g.extra.skipLauncher);
        SmallText("To see the launcher again: hold Shift while the game starts, or run swrots.exe --launcher.");
    } else if (g.game == GameState::Damaged) {
        PageTitle("GAME FILES DAMAGED");
        MessageBoxWidget(kRed, "The installed game is not the supported release.",
            "GameData\\default.xbe is missing parts or is not the North American release's executable. Uninstall "
            "the game files, then install them again from your disc image.");
        if (BigButton("INSTALLATION", P(260, 48), false))
            g.page = Page::Installation;
    } else {
        PageTitle("INSTALL THE GAME");
        InstallFlow();
    }
}

void InstallationPage()
{
    PageTitle("INSTALLATION");
    const char* status = g.game == GameState::Installed ? "Installed (North American release)"
        : g.game == GameState::Damaged ? "Damaged or unsupported (see Play)"
        : g.gameFolder ? "Not installed (an unfinished or foreign GameData folder is there)" : "Not installed";
    const ImU32 statusColour = g.game == GameState::Installed ? kGreen : g.game == GameState::Damaged ? kRed : kGold;
    ImGui::TextUnformatted("Game files:");
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, Colour(statusColour));
    ImGui::TextUnformatted(status);
    ImGui::PopStyleColor();
    SmallText(("Folder: " + Utf8(g.gameData)).c_str());
    Space(6);
    if (ImGui::Button("Open the game's folder"))
        ShellExecuteW(g.hwnd, L"open", g.exeDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    ImGui::SameLine();
    if (ImGui::Button("Open the log"))
        ShellExecuteW(g.hwnd, L"open", (g.exeDir + L"\\logs\\swrots.log").c_str(), nullptr, nullptr, SW_SHOWNORMAL);

    if (g.uninstalling) {
        Space(10);
        MessageBoxWidget(kGold, "Removing the game files...", "This takes a moment.");
        return;
    }
    if (!g.uninstallError.empty())
        MessageBoxWidget(kRed, "Not everything could be removed.", Utf8(g.uninstallError));

    if (g.game == GameState::NotInstalled && !g.gameFolder) {
        SectionTitle("INSTALL");
        InstallFlow();
        return;
    }
    if (g.job) {
        SectionTitle("INSTALL");
        InstallFlow();
        return;
    }
    SectionTitle("UNINSTALL");
    ImGui::TextWrapped("Uninstalling deletes the game files copied from your disc image (the GameData folder), the "
                       "cache, the game's icon and its Start menu and desktop shortcuts. swrots.exe and core.dll stay, "
                       "so the game can be installed again from this launcher.");
    Space(4);
    ImGui::Checkbox("Also delete the saved games (saves)", &g.uninstall.saves);
    ImGui::Checkbox("Also reset the settings and the controls (settings.ini, controls.ini)", &g.uninstall.settings);
    ImGui::Checkbox("Also delete the mods (mods)", &g.uninstall.mods);
    Space(10);
    if (BigButton("UNINSTALL", P(240, 46), false))
        g.openUninstall = true;
    if (g.game == GameState::NotInstalled) {
        Space(4);
        SmallText("To install over what an earlier attempt left, uninstall first.");
    }
}

// --- Pages: settings ------------------------------------------------------------------------------

struct WindowSize {
    int width, height;
    const char* name;
};
const WindowSize kWindowSizes[] = {
    { 1280, 720, "1280 x 720" }, { 1600, 900, "1600 x 900" }, { 1920, 1080, "1920 x 1080" },
    { 2560, 1440, "2560 x 1440" }, { 3840, 2160, "3840 x 2160" },
};

void SettingsPage()
{
    PageTitle("SETTINGS");
    Settings& s = EditSettings();
    bool changed = false;
    if (!ImGui::BeginTabBar("settings"))
        return;
    if (ImGui::BeginTabItem("Display")) {
        Space(4);
        if (BeginRows("display")) {
            changed |= CheckRow("Fullscreen", &s.fullscreen, "A borderless window covering the monitor.");
            // The window's size: one of the usual ones, or any other.
            const char* names[std::size(kWindowSizes) + 1];
            int chosen = int(std::size(kWindowSizes));
            for (size_t i = 0; i < std::size(kWindowSizes); ++i) {
                names[i] = kWindowSizes[i].name;
                if (!g.customSize && kWindowSizes[i].width == s.width && kWindowSizes[i].height == s.height)
                    chosen = int(i);
            }
            names[std::size(kWindowSizes)] = "Custom";
            ImGui::BeginDisabled(s.fullscreen);
            if (ComboRow("Window size", &chosen, names, int(std::size(names)), "The picture's size in the window, in screen pixels.")) {
                g.customSize = chosen == int(std::size(kWindowSizes));
                if (!g.customSize) {
                    s.width = kWindowSizes[chosen].width;
                    s.height = kWindowSizes[chosen].height;
                }
                changed = true;
            }
            if (chosen == int(std::size(kWindowSizes))) {
                RowLabel("Custom size", "Width and height, at least 640 x 360.");
                int size[2] = { s.width, s.height };
                if (ImGui::InputInt2("##value", size)) {
                    s.width = std::clamp(size[0], 640, 16384);
                    s.height = std::clamp(size[1], 360, 16384);
                    g.customSize = true;
                    changed = true;
                }
                ImGui::PopID();
            }
            ImGui::EndDisabled();
            changed |= CheckRow("Vertical sync", &s.vsync, "Waits for the display's refresh when showing a frame.");
            changed |= CheckRow("Stretch to the window", &s.stretch,
                "Fills the window without black bars; the picture may look squashed or stretched.");
            ImGui::EndTable();
        }
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Graphics")) {
        Space(4);
        if (BeginRows("graphics")) {
            static const char* const kScales[] = { "Automatic", "1x (640 x 480, the Xbox's)", "2x", "3x", "4x", "5x", "6x", "7x", "8x" };
            changed |= ComboRow("Internal resolution", &s.resolutionScale, kScales, int(std::size(kScales)),
                "The Xbox's resolution times this. Automatic fits the screen. Higher needs more video memory.");
            changed |= CheckRow("Widescreen (16:9)", &s.widescreen, "Off: 4:3 with bars at the sides.");
            static const char* const kFilters[] = { "Off", "2x", "4x", "8x", "16x" };
            static const int kFilterValues[] = { 1, 2, 4, 8, 16 };
            int filter = 0;
            for (int i = 0; i < int(std::size(kFilterValues)); ++i)
                if (s.anisotropy >= kFilterValues[i])
                    filter = i;
            if (ComboRow("Anisotropic filtering", &filter, kFilters, int(std::size(kFilters)), "Sharper textures at a distance.")) {
                s.anisotropy = kFilterValues[filter];
                changed = true;
            }
            changed |= CheckRow("Bloom", &s.bloom, "The game's soft glow around bright areas, as on the Xbox.");
            ImGui::EndTable();
        }
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Game")) {
        Space(4);
        if (BeginRows("game")) {
            static const char* const kRates[] = { "30 fps (as the original)", "60 fps (experimental)" };
            int rate = s.fpsLimit >= 60 ? 1 : 0;
            if (ComboRow("Frame rate", &rate, kRates, 2, "The game was made for 30; at 60 some of its logic can glitch.")) {
                s.fpsLimit = rate ? 60 : 30;
                changed = true;
            }
            static const char* const kOverhead[] = { "Off", "Allies only", "Everyone" };
            changed |= ComboRow("Heroes vs Villains: names overhead", &s.hvvOverheadInfo, kOverhead, 3,
                "The fighters' name, health and Force over their heads.");
            changed |= CheckRow("Heroes vs Villains: others' slow motion", &s.hvvOthersSlowMotion,
                "Other fighters' slow motion and saber-lock camera reach you too.");
            ImGui::EndTable();
        }
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Co-op")) {
        Space(4);
        if (BeginRows("coop")) {
            changed |= CheckRow("Co-op in the story missions", &s.coop, "A second player plays the mission's companion.");
            static const char* const kInputs[] = { "Automatic", "Keyboard is player 1, controller player 2", "Two controllers" };
            changed |= ComboRow("Who plays with what", &s.coopInput, kInputs, 3);
            static const char* const kDeaths[] = { "Comes back beside player 1", "The mission is lost" };
            changed |= ComboRow("When player 2 dies", &s.coopDeath, kDeaths, 2);
            static const char* const kCameras[] = { "Shared: both players in view", "Follows player 1" };
            changed |= ComboRow("Camera", &s.coopCamera, kCameras, 2);
            changed |= CheckRow("Player 2 plays the boss", &s.coopBoss, "In boss fights, as in Versus; off: fights beside player 1.");
            changed |= CheckRow("Friendly fire", &s.coopFriendlyFire, "The two players' blows hurt each other.");
            changed |= CheckRow("Leave cutscenes to the game", &s.coopStorySafety, "Recommended.");
            RowLabel("Player 2's character", "Where the mission has no companion, e.g. IObiwan. Empty: automatic.");
            char player2[64] = {};
            snprintf(player2, sizeof(player2), "%s", s.coopPlayer2.c_str());
            if (ImGui::InputText("##value", player2, sizeof(player2))) {
                s.coopPlayer2.clear();
                for (const char* c = player2; *c; ++c)
                    if (*c > 32 && *c < 127)
                        s.coopPlayer2 += *c;
                changed = true;
            }
            ImGui::PopID();
            ImGui::EndTable();
        }
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Advanced")) {
        Space(4);
        ExtraSettings& e = g.extra;
        if (BeginRows("advanced")) {
            if (CheckRow("Debug menu", &e.debugMenu, "The ~ menu: the game's console, variables and developer switches."))
                SetIniFlag(g.ini, L"Debug", L"DebugMenu", e.debugMenu);
            if (CheckRow("Engine debug displays", &e.debugDisplays, "The engine's fps counter, profiler and memory display."))
                SetIniFlag(g.ini, L"Debug", L"DebugDisplays", e.debugDisplays);
            if (CheckRow("Log window", &e.console, "A console window showing the log while the game runs."))
                SetIniFlag(g.ini, L"Debug", L"Console", e.console);
            if (CheckRow("Log resources", &e.logResources, "Every resource a level loads, with the path a mod file must use."))
                SetIniFlag(g.ini, L"Debug", L"LogResources", e.logResources);
            if (CheckRow("Flight recorder", &e.flightRecorder, "Ctrl+Shift+F10 saves the last 3 seconds of frames."))
                SetIniFlag(g.ini, L"Debug", L"FlightRecorder", e.flightRecorder);
            if (CheckRow("Dump resources", &e.dumpResources, "For modders: writes the resources the game loads to dump."))
                SetIniFlag(g.ini, L"Mods", L"DumpResources", e.dumpResources);
            if (CheckRow("Skip the launcher", &e.skipLauncher, "Start the game directly. Shift at start, or --launcher, shows it again."))
                SetIniFlag(g.ini, L"Launcher", L"Skip", e.skipLauncher);
            ImGui::EndTable();
        }
        Space(8);
        if (ImGui::Button("Open settings.ini"))
            ShellExecuteW(g.hwnd, L"open", g.ini.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        ImGui::SameLine();
        if (ImGui::Button("Restore the default settings"))
            g.openResetSettings = true;
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
    if (changed)
        SaveSettings();
}

// --- Pages: controls ------------------------------------------------------------------------------

struct ActionLabel {
    const wchar_t* name;
    const char* label;
    const char* xbox;
};
const ActionLabel kActionLabels[] = {
    { L"MoveForward", "Move forward", "Left stick" },
    { L"MoveBack", "Move back", "Left stick" },
    { L"MoveLeft", "Move left", "Left stick" },
    { L"MoveRight", "Move right", "Left stick" },
    { L"ForceTargetUp", "Force target / deflect up", "Right stick" },
    { L"ForceTargetDown", "Force target / deflect down", "Right stick" },
    { L"ForceTargetLeft", "Force target / deflect left", "Right stick" },
    { L"ForceTargetRight", "Force target / deflect right", "Right stick" },
    { L"Jump", "Jump", "A" },
    { L"FastAttack", "Fast attack", "X" },
    { L"StrongAttack", "Strong attack", "Y" },
    { L"CriticalAttack", "Critical attack / interact", "B" },
    { L"Block", "Block / strafe", "Left trigger" },
    { L"PushGrasp", "Force push / grasp", "Right trigger" },
    { L"SaberThrow", "Saber throw", "White" },
    { L"StunLightning", "Force stun / lightning", "Black" },
    { L"ForceHeal", "Force heal", "Both stick clicks" },
    { L"Pause", "Pause", "Start" },
    { L"Back", "Back", "Back" },
    { L"MenuUp", "Menu up", "D-pad" },
    { L"MenuDown", "Menu down", "D-pad" },
    { L"MenuLeft", "Menu left", "D-pad" },
    { L"MenuRight", "Menu right", "D-pad" },
    { L"MenuAccept", "Menu accept", "A" },
    { L"MenuBack", "Menu back", "B" },
    { L"LeftStickClick", "Left stick click", "Left stick click" },
    { L"RightStickClick", "Right stick click", "Right stick click" },
    { L"Walk", "Walk (hold with the movement keys)", "-" },
};

const ActionLabel* LabelOf(const wchar_t* name)
{
    for (const ActionLabel& label : kActionLabels)
        if (wcscmp(label.name, name) == 0)
            return &label;
    return nullptr;
}

std::string ActionTitle(size_t action)
{
    const ActionLabel* label = LabelOf(g.actions[action].name);
    return label ? std::string(label->label) : Utf8(g.actions[action].name);
}

// Binds `vk` to the action's key `slot` (a new key when the action has fewer), and says which other
// actions have the same key.
void SetBinding(size_t action, int slot, int vk)
{
    std::vector<int>& keys = g.binds[action];
    keys.erase(std::remove(keys.begin(), keys.end(), vk), keys.end());
    if (slot < int(keys.size()))
        keys[size_t(slot)] = vk;
    else
        keys.push_back(vk);
    WriteBinding(action);
    g.controlsNote.clear();
    std::string others;
    for (size_t i = 0; i < g.binds.size(); ++i) {
        if (i == action || std::find(g.binds[i].begin(), g.binds[i].end(), vk) == g.binds[i].end())
            continue;
        if (!others.empty())
            others += ", ";
        others += ActionTitle(i);
    }
    if (!others.empty())
        g.controlsNote = input::KeyName(vk) + " is also bound to: " + others + ".";
}

void ApplyCapturedKey()
{
    if (g.listenAction < 0 || !g.capturedKey)
        return;
    const int vk = g.capturedKey;
    g.capturedKey = 0;
    if (vk == VK_ESCAPE) {
        g.listenAction = -1;
        return;
    }
    if (input::KeyName(vk).empty()) {
        g.controlsNote = "That key cannot be bound. Press another, or Escape to cancel.";
        return;
    }
    SetBinding(size_t(g.listenAction), g.listenSlot, vk);
    g.listenAction = -1;
}

void KeyboardTab()
{
    if (g.listenAction >= 0)
        SmallText("Press a key or a mouse button for this action. Escape cancels.", kGold);
    else if (!g.controlsNote.empty())
        SmallText(g.controlsNote.c_str(), kGold);
    else
        SmallText("Click a key to change it; right-click it to remove it. Each action can have two keys here.");
    {
        if (ImGui::BeginTable("keys", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                ImVec2(0, std::max(S(120), ImGui::GetContentRegionAvail().y - S(2))))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Controller", ImGuiTableColumnFlags_WidthFixed, S(120));
            ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, S(118));
            ImGui::TableSetupColumn("Second key", ImGuiTableColumnFlags_WidthFixed, S(118));
            ImGui::TableSetupColumn("##reset", ImGuiTableColumnFlags_WidthFixed, S(74));
            ImGui::TableHeadersRow();
            for (size_t i = 0; i < g.actions.size(); ++i) {
                const ActionLabel* label = LabelOf(g.actions[i].name);
                ImGui::PushID(int(i));
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(ActionTitle(i).c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::AlignTextToFramePadding();
                ImGui::PushStyleColor(ImGuiCol_Text, Colour(kDim));
                ImGui::TextUnformatted(label ? label->xbox : "");
                ImGui::PopStyleColor();
                for (int slot = 0; slot < 2; ++slot) {
                    ImGui::TableSetColumnIndex(2 + slot);
                    ImGui::PushID(slot);
                    const bool listening = g.listenAction == int(i) && g.listenSlot == slot;
                    const bool bound = slot < int(g.binds[i].size());
                    const std::string name = listening ? std::string("...") : bound ? input::KeyName(g.binds[i][size_t(slot)]) : std::string("-");
                    if (listening)
                        ImGui::PushStyleColor(ImGuiCol_Button, Colour(Faded(kGold, 0.45f)));
                    if (ImGui::Button((name + "##key").c_str(), ImVec2(-FLT_MIN, 0))) {
                        g.listenAction = int(i);
                        // An empty second key next to an empty first one fills the first.
                        g.listenSlot = std::min(slot, int(g.binds[i].size()));
                        g.capturedKey = 0;
                        g.controlsNote.clear();
                    }
                    if (listening)
                        ImGui::PopStyleColor();
                    if (bound && !listening && ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                        g.binds[i].erase(g.binds[i].begin() + slot);
                        WriteBinding(i);
                        g.controlsNote.clear();
                    }
                    ImGui::PopID();
                }
                ImGui::TableSetColumnIndex(4);
                if (ImGui::SmallButton("Default")) {
                    g.binds[i] = input::ParseKeyList(g.actions[i].defaults);
                    WriteBinding(i);
                    g.listenAction = -1;
                    g.controlsNote.clear();
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
}

void MouseTab()
{
    Space(4);
    if (BeginRows("mouse")) {
        if (CheckRow("Mouse movement is the right stick", &g.mouseStick, "To deflect blaster bolts: hold Block and circle the mouse."))
            SetIniFlag(g.controlsIni, L"Mouse", L"RightStick", g.mouseStick);
        RowLabel("Mouse sensitivity", "Higher: less mouse movement for a full push of the stick.");
        if (ImGui::SliderFloat("##value", &g.sensitivity, 0.1f, 5.0f, "%.2f"))
            SetIniFloat(g.controlsIni, L"Mouse", L"Sensitivity", g.sensitivity);
        ImGui::PopID();
        RowLabel("Walk speed", "How far the Walk key pushes the stick while held with the movement keys.");
        if (ImGui::SliderFloat("##value", &g.walkSpeed, 0.1f, 1.0f, "%.2f"))
            SetIniFloat(g.controlsIni, L"Keyboard", L"WalkSpeed", g.walkSpeed);
        ImGui::PopID();
        ImGui::EndTable();
    }
}

void ControllerTab()
{
    Space(4);
    Settings& s = EditSettings();
    if (BeginRows("prompts")) {
        static const char* const kPrompts[] = { "The device used last", "Controller buttons", "Keyboard keys" };
        int prompts = s.buttonPrompts < 0 ? 0 : s.buttonPrompts == 0 ? 1 : 2;
        if (ComboRow("Button prompts on screen", &prompts, kPrompts, 3)) {
            s.buttonPrompts = prompts == 0 ? -1 : prompts == 1 ? 0 : 1;
            SaveSettings();
        }
        ImGui::EndTable();
    }
    SectionTitle("CONTROLLERS");
    ImGui::TextWrapped("Xbox controllers, and anything Windows shows as one (XInput), work with the original layout. "
                       "PlayStation controllers (DualShock 4, DualSense) work directly over USB or Bluetooth, with rumble. "
                       "They can be connected at any time; controllers keep their layout, only the keyboard and mouse "
                       "can be rebound.");
    Space(4);
    if (ImGui::BeginTable("pads", 3, ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Xbox", ImGuiTableColumnFlags_WidthFixed, S(170));
        ImGui::TableSetupColumn("PlayStation", ImGuiTableColumnFlags_WidthFixed, S(190));
        ImGui::TableHeadersRow();
        static const char* const kRows[][3] = {
            { "Move", "Left stick", "Left stick" },
            { "Jump", "A", "Cross" },
            { "Fast / strong / critical attack", "X / Y / B", "Square / Triangle / Circle" },
            { "Block", "Left trigger", "L2" },
            { "Force push / grasp", "Right trigger", "R2" },
            { "Saber throw", "White", "L1" },
            { "Force stun / lightning", "Black", "R1" },
            { "Deflect bolts, Force targeting", "Block + right stick", "L2 + right stick" },
            { "Pause / back", "Start / Back", "Options / Share or touchpad" },
        };
        for (const auto& row : kRows) {
            ImGui::TableNextRow();
            for (int column = 0; column < 3; ++column) {
                ImGui::TableSetColumnIndex(column);
                ImGui::TextUnformatted(row[column]);
            }
        }
        ImGui::EndTable();
    }
}

void ControlsPage()
{
    PageTitle("CONTROLS");
    if (!ImGui::BeginTabBar("controls"))
        return;
    if (ImGui::BeginTabItem("Keyboard")) {
        KeyboardTab();
        ImGui::EndTabItem();
    } else {
        g.listenAction = -1;
    }
    if (ImGui::BeginTabItem("Mouse and walking")) {
        MouseTab();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Controller")) {
        ControllerTab();
        ImGui::EndTabItem();
    }
    if (ImGui::TabItemButton("Restore defaults", ImGuiTabItemFlags_Trailing))
        g.openResetControls = true;
    ImGui::EndTabBar();
}

// --- The frame ------------------------------------------------------------------------------------

void DrawBackground(ImDrawList* draw, ImVec2 size)
{
    draw->AddRectFilledMultiColor(ImVec2(0, 0), size, IM_COL32(9, 12, 22, 255), IM_COL32(9, 12, 22, 255),
        IM_COL32(3, 4, 8, 255), IM_COL32(3, 4, 8, 255));
    const float t = float(ImGui::GetTime());
    for (const Star& star : g.stars) {
        const float twinkle = 0.55f + 0.45f * std::sin(t * 0.8f + star.phase);
        const ImU32 colour = IM_COL32(215, 225, 255, int(40 + 150 * twinkle * star.size));
        const float r = S(0.6f + star.size);
        draw->AddRectFilled(ImVec2(star.x * size.x - r / 2, star.y * size.y - r / 2),
            ImVec2(star.x * size.x + r / 2, star.y * size.y + r / 2), colour);
    }
    // The header: the emblem, the title, a gold rule fading to the right.
    float x = S(kMargin);
    if (g.emblem) {
        draw->AddImage(reinterpret_cast<ImTextureID>(g.emblem), ImVec2(x, S(16)), ImVec2(x + S(76), S(92)));
        x += S(92);
    }
    Heading(draw, ImVec2(x, S(26)), S(15), kGold, "STAR WARS EPISODE III");
    Heading(draw, ImVec2(x, S(50)), S(34), kWhite, "REVENGE OF THE SITH");
    draw->AddRectFilledMultiColor(ImVec2(S(kMargin), S(kHeaderBottom)), ImVec2(size.x - S(kMargin), S(kHeaderBottom) + S(2)),
        kGold, Faded(kGold, 0.0f), Faded(kGold, 0.0f), kGold);
    // The footer.
    const std::string version = std::string("SWROTS-PC ") + SWROTS_VERSION;
    draw->AddText(g.small, g.small->FontSize, ImVec2(S(kMargin), size.y - S(30)), kDim, version.c_str());
    const char* note = "Unofficial port. Needs your own copy of the game.";
    const float width = g.small->CalcTextSizeA(g.small->FontSize, FLT_MAX, 0.0f, note).x;
    draw->AddText(g.small, g.small->FontSize, ImVec2(size.x - S(kMargin) - width, size.y - S(30)), kDim, note);
}

bool NavItem(const char* label, bool selected, float y)
{
    ImGui::SetCursorPos(P(kMargin, y));
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const ImVec2 size = P(kNavWidth, 44);
    const bool clicked = ImGui::InvisibleButton(label, size);
    const bool hovered = ImGui::IsItemHovered() || ImGui::IsItemFocused();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (selected || hovered)
        draw->AddRectFilled(at, ImVec2(at.x + size.x, at.y + size.y), IM_COL32(255, 255, 255, selected ? 16 : 9));
    if (selected)
        draw->AddRectFilled(at, ImVec2(at.x + S(4), at.y + size.y), kGold);
    Heading(draw, ImVec2(at.x + S(18), at.y + (size.y - S(19)) / 2), S(19), selected ? kGold : hovered ? kWhite : kDim, label);
    return clicked;
}

// A modal in the launcher's look; returns whether it is open (then End with ImGui::EndPopup).
bool BeginModal(const char* id, const char* title)
{
    const ImVec2 centre(ImGui::GetIO().DisplaySize.x / 2, ImGui::GetIO().DisplaySize.y / 2);
    ImGui::SetNextWindowPos(centre, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(S(540), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, P(24, 20));
    const bool open = ImGui::BeginPopupModal(id, nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar();
    if (open) {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        Heading(ImGui::GetWindowDrawList(), at, S(20), kGold, title);
        ImGui::Dummy(ImVec2(1, S(20)));
        Space(6);
    }
    return open;
}

void DrawModals()
{
    if (g.openUninstall) {
        ImGui::OpenPopup("uninstall");
        g.openUninstall = false;
    }
    if (g.openStop) {
        ImGui::OpenPopup("stop");
        g.openStop = false;
    }
    if (g.openResetSettings) {
        ImGui::OpenPopup("resetsettings");
        g.openResetSettings = false;
    }
    if (g.openResetControls) {
        ImGui::OpenPopup("resetcontrols");
        g.openResetControls = false;
    }
    if (BeginModal("uninstall", "UNINSTALL THE GAME FILES")) {
        ImGui::TextWrapped("These are deleted for good (they do not go to the Recycle Bin):");
        Space(2);
        ImGui::BulletText("The game files: GameData, the cache, the icon and the shortcuts");
        if (g.uninstall.saves)
            ImGui::BulletText("Your saved games (saves)");
        if (g.uninstall.settings)
            ImGui::BulletText("Your settings and controls (back to the defaults)");
        if (g.uninstall.mods)
            ImGui::BulletText("Your mods (mods)");
        Space(2);
        DimText(g.uninstall.saves ? "Your disc image is not touched."
                                  : "Your saved games are kept, and your disc image is not touched.");
        Space(12);
        if (BigButton("DELETE", P(180, 44), true)) {
            StartUninstall();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine(0, S(14));
        if (BigButton("KEEP", P(180, 44), false))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (BeginModal("stop", "STOP THE INSTALLATION")) {
        ImGui::TextWrapped("The game files are still being copied. Stop setting up the game and close the launcher?");
        Space(12);
        if (BigButton("STOP", P(180, 44), false)) {
            if (g.job)
                g.job->cancel = true;
            g.quit = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine(0, S(14));
        if (BigButton("CONTINUE", P(180, 44), true))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (BeginModal("resetsettings", "DEFAULT SETTINGS")) {
        ImGui::TextWrapped("Put every setting of this page back to its default? The controls are not changed.");
        Space(12);
        if (BigButton("RESTORE", P(180, 44), true)) {
            ResetSettings();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine(0, S(14));
        if (BigButton("CANCEL", P(180, 44), false))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (BeginModal("resetcontrols", "DEFAULT CONTROLS")) {
        ImGui::TextWrapped("Put every key, the mouse sensitivity and the walk speed back to their defaults?");
        Space(12);
        if (BigButton("RESTORE", P(180, 44), true)) {
            ResetControls();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine(0, S(14));
        if (BigButton("CANCEL", P(180, 44), false))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void DrawFrame()
{
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(display);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("launcher", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground
        | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    DrawBackground(draw, display);

    // The menu, as the game's: a column of entries, the chosen one in gold.
    const bool busy = g.job || g.uninstalling;
    float y = kHeaderBottom + 26;
    const Page before = g.page;
    if (NavItem(g.game == GameState::Installed ? "PLAY" : "INSTALL", g.page == Page::Home, y))
        g.page = Page::Home;
    if (NavItem("SETTINGS", g.page == Page::Settings, y += 48))
        g.page = Page::Settings;
    if (NavItem("CONTROLS", g.page == Page::Controls, y += 48))
        g.page = Page::Controls;
    if (NavItem("INSTALLATION", g.page == Page::Installation, y += 48))
        g.page = Page::Installation;
    if (NavItem("QUIT", false, kHeight - 52 - 44))
        g.closeRequested = true;
    if (g.page != before) {
        g.listenAction = -1;
        g.controlsNote.clear();
    }

    // The page, in a panel with gold corners.
    const ImVec2 p0 = P(kMargin + kNavWidth + 24, kHeaderBottom + 20), p1(display.x - S(kMargin), display.y - S(52));
    draw->AddRectFilled(p0, p1, kPanel);
    draw->AddRect(p0, p1, kEdge);
    const float corner = S(14);
    draw->AddLine(p0, ImVec2(p0.x + corner, p0.y), kGold, S(2));
    draw->AddLine(p0, ImVec2(p0.x, p0.y + corner), kGold, S(2));
    draw->AddLine(p1, ImVec2(p1.x - corner, p1.y), kGold, S(2));
    draw->AddLine(p1, ImVec2(p1.x, p1.y - corner), kGold, S(2));
    ImGui::SetCursorPos(p0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, P(24, 18));
    if (ImGui::BeginChild("page", ImVec2(p1.x - p0.x, p1.y - p0.y), ImGuiChildFlags_AlwaysUseWindowPadding)) {
        switch (g.page) {
        case Page::Home: HomePage(); break;
        case Page::Settings: SettingsPage(); break;
        case Page::Controls: ControlsPage(); break;
        case Page::Installation: InstallationPage(); break;
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();

    if (g.closeRequested) {
        g.closeRequested = false;
        if (g.job && !g.job->cancel)
            g.openStop = true;
        else if (!busy || g.job)
            g.quit = true;
    }
    DrawModals();
    ImGui::End();
}

void Render()
{
    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    DrawFrame();
    ImGui::EndFrame();
    g.device->SetRenderState(D3DRS_ZENABLE, FALSE);
    g.device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g.device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    g.device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(3, 4, 8), 1.0f, 0);
    if (SUCCEEDED(g.device->BeginScene())) {
        ImGui::Render();
        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
        g.device->EndScene();
    }
    if (g.device->Present(nullptr, nullptr, nullptr, nullptr) == D3DERR_DEVICELOST)
        g.deviceLost = true;
}

// True when the device can draw again (after the display mode changed, the session was locked...).
bool RecoverDevice()
{
    const HRESULT state = g.device->TestCooperativeLevel();
    if (state == D3DERR_DEVICELOST)
        return false;
    if (state == D3DERR_DEVICENOTRESET) {
        ImGui_ImplDX9_InvalidateDeviceObjects();
        if (FAILED(g.device->Reset(&g.pp)))
            return false;
        ImGui_ImplDX9_CreateDeviceObjects();
    }
    g.deviceLost = false;
    return true;
}

// --- The window -----------------------------------------------------------------------------------

// The key of a WM_KEYDOWN, with the left and right Shift, Ctrl and Alt told apart (as controls.ini names them).
int KeyOfMessage(WPARAM wParam, LPARAM lParam)
{
    const int vk = int(wParam);
    const bool extended = (lParam & 0x01000000) != 0;
    switch (vk) {
    case VK_SHIFT: {
        const UINT sided = MapVirtualKeyW(UINT((lParam >> 16) & 0xFF), MAPVK_VSC_TO_VK_EX);
        return sided == VK_RSHIFT ? VK_RSHIFT : VK_LSHIFT;
    }
    case VK_CONTROL: return extended ? VK_RCONTROL : VK_LCONTROL;
    case VK_MENU: return extended ? VK_RMENU : VK_LMENU;
    }
    return vk;
}

LRESULT CALLBACK LauncherProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    // While a key is being chosen for an action, keys and mouse buttons are that choice, not the launcher's.
    if (g.listenAction >= 0) {
        switch (msg) {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            g.capturedKey = KeyOfMessage(wParam, lParam);
            return 0;
        case WM_KEYUP:
        case WM_SYSKEYUP:
        case WM_CHAR:
        case WM_SYSCHAR:
            return 0;
        case WM_LBUTTONDOWN: g.capturedKey = VK_LBUTTON; return 0;
        case WM_RBUTTONDOWN: g.capturedKey = VK_RBUTTON; return 0;
        case WM_MBUTTONDOWN: g.capturedKey = VK_MBUTTON; return 0;
        case WM_XBUTTONDOWN:
            g.capturedKey = HIWORD(wParam) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2;
            return TRUE;
        case WM_KILLFOCUS:
            g.listenAction = -1;
            break;
        }
    }
    if (ImGui::GetCurrentContext() && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
        return 1;
    switch (msg) {
    case WM_CLOSE:
        g.closeRequested = true;
        return 0;
    case WM_DROPFILES: {
        HDROP drop = reinterpret_cast<HDROP>(wParam);
        wchar_t file[MAX_PATH] = {};
        if (DragQueryFileW(drop, 0, file, MAX_PATH))
            g.droppedFile = file;
        DragFinish(drop);
        return 0;
    }
    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == SC_KEYMENU) // Alt alone must not open the window's menu
            return 0;
        break;
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool CreateLauncherWindow()
{
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = LauncherProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);

    // The layout's size at the screen's scale, made smaller on a screen it would not fit.
    const UINT dpi = GetDpiForSystem();
    RECT work = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    g.scale = float(dpi ? dpi : 96) / 96.0f;
    const float fitX = float(work.right - work.left) * 0.94f / kWidth, fitY = float(work.bottom - work.top) * 0.9f / kHeight;
    if (fitX > 0.3f && fitY > 0.3f)
        g.scale = std::min({ g.scale, fitX, fitY });
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r = { 0, 0, LONG(kWidth * g.scale), LONG(kHeight * g.scale) };
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, dpi ? dpi : 96);
    const int w = r.right - r.left, h = r.bottom - r.top;
    g.hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, kClass, kTitle, style, work.left + (work.right - work.left - w) / 2,
        work.top + (work.bottom - work.top - h) / 2, w, h, nullptr, nullptr, wc.hInstance, nullptr);
    return g.hwnd != nullptr;
}

bool CreateDevice()
{
    g.d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!g.d3d)
        return false;
    g.pp = {};
    g.pp.Windowed = TRUE;
    g.pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    g.pp.BackBufferFormat = D3DFMT_UNKNOWN;
    g.pp.hDeviceWindow = g.hwnd;
    g.pp.EnableAutoDepthStencil = FALSE;
    g.pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    for (DWORD processing : { DWORD(D3DCREATE_HARDWARE_VERTEXPROCESSING), DWORD(D3DCREATE_SOFTWARE_VERTEXPROCESSING) }) {
        if (SUCCEEDED(g.d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g.hwnd, processing | D3DCREATE_FPU_PRESERVE,
                &g.pp, &g.device)))
            return true;
    }
    g.d3d->Release();
    g.d3d = nullptr;
    return false;
}

// A system font for the text (the game's font has capitals only): the first of `names` Windows has.
ImFont* AddSystemFont(std::initializer_list<const wchar_t*> names, float pixels)
{
    wchar_t windows[MAX_PATH] = {};
    GetWindowsDirectoryW(windows, MAX_PATH);
    ImGuiIO& io = ImGui::GetIO();
    for (const wchar_t* name : names) {
        const std::wstring path = std::wstring(windows) + L"\\Fonts\\" + name;
        if (!Exists(path))
            continue;
        if (ImFont* font = io.Fonts->AddFontFromFileTTF(Utf8(path).c_str(), pixels))
            return font;
    }
    ImFontConfig config;
    config.SizePixels = std::max(13.0f, std::floor(pixels / 13.0f) * 13.0f);
    return io.Fonts->AddFontDefault(&config);
}

void ApplyStyle()
{
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = style.ChildRounding = style.FrameRounding = style.PopupRounding = 0.0f;
    style.ScrollbarRounding = style.GrabRounding = style.TabRounding = 0.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FramePadding = ImVec2(9, 5);
    style.ItemSpacing = ImVec2(10, 7);
    style.CellPadding = ImVec2(6, 5);
    style.ScrollbarSize = 12.0f;
    style.GrabMinSize = 12.0f;
    style.TabBarBorderSize = 1.0f;
    ImVec4* c = style.Colors;
    const ImVec4 gold = Colour(kGold);
    auto goldAt = [gold](float alpha) { return ImVec4(gold.x, gold.y, gold.z, alpha); };
    auto white = [](float alpha) { return ImVec4(1.0f, 1.0f, 1.0f, alpha); };
    c[ImGuiCol_Text] = Colour(kWhite);
    c[ImGuiCol_TextDisabled] = Colour(kDim);
    c[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = ImVec4(0.04f, 0.05f, 0.08f, 0.98f);
    c[ImGuiCol_Border] = Colour(kEdge);
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = white(0.05f);
    c[ImGuiCol_FrameBgHovered] = white(0.11f);
    c[ImGuiCol_FrameBgActive] = white(0.17f);
    c[ImGuiCol_Button] = white(0.07f);
    c[ImGuiCol_ButtonHovered] = goldAt(0.30f);
    c[ImGuiCol_ButtonActive] = goldAt(0.45f);
    c[ImGuiCol_Header] = goldAt(0.22f);
    c[ImGuiCol_HeaderHovered] = goldAt(0.32f);
    c[ImGuiCol_HeaderActive] = goldAt(0.45f);
    c[ImGuiCol_CheckMark] = gold;
    c[ImGuiCol_SliderGrab] = gold;
    c[ImGuiCol_SliderGrabActive] = ImVec4(1.0f, 0.86f, 0.55f, 1.0f);
    c[ImGuiCol_Tab] = white(0.04f);
    c[ImGuiCol_TabHovered] = goldAt(0.30f);
    c[ImGuiCol_TabSelected] = goldAt(0.20f);
    c[ImGuiCol_TabSelectedOverline] = gold;
    c[ImGuiCol_TabDimmed] = white(0.04f);
    c[ImGuiCol_TabDimmedSelected] = goldAt(0.20f);
    c[ImGuiCol_TabDimmedSelectedOverline] = gold;
    c[ImGuiCol_Separator] = Colour(kEdge);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = white(0.22f);
    c[ImGuiCol_ScrollbarGrabHovered] = white(0.34f);
    c[ImGuiCol_ScrollbarGrabActive] = goldAt(0.70f);
    c[ImGuiCol_TableHeaderBg] = white(0.07f);
    c[ImGuiCol_TableBorderStrong] = Colour(kEdge);
    c[ImGuiCol_TableBorderLight] = white(0.08f);
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = white(0.03f);
    c[ImGuiCol_TextSelectedBg] = goldAt(0.35f);
    c[ImGuiCol_NavCursor] = Colour(kBlue);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.62f);
    style.ScaleAllSizes(g.scale);
}

void MakeStars()
{
    uint32_t seed = 0x5EED1977;
    auto next = [&seed] {
        seed = seed * 1664525u + 1013904223u;
        return float(seed >> 8) / float(1u << 24);
    };
    g.stars.clear();
    for (int i = 0; i < 150; ++i) {
        Star star;
        star.x = next();
        star.y = next();
        const float size = next();
        star.size = size * size; // few large ones
        star.phase = next() * 6.2832f;
        g.stars.push_back(star);
    }
}

std::wstring QuoteArgument(const std::wstring& argument)
{
    if (!argument.empty() && argument.find_first_of(L" \t\"") == std::wstring::npos)
        return argument;
    std::wstring quoted = L"\"";
    size_t backslashes = 0;
    for (wchar_t c : argument) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        // Backslashes count double before a quote (and the closing one), single otherwise.
        quoted.append(c == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
        quoted += c;
        backslashes = 0;
    }
    quoted.append(backslashes * 2, L'\\');
    quoted += L'"';
    return quoted;
}

} // namespace

LauncherResult RunLauncher(const std::wstring& exeDir, const std::wstring& installImage)
{
    g.exeDir = exeDir;
    g.gameData = exeDir + L"\\GameData";
    g.ini = exeDir + L"\\settings.ini";
    g.controlsIni = exeDir + L"\\controls.ini";
    if (!CreateLauncherWindow() || !CreateDevice()) {
        LOG_WARN("Launcher: its window could not be made; starting without it");
        if (g.hwnd)
            DestroyWindow(g.hwnd);
        g.hwnd = nullptr;
        return LauncherResult::Unavailable;
    }
    LOG_INFO("Launcher: shown (scale %.2f)", g.scale);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    g.body = AddSystemFont({ L"segoeui.ttf", L"tahoma.ttf", L"arial.ttf" }, std::floor(17.0f * g.scale));
    g.small = AddSystemFont({ L"segoeui.ttf", L"tahoma.ttf", L"arial.ttf" }, std::floor(14.0f * g.scale));
    g.head = AddSystemFont({ L"segoeuib.ttf", L"tahomabd.ttf", L"arialbd.ttf" }, std::floor(34.0f * g.scale));
    ApplyStyle();
    ImGui_ImplWin32_Init(g.hwnd);
    ImGui_ImplDX9_Init(g.device);
    MakeStars();

    LoadExtraSettings();
    LoadControlsPage();
    {
        const Settings& s = GetSettings();
        g.customSize = std::none_of(std::begin(kWindowSizes), std::end(kWindowSizes),
            [&s](const WindowSize& size) { return size.width == s.width && size.height == s.height; });
    }
    RefreshGameState();
    if (g.game == GameState::Installed)
        StartFontThread();
    else if (!installImage.empty() && g.game == GameState::NotInstalled)
        StartInstall(installImage);

    ShowWindow(g.hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(g.hwnd);
    UpdateWindow(g.hwnd);

    for (;;) {
        const ULONGLONG frameStart = GetTickCount64();
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT)
                g.quit = true;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        PollInstall();
        PollUninstall();
        PollFont();
        ApplyCapturedKey();
        // What must not run in the middle of a frame: Windows' file dialog, a dropped file.
        if (g.chooseImage) {
            g.chooseImage = false;
            const std::wstring image = ChooseDiscImage(g.hwnd);
            if (!image.empty())
                StartInstall(image);
        }
        if (!g.droppedFile.empty()) {
            const std::wstring image = std::move(g.droppedFile);
            g.droppedFile.clear();
            if (g.game == GameState::NotInstalled) {
                g.page = Page::Home;
                StartInstall(image);
            }
        }
        const bool busy = g.job || g.uninstalling;
        if ((g.quit || g.play) && !busy)
            break;
        if (g.quit && g.job)
            g.job->cancel = true;
        if (IsIconic(g.hwnd) || (g.deviceLost && !RecoverDevice())) {
            Sleep(50);
            continue;
        }
        Render();
        // Without vertical sync (a driver setting) the loop would spin: at most about 100 frames a second.
        const ULONGLONG spent = GetTickCount64() - frameStart;
        if (spent < 10)
            Sleep(DWORD(10 - spent));
    }

    const bool play = g.play && !g.quit && g.game == GameState::Installed;
    StopFontThread();
    ReleaseGameArt();
    ImGui_ImplDX9_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g.device->Release();
    g.device = nullptr;
    g.d3d->Release();
    g.d3d = nullptr;
    DestroyWindow(g.hwnd);
    g.hwnd = nullptr;
    UnregisterClassW(kClass, GetModuleHandleW(nullptr));
    LOG_INFO("Launcher: %s", play ? "play" : "closed");
    return play ? LauncherResult::Play : LauncherResult::Quit;
}

bool StartGameProcess(const std::wstring& exeDir)
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring command = L"\"" + std::wstring(exe) + L"\" --from-launcher";
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i) {
        if (wcscmp(argv[i], L"--install") == 0) { // and its image: installed by now
            ++i;
            continue;
        }
        if (wcscmp(argv[i], L"--launcher") == 0)
            continue;
        command += L" " + QuoteArgument(argv[i]);
    }
    if (argv)
        LocalFree(argv);
    LOG_INFO("Launcher: starting the game");
    LogFlush();
    LogClose(); // the game's process continues this log
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE, 0, nullptr, exeDir.c_str(), &si, &pi)) {
        const DWORD error = GetLastError();
        MessageBoxW(nullptr, (L"The game could not be started (Windows error " + std::to_wstring(error) + L").").c_str(),
            kTitle, MB_OK | MB_ICONERROR);
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

} // namespace swrots
