#include "core/install.h"

#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/icon.h"
#include "core/log.h"
#include "core/xbe.h"
#include "game/game.h"

namespace swrots {

namespace {

const wchar_t* kTitle = L"Star Wars: Episode III - Revenge of the Sith";

// --- Progress window ------------------------------------------------------------------------

using Progress = InstallProgress;

Progress* g_Progress = nullptr;

LRESULT CALLBACK ProgressProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_TIMER:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_CLOSE:
        if (MessageBoxW(hwnd, L"Stop setting up the game?", kTitle, MB_YESNO | MB_ICONQUESTION) == IDYES)
            g_Progress->cancel = true;
        return 0;
    case WM_PAINT: {
        const UINT dpi = GetDpiForWindow(hwnd);
        auto S = [dpi](int v) { return MulDiv(v, int(dpi), 96); }; // layout in 96-DPI units
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT client;
        GetClientRect(hwnd, &client);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, client.right, client.bottom);
        HGDIOBJ oldBmp = SelectObject(mem, bmp);
        FillRect(mem, &client, GetSysColorBrush(COLOR_WINDOW));
        NONCLIENTMETRICSW ncm = { sizeof(ncm) };
        SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi);
        HFONT font = CreateFontIndirectW(&ncm.lfMessageFont);
        HGDIOBJ oldFont = SelectObject(mem, font);
        SetBkMode(mem, TRANSPARENT);
        const std::wstring status = g_Progress->Status();
        RECT text = { S(16), S(14), client.right - S(16), S(60) };
        DrawTextW(mem, status.c_str(), -1, &text, DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);
        RECT bar = { S(16), S(64), client.right - S(16), S(84) };
        FrameRect(mem, &bar, GetSysColorBrush(COLOR_BTNSHADOW));
        const uint64_t total = g_Progress->total, done = g_Progress->done;
        if (total) {
            RECT fill = bar;
            InflateRect(&fill, -S(2), -S(2));
            fill.right = fill.left + LONG((fill.right - fill.left) * std::min(1.0, double(done) / double(total)));
            FillRect(mem, &fill, GetSysColorBrush(COLOR_HIGHLIGHT));
            wchar_t pct[64];
            swprintf_s(pct, L"%.0f%%  (%.1f of %.1f GB)", 100.0 * done / total, done / 1e9, total / 1e9);
            RECT pr = { S(16), S(90), client.right - S(16), S(112) };
            DrawTextW(mem, pct, -1, &pr, DT_LEFT);
        }
        BitBlt(dc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldFont);
        DeleteObject(font);
        SelectObject(mem, oldBmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// Runs `work` on a worker thread while showing its progress.
void RunWithProgress(Progress& p, const std::function<void()>& work)
{
    g_Progress = &p;
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = ProgressProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"SWROTSSetup";
    RegisterClassExW(&wc);
    const UINT dpi = GetDpiForSystem();
    RECT r = { 0, 0, MulDiv(520, int(dpi), 96), MulDiv(124, int(dpi), 96) };
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, dpi);
    const int w = r.right - r.left, h = r.bottom - r.top;
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Setting up Star Wars: Episode III", style,
        (GetSystemMetrics(SM_CXSCREEN) - w) / 2, (GetSystemMetrics(SM_CYSCREEN) - h) / 2, w, h, nullptr, nullptr,
        wc.hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOW);
    SetTimer(hwnd, 1, 100, nullptr);

    std::thread worker([&] {
        work();
        p.finished = true;
        PostMessageW(hwnd, WM_NULL, 0, 0);
    });
    MSG msg;
    while (!p.finished && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    worker.join();
    DestroyWindow(hwnd);
    g_Progress = nullptr;
}

// --- Helpers ------------------------------------------------------------------------------------

std::wstring Lower(std::wstring s)
{
    for (wchar_t& c : s)
        c = wchar_t(towlower(c));
    return s;
}

bool EndsWith(const std::wstring& s, const wchar_t* suffix)
{
    const size_t n = wcslen(suffix);
    return s.size() >= n && Lower(s.substr(s.size() - n)) == suffix;
}

bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

void DeleteTree(const std::wstring& path)
{
    if (!Exists(path))
        return;
    std::wstring from = path;
    from.push_back(L'\0'); // double-terminated list
    SHFILEOPSTRUCTW op = {};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_NO_UI;
    SHFileOperationW(&op);
}

uint64_t FileSize(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a))
        return 0;
    return (uint64_t(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
}

// Free bytes on the drive of `dir` (an existing folder) for this user; UINT64_MAX when unknown.
uint64_t FreeSpace(const std::wstring& dir)
{
    ULARGE_INTEGER available = {};
    if (!GetDiskFreeSpaceExW(dir.c_str(), &available, nullptr, nullptr))
        return UINT64_MAX;
    return available.QuadPart;
}

std::wstring Gigabytes(uint64_t bytes)
{
    wchar_t text[32];
    swprintf_s(text, L"%.1f GB", double(bytes) / 1e9);
    return text;
}

// Runs Windows' tar.exe (libarchive: reads 7z, zip, rar, ...). `output`
// receives stdout if given. While it runs, `poll` is called every 200 ms.
bool RunTar(const std::wstring& args, std::string* output, Progress& p, const std::function<void()>& poll)
{
    wchar_t system[MAX_PATH];
    GetSystemDirectoryW(system, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(system) + L"\\tar.exe\" " + args;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (output) {
        CreatePipe(&readPipe, &writePipe, &sa, 0);
        SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
    }
    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = nullptr;
    si.hStdInput = nullptr;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        if (readPipe) CloseHandle(readPipe);
        if (writePipe) CloseHandle(writePipe);
        return false;
    }
    if (writePipe)
        CloseHandle(writePipe);
    std::thread reader;
    if (output)
        reader = std::thread([&] {
            char buf[4096];
            DWORD n;
            while (ReadFile(readPipe, buf, sizeof(buf), &n, nullptr) && n)
                output->append(buf, n);
        });
    while (WaitForSingleObject(pi.hProcess, 200) == WAIT_TIMEOUT) {
        if (p.cancel) {
            TerminateProcess(pi.hProcess, 1);
            break;
        }
        if (poll)
            poll();
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    if (reader.joinable())
        reader.join();
    if (readPipe)
        CloseHandle(readPipe);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code == 0 && !p.cancel;
}

// Where the Xbox game partition's header ("MICROSOFT*XBOX*MEDIA", sector 32) can be: an XISO
// starts with the game partition, full disc images have the video partition first.
constexpr uint64_t kXboxPartitionBases[] = { 0ull, 0x18300000ull, 0xFD90000ull, 0x2080000ull };
constexpr uint64_t kXboxHeaderOffset = 32 * 2048;
constexpr char kXboxMagic[] = "MICROSOFT*XBOX*MEDIA";

// Streams the start of `member` out of the archive (tar -xO) and looks for the Xbox game
// partition header, stopping as soon as it is found or cannot be there any more: a few
// seconds, instead of unpacking gigabytes to find out.
bool ArchiveImageIsXbox(const std::wstring& archive, const std::wstring& member, Progress& p)
{
    p.SetStatus(L"Checking the disc image...");
    wchar_t system[MAX_PATH];
    GetSystemDirectoryW(system, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(system) + L"\\tar.exe\" -xOf \"" + archive + L"\" \"" + member + L"\"";
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 1 << 20))
        return false;
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(readPipe);
        CloseHandle(writePipe);
        return false;
    }
    CloseHandle(writePipe);

    uint64_t last = 0;
    for (uint64_t base : kXboxPartitionBases)
        last = std::max(last, base + kXboxHeaderOffset + sizeof(kXboxMagic) - 1);
    char seen[std::size(kXboxPartitionBases)][sizeof(kXboxMagic)] = {};
    std::vector<char> buffer(1 << 20);
    uint64_t position = 0;
    bool found = false;
    DWORD n = 0;
    while (!found && !p.cancel && position < last && ReadFile(readPipe, buffer.data(), DWORD(buffer.size()), &n, nullptr) && n) {
        for (size_t i = 0; i < std::size(kXboxPartitionBases); ++i) {
            const uint64_t at = kXboxPartitionBases[i] + kXboxHeaderOffset;
            for (uint64_t b = std::max(at, position); b < std::min(at + sizeof(kXboxMagic) - 1, position + n); ++b)
                seen[i][b - at] = buffer[size_t(b - position)];
            if (position + n >= at + sizeof(kXboxMagic) - 1 && memcmp(seen[i], kXboxMagic, sizeof(kXboxMagic) - 1) == 0)
                found = true;
        }
        position += n;
    }
    TerminateProcess(pi.hProcess, 0);
    CloseHandle(readPipe);
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    LOG_INFO("Setup: %ls %s an Xbox disc image (%llu bytes read)", member.c_str(), found ? "is" : "is not", position);
    return found;
}

// Finds the disc image in an archive and unpacks it to `dir`. Returns its path.
std::wstring UnpackArchive(const std::wstring& archive, const std::wstring& dir, Progress& p)
{
    p.SetStatus(L"Reading " + archive.substr(archive.find_last_of(L"\\/") + 1) + L"...");
    std::string listing;
    if (!RunTar(L"-tvf \"" + archive + L"\"", &listing, p, nullptr)) {
        if (!p.cancel)
            p.Fail(InstallError::ArchiveUnreadable,
                L"Windows could not read this archive (it may be damaged, incomplete or protected by a password). "
                L"Unpack it yourself and choose the .iso inside.");
        return {};
    }
    // bsdtar -tv: "mode links uid gid size month day year|time name"
    std::string name;
    uint64_t size = 0;
    size_t start = 0;
    while (start < listing.size()) {
        size_t end = listing.find('\n', start);
        std::string line = listing.substr(start, end == std::string::npos ? std::string::npos : end - start);
        start = end == std::string::npos ? listing.size() : end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        size_t pos = 0;
        std::string fields[8];
        for (std::string& field : fields) {
            while (pos < line.size() && line[pos] == ' ')
                ++pos;
            size_t e = line.find(' ', pos);
            field = line.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
            pos = e == std::string::npos ? line.size() : e;
        }
        while (pos < line.size() && line[pos] == ' ')
            ++pos;
        std::string member = line.substr(pos);
        if ((member.size() > 4 && _stricmp(member.c_str() + member.size() - 4, ".iso") == 0)
            || (member.size() > 5 && _stricmp(member.c_str() + member.size() - 5, ".xiso") == 0)) {
            name = member;
            size = _strtoui64(fields[4].c_str(), nullptr, 10);
            break;
        }
    }
    if (name.empty()) {
        p.Fail(InstallError::ArchiveWithoutImage,
            L"The archive does not contain an .iso disc image. If the image inside has another format, convert it "
            L"to an .iso first.");
        return {};
    }
    std::wstring wname(name.begin(), name.end());
    if (!ArchiveImageIsXbox(archive, wname, p)) {
        if (!p.cancel)
            p.Fail(InstallError::NotAnXboxImage,
                L"The disc image in this archive (" + wname + L") is not an Xbox disc. The port needs the Xbox "
                L"version of the game, not the PlayStation 2 one.");
        return {};
    }
    std::wstring out = dir + L"\\" + wname;
    p.SetStatus(L"Unpacking " + wname + L"...");
    p.done = 0;
    p.total = size;
    CreateDirectoryW(dir.c_str(), nullptr);
    if (const uint64_t space = FreeSpace(dir); space < size) {
        p.Fail(InstallError::NotEnoughSpace, L"Not enough free space to unpack the archive: the disc image takes "
            + Gigabytes(size) + L" and the game files about 2.5 GB more, and the drive has " + Gigabytes(space)
            + L" free.");
        return {};
    }
    if (!RunTar(L"-xf \"" + archive + L"\" -C \"" + dir + L"\" \"" + wname + L"\"", nullptr, p,
            [&] { p.done = FileSize(out); })) {
        if (!p.cancel)
            p.Fail(InstallError::ArchiveUnpackFailed,
                L"The disc image could not be unpacked from the archive (the archive may be damaged, or the drive "
                L"full). Unpack it yourself and choose the .iso inside.");
        return {};
    }
    return out;
}

// --- XISO ---------------------------------------------------------------------------------------

constexpr uint32_t kSector = 2048;

struct IsoFile {
    std::wstring path; // relative
    uint32_t sector, size;
    bool directory;
};

class Iso {
public:
    ~Iso()
    {
        if (m_file != INVALID_HANDLE_VALUE)
            CloseHandle(m_file);
    }

    enum class Kind {
        Xbox,       // opened: an Xbox game partition
        Unreadable, // the file cannot be opened
        Iso9660,    // a standard CD/DVD image without an Xbox game partition
        Unknown,    // anything else
    };

    Kind Open(const std::wstring& path)
    {
        m_file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (m_file == INVALID_HANDLE_VALUE)
            return Kind::Unreadable;
        LARGE_INTEGER size = {};
        GetFileSizeEx(m_file, &size);
        m_size = uint64_t(size.QuadPart);
        // The game partition starts at 0 (XISO) or after the video partition (full disc images).
        for (uint64_t base : { 0ull, 0x18300000ull, 0xFD90000ull, 0x2080000ull }) {
            char magic[20];
            if (Read(base + 32 * kSector, magic, 20) && memcmp(magic, "MICROSOFT*XBOX*MEDIA", 20) == 0) {
                m_base = base;
                uint32_t root[2];
                Read(base + 32 * kSector + 20, root, 8);
                m_rootSector = root[0];
                m_rootSize = root[1];
                return Kind::Xbox;
            }
        }
        // ISO 9660's volume descriptor ("CD001" in sector 16): a PC, PlayStation 2 or video disc, or
        // only the video partition of an Xbox disc.
        char cd[5];
        if (Read(16 * kSector + 1, cd, 5) && memcmp(cd, "CD001", 5) == 0)
            return Kind::Iso9660;
        return Kind::Unknown;
    }

    uint64_t Size() const { return m_size; }

    bool Read(uint64_t offset, void* out, uint32_t bytes)
    {
        OVERLAPPED ov = {};
        ov.Offset = DWORD(offset);
        ov.OffsetHigh = DWORD(offset >> 32);
        DWORD n = 0;
        return ReadFile(m_file, out, bytes, &n, &ov) && n == bytes;
    }

    bool List(std::vector<IsoFile>& files) { return ListDirectory(m_rootSector, m_rootSize, L"", files); }

    bool ReadFileData(const IsoFile& f, std::vector<uint8_t>& out)
    {
        out.resize(f.size);
        return f.size == 0 || Read(Offset(f.sector), out.data(), f.size);
    }

    uint64_t Offset(uint32_t sector) const { return m_base + uint64_t(sector) * kSector; }

private:
    // A directory is a binary tree of entries: left/right subtree offsets (in
    // dwords, 0xFFFF = none), start sector, size, attributes, name.
    bool ListDirectory(uint32_t sector, uint32_t size, const std::wstring& prefix, std::vector<IsoFile>& files)
    {
        if (size == 0)
            return true;
        std::vector<uint8_t> table(size);
        if (!Read(Offset(sector), table.data(), size))
            return false;
        std::vector<uint32_t> pending = { 0 };
        while (!pending.empty()) {
            const uint32_t off = pending.back() * 4;
            pending.pop_back();
            if (off + 14 > size)
                continue;
            const uint8_t* e = table.data() + off;
            const uint16_t left = uint16_t(e[0] | e[1] << 8), right = uint16_t(e[2] | e[3] << 8);
            if (left == 0xFFFF && right == 0xFFFF && e[13] == 0xFF)
                continue; // padding
            uint32_t start, bytes;
            memcpy(&start, e + 4, 4);
            memcpy(&bytes, e + 8, 4);
            const uint8_t attributes = e[12], nameLength = e[13];
            if (off + 14 + nameLength > size)
                return false;
            std::wstring name;
            for (int i = 0; i < nameLength; ++i)
                name += wchar_t(e[14 + i]);
            if (name.empty() || name == L"." || name == L".." || name.find_first_of(L"\\/:") != std::wstring::npos)
                return false;
            const std::wstring path = prefix + name;
            const bool directory = (attributes & 0x10) != 0;
            files.push_back({ path, start, bytes, directory });
            if (directory && !ListDirectory(start, bytes, path + L"\\", files))
                return false;
            if (left && left != 0xFFFF)
                pending.push_back(left);
            if (right && right != 0xFFFF)
                pending.push_back(right);
        }
        return true;
    }

    HANDLE m_file = INVALID_HANDLE_VALUE;
    uint64_t m_base = 0, m_size = 0;
    uint32_t m_rootSector = 0, m_rootSize = 0;
};

// Which game and release a default.xbe is, from its certificate: the supported one is known by its
// MD5; for any other, the error says what the disc is instead.
bool CheckExecutable(const std::vector<uint8_t>& xbe, Progress& p)
{
    if (Md5Hex(xbe.data(), xbe.size()) == game::kRetailMd5)
        return true;
    uint32_t titleId = 0, region = 0;
    std::wstring title;
    if (xbe.size() < sizeof(XbeHeader) || memcmp(xbe.data(), "XBEH", 4) != 0) {
        p.Fail(InstallError::DamagedImage,
            L"The disc's default.xbe is not an Xbox executable: the image is damaged. Make or get the image again.");
        return false;
    }
    {
        XbeHeader header;
        memcpy(&header, xbe.data(), sizeof(header));
        const uint64_t at = uint64_t(header.CertificateAddress) - header.BaseAddress;
        if (header.Magic == 'HEBX' && header.CertificateAddress >= header.BaseAddress
            && at + sizeof(XbeCertificate) <= xbe.size()) {
            XbeCertificate certificate;
            memcpy(&certificate, xbe.data() + size_t(at), sizeof(certificate));
            titleId = certificate.TitleId;
            region = certificate.GameRegion;
            for (wchar_t c : certificate.TitleName) {
                if (!c)
                    break;
                title += c >= 32 ? c : L' ';
            }
        }
    }
    if (titleId != game::kTitleId) {
        p.Fail(InstallError::OtherGame, title.empty()
            ? std::wstring(L"This Xbox disc image is not Star Wars: Episode III - Revenge of the Sith.")
            : L"This is the disc of another Xbox game: \"" + title + L"\". The port needs Star Wars: Episode III - "
              L"Revenge of the Sith.");
        return false;
    }
    // The certificate's region flags: 1 North America, 2 Japan, 4 the rest of the world (PAL).
    std::wstring regions;
    if (region & 1)
        regions += L"North America";
    if (region & 2)
        regions += regions.empty() ? L"Japan" : L", Japan";
    if (region & 4)
        regions += regions.empty() ? L"Europe / PAL" : L", Europe / PAL";
    if ((region & 7) == 1)
        p.Fail(InstallError::OtherRelease,
            L"This is Revenge of the Sith, but its executable is not the one the port supports: the image is a "
            L"different revision of the North American release, or it was modified (patched or trained). Use an "
            L"unmodified image of the North American (USA, NTSC) release.");
    else
        p.Fail(InstallError::OtherRelease, L"This is Revenge of the Sith, but another release of it"
            + (regions.empty() ? std::wstring() : L" (" + regions + L")")
            + L". Only the North American (USA, NTSC) release is supported.");
    return false;
}

bool ExtractIso(const std::wstring& image, const std::wstring& target, Progress& p)
{
    p.SetStatus(L"Reading the disc image...");
    Iso iso;
    std::vector<IsoFile> files;
    switch (iso.Open(image)) {
    case Iso::Kind::Xbox:
        break;
    case Iso::Kind::Unreadable:
        p.Fail(InstallError::ImageNotFound, L"The file could not be opened:\n" + image);
        return false;
    case Iso::Kind::Iso9660:
        p.Fail(InstallError::NotAnXboxImage,
            L"This is a disc image, but not of an Xbox game: it has no Xbox game partition. It may be the "
            L"PlayStation 2 version of the game, another kind of disc, or an incomplete copy of the Xbox disc. "
            L"The port needs the Xbox version.");
        return false;
    case Iso::Kind::Unknown:
        p.Fail(InstallError::NotADiscImage,
            L"This file is not a disc image the port can read. Choose an Xbox .iso / .xiso (an XISO or a full "
            L"Redump-style image), or a .7z / .zip / .rar archive containing one.");
        return false;
    }
    if (!iso.List(files)) {
        p.Fail(InstallError::DamagedImage,
            L"This is an Xbox disc image, but its file table cannot be read: the image is damaged or incomplete. "
            L"Make or get the image again.");
        return false;
    }
    // Every file must lie inside the image: a download or a copy that stopped early ends before them.
    for (const IsoFile& f : files) {
        if (!f.directory && iso.Offset(f.sector) + f.size > iso.Size()) {
            p.Fail(InstallError::DamagedImage, L"This Xbox disc image is incomplete: it ends before its files do ("
                + f.path + L" is missing or cut short). The copy or download probably stopped early; make or get "
                L"the image again.");
            return false;
        }
    }
    // The game itself, checked before any file is copied.
    p.SetStatus(L"Checking the game...");
    auto xbe = std::find_if(files.begin(), files.end(),
        [](const IsoFile& f) { return !f.directory && _wcsicmp(f.path.c_str(), L"default.xbe") == 0; });
    std::vector<uint8_t> xbeBytes;
    if (xbe == files.end()) {
        p.Fail(InstallError::NoGameExecutable,
            L"This Xbox disc image does not contain a game (it has no default.xbe). It may be a data or video disc.");
        return false;
    }
    if (!iso.ReadFileData(*xbe, xbeBytes)) {
        p.Fail(InstallError::ReadFailed, L"The game's executable (default.xbe) could not be read from the image.");
        return false;
    }
    if (!CheckExecutable(xbeBytes, p))
        return false;
    uint64_t total = 0;
    for (const IsoFile& f : files)
        if (!f.directory)
            total += f.size;
    p.done = 0;
    p.total = total;
    // The drive must hold the files (and a little more, for the folders and the file system's own records).
    const std::wstring parent = target.substr(0, target.find_last_of(L"\\/"));
    if (const uint64_t space = FreeSpace(parent); space < total + (64ull << 20)) {
        p.Fail(InstallError::NotEnoughSpace, L"Not enough free space: the game files take " + Gigabytes(total)
            + L", and the drive of\n" + parent + L"\nhas " + Gigabytes(space) + L" free.");
        return false;
    }
    CreateDirectoryW(target.c_str(), nullptr);
    std::vector<uint8_t> buffer(4 << 20);
    for (const IsoFile& f : files) {
        if (p.cancel)
            return false;
        const std::wstring out = target + L"\\" + f.path;
        if (f.directory) {
            CreateDirectoryW(out.c_str(), nullptr);
            continue;
        }
        p.SetStatus(L"Copying game files...\n" + f.path);
        HANDLE h = CreateFileW(out.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            p.Fail(InstallError::WriteFailed, L"Could not write\n" + out
                + L"\n(Windows error " + std::to_wstring(GetLastError()) + L"). Check that the game's folder can be "
                L"written to.");
            return false;
        }
        uint64_t offset = iso.Offset(f.sector);
        uint32_t left = f.size;
        bool readOk = true, writeOk = true;
        DWORD writeError = 0;
        while (left && readOk && writeOk && !p.cancel) {
            const uint32_t n = std::min<uint32_t>(left, uint32_t(buffer.size()));
            DWORD written = 0;
            readOk = iso.Read(offset, buffer.data(), n);
            if (readOk) {
                writeOk = WriteFile(h, buffer.data(), n, &written, nullptr) && written == n;
                if (!writeOk)
                    writeError = GetLastError();
            }
            offset += n;
            left -= n;
            p.done += n;
        }
        CloseHandle(h);
        if (!readOk) {
            p.Fail(InstallError::ReadFailed, L"Could not read " + f.path + L" from the disc image: the image is "
                L"damaged, or the drive it is on cannot be read.");
            return false;
        }
        if (!writeOk) {
            if (writeError == ERROR_DISK_FULL || writeError == ERROR_HANDLE_DISK_FULL)
                p.Fail(InstallError::NotEnoughSpace, L"The drive is full: " + f.path + L" could not be written.");
            else
                p.Fail(InstallError::WriteFailed, L"Could not write " + f.path + L" (Windows error "
                    + std::to_wstring(writeError) + L").");
            return false;
        }
    }
    return !p.cancel;
}

// --- Shortcuts ----------------------------------------------------------------------------------

// The shortcut's file in a known folder (the Start menu's programs, the desktop); empty when the
// folder is not there.
std::wstring ShortcutPath(REFKNOWNFOLDERID folder)
{
    PWSTR base = nullptr;
    if (FAILED(SHGetKnownFolderPath(folder, 0, nullptr, &base)))
        return {};
    const std::wstring link = std::wstring(base) + L"\\Star Wars Episode III - Revenge of the Sith.lnk";
    CoTaskMemFree(base);
    return link;
}

void CreateShortcut(REFKNOWNFOLDERID folder, const std::wstring& exe, const std::wstring& dir, const std::wstring& icon)
{
    const std::wstring link = ShortcutPath(folder);
    if (link.empty())
        return;
    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl))))
        return;
    sl->SetPath(exe.c_str());
    sl->SetWorkingDirectory(dir.c_str());
    sl->SetDescription(kTitle);
    if (Exists(icon))
        sl->SetIconLocation(icon.c_str(), 0);
    IPersistFile* pf = nullptr;
    if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf)))) {
        pf->Save(link.c_str(), TRUE);
        pf->Release();
    }
    sl->Release();
}

} // namespace

std::wstring ChooseDiscImage(HWND owner)
{
    wchar_t file[MAX_PATH] = {};
    OPENFILENAMEW ofn = { sizeof(ofn) };
    ofn.hwndOwner = owner;
    ofn.lpstrTitle = L"Choose your Star Wars: Episode III disc image";
    ofn.lpstrFilter = L"Disc images and archives (*.iso, *.xiso, *.7z, *.zip, *.rar)\0*.iso;*.xiso;*.7z;*.zip;*.rar\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    // The dialog must not change the process's current folder (relative paths are the game's).
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&ofn) ? std::wstring(file) : std::wstring();
}

const wchar_t* InstallErrorTitle(InstallError code)
{
    switch (code) {
    case InstallError::None: return L"";
    case InstallError::Cancelled: return L"Setup was cancelled";
    case InstallError::ImageNotFound: return L"The file cannot be opened";
    case InstallError::ArchiveUnreadable: return L"The archive cannot be read";
    case InstallError::ArchiveWithoutImage: return L"No disc image in the archive";
    case InstallError::ArchiveUnpackFailed: return L"The archive could not be unpacked";
    case InstallError::NotADiscImage: return L"Not a disc image";
    case InstallError::NotAnXboxImage: return L"Not an Xbox disc";
    case InstallError::DamagedImage: return L"The disc image is damaged or incomplete";
    case InstallError::NoGameExecutable: return L"No game on this disc";
    case InstallError::OtherGame: return L"Another game's disc";
    case InstallError::OtherRelease: return L"Unsupported release of the game";
    case InstallError::NotEnoughSpace: return L"Not enough free space";
    case InstallError::FolderInTheWay: return L"The GameData folder is in the way";
    case InstallError::WriteFailed: return L"The game files could not be written";
    case InstallError::ReadFailed: return L"The disc image could not be read";
    }
    return L"Setup could not finish";
}

bool GameDataInstalled(const std::wstring& gameData) { return Exists(gameData + L"\\default.xbe"); }

void InstallGameFiles(const std::wstring& exeDir, const std::wstring& gameData, const std::wstring& image,
    InstallProgress& p)
{
    p.ok = false;
    p.code = InstallError::None;
    p.error.clear();
    LOG_INFO("Setup: installing game files from %ls", image.c_str());
    const std::wstring partial = gameData + L".partial", unpackDir = exeDir + L"\\cache\\setup";
    [&] {
        if (!Exists(image)) {
            p.Fail(InstallError::ImageNotFound, L"The file does not exist:\n" + image);
            return;
        }
        // An empty folder from an earlier attempt goes; anything else in it is not ours to delete.
        if (Exists(gameData) && !RemoveDirectoryW(gameData.c_str())) {
            p.Fail(InstallError::FolderInTheWay, L"The folder\n" + gameData + L"\nalready exists and is not empty. "
                L"Uninstall the game files (Installation > Uninstall), or move or delete that folder, then try again.");
            return;
        }
        DeleteTree(partial);
        std::wstring iso = image;
        if (!EndsWith(image, L".iso") && !EndsWith(image, L".xiso")) {
            CreateDirectoryW((exeDir + L"\\cache").c_str(), nullptr);
            iso = UnpackArchive(image, unpackDir, p);
            if (iso.empty())
                return;
        }
        if (!ExtractIso(iso, partial, p))
            return;
        p.SetStatus(L"Checking the game files...");
        XbeFile xbe;
        std::string error;
        if (!xbe.Load(partial + L"\\default.xbe", error)) {
            p.Fail(InstallError::NoGameExecutable, L"The copied game files have no default.xbe.");
            return;
        }
        if (xbe.Md5Hex() != game::kRetailMd5) {
            p.Fail(InstallError::ReadFailed, L"The copied default.xbe is not the one read from the image: the "
                L"image or the drive could not be read reliably. Try again.");
            return;
        }
        p.ok = MoveFileExW(partial.c_str(), gameData.c_str(), 0) != 0;
        if (!p.ok)
            p.Fail(InstallError::WriteFailed, L"Could not create\n" + gameData + L"\n(Windows error "
                + std::to_wstring(GetLastError()) + L").");
    }();
    DeleteTree(unpackDir);
    if (!p.ok) {
        DeleteTree(partial);
        if (p.cancel && p.code == InstallError::None)
            p.Fail(InstallError::Cancelled, L"Setup was cancelled.");
        else if (p.code == InstallError::None)
            p.Fail(InstallError::WriteFailed, L"Setup could not finish.");
        LOG_WARN("Setup did not finish (%d): %ls", int(p.code), p.error.c_str());
        return;
    }
    LOG_INFO("Setup: game files installed");
}

void CreateGameShortcuts(const std::wstring& exeDir, const std::wstring& gameData, bool desktop)
{
    XbeFile xbe;
    std::string error;
    const std::wstring icon = exeDir + L"\\swrots.ico";
    if (xbe.Load(gameData + L"\\default.xbe", error) && xbe.Md5Hex() == game::kRetailMd5)
        WriteIconFile(xbe, icon);
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    CreateShortcut(FOLDERID_Programs, exe, exeDir, icon);
    if (desktop)
        CreateShortcut(FOLDERID_Desktop, exe, exeDir, icon);
    if (SUCCEEDED(com))
        CoUninitialize();
}

bool UninstallGame(const std::wstring& exeDir, const UninstallOptions& options, std::wstring& error)
{
    LOG_INFO("Uninstall: game files%s%s%s", options.saves ? ", saves" : "", options.settings ? ", settings" : "",
        options.mods ? ", mods" : "");
    std::vector<std::wstring> folders = { exeDir + L"\\GameData", exeDir + L"\\GameData.partial", exeDir + L"\\cache" };
    std::vector<std::wstring> files = { exeDir + L"\\swrots.ico" };
    for (const KNOWNFOLDERID* folder : { &FOLDERID_Programs, &FOLDERID_Desktop })
        if (const std::wstring link = ShortcutPath(*folder); !link.empty())
            files.push_back(link);
    if (options.saves)
        folders.push_back(exeDir + L"\\saves");
    if (options.mods)
        folders.push_back(exeDir + L"\\mods");
    if (options.settings) {
        files.push_back(exeDir + L"\\settings.ini");
        files.push_back(exeDir + L"\\controls.ini");
    }
    std::wstring left;
    for (const std::wstring& folder : folders) {
        DeleteTree(folder);
        if (Exists(folder))
            left += L"\n" + folder;
    }
    for (const std::wstring& file : files) {
        if (Exists(file) && !DeleteFileW(file.c_str()))
            left += L"\n" + file;
    }
    if (!left.empty()) {
        error = L"These could not be deleted (a file in them may be open in another program):" + left;
        LOG_WARN("Uninstall: not everything could be deleted:%ls", left.c_str());
        return false;
    }
    LOG_INFO("Uninstall: done");
    return true;
}

bool EnsureGameData(const std::wstring& exeDir, const std::wstring& gameData, const std::wstring& imagePath)
{
    if (GameDataInstalled(gameData))
        return true;

    std::wstring image = imagePath;
    if (image.empty()) {
        const int answer = MessageBoxW(nullptr,
            L"The game files are not installed yet.\n\n"
            L"Choose your Star Wars: Episode III - Revenge of the Sith (Xbox) disc image: an .iso / .xiso file, "
            L"or a .7z / .zip / .rar archive containing one. The game files are copied into the GameData folder "
            L"next to swrots.exe (about 2.5 GB).",
            kTitle, MB_OKCANCEL | MB_ICONINFORMATION);
        if (answer != IDOK)
            return false;
        image = ChooseDiscImage(nullptr);
        if (image.empty())
            return false;
    }

    Progress p;
    RunWithProgress(p, [&] { InstallGameFiles(exeDir, gameData, image, p); });
    if (!p.ok) {
        if (p.code != InstallError::Cancelled)
            MessageBoxW(nullptr, (std::wstring(L"Setup could not finish: ") + InstallErrorTitle(p.code) + L".\n\n"
                                  + p.error).c_str(), kTitle, MB_OK | MB_ICONERROR);
        return false;
    }

    // The game's own icon for shortcuts, and the shortcuts themselves.
    const bool desktop = MessageBoxW(nullptr, L"The game is installed and will be added to the Start menu.\n\nAlso "
                                              L"add a shortcut to the desktop?", kTitle, MB_YESNO | MB_ICONINFORMATION)
        == IDYES;
    CreateGameShortcuts(exeDir, gameData, desktop);
    return true;
}

} // namespace swrots
