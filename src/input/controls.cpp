#include "input/controls.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/window.h"
#include "debug/menu.h"

namespace swrots::input {

namespace {

enum : WORD {
    XB_DPAD_UP = 0x0001, XB_DPAD_DOWN = 0x0002, XB_DPAD_LEFT = 0x0004, XB_DPAD_RIGHT = 0x0008,
    XB_START = 0x0010, XB_BACK = 0x0020, XB_LEFT_THUMB = 0x0040, XB_RIGHT_THUMB = 0x0080,
};
enum { XB_A, XB_B, XB_X, XB_Y, XB_BLACK, XB_WHITE, XB_LEFT_TRIGGER, XB_RIGHT_TRIGGER, XB_NONE = -1 };
enum Stick { NoStick, LeftUp, LeftDown, LeftLeft, LeftRight, RightUp, RightDown, RightLeft, RightRight };

// A game action: the controller inputs it presses, and its default keys.
struct Action {
    const wchar_t* name;
    WORD buttons;
    int analog;
    Stick stick;
    const char* defaults;
    std::vector<int> keys;
};

// Named after the game manual's controls page.
Action g_Actions[] = {
    { L"MoveForward", 0, XB_NONE, LeftUp, "W" },
    { L"MoveBack", 0, XB_NONE, LeftDown, "S" },
    { L"MoveLeft", 0, XB_NONE, LeftLeft, "A" },
    { L"MoveRight", 0, XB_NONE, LeftRight, "D" },
    { L"ForceTargetUp", 0, XB_NONE, RightUp, "I" },
    { L"ForceTargetDown", 0, XB_NONE, RightDown, "K" },
    { L"ForceTargetLeft", 0, XB_NONE, RightLeft, "J" },
    { L"ForceTargetRight", 0, XB_NONE, RightRight, "L" },
    { L"Jump", 0, XB_A, NoStick, "Space" },
    { L"FastAttack", 0, XB_X, NoStick, "Mouse1" },
    { L"StrongAttack", 0, XB_Y, NoStick, "Mouse2" },
    { L"CriticalAttack", 0, XB_B, NoStick, "E, Mouse3" }, // also interacts with objects
    { L"Block", 0, XB_LEFT_TRIGGER, NoStick, "Shift" },   // block / strafe
    { L"PushGrasp", 0, XB_RIGHT_TRIGGER, NoStick, "F" },
    { L"SaberThrow", 0, XB_WHITE, NoStick, "Q" },
    { L"StunLightning", 0, XB_BLACK, NoStick, "R" },
    { L"ForceHeal", XB_LEFT_THUMB | XB_RIGHT_THUMB, XB_NONE, NoStick, "H" },
    { L"Pause", XB_START, XB_NONE, NoStick, "Escape" },
    { L"Back", XB_BACK, XB_NONE, NoStick, "Tab" },
    { L"MenuUp", XB_DPAD_UP, XB_NONE, NoStick, "Up" },
    { L"MenuDown", XB_DPAD_DOWN, XB_NONE, NoStick, "Down" },
    { L"MenuLeft", XB_DPAD_LEFT, XB_NONE, NoStick, "Left" },
    { L"MenuRight", XB_DPAD_RIGHT, XB_NONE, NoStick, "Right" },
    { L"MenuAccept", 0, XB_A, NoStick, "Enter" },
    { L"MenuBack", 0, XB_B, NoStick, "Backspace" },
    { L"LeftStickClick", XB_LEFT_THUMB, XB_NONE, NoStick, "" },
    { L"RightStickClick", XB_RIGHT_THUMB, XB_NONE, NoStick, "" },
    // Not a controller input: while held, keyboard movement is a walk instead of a run.
    { L"Walk", 0, XB_NONE, NoStick, "LCtrl" },
};

// While walking, keyboard movement deflects the left stick this far ([Keyboard] WalkSpeed).
float g_WalkDeflection = 0.5f;

// Mouse movement as the right stick (deflecting blaster bolts: hold Block and circle the
// stick; Force targeting). [Mouse] in controls.ini.
bool g_MouseRightStick = true;
float g_MouseSensitivity = 1.0f;
std::atomic<LONG> g_MouseDx = 0, g_MouseDy = 0;
float g_StickX = 0.0f, g_StickY = 0.0f;
ULONGLONG g_LastMouseRead = 0;

struct KeyName {
    const char* name;
    int vk;
};
const KeyName kKeyNames[] = {
    { "Space", VK_SPACE }, { "Enter", VK_RETURN }, { "Escape", VK_ESCAPE }, { "Tab", VK_TAB },
    { "Backspace", VK_BACK }, { "Shift", VK_SHIFT }, { "LShift", VK_LSHIFT }, { "RShift", VK_RSHIFT },
    { "Ctrl", VK_CONTROL }, { "LCtrl", VK_LCONTROL }, { "RCtrl", VK_RCONTROL }, { "Alt", VK_MENU },
    { "LAlt", VK_LMENU }, { "RAlt", VK_RMENU }, { "Up", VK_UP }, { "Down", VK_DOWN }, { "Left", VK_LEFT },
    { "Right", VK_RIGHT }, { "Insert", VK_INSERT }, { "Delete", VK_DELETE }, { "Home", VK_HOME },
    { "End", VK_END }, { "PageUp", VK_PRIOR }, { "PageDown", VK_NEXT }, { "CapsLock", VK_CAPITAL },
    { "Mouse1", VK_LBUTTON }, { "Mouse2", VK_RBUTTON }, { "Mouse3", VK_MBUTTON }, { "Mouse4", VK_XBUTTON1 },
    { "Mouse5", VK_XBUTTON2 }, { "Comma", VK_OEM_COMMA }, { "Period", VK_OEM_PERIOD }, { "Minus", VK_OEM_MINUS },
    { "Plus", VK_OEM_PLUS }, { "Semicolon", VK_OEM_1 }, { "Slash", VK_OEM_2 }, { "Tilde", VK_OEM_3 },
    { "LBracket", VK_OEM_4 }, { "Backslash", VK_OEM_5 }, { "RBracket", VK_OEM_6 }, { "Quote", VK_OEM_7 },
};

// "W", "F5", "Numpad3", "Mouse1", "Space", ... -> virtual key (0 if unknown).
int ParseKey(std::string name)
{
    name.erase(std::remove_if(name.begin(), name.end(), [](unsigned char c) { return std::isspace(c); }), name.end());
    if (name.empty())
        return 0;
    if (name.size() == 1) {
        char c = char(std::toupper(static_cast<unsigned char>(name[0])));
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            return c;
    }
    for (const KeyName& k : kKeyNames)
        if (_stricmp(k.name, name.c_str()) == 0)
            return k.vk;
    if ((name[0] == 'F' || name[0] == 'f') && name.size() <= 3) {
        int n = atoi(name.c_str() + 1);
        if (n >= 1 && n <= 24)
            return VK_F1 + n - 1;
    }
    if (_strnicmp(name.c_str(), "Numpad", 6) == 0 && name.size() == 7 && std::isdigit(static_cast<unsigned char>(name[6])))
        return VK_NUMPAD0 + (name[6] - '0');
    return 0;
}

std::vector<int> ParseKeys(const std::string& list)
{
    std::vector<int> keys;
    size_t start = 0;
    while (start <= list.size()) {
        size_t comma = list.find(',', start);
        std::string one = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (int vk = ParseKey(one))
            keys.push_back(vk);
        else if (one.find_first_not_of(" \t") != std::string::npos)
            LOG_WARN("controls.ini: unknown key '%s'", one.c_str());
        if (comma == std::string::npos)
            break;
        start = comma + 1;
    }
    return keys;
}

bool IsMouseButton(int vk)
{
    return vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2;
}

} // namespace

void LoadControls(const std::wstring& path)
{
    const bool exists = GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (!exists) {
        FILE* f = _wfopen(path.c_str(), L"w");
        if (f) {
            fputs("; Keyboard and mouse controls. Each action takes one or more keys, separated by commas.\n"
                  "; Keys: A-Z, 0-9, F1-F24, Numpad0-9, Space, Enter, Escape, Tab, Backspace, Shift, Ctrl, Alt\n"
                  "; (L/R variants too), Up, Down, Left, Right, Insert, Delete, Home, End, PageUp, PageDown,\n"
                  "; Mouse1-Mouse5. Leave an action empty to unbind it. Controllers keep their normal layout.\n"
                  "; Walk: held with the movement keys, walk instead of run.\n"
                  "; [Mouse] RightStick=1: mouse movement moves the right stick (circle it to deflect blaster\n"
                  "; bolts while blocking); Sensitivity scales it.\n\n",
                f);
            fclose(f);
        }
    }
    for (Action& a : g_Actions) {
        wchar_t value[256] = {};
        // "\x1" marks a missing key (an empty value means the action is unbound).
        GetPrivateProfileStringW(L"Keyboard", a.name, L"\x1", value, 256, path.c_str());
        if (!exists || value[0] == 1) {
            std::wstring def(a.defaults, a.defaults + strlen(a.defaults));
            WritePrivateProfileStringW(L"Keyboard", a.name, def.c_str(), path.c_str());
            wcsncpy_s(value, def.c_str(), _TRUNCATE);
        }
        std::string narrow;
        for (const wchar_t* c = value; *c; ++c)
            narrow += char(*c < 128 ? *c : '?');
        a.keys = ParseKeys(narrow);
    }
    wchar_t value[32] = {};
    GetPrivateProfileStringW(L"Mouse", L"RightStick", L"\x1", value, 32, path.c_str());
    if (value[0] == 1) {
        WritePrivateProfileStringW(L"Mouse", L"RightStick", L"1", path.c_str());
        WritePrivateProfileStringW(L"Mouse", L"Sensitivity", L"1.0", path.c_str());
    }
    GetPrivateProfileStringW(L"Keyboard", L"WalkSpeed", L"\x1", value, 32, path.c_str());
    if (value[0] == 1)
        WritePrivateProfileStringW(L"Keyboard", L"WalkSpeed", L"0.5", path.c_str());
    GetPrivateProfileStringW(L"Keyboard", L"WalkSpeed", L"0.5", value, 32, path.c_str());
    g_WalkDeflection = std::clamp(float(_wtof(value)), 0.1f, 1.0f);
    g_MouseRightStick = GetPrivateProfileIntW(L"Mouse", L"RightStick", 1, path.c_str()) != 0;
    GetPrivateProfileStringW(L"Mouse", L"Sensitivity", L"1.0", value, 32, path.c_str());
    g_MouseSensitivity = std::clamp(float(_wtof(value)), 0.05f, 20.0f);
    LOG_INFO("Controls loaded from %ls", path.c_str());
}

std::vector<int> ActionKeys(const wchar_t* action)
{
    for (const Action& a : g_Actions)
        if (_wcsicmp(a.name, action) == 0)
            return a.keys;
    return {};
}

bool MouseIsRightStick() { return g_MouseRightStick; }

void AddMouseMotion(LONG dx, LONG dy)
{
    g_MouseDx += dx;
    g_MouseDy += dy;
}

std::atomic<LONG> g_MouseWheel = 0;

void AddMouseWheel(SHORT delta)
{
    g_MouseWheel += delta;
}

void TakeMouseInput(LONG& dx, LONG& dy, LONG& wheel)
{
    dx = g_MouseDx.exchange(0);
    dy = g_MouseDy.exchange(0);
    wheel = g_MouseWheel.exchange(0);
}

// The mouse's recent motion as a stick position: movement pushes the stick, which springs
// back to the centre within about a tenth of a second when the mouse stops.
static void MouseStick(KeyboardPad& pad, bool active)
{
    const ULONGLONG now = GetTickCount64();
    const float dt = g_LastMouseRead ? std::min(float(now - g_LastMouseRead), 100.0f) / 1000.0f : 0.0f;
    g_LastMouseRead = now;
    const LONG dx = g_MouseDx.exchange(0), dy = g_MouseDy.exchange(0);
    g_MouseWheel = 0; // only the free camera uses it
    if (!active || !g_MouseRightStick) {
        g_StickX = g_StickY = 0.0f;
        return;
    }
    const float push = 0.02f * g_MouseSensitivity; // stick per mouse count
    const float decay = std::exp(-dt / 0.08f);
    g_StickX = std::clamp(g_StickX * decay + dx * push, -1.0f, 1.0f);
    g_StickY = std::clamp(g_StickY * decay - dy * push, -1.0f, 1.0f); // mouse up = stick up
    if (!pad.rx)
        pad.rx = SHORT(g_StickX * 32767.0f);
    if (!pad.ry)
        pad.ry = SHORT(g_StickY * 32767.0f);
}

KeyboardPad ReadKeyboardPad()
{
    KeyboardPad pad = {};
    if (!GameWindowActive() || debug::MenuOpen()) { // the debug menu takes the keyboard
        MouseStick(pad, false);
        return pad;
    }
    const bool mouse = MouseButtonsAreGameInput();
    auto down = [mouse](int vk) { return (!IsMouseButton(vk) || mouse) && (GetAsyncKeyState(vk) & 0x8000) != 0; };
    bool walk = false;
    for (const Action& a : g_Actions) {
        if (!std::any_of(a.keys.begin(), a.keys.end(), down))
            continue;
        if (wcscmp(a.name, L"Walk") == 0) {
            walk = true;
            continue;
        }
        pad.buttons |= a.buttons;
        if (a.analog != XB_NONE)
            pad.analog[a.analog] = 0xFF;
        switch (a.stick) {
        case LeftUp: pad.ly = 32767; break;
        case LeftDown: pad.ly = -32768; break;
        case LeftLeft: pad.lx = -32768; break;
        case LeftRight: pad.lx = 32767; break;
        case RightUp: pad.ry = 32767; break;
        case RightDown: pad.ry = -32768; break;
        case RightLeft: pad.rx = -32768; break;
        case RightRight: pad.rx = 32767; break;
        case NoStick: break;
        }
    }
    if (walk) {
        pad.lx = SHORT(pad.lx * g_WalkDeflection);
        pad.ly = SHORT(pad.ly * g_WalkDeflection);
    }
    MouseStick(pad, mouse);
    return pad;
}

} // namespace swrots::input
