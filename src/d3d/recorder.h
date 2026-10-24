#pragma once
// Flight recorder: keeps the last few seconds of frames (a small screenshot of
// each plus every draw call and its state) and writes them out when the player
// presses Ctrl+Shift+F10, for diagnosing one-frame glitches.

#include <windows.h>
#include <d3d9.h>

#include <string>

namespace swrots::d3d {

// Called once at startup ([Debug] FlightRecorder=1 in settings.ini; off by default).
void ConfigureFlightRecorder(bool enabled, const std::wstring& outputDir);

// Remembers the game code that issued the draw about to be recorded. Call at
// the top of a replaced draw function with _AddressOfReturnAddress().
void FlightNoteCaller(void* addressOfReturnAddress);
// Records the draw being made from the current Xbox state.
// `indices` lists the vertexCount vertex indices drawn, `firstVertex`..`lastVertex` their range.
void FlightRecordDraw(DWORD primitive, UINT vertexCount, UINT firstVertex, int vertexMode, const void* vertexData,
    UINT stride, const uint32_t* indices, UINT lastVertex);
// Marks the last recorded draw as skipped (not sent to the host device).
void FlightMarkSkipped();
// Adds a free-form event to the current frame.
void FlightNote(const char* fmt, ...);
// Ends a frame: keeps a thumbnail of the finished back buffer and handles the hotkey.
void FlightEndFrame(IDirect3DSurface9* backBuffer);
// Screenshots: the game's frame as the window shows it (without the debug menu), saved as PNG into
// `directory` (set once at startup). A request is taken at the next presented frame; `name` is the file's
// name without .png (empty: the date and time). Safe to call from any thread.
void SetScreenshotDirectory(const std::wstring& directory);
void RequestScreenshot(const std::string& name);
// Saves a requested screenshot from the window's back buffer (call before drawing the debug menu).
void SavePendingScreenshot(IDirect3DSurface9* windowBuffer);
// A developer tool's look at every presented frame (the window's back buffer as screenshots take it, before the
// debug menu): called on the render thread; null for none.
using FrameTap = void (*)(IDirect3DSurface9* windowBuffer);
void SetFrameTap(FrameTap tap);
void TapFrame(IDirect3DSurface9* windowBuffer);
// Frees GPU resources (device teardown).
void FlightReleaseResources();

} // namespace swrots::d3d
