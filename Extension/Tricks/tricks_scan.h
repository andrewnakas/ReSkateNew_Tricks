#pragma once
// <dev> `tricks scan`: a value search over the game's memory (tricks_scan.cpp).
#include <string>

namespace dingosdk::tricks {
// what: "new" (search for `value`), "next" (keep the addresses now holding `value`), "list", "status", "add" (add
// `value` to every kept address, once few are left).
std::string scan_command(const std::string &what, double value);
// Addresses still kept (a large number before the first search), and whether a search runs.
std::size_t scan_left();
bool scan_busy();
} // namespace dingosdk::tricks
