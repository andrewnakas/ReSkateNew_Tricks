#pragma once
// ReSkate Tricks: tricks the community asked for, built from what the game already ships.
//
// The game thread owns the state (tricks.cpp). Every change is a `tricks ...` console command.
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dingosdk::tricks {
// Each client tick; `playing` while a local skater can exist.
void tick(std::uintptr_t base, std::uintptr_t client, bool playing) noexcept;
// Runs one `tricks` command and returns the line to print. Only from the console dispatcher.
std::string command(std::string_view verb, const std::vector<std::string> &arguments);
} // namespace dingosdk::tricks
