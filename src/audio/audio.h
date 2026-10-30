#pragma once

#include <cstdint>

namespace swrots::audio {

// Points DirectSound library globals at runtime-owned data (call once the game
// image is mapped).
void InitXboxGlobals();

// A developer tool's copy of the final mix (before the output volume): interleaved float samples, or null for that
// many frames of silence; called on the audio thread. Null for none.
using MixTap = void (*)(const float* samples, unsigned frames, unsigned channels, unsigned sampleRate);
void SetMixTap(MixTap tap);

// Netplay (net/rollback.h): while on, everything the game learns from the sound layer (whether a buffer plays,
// where it is, the audio clock, when a stream packet is done) comes from a deterministic model on the fixed clock
// (kernel/clock.h), whose state is saved and restored with the game's (DeterministicState). Called on the game's
// main thread at a frame boundary.
void SetDeterministic(bool on);
bool Deterministic();
// A frame played again: the model answers as the first time, nothing new is heard.
void SetReplaying(bool replaying);
// At every frame boundary while on: the model's stream packets done by the frame's start are completed (the same
// in every play of the frame).
void OnFrame();
void* DeterministicState(uint32_t& size);

// Stops all sound output (used when the game restarts itself).
void Silence();

// The game rebooted in-process: destroys every buffer and stream it created (they point
// into its memory) and forgets its pending stream callbacks.
void ResetForReboot();

} // namespace swrots::audio
