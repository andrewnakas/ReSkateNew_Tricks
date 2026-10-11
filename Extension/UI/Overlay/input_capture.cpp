#include "Engine/Core/Log/logging.h"
#include "overlay_internal.h"
#include "input_capture.h"
#include "held_input.h"
#include "Engine/Game/Input/playstation_report.h"
#include <hidsdi.h>
#include <atomic>
#include <format>
#include <array>
#include <cstring>
#include <string>
#include <mutex>
#include <set>
#include <span>
#include <tuple>

namespace dingosdk::overlay::detail {
// Window messages do not cover the game's independently polled input devices.
// Only the overlay's own Win32 backend and binding reader may bypass these gates.
thread_local HRAWINPUT noted_raw_input = nullptr;
namespace {
std::atomic<unsigned> prompt_xinput{}, prompt_direct{}, prompt_hid{}, prompt_raw{};
// A read the game made while a card held its input: counted, by the way it read.
void note_prompt_read(std::atomic<unsigned>& count) {
    if (!overlay_input_access && state().prompt_input_active.load(std::memory_order_relaxed)) count.fetch_add(1, std::memory_order_relaxed);
}
}
PromptReads take_prompt_reads() noexcept {
    return {prompt_xinput.exchange(0), 0, prompt_direct.exchange(0), prompt_hid.exchange(0), prompt_raw.exchange(0)};
}
bool block_polled_input() {
    const auto error = GetLastError();
    const bool capture = !overlay_input_access && (owns_menu_cursor(state()) || state().freecam_controller_active.load(std::memory_order_relaxed) ||
                                                    state().prompt_input_active.load(std::memory_order_relaxed));
    SetLastError(error);
    return capture;
}

void note_input_record(const RAWINPUT& record) noexcept;
HeldInput& held_input() { static auto* value = new HeldInput; return *value; }

struct InputCaptureHooks {
    Hook hid_attributes, read_file, overlapped, overlapped_ex, completion, completion_ex, close_handle;
    struct HidRead { HANDLE file; void* buffer; DWORD capacity; PlayStationPad kind; };
    std::mutex hid_mutex;
    std::map<HANDLE, PlayStationPad> hid_devices;
    std::map<OVERLAPPED*, HidRead> hid_reads;
    Hook register_raw;
    // <dev>
    Hook foreground, active, focus;
    // </dev>
    Hook async_key, key, keyboard, raw_data, raw_buffer, capture, release_capture, cursor, show_cursor, physical_cursor, cursor_position;
    std::array<Hook, 5> xinput_state, xinput_extended, xinput_keystroke;
    // Hooks other programs put in front of those, hooked in turn (keep_xinput_capture_first).
    static constexpr std::size_t front_slots = 8;
    std::array<Hook, front_slots> xinput_front;
    std::mutex front_mutex;
    Hook direct_create;
    std::array<Hook, 2> device_create;
    // Mouse/keyboard and A/W devices may have distinct implementations. Slots
    // describe callable addresses, not the CreateDevice interface index.
    static constexpr std::size_t device_slots = 8;
    std::array<Hook, device_slots> device_state, device_data, device_format;
    struct Format { DWORD size{}; std::vector<DIOBJECTDATAFORMAT> objects; };
    std::mutex formats_mutex;
    std::map<void*, Format> formats;
};
InputCaptureHooks& input_hooks() { static auto* hooks = new InputCaptureHooks; return *hooks; }

BOOLEAN __stdcall captured_hid_attributes(HANDLE file, PHIDD_ATTRIBUTES attributes) {
    const auto result = original<decltype(&HidD_GetAttributes)>(input_hooks().hid_attributes)(file, attributes);
    const auto error = GetLastError();
    if (result && attributes) {
        const auto kind = playstation_pad(attributes->VendorID, attributes->ProductID);
        if (kind != PlayStationPad::none) {
            std::lock_guard lock(input_hooks().hid_mutex);
            input_hooks().hid_devices[file] = kind;
        }
    }
    SetLastError(error);
    return result;
}
void neutral_hid(const InputCaptureHooks::HidRead& read, DWORD size) {
    if (!block_polled_input() || !read.buffer || size > read.capacity) return;
    auto* bytes = static_cast<std::uint8_t*>(read.buffer);
    // USB or Bluetooth decides where the buttons are in a report with id 1. It is the device's
    // report length that tells them apart, and Windows pads every read to that length, so it
    // is how many bytes were read: not how large a buffer the game happens to read into. A
    // game reading a USB DualSense into a larger buffer had its d-pad byte written as "up"
    // here, and placed a session marker whenever a menu took the input. (Nothing is asked of
    // the game's handle for this: 2.0.4 did, as the pad was first read.)
    const bool bluetooth = playstation_bluetooth(size);
    if (!parse_playstation_report(read.kind, bluetooth, bytes, size)) return;
    const bool sense_full = read.kind == PlayStationPad::dualsense &&
        (bytes[0] == 0x31 || (bytes[0] == 1 && !bluetooth));
    const unsigned offset = bytes[0] == 0x31 ? 2 : bytes[0] == 0x11 ? 3 : 1;
    auto* data = bytes + offset;
    std::fill_n(data, 4, std::uint8_t{128});
    if (sense_full) {
        data[4] = data[5] = 0; // triggers; retain report sequence at +6
        data[7] = 8; data[8] = data[9] = 0;
    } else {
        data[4] = 8; data[5] = 0; data[6] &= 0xfc; // retain DS4 sequence bits
        data[7] = data[8] = 0;
    }
    // Full Bluetooth reports have a CRC over the 0xA1 input prefix and payload.
    // A stale checksum makes the native reader discard the neutral report.
    if (size == 78 && (bytes[0] == 0x31 || bytes[0] == 0x11)) {
        std::uint32_t crc = 0xffffffffu;
        const auto feed = [&](std::uint8_t byte) {
            crc ^= byte;
            for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        };
        feed(0xa1);
        for (DWORD i = 0; i < size - 4; ++i) feed(bytes[i]);
        crc = ~crc;
        std::memcpy(bytes + size - 4, &crc, sizeof(crc));
    }
    static std::atomic<bool> reported{};
    if (!reported.exchange(true)) logging::printf(logging::Level::info, logging::Channel::input,
        "Native PlayStation HID report neutralized; overlay retains its independent input stream. (%s over %s, report 0x%02x of %lu bytes, "
        "the game's buffer %lu.)", read.kind == PlayStationPad::dualsense ? "DualSense" : "DualShock 4",
        bluetooth ? "Bluetooth" : "USB", bytes[0], size, read.capacity);
}
void complete_hid(OVERLAPPED* request, DWORD bytes, bool success, bool finished) {
    auto& h = input_hooks();
    InputCaptureHooks::HidRead read{};
    {
        std::lock_guard lock(h.hid_mutex);
        const auto found = h.hid_reads.find(request);
        if (found == h.hid_reads.end()) return;
        read = found->second;
        if (finished) h.hid_reads.erase(found);
    }
    if (success) neutral_hid(read, bytes);
}
BOOL WINAPI captured_read_file(HANDLE file, LPVOID buffer, DWORD size, LPDWORD bytes, LPOVERLAPPED request) {
    auto& h = input_hooks();
    InputCaptureHooks::HidRead read{file, buffer, size, PlayStationPad::none};
    if (!overlay_input_access) {
        // Some readers inspect a temporary enumeration handle, then open a
        // second handle for reports. Identify that handle as well.
        bool known{};
        {
            std::lock_guard lock(h.hid_mutex);
            known = h.hid_devices.contains(file);
        }
        if (!known && size >= 10 && size <= 512) {
            const auto error = GetLastError();
            HIDD_ATTRIBUTES attributes{sizeof(attributes)};
            const auto okay = original<decltype(&HidD_GetAttributes)>(h.hid_attributes)(file, &attributes);
            const auto kind = okay ? playstation_pad(attributes.VendorID, attributes.ProductID) : PlayStationPad::none;
            {
                std::lock_guard lock(h.hid_mutex);
                h.hid_devices[file] = kind;
            }
            SetLastError(error);
        }
        std::lock_guard lock(h.hid_mutex);
        const auto found = h.hid_devices.find(file);
        if (found != h.hid_devices.end() && found->second != PlayStationPad::none) {
            read.kind = found->second;
            if (request) h.hid_reads[request] = read;
        }
    }
    const auto result = original<decltype(&ReadFile)>(h.read_file)(file, buffer, size, bytes, request);
    const auto error = GetLastError();
    if (read.kind != PlayStationPad::none) {
        if (result) neutral_hid(read, bytes ? *bytes : request ? static_cast<DWORD>(request->InternalHigh) : 0);
        if (!result && error != ERROR_IO_PENDING && request) complete_hid(request, 0, false, true);
    }
    SetLastError(error);
    return result;
}
BOOL WINAPI captured_overlapped(HANDLE file, LPOVERLAPPED request, LPDWORD bytes, BOOL wait) {
    const auto result = original<decltype(&GetOverlappedResult)>(input_hooks().overlapped)(file, request, bytes, wait);
    const auto error = GetLastError();
    complete_hid(request, result && bytes ? *bytes : 0, result != FALSE, result || error != ERROR_IO_INCOMPLETE);
    SetLastError(error); return result;
}
BOOL WINAPI captured_overlapped_ex(HANDLE file, LPOVERLAPPED request, LPDWORD bytes, DWORD timeout, BOOL alertable) {
    const auto result = original<decltype(&GetOverlappedResultEx)>(input_hooks().overlapped_ex)(file, request, bytes, timeout, alertable);
    const auto error = GetLastError();
    complete_hid(request, result && bytes ? *bytes : 0, result != FALSE,
        result || (error != ERROR_IO_INCOMPLETE && error != WAIT_TIMEOUT && error != WAIT_IO_COMPLETION));
    SetLastError(error); return result;
}
BOOL WINAPI captured_completion(HANDLE port, LPDWORD bytes, PULONG_PTR key, LPOVERLAPPED* request, DWORD timeout) {
    const auto result = original<decltype(&GetQueuedCompletionStatus)>(input_hooks().completion)(port, bytes, key, request, timeout);
    const auto error = GetLastError();
    if (request && *request) complete_hid(*request, result && bytes ? *bytes : 0, result != FALSE, true);
    SetLastError(error); return result;
}
BOOL WINAPI captured_completion_ex(HANDLE port, LPOVERLAPPED_ENTRY entries, ULONG count, PULONG removed, DWORD timeout, BOOL alertable) {
    const auto result = original<decltype(&GetQueuedCompletionStatusEx)>(input_hooks().completion_ex)(port, entries, count, removed, timeout, alertable);
    const auto error = GetLastError();
    if (result && entries && removed) for (ULONG i = 0; i < *removed; ++i)
        complete_hid(entries[i].lpOverlapped, entries[i].dwNumberOfBytesTransferred,
            static_cast<LONG>(entries[i].Internal) >= 0, true);
    SetLastError(error); return result;
}
BOOL WINAPI captured_close_handle(HANDLE file) {
    auto& h = input_hooks();
    {
        std::lock_guard lock(h.hid_mutex);
        if (h.hid_devices.erase(file))
            std::erase_if(h.hid_reads, [&](const auto& entry) { return entry.second.file == file; });
    }
    return original<decltype(&CloseHandle)>(h.close_handle)(file);
}

// Where the jump at an entry point goes, through any stubs on the way: the first address inside
// a module, and that module. Null when the entry point is not hooked (or cannot be read).
struct Landing { void* at{}; HMODULE module{}; };
Landing hook_landing(void* entry) {
    const auto self = GetCurrentProcess();
    const auto read = [&](const void* at, void* out, std::size_t size) {
        SIZE_T got{};
        return ReadProcessMemory(self, at, out, size, &got) && got == size;
    };
    auto* at = static_cast<std::uint8_t*>(entry);
    for (int hop = 0; hop < 8; ++hop) {
        std::uint8_t code[16]{};
        if (!read(at, code, sizeof(code))) return {};
        std::uint8_t* next{};
        if (code[0] == 0xe9) {
            std::int32_t offset; std::memcpy(&offset, code + 1, 4);
            next = at + 5 + offset;
        } else if (code[0] == 0xff && code[1] == 0x25) {
            std::int32_t offset; std::memcpy(&offset, code + 2, 4);
            if (!read(at + 6 + offset, &next, sizeof(next))) return {};
        } else if (code[0] == 0x48 && code[1] == 0xb8 && code[10] == 0xff && code[11] == 0xe0) {
            std::memcpy(&next, code + 2, 8);
        } else return {};
        at = next;
        HMODULE module{};
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(at), &module) && module) return {at, module};
    }
    return {};
}
HMODULE module_of(const void* address) {
    HMODULE module{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(address), &module);
    return module;
}
std::string module_name(HMODULE module) {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(module, path, MAX_PATH);
    const wchar_t* name = path;
    for (const wchar_t* c = path; *c; ++c) if (*c == L'\\') name = c + 1;
    std::string text;
    for (; *name; ++name) text += static_cast<char>(*name);
    return text;
}
std::string xinput_entry_owners() {
    std::string text;
    const auto& h = input_hooks();
    const auto owner = [](void* entry) {
        const auto landing = hook_landing(entry);
        return landing.module ? module_name(landing.module) : std::string("not hooked");
    };
    constexpr std::array names{"xinput1_4", "xinput1_3", "xinput9_1_0", "xinput1_2", "xinput1_1"};
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (h.xinput_state[i].target) text += (text.empty() ? "" : ", ") + std::string(names[i]) + " " + owner(h.xinput_state[i].target);
        if (h.xinput_extended[i].target) text += (text.empty() ? "" : ", ") + std::string(names[i]) + " (extended) " + owner(h.xinput_extended[i].target);
    }
    return text.empty() ? "none" : text;
}

// Another program's hook on XInput that was put in front of the capture's own after it was
// installed (Steam's overlay does, for Steam Input, and answers the game with its own pad
// without going on to XInput): the capture then never sees the game's reads, and a menu that
// holds the game's input does not hold its controller. That hook's own function is hooked
// then, so the capture is in front of it again.
template<std::size_t I>
DWORD WINAPI captured_front(DWORD user, XINPUT_STATE* output) {
    const bool capture = block_polled_input();
    note_prompt_read(prompt_xinput);
    DWORD result;
    { // It may go on to XInput's own entry point, which is hooked as well.
        OverlayInputAccess access;
        result = original<DWORD(WINAPI*)(DWORD, XINPUT_STATE*)>(input_hooks().xinput_front[I])(user, output);
    }
    if (result == ERROR_SUCCESS && output && capture) {
        output->Gamepad = {};
        output->dwPacketNumber ^= 0x80000000u;
    }
    return result;
}
void keep_xinput_capture_first() {
    auto& h = input_hooks();
    static const HMODULE self = module_of(&keep_xinput_capture_first);
    const auto detours = []<std::size_t... I>(std::index_sequence<I...>) {
        return std::array{reinterpret_cast<void*>(captured_front<I>)...};
    }(std::make_index_sequence<InputCaptureHooks::front_slots>{});
    std::lock_guard lock(h.front_mutex);
    const auto guard = [&](void* entry) {
        if (!entry) return;
        const auto landing = hook_landing(entry);
        // Not hooked over, the capture's own, or a jump inside XInput itself.
        if (!landing.at || landing.module == self || landing.module == module_of(entry)) return;
        for (const auto& hook : h.xinput_front) if (hook.target == landing.at) return;
        for (std::size_t slot = 0; slot < h.xinput_front.size(); ++slot) {
            if (h.xinput_front[slot].target) continue;
            const bool okay = install(h.xinput_front[slot], landing.at, detours[slot]);
            logging::printf(okay ? logging::Level::info : logging::Level::warning, logging::Channel::input,
                okay ? "XInput is answered by %s ahead of ReSkate: its reads are now held with the rest of the game's input."
                     : "XInput is answered by %s ahead of ReSkate, and that could not be hooked: a controller may reach the game behind a menu.",
                module_name(landing.module).c_str());
            return;
        }
    };
    for (std::size_t i = 0; i < h.xinput_state.size(); ++i) guard(h.xinput_state[i].target), guard(h.xinput_extended[i].target);
}
SHORT WINAPI captured_async_key(int key) {
    const auto value = original<SHORT(WINAPI*)(int)>(input_hooks().async_key)(key);
    return block_polled_input() ? 0 : value;
}
SHORT WINAPI captured_key(int key) {
    const auto value = original<SHORT(WINAPI*)(int)>(input_hooks().key)(key);
    return block_polled_input() ? 0 : value;
}
BOOL WINAPI captured_keyboard(PBYTE keys) {
    const auto result = original<BOOL(WINAPI*)(PBYTE)>(input_hooks().keyboard)(keys);
    if (result && keys && block_polled_input()) std::memset(keys, 0, 256);
    return result;
}
UINT WINAPI captured_raw_data(HRAWINPUT input, UINT command, LPVOID data, PUINT size, UINT header_size) {
    const auto result = original<UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT)>(
        input_hooks().raw_data)(input, command, data, size, header_size);
    if (result != UINT(-1) && data && command == RID_INPUT && !overlay_input_access && input != noted_raw_input &&
        result >= offsetof(RAWINPUT, data) + sizeof(RAWMOUSE) &&
        static_cast<RAWINPUT*>(data)->header.dwType == RIM_TYPEMOUSE)
        note_raw_mouse_packet(static_cast<RAWINPUT*>(data)->data.mouse);
    if (result != UINT(-1) && data && command == RID_INPUT && !overlay_input_access &&
        result >= offsetof(RAWINPUT, data) + sizeof(RAWKEYBOARD))
        note_input_record(*static_cast<RAWINPUT*>(data));
    if (result != UINT(-1) && data && command == RID_INPUT && result >= sizeof(RAWINPUTHEADER)) {
        auto* raw = static_cast<RAWINPUT*>(data);
        const bool mouse = raw->header.dwType == RIM_TYPEMOUSE && result >= offsetof(RAWINPUT, data) + sizeof(RAWMOUSE);
        const bool keyboard =
            raw->header.dwType == RIM_TYPEKEYBOARD && result >= offsetof(RAWINPUT, data) + sizeof(RAWKEYBOARD);
        if (!block_polled_input()) {
            if ((mouse || keyboard) && !overlay_input_access) held_input().seen(*raw);
        } else if ((mouse || keyboard) && held_input().release_only(*raw)) {
            // A key or button the game holds still comes up; presses stay blocked.
        } else if (mouse)
            raw->data.mouse = {};
        else if (keyboard) {
            raw->data.keyboard = {};
            raw->data.keyboard.VKey = 255; // documented discard packet; never synthesize a key press
            raw->data.keyboard.Flags = RI_KEY_BREAK;
            raw->data.keyboard.Message = WM_KEYUP;
        } else if (raw->header.dwType == RIM_TYPEHID && result >= offsetof(RAWINPUT, data) + 8)
            raw->data.hid.dwCount = 0;
    }
    return result;
}
// The latest keyboard and mouse input the game read, with the device it came from, for the
// log line when the game switches to mouse and keyboard: which input made it switch. Raw
// input injected by software (SendInput: Steam Input's desktop layout, macro tools) carries
// no device handle.
struct LastInput {
    std::atomic<ULONGLONG> key_at{}, mouse_at{}, message_at{};
    std::atomic<std::uint32_t> key{}; // virtual key | make code << 16
    std::atomic<std::int32_t> mouse_x{}, mouse_y{};
    std::atomic<std::uint32_t> mouse_buttons{};
    std::atomic<HANDLE> key_device{}, mouse_device{};
    std::atomic<std::uint32_t> message{}, message_key{}; // the game window's last input message
};
LastInput& last_input() { static auto* value = new LastInput; return *value; }
void note_input_record(const RAWINPUT& record) noexcept {
    auto& last = last_input();
    if (record.header.dwType == RIM_TYPEKEYBOARD) {
        last.key.store(record.data.keyboard.VKey | (std::uint32_t{record.data.keyboard.MakeCode} << 16),
                       std::memory_order_relaxed);
        last.key_device.store(record.header.hDevice, std::memory_order_relaxed);
        last.key_at.store(GetTickCount64(), std::memory_order_relaxed);
    } else if (record.header.dwType == RIM_TYPEMOUSE &&
               (record.data.mouse.lLastX || record.data.mouse.lLastY || record.data.mouse.usButtonFlags)) {
        last.mouse_x.store(record.data.mouse.lLastX, std::memory_order_relaxed);
        last.mouse_y.store(record.data.mouse.lLastY, std::memory_order_relaxed);
        last.mouse_buttons.store(record.data.mouse.usButtonFlags, std::memory_order_relaxed);
        last.mouse_device.store(record.header.hDevice, std::memory_order_relaxed);
        last.mouse_at.store(GetTickCount64(), std::memory_order_relaxed);
    }
}
std::string input_device_name(HANDLE device) {
    if (!device) return "no device (injected by software)";
    std::array<wchar_t, 256> name{};
    UINT size = static_cast<UINT>(name.size());
    if (GetRawInputDeviceInfoW(device, RIDI_DEVICENAME, name.data(), &size) == UINT(-1)) return "an unknown device";
    std::string text;
    for (const wchar_t* c = name.data(); *c && text.size() < 120; ++c) text.push_back(*c < 128 ? static_cast<char>(*c) : '?');
    return text;
}
void note_input_message(UINT message, WPARAM wp) noexcept {
    auto& last = last_input();
    last.message.store(message, std::memory_order_relaxed);
    last.message_key.store(static_cast<std::uint32_t>(wp), std::memory_order_relaxed);
    last.message_at.store(GetTickCount64(), std::memory_order_relaxed);
}
// What the game last read from the keyboard and mouse, within the 3 s before now.
std::string recent_input() {
    auto& last = last_input();
    const auto now = GetTickCount64();
    std::string text;
    const auto age = [&](ULONGLONG at) { return std::to_string(now - at) + " ms before"; };
    if (const auto at = last.key_at.load(); at && now - at <= 3000) {
        const auto key = last.key.load();
        text += std::format("key 0x{:02x} (scan 0x{:02x}) from {}, {}", key & 0xffff, key >> 16,
                            input_device_name(last.key_device.load()), age(at));
    }
    if (const auto at = last.mouse_at.load(); at && now - at <= 3000) {
        if (!text.empty()) text += "; ";
        text += std::format("mouse move ({}, {}) buttons 0x{:x} from {}, {}", last.mouse_x.load(), last.mouse_y.load(),
                            last.mouse_buttons.load(), input_device_name(last.mouse_device.load()), age(at));
    }
    if (const auto at = last.message_at.load(); at && now - at <= 3000) {
        if (!text.empty()) text += "; ";
        text += std::format("window message 0x{:x} ({:#x}), {}", last.message.load(), last.message_key.load(), age(at));
    }
    return text.empty() ? "no keyboard or mouse input in the 3 s before" : text;
}
UINT WINAPI captured_raw_buffer(PRAWINPUT data, PUINT size, UINT header_size) {
    // Calling the API drains queued input. While the overlay owns input the game gets only the
    // releases of keys and buttons it holds (HeldInput), so nothing pressed in the menu or chat
    // replays when it closes and nothing pressed before it stays stuck.
    const auto result = original<UINT(WINAPI*)(PRAWINPUT, PUINT, UINT)>(
        input_hooks().raw_buffer)(data, size, header_size);
    const bool capture = result != UINT(-1) && data && block_polled_input();
    note_prompt_read(prompt_raw);
    if (result != UINT(-1) && data && header_size == sizeof(RAWINPUTHEADER)) {
        // Skate drains its mouse here; read the packets on the way past.
        // NEXTRAWINPUTBLOCK: records are 8-byte aligned on x64.
        const auto align = [](std::uintptr_t at) { return (at + 7) & ~std::uintptr_t{7}; };
        auto* record = data;
        auto write = reinterpret_cast<std::uintptr_t>(data);
        UINT kept{};
        for (UINT i = 0; i < result && record; ++i) {
            const auto bytes = record->header.dwSize;
            const auto next = align(reinterpret_cast<std::uintptr_t>(record) + bytes);
            if (record->header.dwType == RIM_TYPEMOUSE) note_raw_mouse_packet(record->data.mouse);
            note_input_record(*record);
            const bool mouse = record->header.dwType == RIM_TYPEMOUSE && bytes >= offsetof(RAWINPUT, data) + sizeof(RAWMOUSE);
            const bool keyboard =
                record->header.dwType == RIM_TYPEKEYBOARD && bytes >= offsetof(RAWINPUT, data) + sizeof(RAWKEYBOARD);
            if (!capture) {
                if ((mouse || keyboard) && !overlay_input_access) held_input().seen(*record);
            } else if ((mouse || keyboard) && held_input().release_only(*record)) {
                // Kept records move forward over the dropped ones, in the same layout.
                if (write != reinterpret_cast<std::uintptr_t>(record))
                    std::memmove(reinterpret_cast<void*>(write), record, bytes);
                write = align(write + bytes);
                ++kept;
            }
            record = reinterpret_cast<RAWINPUT*>(next);
        }
        if (capture) return kept;
    }
    return capture ? 0 : result;
}
// The game's latest mouse registration, and whether the overlay has replaced
// it with a cursor-moving one.
struct RawMouse {
    std::mutex mutex;
    RAWINPUTDEVICE game{};
    bool known{}, overridden{};
};
RawMouse& raw_mouse() { static auto* value = new RawMouse; return *value; }
constexpr DWORD cursor_blocking_flags = RIDEV_NOLEGACY | RIDEV_CAPTUREMOUSE | RIDEV_NOHOTKEYS;
bool is_mouse(const RAWINPUTDEVICE& device) { return device.usUsagePage == 1 && device.usUsage == 2; }

// Logs how the game registers raw input (flags change with its input mode),
// once per distinct registration.
BOOL WINAPI captured_register_raw(PCRAWINPUTDEVICE devices, UINT count, UINT size) {
    const auto register_raw = original<BOOL(WINAPI*)(PCRAWINPUTDEVICE, UINT, UINT)>(input_hooks().register_raw);
    BOOL result{};
    if (devices && count && size == sizeof(RAWINPUTDEVICE) && count <= 64) {
        std::vector<RAWINPUTDEVICE> requested(devices, devices + count);
        auto& mouse = raw_mouse();
        std::lock_guard lock(mouse.mutex);
        for (auto& device : requested) {
            if (!is_mouse(device)) continue;
            if (device.dwFlags & RIDEV_REMOVE) mouse.known = false;
            else { mouse.game = device; mouse.known = true; }
            // While the overlay owns the pointer the cursor must keep moving.
            if (mouse.overridden && !(device.dwFlags & RIDEV_REMOVE)) device.dwFlags &= ~cursor_blocking_flags;
        }
        result = register_raw(requested.data(), count, size);
        if (result)
            for (const auto& device : requested)
                if (is_mouse(device))
                    state().mouse_legacy_blocked.store(!(device.dwFlags & RIDEV_REMOVE) &&
                        (device.dwFlags & RIDEV_NOLEGACY) == RIDEV_NOLEGACY);
        // Skate re-registers the mouse whenever it switches between controller and
        // mouse/keyboard: the one trace of which input it thinks the player uses.
        static DWORD last_mode = ~0ul;
        for (const auto& device : devices ? std::span(devices, count) : std::span<const RAWINPUTDEVICE>{})
            if (is_mouse(device) && device.dwFlags != last_mode) {
                last_mode = device.dwFlags;
                // Leaving controller mode: say what input came just before.
                const auto cause = device.dwFlags & RIDEV_NOLEGACY ? std::string{} : "; last input: " + recent_input();
                dingosdk::logging::printf(dingosdk::logging::Level::info, dingosdk::logging::Channel::input,
                    "Game input mode: mouse registered with flags 0x%lx (%s)%s%s.", device.dwFlags,
                    device.dwFlags & RIDEV_REMOVE ? "removed"
                    : (device.dwFlags & RIDEV_NOLEGACY) ? "controller: no Windows mouse messages" : "mouse and keyboard",
                    mouse.overridden ? "; the overlay owns the pointer" : "", cause.c_str());
            }
    } else {
        result = register_raw(devices, count, size);
    }
    try {
        static std::mutex mutex;
        static std::set<std::tuple<USHORT, USHORT, DWORD>> seen;
        for (UINT i = 0; devices && size == sizeof(RAWINPUTDEVICE) && i < count; ++i) {
            const auto& device = devices[i];
            {
                std::lock_guard lock(mutex);
                if (!seen.emplace(device.usUsagePage, device.usUsage, device.dwFlags).second) continue;
            }
            dingosdk::logging::printf(dingosdk::logging::Level::debug, dingosdk::logging::Channel::input,
                "Game raw input registration: page=%u usage=%u flags=0x%lx window=%p result=%d",
                device.usUsagePage, device.usUsage, device.dwFlags, device.hwndTarget, result ? 1 : 0);
        }
    } catch (...) {}
    return result;
}
void apply_raw_mouse(RawMouse& mouse, bool overlay) {
    auto device = mouse.game;
    if (overlay) device.dwFlags &= ~cursor_blocking_flags;
    const auto register_raw = original<BOOL(WINAPI*)(PCRAWINPUTDEVICE, UINT, UINT)>(input_hooks().register_raw);
    OverlayInputAccess access;
    const bool applied = register_raw ? register_raw(&device, 1, sizeof(device)) != FALSE
                                      : RegisterRawInputDevices(&device, 1, sizeof(device)) != FALSE;
    if (applied) state().mouse_legacy_blocked.store((device.dwFlags & RIDEV_NOLEGACY) == RIDEV_NOLEGACY);
    dingosdk::logging::printf(dingosdk::logging::Level::info, dingosdk::logging::Channel::input,
        "Raw mouse registration %s (flags=0x%lx, %s, Windows error %lu).",
        overlay ? "released for the overlay" : "returned to the game", device.dwFlags, applied ? "ok" : "failed",
        applied ? 0ul : GetLastError());
}
HWND WINAPI captured_capture(HWND window) {
    if (block_polled_input() || (overlay_input_access &&
        GetWindowThreadProcessId(window, nullptr) != GetCurrentThreadId())) return GetCapture();
    return original<HWND(WINAPI*)(HWND)>(input_hooks().capture)(window);
}
BOOL WINAPI captured_release_capture() {
    if (block_polled_input()) return TRUE;
    return original<BOOL(WINAPI*)()>(input_hooks().release_capture)();
}
HCURSOR WINAPI captured_cursor(HCURSOR cursor) {
    if (block_polled_input()) return GetCursor();
    return original<HCURSOR(WINAPI*)(HCURSOR)>(input_hooks().cursor)(cursor);
}
int WINAPI captured_show_cursor(BOOL show) {
    // ImGui draws the menu cursor. Do not let the game change the OS display
    // counter while it is open; the counter resumes unchanged on close.
    if (block_polled_input()) return show ? 0 : -1;
    return original<int(WINAPI*)(BOOL)>(input_hooks().show_cursor)(show);
}
BOOL WINAPI captured_physical_cursor(int x, int y) {
    if (block_polled_input()) return TRUE;
    return original<BOOL(WINAPI*)(int, int)>(input_hooks().physical_cursor)(x, y);
}
BOOL WINAPI captured_cursor_position(LPPOINT output) {
    thread_local POINT last{};
    thread_local bool sampled = false;
    if (output && block_polled_input() && sampled) { *output = last; return TRUE; }
    const auto result = original<BOOL(WINAPI*)(LPPOINT)>(input_hooks().cursor_position)(output);
    if (result && output && !overlay_input_access) { last = *output; sampled = true; }
    return result;
}
// <dev>
// A scripted controller (the trainer's `pad` command): while on, the game's polls of the
// first controller read this state, whether or not a real one is connected.
struct ScriptedPad {
    std::atomic<bool> on{};
    std::atomic<std::uint64_t> low{};  // XINPUT_GAMEPAD bytes 0..7
    std::atomic<std::uint32_t> high{}; // bytes 8..11
    std::atomic<std::uint32_t> packet{1};
    std::atomic<std::uint64_t> polls{}, served{}, blanked{};
};
ScriptedPad& scripted_pad() { static auto* value = new ScriptedPad; return *value; }
// While a script owns the controller the game is the foreground window as far as it can tell,
// so a scripted run does not need the desktop (the game reads no input in the background).
HWND scripted_window(bool root) {
    if (!scripted_pad().on.load(std::memory_order_acquire)) return nullptr;
    const HWND window = state().window.load();
    const HWND top = window && root ? GetAncestor(window, GA_ROOT) : nullptr;
    return top ? top : window;
}
HWND WINAPI scripted_foreground() {
    const HWND window = scripted_window(true);
    return window ? window : original<HWND(WINAPI*)()>(input_hooks().foreground)();
}
HWND WINAPI scripted_active() {
    const HWND window = scripted_window(true);
    return window ? window : original<HWND(WINAPI*)()>(input_hooks().active)();
}
HWND WINAPI scripted_focus() {
    const HWND window = scripted_window(false);
    return window ? window : original<HWND(WINAPI*)()>(input_hooks().focus)();
}
// </dev>
// ---- Dark Pop auto release ----------------------------------------------------------
// Driven by the physics selector hook (Extension/Skater/no_bail.cpp) through the extern "C" functions at the end
// of this file. During a timed flight it changes only what the game's own XInput polls see of D-pad Right. A trick
// held with RB (the catch) primes the flight; pressing D-pad Right after that fires it: the game sees D-pad Right
// held until the release time and then released until the player really lets go. D-pad Right without RB first is
// left alone. ReSkate's own readers (overlay_input_access) always get the real pad.
struct DarkPopRelease {
    std::atomic<bool> enabled{}, primed{}, latched{}, released{}, hiding{}; // primed: RB held in this flight
    std::atomic<std::int64_t> release_at{}; // QueryPerformanceCounter; 0 = no timed flight
    std::atomic<std::int64_t> shown_release{}; // when the game was shown the release (QPC), 0 = not yet
    std::atomic<unsigned> flags{}; // last change made: 1 = shown held, 2 = hidden (for the probe)
    std::atomic<std::uint64_t> polls{}; // the game's own XInput polls while enabled
};
DarkPopRelease& dark_pop_release() { static auto* value = new DarkPopRelease; return *value; }
// ---- One stick on a grind (the trainer's "Grinds feel like Skate 3") -----------------
// skate. turns the board on a grind with the left stick and leans with the right one; Skate 3 does both with the left.
// While this is on (the trainer turns it on only while the local skater is on a grind) the game's own polls see the
// left stick's sideways position on the right stick too, times this, unless the player is pushing the right stick
// sideways. The right stick's up and down stays the player's (it tips the board: 5-0, nose grind), and while it is being
// flicked (a pop) the game sees the right stick as it is, so a pop stays a pop.
std::atomic<float>& grind_stick_mirror() { static std::atomic<float> value{0.0f}; return value; }
std::atomic<std::uint64_t>& grind_stick_mirrored() { static std::atomic<std::uint64_t> value{0}; return value; }
// Skate 3's turn on a grind: while on, the game's own polls see a right stick whose sideways part is the trainer's (`x`,
// from the left stick) and whose up and down is the player's own, times `y_scale`, plus `y_add`. For 150 ms after the
// player's right stick jumps (the flick of a pop) the game sees the stick as it is. `sent_*` is what the game last saw.
struct GrindStickTurn {
    std::atomic<bool> on{};
    std::atomic<float> x{}, y_add{}, y_scale{1.0f};
    std::atomic<float> sent_x{}, sent_y{}, raw_y{};
    std::atomic<int> last_ry{0};
    std::atomic<std::uint64_t> flicked_at{0};
};
GrindStickTurn& grind_stick_turn() { static auto* value = new GrindStickTurn; return *value; }
bool turn_grind_stick(XINPUT_GAMEPAD& pad) noexcept {
    auto& turn = grind_stick_turn();
    if (!turn.on.load(std::memory_order_relaxed)) return false;
    const auto now = GetTickCount64();
    const int ry = pad.sThumbRY;
    if (std::abs(ry - turn.last_ry.exchange(ry, std::memory_order_relaxed)) > 5000) turn.flicked_at.store(now, std::memory_order_relaxed);
    const float raw_y = static_cast<float>(ry) / 32767.0f;
    turn.raw_y.store(raw_y, std::memory_order_relaxed);
    if (now - turn.flicked_at.load(std::memory_order_relaxed) >= 150) {
        // The game flips the stick's sideways sign while the stick is held down (the nose is to point where the stick
        // does): flipped here first, so the left stick turns the board the same way in a 5-0 as in a nose grind.
        const float x = std::clamp(turn.x.load(std::memory_order_relaxed), -1.0f, 1.0f) * (raw_y < -0.2f ? -1.0f : 1.0f);
        const float y = std::clamp(raw_y * turn.y_scale.load(std::memory_order_relaxed) + turn.y_add.load(std::memory_order_relaxed), -1.0f, 1.0f);
        pad.sThumbRX = static_cast<SHORT>(x * 32767.0f);
        pad.sThumbRY = static_cast<SHORT>(y * 32767.0f);
        grind_stick_mirrored().fetch_add(1, std::memory_order_relaxed);
    }
    turn.sent_x.store(static_cast<float>(pad.sThumbRX) / 32767.0f, std::memory_order_relaxed);
    turn.sent_y.store(static_cast<float>(pad.sThumbRY) / 32767.0f, std::memory_order_relaxed);
    return true;
}
void mirror_grind_stick(XINPUT_GAMEPAD& pad) noexcept {
    if (turn_grind_stick(pad)) return;
    const float amount = grind_stick_mirror().load(std::memory_order_relaxed);
    if (amount == 0.0f) return;
    // Skate 3's dead zone on this stick: nothing inside a quarter, the rest stretched over the whole range.
    constexpr int right_idle = 9000, flick = 5000;
    constexpr float dead = 0.25f;
    static std::atomic<int> last_ry{0};
    static std::atomic<std::uint64_t> flicked_at{0};
    const auto now = GetTickCount64();
    const int ry = pad.sThumbRY;
    if (std::abs(ry - last_ry.exchange(ry, std::memory_order_relaxed)) > flick) flicked_at.store(now, std::memory_order_relaxed);
    if (now - flicked_at.load(std::memory_order_relaxed) < 150) return;
    const auto past_dead = [](SHORT raw) {
        const float value = static_cast<float>(raw) / 32767.0f;
        return std::abs(value) <= dead ? 0.0f : (std::abs(value) - dead) / (1.0f - dead) * (value < 0 ? -1.0f : 1.0f);
    };
    // Up and down too (the left stick holds a 5-0 or a nose grind), but never faster than a third of a second from end
    // to end: the game must not read the left stick as the flick of a pop.
    static float tipped = 0.0f;
    static std::int64_t tipped_at = 0;
    LARGE_INTEGER clock{}, rate{};
    QueryPerformanceCounter(&clock);
    QueryPerformanceFrequency(&rate);
    const float passed = tipped_at && rate.QuadPart ? std::clamp(static_cast<float>(clock.QuadPart - tipped_at) / static_cast<float>(rate.QuadPart), 0.0f, 0.1f) : 0.0f;
    tipped_at = clock.QuadPart;
    const float tip_wanted = std::abs(int{pad.sThumbRY}) > right_idle ? 0.0f : past_dead(pad.sThumbLY) * std::abs(amount);
    tipped += std::clamp(tip_wanted - tipped, -6.0f * passed, 6.0f * passed);
    const float turned = std::abs(int{pad.sThumbRX}) > right_idle ? 0.0f : past_dead(pad.sThumbLX) * amount;
    if (turned == 0.0f && std::abs(tipped) < 0.01f) return;
    if (turned != 0.0f) pad.sThumbRX = static_cast<SHORT>(std::clamp(static_cast<int>(turned * 32767.0f), -32767, 32767));
    if (std::abs(tipped) >= 0.01f && std::abs(int{pad.sThumbRY}) <= right_idle)
        pad.sThumbRY = static_cast<SHORT>(std::clamp(static_cast<int>(tipped * 32767.0f), -32767, 32767));
    grind_stick_mirrored().fetch_add(1, std::memory_order_relaxed);
}
// Leaning into the ramp on a coping grind: while on, the game's own polls see the left stick held at (x, y) for as long
// as the player's own left stick is at rest (a stick the player moves is theirs). `raw_*` is the player's own stick as
// the last poll had it, for the trainer's own reads of the left stick.
struct GrindStickLean {
    std::atomic<bool> on{};
    std::atomic<float> x{}, y{}, raw_x{}, raw_y{};
    std::atomic<std::uint64_t> given{0};
};
GrindStickLean& grind_stick_lean() { static auto* value = new GrindStickLean; return *value; }
void lean_grind_stick(XINPUT_GAMEPAD& pad) noexcept {
    auto& lean = grind_stick_lean();
    if (!lean.on.load(std::memory_order_relaxed)) return;
    lean.raw_x.store(static_cast<float>(pad.sThumbLX) / 32767.0f, std::memory_order_relaxed);
    lean.raw_y.store(static_cast<float>(pad.sThumbLY) / 32767.0f, std::memory_order_relaxed);
    constexpr int idle = 8000; // about a quarter of the stick
    if (std::abs(int{pad.sThumbLX}) > idle || std::abs(int{pad.sThumbLY}) > idle) return;
    pad.sThumbLX = static_cast<SHORT>(std::clamp(lean.x.load(std::memory_order_relaxed), -1.0f, 1.0f) * 32767.0f);
    pad.sThumbLY = static_cast<SHORT>(std::clamp(lean.y.load(std::memory_order_relaxed), -1.0f, 1.0f) * 32767.0f);
    lean.given.fetch_add(1, std::memory_order_relaxed);
}
std::int64_t dark_pop_clock() noexcept { LARGE_INTEGER now{}; QueryPerformanceCounter(&now); return now.QuadPart; }
// The game's own poll: returns 1 if D-pad Right was added, 2 if it was taken away, 0 if the pad is unchanged.
unsigned dark_pop_filter(WORD& buttons) noexcept {
    auto& r = dark_pop_release();
    if (!r.enabled.load(std::memory_order_relaxed)) return 0;
    r.polls.fetch_add(1, std::memory_order_relaxed);
    const bool physical = (buttons & XINPUT_GAMEPAD_DPAD_RIGHT) != 0;
    const auto release_at = r.release_at.load(std::memory_order_acquire);
    if (release_at && !r.released.load(std::memory_order_relaxed)) {
        const auto now = dark_pop_clock();
        if (now < release_at) {
            if (buttons & XINPUT_GAMEPAD_RIGHT_SHOULDER) r.primed.store(true, std::memory_order_relaxed);
            if (physical && r.primed.load(std::memory_order_relaxed)) r.latched.store(true, std::memory_order_relaxed);
            if (!r.latched.load(std::memory_order_relaxed)) return 0;
            buttons = static_cast<WORD>(buttons | XINPUT_GAMEPAD_DPAD_RIGHT); // held for the game, even if the player already let go
            return physical ? 0u : 1u;
        }
        if (r.latched.load(std::memory_order_relaxed)) {
            r.released.store(true, std::memory_order_relaxed);
            r.shown_release.store(now, std::memory_order_relaxed);
            r.hiding.store(true, std::memory_order_relaxed);
        }
    }
    if (!r.hiding.load(std::memory_order_relaxed)) return 0;
    if (!physical) { r.hiding.store(false, std::memory_order_relaxed); return 0; } // the player let go too
    buttons = static_cast<WORD>(buttons & ~XINPUT_GAMEPAD_DPAD_RIGHT);
    return 2;
}
template<std::size_t I, bool Extended = false>
DWORD WINAPI captured_xinput(DWORD user, XINPUT_STATE* output) {
    const auto& hook = Extended ? input_hooks().xinput_extended[I] : input_hooks().xinput_state[I];
    const bool game_poll = !overlay_input_access;
    const bool capture = block_polled_input();
    note_prompt_read(prompt_xinput);
    DWORD result;
    { // XInput exports can forward through another hooked XInput export.
        OverlayInputAccess access;
        result = original<DWORD(WINAPI*)(DWORD, XINPUT_STATE*)>(hook)(user, output);
    }
// <dev>
    // The overlay's own reads see the scripted state too, so the trainer's pad shortcuts can be tested.
    if (auto& scripted = scripted_pad(); user == 0 && output) {
        if (!overlay_input_access) scripted.polls.fetch_add(1, std::memory_order_relaxed);
        if (scripted.on.load(std::memory_order_acquire)) {
            const auto low = scripted.low.load(std::memory_order_relaxed);
            const auto high = scripted.high.load(std::memory_order_relaxed);
            std::memcpy(&output->Gamepad, &low, 8);
            std::memcpy(reinterpret_cast<std::byte*>(&output->Gamepad) + 8, &high, 4);
            output->dwPacketNumber = scripted.packet.load(std::memory_order_relaxed);
            scripted.served.fetch_add(1, std::memory_order_relaxed);
            if (capture) scripted.blanked.fetch_add(1, std::memory_order_relaxed);
            result = ERROR_SUCCESS;
        }
    }
// </dev>
    if (result == ERROR_SUCCESS && output && capture) {
        output->Gamepad = {};
        // The game must notice a neutral state even if the physical packet did
        // not change between opening the menu and its next controller poll.
        output->dwPacketNumber ^= 0x80000000u;
    } else if (result == ERROR_SUCCESS && output && game_poll) {
        // A changed button needs a changed packet number, or the game may keep the state it already has.
        const auto changed = dark_pop_filter(output->Gamepad.wButtons);
        if (changed) output->dwPacketNumber ^= changed << 29;
        dark_pop_release().flags.store(changed, std::memory_order_relaxed);
        mirror_grind_stick(output->Gamepad);
        lean_grind_stick(output->Gamepad);
    }
    return result;
}
// <dev>
}
// `gamepad`: twelve bytes laid out as XINPUT_GAMEPAD, or null to hand the controller back.
extern "C" void DingoSDKOverlayScriptController(const void* gamepad) {
    auto& scripted = dingosdk::overlay::detail::scripted_pad();
    if (!gamepad) { scripted.on.store(false, std::memory_order_release); return; }
    std::uint64_t low{};
    std::uint32_t high{};
    std::memcpy(&low, gamepad, 8);
    std::memcpy(&high, static_cast<const std::byte*>(gamepad) + 8, 4);
    if (low != scripted.low.load() || high != scripted.high.load() || !scripted.on.load()) scripted.packet.fetch_add(1);
    // A scripted run must not need the desktop: the game is told it is the active window and,
    // while the script owns the controller, is not told when it stops being so (overlay_input.cpp).
    if (!scripted.on.load())
        if (const HWND window = dingosdk::overlay::detail::state().window.load()) {
            PostMessageW(window, WM_ACTIVATEAPP, TRUE, 0);
            PostMessageW(window, WM_ACTIVATE, WA_ACTIVE, 0);
            PostMessageW(window, WM_SETFOCUS, 0, 0);
        }
    scripted.low.store(low, std::memory_order_relaxed);
    scripted.high.store(high, std::memory_order_relaxed);
    scripted.on.store(true, std::memory_order_release);
}
// How often the game polled the first controller, and how often it was given the scripted state.
extern "C" void DingoSDKOverlayScriptControllerPolls(std::uint64_t* polls, std::uint64_t* served) {
    auto& scripted = dingosdk::overlay::detail::scripted_pad();
    if (polls) *polls = scripted.polls.load(std::memory_order_relaxed);
    if (served) *served = scripted.served.load(std::memory_order_relaxed);
}
extern "C" bool DingoSDKOverlayScriptControllerOn() {
    return dingosdk::overlay::detail::scripted_pad().on.load(std::memory_order_acquire);
}
extern "C" std::uint64_t DingoSDKOverlayScriptControllerBlanked() {
    return dingosdk::overlay::detail::scripted_pad().blanked.exchange(0, std::memory_order_relaxed);
}
namespace dingosdk::overlay::detail {
// </dev>
template<std::size_t I>
DWORD WINAPI captured_keystroke(DWORD user, DWORD reserved, PXINPUT_KEYSTROKE output) {
    const auto result = original<DWORD(WINAPI*)(DWORD, DWORD, PXINPUT_KEYSTROKE)>(
        input_hooks().xinput_keystroke[I])(user, reserved, output);
    if (result == ERROR_SUCCESS && output && block_polled_input()) { *output = {}; return ERROR_EMPTY; }
    return result;
}

template<std::size_t I>
HRESULT STDMETHODCALLTYPE captured_device_format(void* device, const DIDATAFORMAT* format) {
    const auto result = original<HRESULT(STDMETHODCALLTYPE*)(void*, const DIDATAFORMAT*)>(
        input_hooks().device_format[I])(device, format);
    if (SUCCEEDED(result) && format && format->dwSize == sizeof(DIDATAFORMAT) &&
        format->dwObjSize == sizeof(DIOBJECTDATAFORMAT) && format->dwNumObjs <= 1024 &&
        (!format->dwNumObjs || format->rgodf)) {
        InputCaptureHooks::Format copy;
        copy.size = format->dwDataSize;
        for (DWORD i = 0; i < format->dwNumObjs; ++i) copy.objects.push_back(format->rgodf[i]);
        std::lock_guard lock(input_hooks().formats_mutex);
        input_hooks().formats[device] = std::move(copy);
    }
    return result;
}
template<std::size_t I>
HRESULT STDMETHODCALLTYPE captured_device_state(void* device, DWORD size, LPVOID data) {
    const auto result = original<HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, LPVOID)>(
        input_hooks().device_state[I])(device, size, data);
    note_prompt_read(prompt_direct);
    if (SUCCEEDED(result) && data && block_polled_input()) {
        std::memset(data, 0, size);
        std::lock_guard lock(input_hooks().formats_mutex);
        const auto found = input_hooks().formats.find(device);
        if (found != input_hooks().formats.end() && found->second.size == size) {
            for (const auto& object : found->second.objects) {
                if (object.dwOfs > size || size - object.dwOfs < 4) continue;
                DWORD neutral = 0;
                if (object.dwType & DIDFT_POV) neutral = 0xffffffffu;
                else if (object.dwType & DIDFT_ABSAXIS) {
                    neutral = 32767; // Standard DirectInput axis midpoint if range lookup fails.
                    DIPROPRANGE range{};
                    range.diph = {sizeof(range), sizeof(range.diph), object.dwOfs, DIPH_BYOFFSET};
                    auto** vtable = *reinterpret_cast<void***>(device);
                    using GetProperty = HRESULT(STDMETHODCALLTYPE*)(void*, REFGUID, LPDIPROPHEADER);
                    if (SUCCEEDED(reinterpret_cast<GetProperty>(vtable[5])(device, DIPROP_RANGE, &range.diph)))
                        neutral = static_cast<DWORD>((static_cast<std::int64_t>(range.lMin) + range.lMax) / 2);
                } else continue;
                std::memcpy(static_cast<std::byte*>(data) + object.dwOfs, &neutral, 4);
            }
        } else if (size == sizeof(DIJOYSTATE) || size == sizeof(DIJOYSTATE2)) {
            // A zeroed DirectInput axis is full travel, not the center. Some
            // controllers use the standard joystick layout without a format
            // record we can inspect; keep those pads neutral while captured.
            auto* bytes = static_cast<std::byte*>(data);
            const DWORD center = 32767;
            const DWORD released_pov = 0xffffffffu;
            for (std::size_t offset = 0; offset < 8 * sizeof(DWORD); offset += sizeof(DWORD))
                std::memcpy(bytes + offset, &center, sizeof(center));
            for (std::size_t offset = 8 * sizeof(DWORD); offset < 12 * sizeof(DWORD); offset += sizeof(DWORD))
                std::memcpy(bytes + offset, &released_pov, sizeof(released_pov));
        }
    }
    return result;
}
template<std::size_t I>
HRESULT STDMETHODCALLTYPE captured_device_data(void* device, DWORD size, LPDIDEVICEOBJECTDATA data, LPDWORD count, DWORD flags) {
    const bool capture = block_polled_input();
    const auto result = original<HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD)>(
        input_hooks().device_data[I])(device, size, data, count, capture ? flags & ~DIGDD_PEEK : flags);
    if (SUCCEEDED(result) && capture && count) *count = 0;
    return result;
}
bool install_device_capture(void* device) {
    auto** table = *reinterpret_cast<void***>(device);
    auto& hooks = input_hooks();
    const auto states = []<std::size_t... I>(std::index_sequence<I...>) {
        return std::array{reinterpret_cast<void*>(captured_device_state<I>)...};
    }(std::make_index_sequence<InputCaptureHooks::device_slots>{});
    const auto data = []<std::size_t... I>(std::index_sequence<I...>) {
        return std::array{reinterpret_cast<void*>(captured_device_data<I>)...};
    }(std::make_index_sequence<InputCaptureHooks::device_slots>{});
    const auto formats = []<std::size_t... I>(std::index_sequence<I...>) {
        return std::array{reinterpret_cast<void*>(captured_device_format<I>)...};
    }(std::make_index_sequence<InputCaptureHooks::device_slots>{});
    const auto add = [&](auto& family, void* target, const auto& detours) {
        for (const auto& hook : family) if (hook.target == target) return true;
        for (std::size_t slot = 0; slot < family.size(); ++slot)
            if (!family[slot].target) return install(family[slot], target, detours[slot]);
        return false;
    };
    // Keep installing independent methods if one fails. Report the exact gap.
    const bool state_ready = add(hooks.device_state, table[9], states);
    const bool data_ready = add(hooks.device_data, table[10], data);
    const bool format_ready = add(hooks.device_format, table[11], formats);
    if (!state_ready || !data_ready || !format_ready)
        logging::log(logging::Level::warning, logging::Channel::input,
            "DirectInput capture incomplete: state={}, buffered={}, format={}; mouse input may reach the game while the menu is open.",
            state_ready, data_ready, format_ready);
    std::lock_guard formats_lock(hooks.formats_mutex);
    hooks.formats.erase(device); // COM addresses can be reused after Release.
    return state_ready && data_ready && format_ready;
}

template<std::size_t I>
HRESULT STDMETHODCALLTYPE captured_device_create(void* owner, REFGUID guid, void** output, LPUNKNOWN outer) {
    const auto result = original<HRESULT(STDMETHODCALLTYPE*)(void*, REFGUID, void**, LPUNKNOWN)>(
        input_hooks().device_create[I])(owner, guid, output, outer);
    if (SUCCEEDED(result) && output && *output) {
        std::lock_guard lock(state().hook_mutex);
        install_device_capture(*output);
    }
    return result;
}

bool prepare_standard_devices(HMODULE direct) {
    using Create = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, void**, IUnknown*);
    const auto create = reinterpret_cast<Create>(GetProcAddress(direct, "DirectInput8Create"));
    if (!create) return false;
    bool ready = true;
    std::array<void*, 2> create_targets{};
    std::size_t create_count{};
    for (const auto& iid : {IID_IDirectInput8A, IID_IDirectInput8W}) {
        ComPtr<IUnknown> owner;
        if (FAILED(create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, iid,
            reinterpret_cast<void**>(owner.GetAddressOf()), nullptr))) { ready = false; continue; }
        auto** table = *reinterpret_cast<void***>(owner.Get());
        create_targets[create_count++] = table[3];
        using CreateDevice = HRESULT(STDMETHODCALLTYPE*)(void*, REFGUID, void**, IUnknown*);
        for (const auto& guid : {GUID_SysMouse, GUID_SysKeyboard}) {
            ComPtr<IUnknown> device;
            if (FAILED(reinterpret_cast<CreateDevice>(table[3])(owner.Get(), guid,
                reinterpret_cast<void**>(device.GetAddressOf()), nullptr))) { ready = false; continue; }
            // No Acquire or cooperative-level change: these temporary objects
            // identify shared implementations before the game starts workers.
            if (!install_device_capture(device.Get())) ready = false;
        }
    }
    auto& family = input_hooks().device_create;
    const std::array detours{reinterpret_cast<void*>(captured_device_create<0>),
        reinterpret_cast<void*>(captured_device_create<1>)};
    for (std::size_t i = 0; i < create_count; ++i) {
        if (std::any_of(family.begin(), family.end(), [&](const auto& hook) { return hook.target == create_targets[i]; })) continue;
        const auto slot = std::find_if(family.begin(), family.end(), [](const auto& hook) { return !hook.target; });
        if (slot == family.end() || !install(*slot, create_targets[i], detours[slot - family.begin()])) ready = false;
    }
    return ready;
}

HRESULT WINAPI captured_direct_create(HINSTANCE instance, DWORD version, REFIID iid, LPVOID* output, LPUNKNOWN outer) {
    const auto result = original<HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN)>(
        input_hooks().direct_create)(instance, version, iid, output, outer);
    if (SUCCEEDED(result) && output && *output) {
        auto** table = *reinterpret_cast<void***>(*output);
        std::lock_guard lock(state().hook_mutex);
        auto& hooks = input_hooks();
        // A/W implementations sometimes share their callable address.
        for (const auto& hook : hooks.device_create) if (hook.target == table[3]) return result;
        if (!hooks.device_create[0].target)
            install(hooks.device_create[0], table[3], reinterpret_cast<void*>(captured_device_create<0>));
        else install(hooks.device_create[1], table[3], reinterpret_cast<void*>(captured_device_create<1>));
    }
    return result;
}

namespace {
// `owns_pointer` is asked only when the packet moves a cursor Windows is not moving.
template<class OwnsPointer> void note_packet(const RAWMOUSE& mouse, OwnsPointer&& owns_pointer) {
    auto& s = state();
    const bool relative = !(mouse.usFlags & MOUSE_MOVE_ABSOLUTE);
    if (relative) {
        s.raw_mouse_x.fetch_add(mouse.lLastX);
        s.raw_mouse_y.fetch_add(mouse.lLastY);
    }
    auto buttons = s.raw_mouse_buttons.load();
    const auto flags = mouse.usButtonFlags;
    if (flags & RI_MOUSE_LEFT_BUTTON_DOWN) buttons |= 1u;
    if (flags & RI_MOUSE_LEFT_BUTTON_UP) buttons &= ~1u;
    if (flags & RI_MOUSE_RIGHT_BUTTON_DOWN) buttons |= 2u;
    if (flags & RI_MOUSE_RIGHT_BUTTON_UP) buttons &= ~2u;
    if (flags & RI_MOUSE_MIDDLE_BUTTON_DOWN) buttons |= 4u;
    if (flags & RI_MOUSE_MIDDLE_BUTTON_UP) buttons &= ~4u;
    s.raw_mouse_buttons.store(buttons);
    s.raw_mouse_time.store(GetTickCount64());
    // Windows is not moving the cursor (RIDEV_NOLEGACY): move it for the
    // overlay so its pointer and the editor's hover keep working.
    if (relative && (mouse.lLastX || mouse.lLastY) && s.mouse_legacy_blocked.load() && owns_pointer()) {
        OverlayInputAccess access;
        POINT cursor{};
        if (GetCursorPos(&cursor)) SetCursorPos(cursor.x + mouse.lLastX, cursor.y + mouse.lLastY);
    }
}
} // namespace

void note_raw_mouse_packet(const RAWMOUSE& mouse) {
    note_packet(mouse, [] { return owns_menu_cursor(state()); });
}

void note_raw_mouse_packet(const RAWMOUSE& mouse, bool overlay_owns_pointer) {
    note_packet(mouse, [overlay_owns_pointer] { return overlay_owns_pointer; });
}

void sync_raw_mouse_registration() {
    sync_raw_mouse_registration(owns_menu_cursor(state()));
}

void sync_raw_mouse_registration(bool overlay_owns_pointer) {
    try {
        auto& mouse = raw_mouse();
        std::lock_guard lock(mouse.mutex);
        const bool overlay = overlay_owns_pointer;
        if (overlay == mouse.overridden) return;
        mouse.overridden = overlay;
        // Only a registration that actually blocks the cursor needs replacing.
        if (!mouse.known || !(mouse.game.dwFlags & cursor_blocking_flags)) return;
        apply_raw_mouse(mouse, overlay);
    } catch (...) {}
}

bool install_input_capture() {
    auto& h = input_hooks();
    const auto hid = LoadLibraryExW(L"hid.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto kernel = GetModuleHandleW(L"kernel32.dll");
    if (!hid || !install(h.hid_attributes, reinterpret_cast<void*>(GetProcAddress(hid, "HidD_GetAttributes")),
            reinterpret_cast<void*>(captured_hid_attributes))) return false;
    const auto kernel_hook = [&](Hook& hook, const char* name, void* detour) {
        return install(hook, reinterpret_cast<void*>(GetProcAddress(kernel, name)), detour);
    };
    if (!kernel_hook(h.read_file, "ReadFile", reinterpret_cast<void*>(captured_read_file)) ||
        !kernel_hook(h.overlapped, "GetOverlappedResult", reinterpret_cast<void*>(captured_overlapped)) ||
        !kernel_hook(h.overlapped_ex, "GetOverlappedResultEx", reinterpret_cast<void*>(captured_overlapped_ex)) ||
        !kernel_hook(h.completion, "GetQueuedCompletionStatus", reinterpret_cast<void*>(captured_completion)) ||
        !kernel_hook(h.completion_ex, "GetQueuedCompletionStatusEx", reinterpret_cast<void*>(captured_completion_ex)) ||
        !kernel_hook(h.close_handle, "CloseHandle", reinterpret_cast<void*>(captured_close_handle))) return false;
    const auto user32 = GetModuleHandleW(L"user32.dll");
    const auto add = [&](Hook& hook, const char* name, void* detour) {
        const bool okay = install(hook, reinterpret_cast<void*>(GetProcAddress(user32, name)), detour);
        if (!okay) dingosdk::logging::write(dingosdk::logging::Level::warning, dingosdk::logging::Channel::input, name);
        return okay;
    };
    add(h.register_raw, "RegisterRawInputDevices", reinterpret_cast<void*>(captured_register_raw));
    if (!add(h.async_key, "GetAsyncKeyState", reinterpret_cast<void*>(captured_async_key)) ||
        !add(h.key, "GetKeyState", reinterpret_cast<void*>(captured_key)) ||
        !add(h.keyboard, "GetKeyboardState", reinterpret_cast<void*>(captured_keyboard)) ||
        !add(h.raw_data, "GetRawInputData", reinterpret_cast<void*>(captured_raw_data)) ||
        !add(h.raw_buffer, "GetRawInputBuffer", reinterpret_cast<void*>(captured_raw_buffer)) ||
        !add(h.capture, "SetCapture", reinterpret_cast<void*>(captured_capture)) ||
        !add(h.release_capture, "ReleaseCapture", reinterpret_cast<void*>(captured_release_capture)) ||
        !add(h.cursor, "SetCursor", reinterpret_cast<void*>(captured_cursor)) ||
        !add(h.cursor_position, "GetCursorPos", reinterpret_cast<void*>(captured_cursor_position)) ||
        !add(h.show_cursor, "ShowCursor", reinterpret_cast<void*>(captured_show_cursor))) return false;
    // <dev>
    add(h.foreground, "GetForegroundWindow", reinterpret_cast<void*>(scripted_foreground));
    add(h.active, "GetActiveWindow", reinterpret_cast<void*>(scripted_active));
    add(h.focus, "GetFocus", reinterpret_cast<void*>(scripted_focus));
    // </dev>
    // Some Windows builds alias both cursor-position exports to one function,
    // which is already intercepted by the overlay's SetCursorPos hook.
    const auto physical = GetProcAddress(user32, "SetPhysicalCursorPos");
    if (physical && physical != GetProcAddress(user32, "SetCursorPos") &&
        !add(h.physical_cursor, "SetPhysicalCursorPos", reinterpret_cast<void*>(captured_physical_cursor))) return false;
    const std::array<void*, 5> states{reinterpret_cast<void*>(captured_xinput<0>), reinterpret_cast<void*>(captured_xinput<1>),
        reinterpret_cast<void*>(captured_xinput<2>), reinterpret_cast<void*>(captured_xinput<3>), reinterpret_cast<void*>(captured_xinput<4>)};
    const std::array<void*, 5> extended{reinterpret_cast<void*>(captured_xinput<0, true>), reinterpret_cast<void*>(captured_xinput<1, true>),
        reinterpret_cast<void*>(captured_xinput<2, true>), reinterpret_cast<void*>(captured_xinput<3, true>), reinterpret_cast<void*>(captured_xinput<4, true>)};
    const std::array<void*, 5> strokes{reinterpret_cast<void*>(captured_keystroke<0>), reinterpret_cast<void*>(captured_keystroke<1>),
        reinterpret_cast<void*>(captured_keystroke<2>), reinterpret_cast<void*>(captured_keystroke<3>), reinterpret_cast<void*>(captured_keystroke<4>)};
    const std::array names{L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll", L"xinput1_2.dll", L"xinput1_1.dll"};
    std::vector<void*> seen;
    for (std::size_t i = 0; i < names.size(); ++i) {
        const auto module = LoadLibraryExW(names[i], nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) continue;
        const auto hook_export = [&](Hook& hook, const char* name, void* detour) {
            const auto target = reinterpret_cast<void*>(GetProcAddress(module, name));
            if (!target || std::find(seen.begin(), seen.end(), target) != seen.end()) return true;
            if (!install(hook, target, detour)) { dingosdk::logging::printf(dingosdk::logging::Level::error, dingosdk::logging::Channel::input, "XInput input hook failed: module=%zu target=%p", i, target); return false; }
            seen.push_back(target); return true;
        };
        if (!hook_export(h.xinput_state[i], "XInputGetState", states[i]) ||
            !hook_export(h.xinput_extended[i], MAKEINTRESOURCEA(100), extended[i]) ||
            !hook_export(h.xinput_keystroke[i], "XInputGetKeystroke", strokes[i])) return false;
    }
    const auto direct = LoadLibraryExW(L"dinput8.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (direct) {
        if (prepare_standard_devices(direct))
            logging::write(logging::Level::info, logging::Channel::input, "DirectInput mouse/keyboard capture prepared before game startup.");
        else logging::write(logging::Level::warning, logging::Channel::input,
            "DirectInput startup preparation incomplete; device creation will retry the missing hooks.");
        if (!install(h.direct_create, reinterpret_cast<void*>(GetProcAddress(direct, "DirectInput8Create")),
            reinterpret_cast<void*>(captured_direct_create))) return false;
    }
    dingosdk::logging::write(dingosdk::logging::Level::info, dingosdk::logging::Channel::input, "Menu input capture installed: keyboard, raw input, XInput, DirectInput, PlayStation HID and cursor ownership.");
    return true;
}

}

// Dark Pop auto release, for the physics selector hook (Extension/Skater/no_bail.cpp) and the client tick.
extern "C" void DingoSDKOverlayDarkPopEnable(bool enabled) noexcept {
    auto& r = dingosdk::overlay::detail::dark_pop_release();
    if (!r.enabled.exchange(enabled, std::memory_order_acq_rel) || enabled) return;
    r.release_at.store(0, std::memory_order_release);
    r.primed.store(false, std::memory_order_relaxed);
    r.latched.store(false, std::memory_order_relaxed);
    r.hiding.store(false, std::memory_order_relaxed);
}
// A timed flight starts: the game is shown D-pad Right released from `release_at` (QueryPerformanceCounter) on.
extern "C" void DingoSDKOverlayDarkPopFlightStart(std::int64_t release_at) noexcept {
    auto& r = dingosdk::overlay::detail::dark_pop_release();
    r.primed.store(false, std::memory_order_relaxed);
    r.latched.store(false, std::memory_order_relaxed);
    r.released.store(false, std::memory_order_relaxed);
    r.shown_release.store(0, std::memory_order_relaxed);
    r.release_at.store(release_at, std::memory_order_release);
}
// The flight ended. Says whether the player pressed D-pad Right in it and when the game was shown the release (0 if the
// landing came first; the game is then shown the release now).
extern "C" bool DingoSDKOverlayDarkPopFlightEnd(std::int64_t* shown_release) noexcept {
    auto& r = dingosdk::overlay::detail::dark_pop_release();
    r.release_at.store(0, std::memory_order_release);
    r.primed.store(false, std::memory_order_relaxed);
    const bool latched = r.latched.exchange(false, std::memory_order_acq_rel);
    if (latched && !r.released.exchange(true, std::memory_order_acq_rel)) r.hiding.store(true, std::memory_order_relaxed);
    if (shown_release) *shown_release = r.shown_release.load(std::memory_order_relaxed);
    return latched;
}
// D-pad Right has been pressed in the current timed flight (as the game's polls saw it).
extern "C" bool DingoSDKOverlayDarkPopLatched() noexcept {
    const auto& r = dingosdk::overlay::detail::dark_pop_release();
    return r.release_at.load(std::memory_order_acquire) && r.latched.load(std::memory_order_relaxed);
}
// 1 = the game's last poll was shown D-pad Right held by the mod, 2 = hidden by the mod, 0 = untouched.
extern "C" unsigned DingoSDKOverlayDarkPopInputFlags() noexcept {
    return dingosdk::overlay::detail::dark_pop_release().flags.load(std::memory_order_relaxed);
}
// The game's own XInput polls counted while the auto release was on (0 means the game does not read the pad through XInput).
extern "C" std::uint64_t DingoSDKOverlayDarkPopGamePolls() noexcept {
    return dingosdk::overlay::detail::dark_pop_release().polls.load(std::memory_order_relaxed);
}
// The left stick's sideways position also goes to the right stick in the game's own polls, times `amount` (0: off).
extern "C" void DingoSDKOverlaySetGrindStickMirror(float amount) noexcept {
    dingosdk::overlay::detail::grind_stick_mirror().store(amount == amount ? amount : 0.0f, std::memory_order_relaxed);
}
// How many of the game's polls were given the left stick on the right one so far.
extern "C" void DingoSDKOverlaySetGrindStickTurn(bool on, float x, float y_add, float y_scale) noexcept {
    auto& turn = dingosdk::overlay::detail::grind_stick_turn();
    turn.x.store(x, std::memory_order_relaxed);
    turn.y_add.store(y_add, std::memory_order_relaxed);
    turn.y_scale.store(y_scale, std::memory_order_relaxed);
    turn.on.store(on, std::memory_order_relaxed);
}
// What the game's last poll saw of the right stick (x, y) and the player's own right stick up / down.
extern "C" void DingoSDKOverlayGrindStickSent(float* values) noexcept {
    auto& turn = dingosdk::overlay::detail::grind_stick_turn();
    values[0] = turn.sent_x.load(std::memory_order_relaxed);
    values[1] = turn.sent_y.load(std::memory_order_relaxed);
    values[2] = turn.raw_y.load(std::memory_order_relaxed);
}
// Leaning into the ramp on a coping grind: the left stick the game's own polls see while the player's is at rest.
extern "C" void DingoSDKOverlaySetGrindStickLean(bool on, float x, float y) noexcept {
    auto& lean = dingosdk::overlay::detail::grind_stick_lean();
    lean.x.store(x == x ? x : 0.0f, std::memory_order_relaxed);
    lean.y.store(y == y ? y : 0.0f, std::memory_order_relaxed);
    lean.on.store(on, std::memory_order_relaxed);
}
// The player's own left stick as the game's last poll had it, while the lean is on (false: it is off, read the controller).
extern "C" bool DingoSDKOverlayGrindStickLeanRaw(float* values) noexcept {
    auto& lean = dingosdk::overlay::detail::grind_stick_lean();
    if (!lean.on.load(std::memory_order_relaxed)) return false;
    values[0] = lean.raw_x.load(std::memory_order_relaxed);
    values[1] = lean.raw_y.load(std::memory_order_relaxed);
    return true;
}
// How many of the game's polls were given the lean so far.
extern "C" std::uint64_t DingoSDKOverlayGrindStickLeaned() noexcept {
    return dingosdk::overlay::detail::grind_stick_lean().given.load(std::memory_order_relaxed);
}
extern "C" std::uint64_t DingoSDKOverlayGrindStickMirrored() noexcept {
    return dingosdk::overlay::detail::grind_stick_mirrored().load(std::memory_order_relaxed);
}
