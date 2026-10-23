// Heroes vs Villains' screen (see hvvhud.h).

#include "debug/hvvhud.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "debug/console.h"
#include "debug/gamefont.h"
#include "game/freecam.h"
#include "game/hvv.h"
#include "imgui.h"

namespace swrots::debug {

namespace {

constexpr ImU32 kHeroes = IM_COL32(110, 175, 255, 255);
constexpr ImU32 kVillains = IM_COL32(255, 85, 70, 255);
constexpr ImU32 kGold = IM_COL32(255, 196, 90, 255);
constexpr ImU32 kWhite = IM_COL32(235, 235, 235, 255);
constexpr ImU32 kDim = IM_COL32(110, 110, 110, 220);
constexpr ImU32 kPanel = IM_COL32(10, 12, 18, 175);
constexpr ImU32 kEdge = IM_COL32(200, 200, 210, 110);

ImU32 SideColour(int side) { return side == 0 ? kHeroes : side == 1 ? kVillains : kWhite; }

ImU32 Faded(ImU32 colour, float alpha)
{
    const ImU32 a = ImU32(float(colour >> 24) * std::clamp(alpha, 0.0f, 1.0f));
    return (colour & 0x00FFFFFF) | (a << 24);
}

ImDrawList* g_Draw = nullptr;

float Width(float h, const std::string& t)
{
    if (GameFontReady())
        return GameFontWidth(h, t);
    return ImGui::GetFont()->CalcTextSizeA(h, FLT_MAX, 0, t.c_str()).x;
}

// Text at (x, y: its top), aligned (0 left, 0.5 middle, 1 right); its width.
float Text(float x, float y, float h, ImU32 colour, const std::string& t, float align = 0)
{
    const float w = Width(h, t);
    const ImVec2 at(x - w * align, y);
    if (!GameFontText(g_Draw, at, h, colour, t)) {
        g_Draw->AddText(ImGui::GetFont(), h, ImVec2(at.x + 1, at.y + 1), IM_COL32(0, 0, 0, colour >> 24), t.c_str());
        g_Draw->AddText(ImGui::GetFont(), h, at, colour, t.c_str());
    }
    return w;
}

// A point of the world on the screen, through the game camera's placement (rows right, up, forward, position) and
// field of view (the renderer's, vertical, radians).
bool OnScreen(const float p[3], const ImGuiViewport* vp, ImVec2& out)
{
    float m[16], fov = 0;
    if (!game::GameCameraPlacement(m, fov) || fov <= 0)
        return false;
    if (fov > 3.2f)
        fov *= 3.14159265f / 180.0f;
    const float d[3] = { p[0] - m[12], p[1] - m[13], p[2] - m[14] };
    const float x = d[0] * m[0] + d[1] * m[1] + d[2] * m[2];
    const float y = d[0] * m[4] + d[1] * m[5] + d[2] * m[6];
    const float z = d[0] * m[8] + d[1] * m[9] + d[2] * m[10];
    if (z < 10.0f)
        return false;
    const float focal = (vp->Size.y / 2) / std::tan(fov / 2);
    out = ImVec2(vp->Pos.x + vp->Size.x / 2 + x / z * focal, vp->Pos.y + vp->Size.y / 2 - y / z * focal);
    return true;
}

void Scoreboard(const game::HvvState& s, const ImGuiViewport* vp, float u)
{
    const float h = 26 * u, middle = vp->Pos.x + vp->Size.x / 2, top = vp->Pos.y + 14 * u;
    char line[32];
    for (int side = 0; side < 2; ++side) {
        const float sign = side == 0 ? -1.0f : 1.0f;
        snprintf(line, sizeof line, "%d", s.lives[side]);
        Text(middle + sign * 18 * u, top, h, SideColour(side), line, side == 0 ? 1.0f : 0.0f);
        Text(middle + sign * 70 * u, top + 6 * u, h * 0.6f, Faded(SideColour(side), 0.85f),
            side == 0 ? "HEROES" : "VILLAINS", side == 0 ? 1.0f : 0.0f);
        for (int k = 0; k < 4; ++k) {
            const auto& f = s.fighters[side * 4 + k];
            const ImVec2 at(middle + sign * (24 + k * 16) * u, top + h + 9 * u);
            g_Draw->AddCircleFilled(at, 5 * u, f.standing || !s.started ? SideColour(side) : kDim);
            if (f.isPlayer)
                g_Draw->AddCircle(at, 8 * u, IM_COL32(255, 255, 255, 230), 0, 1.5f * u);
        }
    }
    g_Draw->AddLine(ImVec2(middle, top), ImVec2(middle, top + h), IM_COL32(230, 230, 230, 200), 2 * u);
}

// Over each teammate standing: a small marker of the side's colour (as Battlefront's).
void Markers(const game::HvvState& s, const ImGuiViewport* vp, float u)
{
    if (!s.fighting)
        return;
    const int mySide = s.fighters[s.player].side;
    for (const auto& f : s.fighters) {
        if (f.isPlayer || !f.standing || !f.placed || f.side != mySide)
            continue;
        ImVec2 at;
        if (!OnScreen(f.head, vp, at))
            continue;
        const float r = 7 * u;
        g_Draw->AddTriangleFilled(ImVec2(at.x - r, at.y - r), ImVec2(at.x + r, at.y - r), ImVec2(at.x, at.y),
            IM_COL32(0, 0, 0, 120));
        g_Draw->AddTriangleFilled(ImVec2(at.x - r * 0.75f, at.y - r * 0.9f), ImVec2(at.x + r * 0.75f, at.y - r * 0.9f),
            ImVec2(at.x, at.y - r * 0.1f), SideColour(mySide));
    }
}

void KillFeed(const game::HvvState& s, const ImGuiViewport* vp, float u)
{
    const float h = 15 * u, right = vp->Pos.x + vp->Size.x - 18 * u;
    float y = vp->Pos.y + 16 * u;
    for (const auto& line : s.feed) {
        const float alpha = std::min(1.0f, (7.0f - line.age) / 1.0f);
        float x = right;
        x -= Text(x, y, h, Faded(SideColour(line.victimSide), alpha), line.victim, 1.0f) + 10 * u;
        if (!line.killer.empty()) {
            // a small blade between the two
            g_Draw->AddLine(ImVec2(x - 18 * u, y + h * 0.5f), ImVec2(x - 2 * u, y + h * 0.5f),
                Faded(kWhite, alpha), 2 * u);
            x -= 26 * u;
            Text(x, y, h, Faded(SideColour(line.killerSide), alpha), line.killer, 1.0f);
        } else {
            Text(x, y, h, Faded(kDim, alpha), "FELL", 1.0f);
        }
        if (line.mine)
            g_Draw->AddRectFilled(ImVec2(right + 6 * u, y), ImVec2(right + 9 * u, y + h), Faded(kGold, alpha));
        y += h + 7 * u;
    }
}

// The player's notice: "DEFEATED <NAME>" with a killstreak, "ASSIST <NAME>", "DEFEATED BY <NAME>".
void Notice(const game::HvvState& s, const ImGuiViewport* vp, float u)
{
    if (s.noticeAge < 0)
        return;
    const float alpha = std::min({ 1.0f, s.noticeAge / 0.15f, (3.5f - s.noticeAge) / 0.5f });
    const float h = 24 * u, middle = vp->Pos.x + vp->Size.x / 2, y = vp->Pos.y + vp->Size.y * 0.62f;
    std::string lead = s.notice, name;
    // "DEFEATED NAME..." / "ASSIST NAME": the verb white, the name in its side's colour
    const char* verbs[] = { "DEFEATED BY ", "DEFEATED ", "ASSIST " };
    for (const char* v : verbs) {
        if (s.notice.rfind(v, 0) == 0) {
            lead = v;
            name = s.notice.substr(strlen(v));
            break;
        }
    }
    const float total = Width(h, lead) + Width(h, name);
    float x = middle - total / 2;
    x += Text(x, y, h, Faded(kWhite, alpha), lead);
    Text(x, y, h, Faded(SideColour(s.noticeSide), alpha), name);
    float below = y + h + 6 * u;
    if (!s.noticeSub.empty()) {
        Text(middle, below, h * 0.62f, Faded(kGold, alpha), s.noticeSub, 0.5f);
        below += h * 0.62f + 5 * u;
    }
    if (s.noticePoints > 0) {
        char points[32];
        snprintf(points, sizeof points, "+%d", s.noticePoints);
        Text(middle, below, h * 0.55f, Faded(kWhite, alpha * 0.85f), points, 0.5f);
    }
}

void Captions(const game::HvvState& s, const ImGuiViewport* vp, float u)
{
    if (s.caption.empty())
        return;
    const float alpha = std::min({ 1.0f, s.captionFade / 0.12f, (1.0f - s.captionFade) / 0.12f });
    const float middle = vp->Pos.x + vp->Size.x / 2;
    if (s.caption == "FIGHT!") {
        Text(middle, vp->Pos.y + vp->Size.y * 0.3f, 64 * u, Faded(kGold, alpha), s.caption, 0.5f);
        return;
    }
    const float y = vp->Pos.y + vp->Size.y * 0.76f;
    Text(middle, y, 40 * u, Faded(SideColour(s.captionSide), alpha), s.caption, 0.5f);
    if (!s.subcaption.empty())
        Text(middle, y + 48 * u, 16 * u, Faded(kWhite, alpha), s.subcaption, 0.5f);
}

void Leaderboard(const game::HvvState& s, const ImGuiViewport* vp, float u)
{
    const float rowH = 22 * u, colW = 330 * u, gap = 24 * u;
    const float width = 2 * colW + gap, left = vp->Pos.x + (vp->Size.x - width) / 2;
    const float top = vp->Pos.y + vp->Size.y * 0.2f;
    g_Draw->AddRectFilled(ImVec2(left - 14 * u, top - 14 * u), ImVec2(left + width + 14 * u, top + 7 * rowH + 6 * u),
        kPanel, 6 * u);
    for (int side = 0; side < 2; ++side) {
        const float x = left + side * (colW + gap);
        Text(x, top, 22 * u, SideColour(side), side == 0 ? "HEROES" : "VILLAINS");
        const float cols[] = { x + colW * 0.56f, x + colW * 0.70f, x + colW * 0.84f, x + colW };
        const char* heads[] = { "D", "A", "F", "SCORE" };
        for (int c = 0; c < 4; ++c)
            Text(cols[c], top + 6 * u, 12 * u, kDim, heads[c], 1.0f);
        std::vector<int> order;
        for (int i = side * 4; i < side * 4 + 4; ++i)
            order.push_back(i);
        std::sort(order.begin(), order.end(), [&](int a, int b) { return s.fighters[a].score > s.fighters[b].score; });
        float y = top + 1.6f * rowH;
        for (int i : order) {
            const auto& f = s.fighters[i];
            if (f.isPlayer)
                g_Draw->AddRectFilled(ImVec2(x - 6 * u, y - 3 * u), ImVec2(x + colW + 6 * u, y + rowH - 4 * u),
                    IM_COL32(255, 196, 90, 40), 3 * u);
            const ImU32 colour = f.out ? kDim : kWhite;
            Text(x, y, 15 * u, colour, f.isPlayer ? "YOU  (" + game::HvvDisplayName(f.className) + ")" : f.name);
            const int values[] = { f.defeats, f.assists, f.falls, f.score };
            for (int c = 0; c < 4; ++c)
                Text(cols[c], y, 15 * u, c == 3 ? kGold : colour, std::to_string(values[c]), 1.0f);
            y += rowH;
        }
    }
}

// The end: the winner, and a card under each of the top five (as Battlefront's end screen).
void EndScreen(const game::HvvState& s, const ImGuiViewport* vp, float u)
{
    const float alpha = std::min(1.0f, s.endingAge / 0.8f);
    const float middle = vp->Pos.x + vp->Size.x / 2;
    if (s.winner >= 0)
        Text(middle, vp->Pos.y + vp->Size.y * 0.08f, 40 * u, Faded(SideColour(s.winner), alpha),
            s.winner == 0 ? "HEROES WIN" : "VILLAINS WIN", 0.5f);
    static const char* kRanks[] = { "FIRST", "SECOND", "THIRD", "FOURTH", "FIFTH" };
    static const float kAt[5][2] = { { 0.5f, 0.68f }, { 0.30f, 0.62f }, { 0.70f, 0.62f }, { 0.11f, 0.70f },
        { 0.89f, 0.70f } };
    for (size_t r = 0; r < s.top.size() && r < 5; ++r) {
        const auto& f = s.fighters[s.top[r]];
        const float w = (r == 0 ? 250 : 210) * u, h = (r == 0 ? 92 : 78) * u;
        const float cx = vp->Pos.x + vp->Size.x * kAt[r][0], y = vp->Pos.y + vp->Size.y * kAt[r][1];
        const float a = std::clamp((s.endingAge - 0.4f - 0.25f * float(r)) / 0.5f, 0.0f, 1.0f);
        g_Draw->AddRectFilled(ImVec2(cx - w / 2, y), ImVec2(cx + w / 2, y + h), Faded(kPanel, a * 1.3f), 4 * u);
        g_Draw->AddRect(ImVec2(cx - w / 2, y), ImVec2(cx + w / 2, y + h), Faded(kEdge, a), 4 * u, 0, 1.5f * u);
        const float big = (r == 0 ? 26 : 19) * u;
        Text(cx, y + 6 * u, big, Faded(kGold, a), kRanks[r], 0.5f);
        g_Draw->AddRectFilled(ImVec2(cx - w / 2 + 6 * u, y + 10 * u + big), ImVec2(cx + w / 2 - 6 * u, y + 30 * u + big),
            Faded(IM_COL32(255, 255, 255, 18), a));
        Text(cx, y + 12 * u + big, 15 * u, Faded(SideColour(f.side), a),
            f.isPlayer ? "YOU" : game::HvvDisplayName(f.className), 0.5f);
        char score[32];
        snprintf(score, sizeof score, "%d SCORE", f.score);
        Text(cx, y + h - 22 * u, 16 * u, Faded(kGold, a), score, 0.5f);
    }
}

bool TabHeld()
{
    if (!(GetAsyncKeyState(VK_TAB) & 0x8000))
        return false;
    DWORD process = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &process);
    return process == GetCurrentProcessId();
}

} // namespace

void DrawHvvHud()
{
    const game::HvvState s = game::GetHvvState();
    // the game's own HUD hidden for the end (its `hud` variable, the opacity), given back after
    static bool hudHidden = false;
    if (s.ending != hudHidden) {
        hudHidden = s.ending;
        WriteGameVariable("hud", hudHidden ? "0" : "1");
    }
    if (!s.active)
        return;
    g_Draw = ImGui::GetForegroundDrawList();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float u = std::max(0.5f, vp->Size.y / 720.0f);
    if (s.ending) {
        EndScreen(s, vp, u);
        return;
    }
    Markers(s, vp, u);
    Scoreboard(s, vp, u);
    KillFeed(s, vp, u);
    Captions(s, vp, u);
    Notice(s, vp, u);
    if (s.started && (s.board || TabHeld()))
        Leaderboard(s, vp, u);
    if (s.winner >= 0)
        Text(vp->Pos.x + vp->Size.x / 2, vp->Pos.y + vp->Size.y * 0.3f, 48 * u, SideColour(s.winner),
            s.winner == 0 ? "HEROES WIN" : "VILLAINS WIN", 0.5f);
}

} // namespace swrots::debug
