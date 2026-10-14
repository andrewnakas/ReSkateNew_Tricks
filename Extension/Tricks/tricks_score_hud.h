#pragma once
// The mod's tricks in skate.'s own trick list (the score HUD's TrickHistory): each trick done is added there as an
// entry of the game's own kind (TrickHistoryStackItem), with its name and points.
//
// ScoringHUDViewModel (schema 0x9b74fc7c) holds TrickHistory, a stack view model whose Items are stack items; each item's
// Content shows UI/Features/HUD/Scoring/TrickHistoryStackItem with a TrickHistoryItemViewModel (0xbf5e8a16) as its data:
// TrickName, TrickScore and TrickCompletedInfo labels (read from a live dump, 2026-10-14). The newest entry is first.
// An entry is cloned from one the game made (fonts, icon, styles), so the first mod trick of a session waits for the
// game's first trick to be shown.
#include <cstdint>
#include <string>

namespace dingosdk::tricks {
// Queues an entry; game (client) thread.
void queue_trick_entry(const std::string &name, int points) noexcept;
// Adds what is queued and frees entries the game has dropped from its list. Game (client) thread, every tick.
void update_trick_entries(std::uintptr_t base) noexcept;
// <dev> The line score the score HUD shows (ScoringHUDViewModel 0x40bff667, a float). Game (client) thread.
bool read_hud_line_score(std::uintptr_t base, float &value) noexcept;
// <dev> Also write our points into the game's line totals (ActiveScoringData): off, it changed nothing on screen.
void set_line_total_writes(bool on) noexcept;
// <dev> What happened, for the logs.
std::string trick_entries_status();
} // namespace dingosdk::tricks
