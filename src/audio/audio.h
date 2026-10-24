#pragma once

namespace swrots::audio {

// Points DirectSound library globals at runtime-owned data (call once the game
// image is mapped).
void InitXboxGlobals();

// A developer tool's copy of the final mix (before the output volume): interleaved float samples, or null for that
// many frames of silence; called on the audio thread. Null for none.
using MixTap = void (*)(const float* samples, unsigned frames, unsigned channels, unsigned sampleRate);
void SetMixTap(MixTap tap);

// Stops all sound output (used when the game restarts itself).
void Silence();

// The game rebooted in-process: destroys every buffer and stream it created (they point
// into its memory) and forgets its pending stream callbacks.
void ResetForReboot();

} // namespace swrots::audio
