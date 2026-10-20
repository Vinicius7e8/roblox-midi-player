/* ------------------------------------
 |
 |   A script to turn Midi file & device inputs into inputs for Roblox pianos (originally for Heartopia).
 |   By Don_Elf
 |   https://github.com/DonElf/Heartopia-Midi-Player
 |
 |   ©2026 Don_Elf
 |   Some Rights Reserved.
 |
 |
 |   Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except in compliance with the License. You may obtain a copy of the License at
 |   http://www.apache.org/licenses/LICENSE-2.0
 |
 |   Unless required by applicable law or agreed to in writing, software distributed under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 |   See the License for the specific language governing permissions and limitations under the License.
 |
 |   Modified 2026 by BlackWolf (zzBlack_Wolf) as Roblox MIDI Player, for Roblox pianos
 |   (Virtual Piano layout, 61 keys, C2..C7). Changes from the original:
 |   - new key map; sharps sent with Shift; 15-key option removed
 |   - notes sent as taps, or held for their length ("Segurar notas")
 |   - F1 play / pause / resume and F2 stop, from inside the game
 |   - pause and resume, progress bar with time
 |   - song list from the Musicas folder next to the .exe; Unicode file names
 |   - new look (cards, Segoe UI, DPI aware) and Portuguese texts
 |   - fixes: negative transpose in live input, playback thread joined on exit
 |
------------------------------------ */

#include <windows.h>
#include <mmsystem.h>
#include <CommCtrl.h>

// Requires MSVC
#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "winmm.lib")


#include <fstream>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <thread>
#include <chrono>
#include <mutex>
#include <atomic>
#include <stdexcept>
#include <string>
#include <optional>
#include <filesystem>
#include <cstdint>

// Dedicated exception for MIDI file errors
class MidiFileException : public std::runtime_error { using runtime_error::runtime_error; };

// Dedicated exception for MIDI device errors
class MidiDeviceException : public std::runtime_error { using runtime_error::runtime_error; };


class KeyboardEmitter {
private: // Defaults, I know. Explicit though :pleading:

	// Turn from a vk (int) to a scancode (WORD).
	// Allows mapping text characters to keyboard buttons, basically.
	WORD GetScanCode(int vKey) const {
		// These characters misbehave with MapVirtualKey, so do them manually.
		// A full generic list, although we'll only need a couple. Remove them if you want IG. Saves a couple CPU instructions?
		static const std::unordered_map<int, WORD> manual{
			{VK_OEM_COMMA, 0x33}, {VK_OEM_PERIOD, 0x34},
			{VK_OEM_1, 0x27}, {VK_OEM_2, 0x35},
			{VK_OEM_3, 0x29}, {VK_OEM_4, 0x1A},
			{VK_OEM_5, 0x2B}, {VK_OEM_6, 0x1B},
			{VK_OEM_7, 0x28}, {VK_OEM_MINUS, 0x0C},
			{VK_OEM_PLUS, 0x0D}, {VK_LSHIFT, 0x2A},
			{VK_SPACE, 0x39}, {VK_RETURN, 0x1C},
			{VK_BACK, 0x0E}, {VK_TAB, 0x0F},
			{VK_ESCAPE, 0x01}
		};

		// If the manual list contains 
		if (auto it = manual.find(vKey); it != manual.end())
			return it->second;

		// And, convert to scancode
		return (WORD)MapVirtualKey(vKey, MAPVK_VK_TO_VSC);
	}

	// Use a couple range checks, since it's pretty fast
	inline bool IsExtended(int vKey) const {
		return (vKey >= VK_PRIOR && vKey <= VK_DOWN) || // Page Up/Down, End, Home, Arrows
			(vKey >= VK_INSERT && vKey <= VK_DELETE) || // Insert, Delete
			(vKey == VK_LWIN || vKey == VK_RWIN) ||     // Windows keys
			(vKey == VK_APPS) ||                        // Menu key
			(vKey == VK_RCONTROL) ||                    // Right Ctrl
			(vKey == VK_RMENU);                         // Right Alt
	}

public:
	void SendKey(int vKey, bool pressed) const {
		// First, create an INPUT (Windows struct :P) using our input 
		INPUT input{};
		input.type = INPUT_KEYBOARD;
		input.ki.wVk = 0;
		input.ki.wScan = GetScanCode(vKey); // Scancode instead of vk, since vk is for typing & scancode is for input.
		input.ki.dwFlags = KEYEVENTF_SCANCODE;

		// If it's an extended range key, enable extended. Crazy, I know.
		if (IsExtended(vKey))
			input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;

		// If it's being unpressed, flip this flag.
		if (!pressed)
			input.ki.dwFlags |= KEYEVENTF_KEYUP;

		// And finally, send the input. This emulated a keyboard press, basically.
		SendInput(1, &input, sizeof(INPUT));
	}

	// Roblox pianos read sharps as Shift + key, so hold Shift long enough for the game to see it.
	void Tap(int vKey, bool shift) const {
		if (shift) {
			SendKey(VK_LSHIFT, true);
			Sleep(ShiftDelayMs);
		}

		SendKey(vKey, true);

		if (shift)
			Sleep(ShiftDelayMs);

		SendKey(vKey, false);

		if (shift)
			SendKey(VK_LSHIFT, false);
	}

	// Like Tap, but the key stays down. Shift is only needed when the key goes down.
	void Press(int vKey, bool shift) const {
		if (shift) {
			SendKey(VK_LSHIFT, true);
			Sleep(ShiftDelayMs);
		}

		SendKey(vKey, true);

		if (shift) {
			Sleep(ShiftDelayMs);
			SendKey(VK_LSHIFT, false);
		}
	}

	static constexpr DWORD ShiftDelayMs = 15;
};


// A piano key: the keyboard button, and whether Shift is held for it (sharps).
struct PianoKey {
	int vKey;
	bool shift;
};

class MidiMapper {
private:
	// Virtual Piano layout: 36 white keys from C2 (1) to C7 (M), sharps are Shift + the key below.
	static std::unordered_map<int, PianoKey> fullMap() {
		const char whites[] = "1234567890QWERTYUIOPASDFGHJKLZXCVBNM";
		const int steps[] = {0, 2, 4, 5, 7, 9, 11}; // C D E F G A B

		std::unordered_map<int, PianoKey> result;
		for (int i = 0; i < 36; ++i) {
			int note = 36 + 12 * (i / 7) + steps[i % 7];
			result[note] = {whites[i], false};

			// C D F G A have a sharp above them. C7 is the last key.
			int degree = note % 12;
			bool hasSharp = degree == 0 || degree == 2 || degree == 5 || degree == 7 || degree == 9;
			if (hasSharp && note < 96)
				result[note + 1] = {whites[i], true};
		}
		return result;
	}

	// The map in question.
	std::unordered_map<int, PianoKey> map;

public:
	// Constructor :yippee:
	MidiMapper() : map(fullMap()) {}

	std::optional<PianoKey> MapNote(int note) const {
		// Find an iterator to the note in the map, if it exists. Then return the corresponding key.
		// Also, inverted for the in-if init statement. Thanks for reminding me, linter.
		if (auto it = map.find(note); it != map.end())
			return it->second;

		return std::nullopt;
	}

	int GetExtremeValue(auto Sort) {
		if (map.empty())
			return 0;

		// Start with the first value in the map.
		int mostValue = map.begin()->first;

		// For each value, sort it based on the lambda to find the most extreme.
		for (auto [note, _] : map) {
			if (Sort(note, mostValue))
				mostValue = note;
		}

		return mostValue;
	}
};

// Struct to store midi events. Self-descriptive, really.
struct MidiEvent {
	uint64_t timeMs; // I LOVE UNSIGNED DATA TYPES. I LOVE CSTDINT. I LOVE ALL UINT##_T TYPES. I HATE UNPACKING OVERHEAD. darn.
	int note;
	bool noteOn;
};

class MidiFileParser {
private:
	// Intermediate event stored with tick time (before tempo conversion)
	struct RawEvent {
		uint32_t tick;
		int note;
		bool noteOn;
	};

	// A tempo change at a specific tick position
	struct TempoChange {
		uint32_t tick;
		uint32_t tempo; // microseconds per quarter note
	};

	// Convert an absolute tick position to microseconds using the global tempo map.
	// This ensures all tracks share the same tempo changes, fixing multi-track timing.
	static uint64_t TickToUs(uint32_t tick, const std::vector<TempoChange>& tempoMap, uint16_t tpqn) {
		uint64_t us = 0;
		uint32_t lastTick = 0;
		uint32_t currentTempo = 500000; // default 120 BPM

		for (const auto& tc : tempoMap) {
			if (tc.tick >= tick) break;
			us += (uint64_t)(tc.tick - lastTick) * currentTempo / tpqn;
			lastTick = tc.tick;
			currentTempo = tc.tempo;
		}

		us += (uint64_t)(tick - lastTick) * currentTempo / tpqn;
		return us;
	}

public:
	// A filesystem path, so names with accents or other alphabets open fine.
	static std::vector<MidiEvent> Parse(const std::filesystem::path& path) {
		// Step 1: Open the file.
		std::ifstream file(path, std::ios::binary);

		// File failed to open.
		if (!file)
			throw MidiFileException("Não consegui abrir o arquivo MIDI.");

		// Invalid file header- not midi.
		if (ReadString(file, 4) != "MThd")
			throw MidiFileException("Esse arquivo não é um MIDI.");

		uint32_t headerLength = Read32(file);
		std::streampos headerStart = file.tellg();
		Read16(file); // format
		uint16_t tracks = Read16(file);
		uint16_t tpqn = Read16(file); // Ticks per quarter note

		// Skip any extra header bytes
		if (headerLength > 6) {
			file.seekg(headerStart + static_cast<std::streamoff>(headerLength));
		}

		std::vector<RawEvent> rawEvents;
		std::vector<TempoChange> tempoMap;

		for (int tr = 0; tr < tracks; ++tr) {
			if (ReadString(file, 4) != "MTrk")
				throw MidiFileException("Arquivo MIDI com defeito.");

			auto trackLength = Read32(file);
			auto trackEnd = file.tellg() + (std::streamoff)trackLength;

			uint32_t tick = 0;
			uint8_t lastStatus = 0;

			while (file.tellg() < trackEnd) {
				uint32_t delta = ReadVar(file);
				tick += delta;

				uint8_t status = 0;
				file.read((char*)&status, 1);

				if (status < 0x80) {
					file.seekg(-1, std::ios::cur);
					status = lastStatus;
				} else {
					lastStatus = status;
				}

				uint8_t type = status & 0xF0;

				if (type == 0x90 || type == 0x80) {
					uint8_t note = 0;
					uint8_t vel = 0;
					file.read((char*)&note, 1);
					file.read((char*)&vel, 1);

					rawEvents.push_back({tick, note, type == 0x90 && vel > 0});
				} else if (status == 0xFF) {
					uint8_t metaType = 0;
					file.read((char*)&metaType, 1);
					uint32_t len = ReadVar(file);

					if (metaType == 0x51 && len == 3) {
						uint8_t buf[3]{};
						file.read((char*)buf, 3);
						uint32_t newTempo = (buf[0] << 16) | (buf[1] << 8) | buf[2];
						tempoMap.push_back({tick, newTempo});
					} else {
						file.seekg(len, std::ios::cur);
					}
				} else {
					SkipEvent(file, status);
				}
			}

			file.seekg(trackEnd);
		}

		// Sort tempo map by tick position
		std::ranges::sort(tempoMap, [](const auto& a, const auto& b) { return a.tick < b.tick; });

		// Convert tick-based events to real-time events using the global tempo map
		std::vector<MidiEvent> events;
		events.reserve(rawEvents.size());

		for (const auto& raw : rawEvents) {
			events.push_back({TickToUs(raw.tick, tempoMap, tpqn) / 1000, raw.note, raw.noteOn});
		}

		// Sort the events by time.
		// At the same time, note offs go first, so a repeated note isn't cut by its own previous off.
		std::ranges::sort(events, [](const auto& a, const auto& b) {
			return a.timeMs != b.timeMs ? a.timeMs < b.timeMs : (!a.noteOn && b.noteOn);
		});

		return events;
	}

private:
	static inline uint32_t ReadVar(std::ifstream& f) {
		uint32_t value = 0;
		uint8_t c = 0;

		// OKAY, so
		// If the first bit is 1, more data follows. Otherwise, this is the end. 
		// It's also only up to 4 bytes in length, so we add a hard limit there, too.
		// If this is data, shift it & add the 7 right bits.
		// (value = value * 128 + next 7 bits)
		for (int i = 0; i < 4; ++i) {  // MIDI VLQ max 4 bytes
			f.read(reinterpret_cast<char*>(&c), 1);
			value = (value << 7) | (c & 0x7F);

			if (!(c & 0x80)) // End!
				return value;
		}

		// Bruh
		return value;
	}

	static inline uint32_t Read32(std::ifstream& f) {
		// Read 4 bytes in big-endian order (MIDI standard)
		uint8_t b[4]{};
		f.read(reinterpret_cast<char*>(b), 4);
		return (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | uint32_t(b[3]);
	}


	static inline uint16_t Read16(std::ifstream& f) {
		// Read 2 bytes in big-endian order (MIDI standard)
		uint8_t b[2]{};
		f.read(reinterpret_cast<char*>(b), 2);
		return (uint16_t(b[0]) << 8) | uint16_t(b[1]);
	}

	static inline std::string ReadString(std::ifstream& f, size_t n) {
		// Erm... Pretty straightforward.
		// String. Read. Return.
		std::string s(n, ' ');
		f.read(s.data(), n);
		return s;
	}

	static void SkipEvent(std::ifstream& f, uint8_t status) {
		/*
		Status contains the event type. One of the following:
		- 0x80 Note Off
		- 0x90 Note On
		- 0xA0 Polyphonic key pressure
		- 0xB0 Control Change
		- 0xC0 Program Change
		- 0xD0 Channel pressure
		- 0xE0 Pitch bend
		*/

		// Get top 4 bits of the status.
		uint8_t type = status & 0xF0;

		// Program Change or Channel pressure. Skip 1.
		if (type == 0xC0 || type == 0xD0)
			f.seekg(1, std::ios::cur);

		// Polyphonic key pressure, Control Change, or Pitch bend. Skip 2.
		else if (type == 0xA0 || type == 0xB0 || type == 0xE0)
			f.seekg(2, std::ios::cur);

		// Meta event
		else if (status == 0xFF) {
			uint8_t metaType = 0;
			f.read(reinterpret_cast<char*>(&metaType), 1);
			f.seekg(ReadVar(f), std::ios::cur);
		}

		// System something or-rather. Check the length, then skip the length.
		else if (status == 0xF0 || status == 0xF7)
			f.seekg(ReadVar(f), std::ios::cur);
	}
};

// Small function to make a number fit in a range.
int Compress(int note, int min, int max) {
	if (note < min)
		while (note < min)
			note += 12;
	else while (note > max)
		note -= 12;

	return note;
}


// Status texts are UTF-8 (they have accents), so convert before showing them.
void SetStatus(HWND status, const std::string& text) {
	std::wstring wide(MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), (int)wide.size());
	SetWindowTextW(status, wide.c_str());
}

class MidiLiveInput {
private:
	HMIDIIN handle{}; // Every Windows handle makes me want to cry.
	MidiMapper& mapper;
	KeyboardEmitter& emitter;
	int min = 0;
	int max = 0;
	int transpose = 0;
	bool compress = false;
	std::mutex mutex;

	static void CALLBACK Callback(HMIDIIN, UINT msg, DWORD_PTR instance, DWORD_PTR param1, DWORD_PTR) {
		// If it's not data, just return.
		// MIM is midi callback messages, btw. Just in case you care. :pleading:
		if (msg != MIM_DATA) return;

		// Should I do reinterpret(static<void>(instance)) to get the linter to shut up? Do I care? Unsafe my ass.
		auto* self = reinterpret_cast<MidiLiveInput*>(instance);

		uint8_t status = param1 & 0xFF;
		int note = self->transpose + (int)((param1 >> 8) & 0xFF); // int, so a negative transpose doesn't wrap
		uint8_t vel = (param1 >> 16) & 0xFF;

		if (self->compress)
			note = Compress(note, self->min, self->max);

		// Get the mapped key.
		auto mapped = self->mapper.MapNote(note);

		// It's optional.
		if (!mapped.has_value())
			return;

		// Mutex, to avoid multithreading issues or smth. Jthread my beloved.
		std::scoped_lock lock(self->mutex);

		// Note on only. Roblox pianos play on press, so each note is a tap and note off is ignored.
		if ((status & 0xF0) == 0x90 && vel > 0)
			self->emitter.Tap(mapped->vKey, mapped->shift);
	}


public:
	MidiLiveInput(MidiMapper& mapper, KeyboardEmitter& emitter, bool compress, int transpose) : mapper(mapper), emitter(emitter), compress(compress), transpose(transpose) {}

	void Start(HWND statusHwnd) {
		// If there are no MIDI devices attached, we can't get midi input. Crazy. Crazy? I was-
		if (midiInGetNumDevs() == 0)
			throw MidiDeviceException("Nenhum teclado MIDI conectado.");

		// Something up with the Midi device. I don't really care what. Not my problem.
		if (midiInOpen(&handle, 0, (DWORD_PTR)&Callback, (DWORD_PTR)this, CALLBACK_FUNCTION) != MMSYSERR_NOERROR)
			throw MidiDeviceException("Não consegui abrir o teclado MIDI.");

		// Get the min and max, for if we compress
		min = mapper.GetExtremeValue([](int a, int b) {return a < b; });
		max = mapper.GetExtremeValue([](int a, int b) {return a > b; });

		// GO GO GO
		// Start listening to midi input.
		midiInStart(handle);

		SetStatus(statusHwnd, "Ouvindo o teclado MIDI...");
	}

	void Stop(HWND statusHwnd) {
		// Stop listening to midi input.
		midiInStop(handle);
		midiInClose(handle);

		SetStatus(statusHwnd, "Parado.");

		// Clear the handle
		handle = {};
	}
};


// Control IDs. have to be ints rather than HMENU to appease the linter's pointer const requirements.
constexpr int ID_EDIT_FILE = 101;
constexpr int ID_BTN_BROWSE = 102;
constexpr int ID_CHK_HOLD = 103;
constexpr int ID_CHK_LOOP = 104;
constexpr int ID_CHK_COMPRESS = 105;
constexpr int ID_EDIT_TRANSPOSE = 106;
constexpr int ID_SPN_TRANSPOSE = 107;
constexpr int ID_BTN_PLAY = 108;
constexpr int ID_BTN_LIVE = 109;
constexpr int ID_BTN_STOP = 110;
constexpr int ID_LBL_STATUS = 111;
constexpr int ID_HOTKEY_F1 = 112;
constexpr int ID_LIST_SONGS = 113;
constexpr int ID_BTN_REFRESH = 114;
constexpr int ID_BTN_PAUSE = 115;
constexpr int ID_HOTKEY_F2 = 116;
constexpr int ID_PROGRESS = 117;
constexpr int ID_LBL_TIME = 118;
constexpr int ID_TIMER_PROGRESS = 119;

struct AppState {
	std::wstring filePath;
	std::filesystem::path songsFolder; // "Musicas", next to the .exe
	std::vector<std::filesystem::path> songs; // Same order as the list box
	DWORD startDelayMs = 3000;
	bool loop = false;
	bool compress = true;
	bool hold = false;
	int transpose = 0;
	std::atomic<bool> playing = false;
	std::atomic<bool> paused = false;
	std::atomic<bool> liveMode = false;

	// Where the song is, for the progress bar. Written by the playback thread, read by a UI timer.
	std::atomic<uint64_t> positionMs = 0;
	std::atomic<uint64_t> lengthMs = 0;

	// Handles
	HWND handleEdit{};
	HWND handleBtnPause{};
	HWND handleProgress{};
	HWND handleTime{};
	HWND handleChkHold{};
	HWND handleChkLoop{};
	HWND handleChkCompress{};
	HWND handleEditTranspose{};
	HWND handleSpinTranspose{};
	HWND handleBtnPlay{};
	HWND handleBtnLive{};
	HWND handleBtnStop{};
	HWND handleStatus{};
	HWND handleList{};

	// Live input session stuff
	std::unique_ptr<MidiMapper> liveMapper;
	std::unique_ptr<KeyboardEmitter> liveEmitter;
	std::unique_ptr<MidiLiveInput> liveInput;
};

// Global state pointer for WndProc
AppState* g_state = nullptr;
std::jthread playThread;

// Look: light grey window, white cards, one blue accent.
namespace Theme {
	constexpr COLORREF Window = RGB(243, 244, 246);
	constexpr COLORREF Card = RGB(255, 255, 255);
	constexpr COLORREF Border = RGB(226, 228, 233);
	constexpr COLORREF Text = RGB(17, 24, 39);
	constexpr COLORREF Muted = RGB(107, 114, 128);
	constexpr COLORREF Accent = RGB(37, 99, 235);
	constexpr COLORREF AccentSoft = RGB(232, 240, 254);
	constexpr COLORREF Track = RGB(229, 231, 235);
}

struct Ui {
	int dpi = 96;
	HFONT body{}, title{}, section{}, list{};
	HBRUSH window{}, card{};
	std::vector<RECT> cards; // At 96 DPI
	std::unordered_set<HWND> onWindow, muted, sections;
	int progress = 0; // 0 to 1000
};

Ui g_ui;

// Scale a size at 96 DPI to the screen.
int S(int value) { return MulDiv(value, g_ui.dpi, 96); }


void Play(AppState& state) {
	// Put the playing and parsing inside a try/catch for "good enough" error handling
	try {
		// Update value
		state.compress = (IsDlgButtonChecked(GetParent(state.handleChkCompress), ID_CHK_COMPRESS) == BST_CHECKED);
		state.hold = (IsDlgButtonChecked(GetParent(state.handleChkHold), ID_CHK_HOLD) == BST_CHECKED);

		// Get the transposition
		std::string buf(256, '\0');
		GetWindowTextA(state.handleEditTranspose, buf.data(), buf.size());
		state.transpose = 12 * stoi(buf);

		// Set up the map, emitter, and parse the midi file.
		// Parsing is probably one of the most likely parts to throw an error.
		MidiMapper mapper;
		KeyboardEmitter emitter;
		auto events = MidiFileParser::Parse(state.filePath);
		state.positionMs = 0;
		state.lengthMs = events.empty() ? 0 : events.back().timeMs;

		// Get the min and max, for if we compress
		const int min = mapper.GetExtremeValue([](int a, int b) {return a < b; });
		const int max = mapper.GetExtremeValue([](int a, int b) {return a > b; });

		// Wait 3 seconds.
		Sleep(state.startDelayMs);

		// Set text anticipatorily. That's not a word, I'm pretty sure. In anticipation.
		SetStatus(state.handleStatus, "Tocando...");

		// How often do you get to use a do while loop in programming? I find them pretty rare, all things considered...
		do {
			// Update
			state.loop = (IsDlgButtonChecked(GetParent(state.handleChkLoop), ID_CHK_LOOP) == BST_CHECKED);

			// Set the start before we play the song.
			// I forgot to move this into the do-while before and spent far too long figuring out the issue.
			auto start = std::chrono::steady_clock::now();

			// Keys held down in hold mode, and the note each one is playing.
			// A natural and its sharp share a key (t and T), so only one of them can be held.
			std::unordered_map<int, int> held;

			auto releaseHeld = [&] {
				for (auto [vKey, _] : held)
					emitter.SendKey(vKey, false);
				held.clear();
			};

			// Wait in small steps, so Stop and Pause answer right away even in a long rest.
			// A pause lifts the held keys and pushes the start forward by however long it lasted.
			auto waitUntil = [&](uint64_t timeMs) {
				while (state.playing) {
					if (state.paused) {
						releaseHeld();
						auto pausedAt = std::chrono::steady_clock::now();

						while (state.paused && state.playing)
							std::this_thread::sleep_for(std::chrono::milliseconds(20));

						start += std::chrono::steady_clock::now() - pausedAt;
						continue;
					}

					auto now = std::chrono::steady_clock::now();
					state.positionMs = std::min<uint64_t>(timeMs, std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count());

					auto target = start + std::chrono::milliseconds(timeMs);
					if (now >= target)
						return;

					std::this_thread::sleep_until(std::min(target, now + std::chrono::milliseconds(20)));
				}
			};

			// Iterate through and play each event
			for (auto const& e : events) {
				// Stop early if requested
				if (!state.playing) break;

				// Without hold, every note is a tap, so note offs are skipped.
				if (!e.noteOn && !state.hold) continue;

				// Sleep intil the event
				waitUntil(e.timeMs);
				if (!state.playing) break;

				// Transpose the note
				int note = e.note + state.transpose;

				if (state.compress)
					note = Compress(note, min, max);

				// Get the corresponding input to the event. If it doesn't exist, skip it.
				auto mapped = mapper.MapNote(note);
				if (!mapped.has_value())
					continue;

				if (!state.hold) {
					emitter.Tap(mapped->vKey, mapped->shift);
					continue;
				}

				auto it = held.find(mapped->vKey);

				if (e.noteOn) {
					// Key already down (same note again, or its natural/sharp pair): lift it to play again.
					if (it != held.end())
						emitter.SendKey(mapped->vKey, false);

					emitter.Press(mapped->vKey, mapped->shift);
					held[mapped->vKey] = note;
				} else if (it != held.end() && it->second == note) {
					// Only lift it if this note still owns the key.
					emitter.SendKey(mapped->vKey, false);
					held.erase(it);
				}
			}

			// Never leave keys stuck down, whether the song ended or was stopped.
			releaseHeld();
		} while (state.loop && state.playing);
	} catch (const std::exception& ex) { // ZOINKS!
		SetStatus(state.handleStatus, ex.what());

		// Re-enable the buttons.
		EnableWindow(state.handleBtnPlay, TRUE);
		EnableWindow(state.handleBtnLive, TRUE);
		EnableWindow(state.handleBtnStop, FALSE);
		EnableWindow(state.handleBtnPause, FALSE);
		state.playing = false;

		// So the message doesn't get overridden.
		return;
	}

	// Re-enable the buttons.
	EnableWindow(state.handleBtnPlay, TRUE);
	EnableWindow(state.handleBtnLive, TRUE);
	EnableWindow(state.handleBtnStop, FALSE);
	EnableWindow(state.handleBtnPause, FALSE);

	// state.playing will be true if we finished, or false if we were stopped.
	if (state.playing) {
		state.playing = false;
		SetStatus(state.handleStatus, "Fim da música.");
	} else {
		SetStatus(state.handleStatus, "Parado.");
	}
}

// From F1 the player is already in the game, so the wait before playing is short.
void StartFilePlayback(AppState& state, DWORD startDelayMs = 3000) {
	state.startDelayMs = startDelayMs;

	// Grab the file path from the edit box
	std::wstring buf(GetWindowTextLengthW(state.handleEdit) + 1, L'\0');
	GetWindowTextW(state.handleEdit, buf.data(), (int)buf.size());

	//resize the path to what's needed
	buf.resize(wcsnlen(buf.data(), buf.size()));

	state.filePath = buf;

	// Null check. Uh. Empty check, sorry. Yuh.
	if (state.filePath.empty()) {
		SetStatus(state.handleStatus, "Escolha uma música na lista.");
		return;
	}

	// Playing. Read line below for more details.
	state.playing = true;
	state.paused = false;
	SetWindowTextW(state.handleBtnPause, L"Pausar");

	// Set the buttons to active/inactive respectively.
	EnableWindow(state.handleBtnPlay, FALSE);
	EnableWindow(state.handleBtnLive, FALSE);
	EnableWindow(state.handleBtnStop, TRUE);
	EnableWindow(state.handleBtnPause, TRUE);

	// We set the text here, but we start the timer in the Play function inside the thread, so we still have the UI on the main thread.
	SetStatus(state.handleStatus, startDelayMs >= 1000 ? "Começa em 3 segundos. Volte para o jogo." : "Começando...");

	// Start the thread.
	playThread = std::jthread(Play, std::ref(state));
}

void StartLiveInput(AppState& state) {
	try {
		// Check compress
		state.compress = (IsDlgButtonChecked(GetParent(state.handleChkCompress), ID_CHK_COMPRESS) == BST_CHECKED);

		// Get the transposition
		std::string buf(256, '\0');
		GetWindowTextA(g_state->handleEditTranspose, buf.data(), buf.size());
		g_state->transpose = 12 * stoi(buf);

		state.liveMapper = std::make_unique<MidiMapper>();
		state.liveEmitter = std::make_unique<KeyboardEmitter>();
		state.liveInput = std::make_unique<MidiLiveInput>(*state.liveMapper, *state.liveEmitter, state.compress, state.transpose);

		state.liveInput->Start(state.handleStatus);

		state.liveMode = true;
		EnableWindow(state.handleBtnPlay, FALSE);
		EnableWindow(state.handleBtnLive, FALSE);
		EnableWindow(state.handleChkHold, FALSE);
		EnableWindow(state.handleChkLoop, FALSE);
		EnableWindow(state.handleChkCompress, FALSE);
		EnableWindow(state.handleEditTranspose, FALSE);
		EnableWindow(state.handleSpinTranspose, FALSE);
		EnableWindow(state.handleBtnStop, TRUE);
	} catch (const std::exception& ex) {
		SetStatus(state.handleStatus, ex.what());
	}
}

void Stop(AppState& state) {
	if (state.playing) {
		state.playing = false; // Signal the playback thread to exit
		state.paused = false;
		SetWindowTextW(state.handleBtnPause, L"Pausar");
	}

	if (state.liveMode) {
		// Clear and reset everything
		state.liveInput->Stop(state.handleStatus);
		state.liveInput.reset();
		state.liveEmitter.reset();
		state.liveMapper.reset();
		state.liveMode = false;

		// Re-enable the buttons.
		EnableWindow(state.handleBtnPlay, TRUE);
		EnableWindow(state.handleBtnLive, TRUE);
		EnableWindow(state.handleChkHold, TRUE);
		EnableWindow(state.handleChkLoop, TRUE);
		EnableWindow(state.handleChkCompress, TRUE);
		EnableWindow(state.handleEditTranspose, TRUE);
		EnableWindow(state.handleSpinTranspose, TRUE);
		EnableWindow(state.handleBtnStop, FALSE);
	}
}

// Pause or resume the song. The playback thread lifts held keys and keeps its place.
void TogglePause(AppState& state) {
	if (!state.playing)
		return;

	state.paused = !state.paused;
	SetWindowTextW(state.handleBtnPause, state.paused ? L"Continuar" : L"Pausar");
	SetStatus(state.handleStatus, state.paused ? "Pausado. F1 ou Continuar para seguir." : "Tocando...");
}

// m:ss, for the time next to the progress bar.
std::wstring FormatTime(uint64_t ms) {
	uint64_t seconds = ms / 1000;
	wchar_t buf[32];
	swprintf(buf, 32, L"%llu:%02llu", seconds / 60, seconds % 60);
	return buf;
}

// Runs on a UI timer, so the playback thread never waits on the window.
void UpdateProgress(AppState& state) {
	uint64_t length = state.lengthMs;
	uint64_t position = state.playing ? state.positionMs.load() : 0;

	int progress = length ? (int)(position * 1000 / length) : 0;
	if (progress != g_ui.progress) {
		g_ui.progress = progress;
		InvalidateRect(state.handleProgress, nullptr, FALSE);
	}
	SetWindowTextW(state.handleTime, (FormatTime(position) + L" / " + FormatTime(length)).c_str());
}

// Fill the list with every .mid in the Musicas folder, by name, without the extension.
void LoadSongs(AppState& state) {
	std::error_code error;
	std::filesystem::create_directories(state.songsFolder, error);

	state.songs.clear();
	for (const auto& entry : std::filesystem::directory_iterator(state.songsFolder, error)) {
		std::wstring extension = entry.path().extension().wstring();
		if (entry.is_regular_file(error) && (lstrcmpiW(extension.c_str(), L".mid") == 0 || lstrcmpiW(extension.c_str(), L".midi") == 0))
			state.songs.push_back(entry.path());
	}

	std::ranges::sort(state.songs, [](const auto& a, const auto& b) {
		return lstrcmpiW(a.stem().c_str(), b.stem().c_str()) < 0;
	});

	SendMessageW(state.handleList, LB_RESETCONTENT, 0, 0);
	for (const auto& song : state.songs)
		SendMessageW(state.handleList, LB_ADDSTRING, 0, (LPARAM)song.stem().c_str());

	if (state.songs.empty())
		SetWindowTextW(state.handleStatus, (L"Nenhuma música ainda. Coloque arquivos .mid em: " + state.songsFolder.wstring()).c_str());
}

// Put the chosen song in the file box, so Play File and F1 use it.
void SelectSong(AppState& state) {
	auto index = SendMessageW(state.handleList, LB_GETCURSEL, 0, 0);
	if (index == LB_ERR || index >= (LRESULT)state.songs.size())
		return;

	state.filePath = state.songs[index].wstring();
	SetWindowTextW(state.handleEdit, state.filePath.c_str());

	// Scroll the box to the end, so the song's name shows instead of the folders.
	SendMessageW(state.handleEdit, EM_SETSEL, state.filePath.size(), state.filePath.size());
}

// Small helper so every control is made the same way, at the scaled size.
HWND MakeControl(HWND parent, DWORD exStyle, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id) {
	HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
	return CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style,
		S(x), S(y), S(w), S(h), parent, (HMENU)(INT_PTR)id, hInst, nullptr);
}

// A small grey uppercase title at the top of a card.
HWND MakeSection(HWND parent, const wchar_t* text, int x, int y, int w) {
	HWND label = MakeControl(parent, 0, L"STATIC", text, 0, x, y, w, 18, 0);
	g_ui.sections.insert(label);
	return label;
}

// Create the UI
// Light grey window, three white cards (song, options, list), one blue accent. Sizes are at 96 DPI.
LRESULT CALLBACK Create(HWND hwnd, LPARAM) {
	// Load DLLs for updown spin
	InitCommonControls();

	// Header, on the window background
	HWND title = MakeControl(hwnd, 0, L"STATIC", L"Roblox MIDI Player", 0, 20, 14, 400, 32, 0);
	HWND subtitle = MakeControl(hwnd, 0, L"STATIC",
		L"F1 toca, pausa e continua   ·   F2 para   ·   funciona com o jogo na frente", 0, 20, 48, 560, 20, 0);
	g_ui.onWindow.insert({title, subtitle});
	g_ui.muted.insert(subtitle);

	// Card 1: the song, its progress and the main buttons
	g_ui.cards.push_back({16, 80, 584, 240});
	MakeSection(hwnd, L"MÚSICA", 32, 94, 200);

	g_state->handleEdit = MakeControl(hwnd, WS_EX_CLIENTEDGE, L"EDIT", L"", ES_AUTOHSCROLL, 32, 118, 440, 26, ID_EDIT_FILE);
	MakeControl(hwnd, 0, L"BUTTON", L"Procurar...", 0, 480, 117, 88, 28, ID_BTN_BROWSE);

	// Progress bar, drawn by hand (WM_DRAWITEM), with the time next to it
	g_state->handleProgress = MakeControl(hwnd, 0, L"STATIC", L"", SS_OWNERDRAW, 32, 162, 440, 8, ID_PROGRESS);
	g_state->handleTime = MakeControl(hwnd, 0, L"STATIC", L"0:00 / 0:00", SS_RIGHT, 480, 156, 88, 20, ID_LBL_TIME);
	g_ui.muted.insert(g_state->handleTime);

	g_state->handleBtnPlay = MakeControl(hwnd, 0, L"BUTTON", L"Tocar", BS_DEFPUSHBUTTON, 32, 188, 100, 34, ID_BTN_PLAY);
	g_state->handleBtnPause = MakeControl(hwnd, 0, L"BUTTON", L"Pausar", 0, 140, 188, 100, 34, ID_BTN_PAUSE);
	g_state->handleBtnStop = MakeControl(hwnd, 0, L"BUTTON", L"Parar", 0, 248, 188, 100, 34, ID_BTN_STOP);
	g_state->handleBtnLive = MakeControl(hwnd, 0, L"BUTTON", L"Teclado MIDI", 0, 448, 188, 120, 34, ID_BTN_LIVE);

	// Disable until playing
	EnableWindow(g_state->handleBtnStop, FALSE);
	EnableWindow(g_state->handleBtnPause, FALSE);

	// Card 2: options
	g_ui.cards.push_back({16, 256, 584, 336});
	MakeSection(hwnd, L"OPÇÕES", 32, 270, 200);

	// Off by default: keys are held for as long as the file says, instead of tapped.
	g_state->handleChkHold = MakeControl(hwnd, 0, L"BUTTON", L"Segurar notas", BS_CHECKBOX, 32, 294, 124, 24, ID_CHK_HOLD);
	g_state->handleChkLoop = MakeControl(hwnd, 0, L"BUTTON", L"Repetir", BS_CHECKBOX, 164, 294, 80, 24, ID_CHK_LOOP);
	g_state->handleChkCompress = MakeControl(hwnd, 0, L"BUTTON", L"Ajustar oitavas", BS_CHECKBOX, 252, 294, 136, 24, ID_CHK_COMPRESS);

	// On by default: notes outside C2..C7 are moved in by octaves instead of dropped.
	CheckDlgButton(hwnd, ID_CHK_COMPRESS, BST_CHECKED);

	// Transpose, in octaves
	MakeControl(hwnd, 0, L"STATIC", L"Oitava", SS_RIGHT, 404, 297, 72, 20, 0);
	g_state->handleEditTranspose = MakeControl(hwnd, WS_EX_CLIENTEDGE, L"EDIT", L"0", ES_NUMBER | ES_CENTER, 484, 294, 84, 26, ID_EDIT_TRANSPOSE);
	g_state->handleSpinTranspose = MakeControl(hwnd, 0, UPDOWN_CLASS, L"",
		UDS_WRAP | UDS_ARROWKEYS | UDS_SETBUDDYINT | UDS_ALIGNRIGHT, 0, 0, 0, 0, ID_SPN_TRANSPOSE);

	// Set buddy & range
	SendMessage(g_state->handleSpinTranspose, UDM_SETBUDDY, (WPARAM)g_state->handleEditTranspose, 0L);
	SendMessage(g_state->handleSpinTranspose, UDM_SETRANGE, 0, MAKELPARAM(3, -3));

	// Card 3: the songs in the Musicas folder next to the .exe
	g_ui.cards.push_back({16, 352, 584, 604});
	MakeSection(hwnd, L"MÚSICAS", 32, 368, 80);
	HWND hint = MakeControl(hwnd, 0, L"STATIC", L"clique duplo para tocar", 0, 104, 367, 240, 20, 0);
	g_ui.muted.insert(hint);
	MakeControl(hwnd, 0, L"BUTTON", L"Atualizar", 0, 480, 361, 88, 28, ID_BTN_REFRESH);

	// Owner drawn, so the rows can be taller with a soft highlight
	g_state->handleList = MakeControl(hwnd, 0, L"LISTBOX", L"",
		WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS, 24, 398, 552, 196, ID_LIST_SONGS);

	// Status line, under the cards
	g_state->handleStatus = MakeControl(hwnd, 0, L"STATIC", L"Pronto. Escolha uma música na lista.", SS_ENDELLIPSIS, 20, 614, 560, 20, ID_LBL_STATUS);
	g_ui.onWindow.insert(g_state->handleStatus);
	g_ui.muted.insert(g_state->handleStatus);

	// Fonts: Segoe UI everywhere, bigger for the title, small and bold for section names
	EnumChildWindows(hwnd, [](HWND child, LPARAM) -> BOOL {
		SendMessageW(child, WM_SETFONT, (WPARAM)g_ui.body, FALSE);
		return TRUE;
	}, 0);
	SendMessageW(title, WM_SETFONT, (WPARAM)g_ui.title, FALSE);
	for (HWND section : g_ui.sections)
		SendMessageW(section, WM_SETFONT, (WPARAM)g_ui.section, FALSE);
	SendMessageW(g_state->handleList, WM_SETFONT, (WPARAM)g_ui.list, FALSE);

	SetTimer(hwnd, ID_TIMER_PROGRESS, 200, nullptr);

	// F1 and F2 work from any window, so the song can be controlled without leaving Roblox.
	bool hasF1 = RegisterHotKey(hwnd, ID_HOTKEY_F1, MOD_NOREPEAT, VK_F1);
	bool hasF2 = RegisterHotKey(hwnd, ID_HOTKEY_F2, MOD_NOREPEAT, VK_F2);
	if (!hasF1 || !hasF2)
		SetStatus(g_state->handleStatus, "F1 ou F2 está em uso por outro programa. Use os botões.");

	std::wstring exePath(MAX_PATH, L'\0');
	exePath.resize(GetModuleFileNameW(nullptr, exePath.data(), (DWORD)exePath.size()));
	g_state->songsFolder = std::filesystem::path(exePath).parent_path() / L"Musicas";

	LoadSongs(*g_state);

	return 0;
}

// Cards: white rounded boxes with a thin border, behind the controls.
void PaintCards(HWND hwnd) {
	PAINTSTRUCT ps;
	HDC dc = BeginPaint(hwnd, &ps);

	HPEN pen = CreatePen(PS_SOLID, 1, Theme::Border);
	HGDIOBJ oldPen = SelectObject(dc, pen);
	HGDIOBJ oldBrush = SelectObject(dc, g_ui.card);

	for (const RECT& r : g_ui.cards)
		RoundRect(dc, S(r.left), S(r.top), S(r.right), S(r.bottom), S(14), S(14));

	SelectObject(dc, oldBrush);
	SelectObject(dc, oldPen);
	DeleteObject(pen);
	EndPaint(hwnd, &ps);
}

// The progress bar and the song rows.
void DrawItem(const DRAWITEMSTRUCT& item) {
	HDC dc = item.hDC;
	RECT r = item.rcItem;

	if (item.CtlID == ID_PROGRESS) {
		FillRect(dc, &r, g_ui.card);
		int radius = r.bottom - r.top;

		HBRUSH track = CreateSolidBrush(Theme::Track);
		HBRUSH fill = CreateSolidBrush(Theme::Accent);
		HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));

		HGDIOBJ oldBrush = SelectObject(dc, track);
		RoundRect(dc, r.left, r.top, r.right + 1, r.bottom + 1, radius, radius);

		int width = (int)((r.right - r.left) * (int64_t)g_ui.progress / 1000);
		if (width > 0) {
			SelectObject(dc, fill);
			RoundRect(dc, r.left, r.top, r.left + std::max(width, radius) + 1, r.bottom + 1, radius, radius);
		}

		SelectObject(dc, oldBrush);
		SelectObject(dc, oldPen);
		DeleteObject(track);
		DeleteObject(fill);
		return;
	}

	if (item.CtlID == ID_LIST_SONGS) {
		bool selected = item.itemState & ODS_SELECTED;
		HBRUSH background = CreateSolidBrush(selected ? Theme::AccentSoft : Theme::Card);
		FillRect(dc, &r, background);
		DeleteObject(background);

		if (item.itemID == (UINT)-1)
			return;

		wchar_t name[MAX_PATH] = {};
		SendMessageW(item.hwndItem, LB_GETTEXT, item.itemID, (LPARAM)name);

		SetBkMode(dc, TRANSPARENT);

		RECT note = r;
		note.left += S(14);
		SetTextColor(dc, Theme::Accent);
		DrawTextW(dc, L"♪", -1, &note, DT_SINGLELINE | DT_VCENTER);

		RECT text = r;
		text.left += S(38);
		text.right -= S(12);
		SetTextColor(dc, selected ? Theme::Accent : Theme::Text);
		DrawTextW(dc, name, -1, &text, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
	}
}

LRESULT CALLBACK Command(WPARAM wParam, HWND hwnd) {
	switch (LOWORD(wParam)) {
		// Open file browser
		case ID_BTN_BROWSE:
		{
			wchar_t buf[MAX_PATH] = {}; // Wide, so any file name works.

			OPENFILENAMEW ofn{};
			ofn.lStructSize = sizeof(ofn);
			ofn.hwndOwner = hwnd;
			ofn.lpstrFilter = L"MIDI Files\0*.mid;*.midi\0All Files\0*.*\0";
			ofn.lpstrFile = buf;
			ofn.nMaxFile = MAX_PATH;
			ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

			if (GetOpenFileNameW(&ofn)) {
				SetWindowTextW(g_state->handleEdit, buf);
				g_state->filePath = buf;
			}

			break;
		}

		case ID_BTN_REFRESH:
			LoadSongs(*g_state);
			break;

		case ID_BTN_PAUSE:
			TogglePause(*g_state);
			break;

		case ID_LIST_SONGS:
			if (HIWORD(wParam) == LBN_SELCHANGE) {
				SelectSong(*g_state);
			} else if (HIWORD(wParam) == LBN_DBLCLK) {
				SelectSong(*g_state);

				// Same rule as F1: only start when nothing is playing.
				if (IsWindowEnabled(g_state->handleBtnPlay))
					StartFilePlayback(*g_state);
			}
			break;

		case ID_CHK_HOLD:
			// Toggle the checkbox state
			g_state->hold = !(IsDlgButtonChecked(hwnd, ID_CHK_HOLD) == BST_CHECKED);
			CheckDlgButton(hwnd, ID_CHK_HOLD, g_state->hold ? BST_CHECKED : BST_UNCHECKED);
			break;

		case ID_CHK_LOOP:
			// Toggle the checkbox state. Same code as above.
			g_state->loop = !(IsDlgButtonChecked(hwnd, ID_CHK_LOOP) == BST_CHECKED);
			CheckDlgButton(hwnd, ID_CHK_LOOP, g_state->loop ? BST_CHECKED : BST_UNCHECKED);
			break;

		case ID_CHK_COMPRESS:
			// Toggle the checkbox state. Same code as above.
			g_state->compress = !(IsDlgButtonChecked(hwnd, ID_CHK_COMPRESS) == BST_CHECKED);
			CheckDlgButton(hwnd, ID_CHK_COMPRESS, g_state->compress ? BST_CHECKED : BST_UNCHECKED);
			break;

		case ID_BTN_PLAY:
			StartFilePlayback(*g_state);
			break;

		case ID_BTN_LIVE:
			StartLiveInput(*g_state);
			break;

		case ID_BTN_STOP:
			Stop(*g_state);
			break;

		default: // Linter.
			break;
	}

	return 0;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	// When a button is pressed, we can just switch it.
	switch (msg) {
		case WM_CREATE:
			return Create(hwnd, lParam);

		case WM_COMMAND:
			return Command(wParam, hwnd);

		case WM_HOTKEY:
			// F1: play, pause, resume. Live input has nothing to pause, so F1 stops it.
			if (wParam == ID_HOTKEY_F1) {
				if (g_state->liveMode)
					Stop(*g_state);
				else if (g_state->playing)
					TogglePause(*g_state);
				// Play stays disabled until a stopped song has fully finished, so wait for that.
				else if (IsWindowEnabled(g_state->handleBtnPlay))
					StartFilePlayback(*g_state, 500);
			}
			// F2: stop, back to the start.
			else if (wParam == ID_HOTKEY_F2) {
				if (g_state->playing || g_state->liveMode)
					Stop(*g_state);
			}
			return 0;

		case WM_PAINT:
			PaintCards(hwnd);
			return 0;

		case WM_MEASUREITEM: {
			auto* item = (MEASUREITEMSTRUCT*)lParam;
			if (item->CtlID == ID_LIST_SONGS) {
				item->itemHeight = S(32);
				return TRUE;
			}
			break;
		}

		case WM_DRAWITEM:
			DrawItem(*(DRAWITEMSTRUCT*)lParam);
			return TRUE;

		// Text on a card gets the card colour, text on the window gets the window colour.
		case WM_CTLCOLORSTATIC: {
			HWND control = (HWND)lParam;
			HDC dc = (HDC)wParam;
			bool onWindow = g_ui.onWindow.contains(control);
			SetBkColor(dc, onWindow ? Theme::Window : Theme::Card);
			SetTextColor(dc, g_ui.muted.contains(control) || g_ui.sections.contains(control) ? Theme::Muted : Theme::Text);
			return (LRESULT)(onWindow ? g_ui.window : g_ui.card);
		}

		case WM_CTLCOLORBTN:
		case WM_CTLCOLORLISTBOX:
			return (LRESULT)g_ui.card;

		case WM_TIMER:
			if (wParam == ID_TIMER_PROGRESS) {
				UpdateProgress(*g_state);
				return 0;
			}
			break;

		case WM_VSCROLL: // spin control for transpose.
			if ((HWND)(lParam) == g_state->handleSpinTranspose) {
				std::string buf(50, '\0');
				GetWindowTextA(g_state->handleEditTranspose, buf.data(), buf.size());
				buf.resize(strnlen(buf.data(), buf.size()));

				g_state->transpose = 12 * std::stoi(buf);

				return 0;
			}
			break;

		case WM_DESTROY:
			UnregisterHotKey(hwnd, ID_HOTKEY_F1);
			UnregisterHotKey(hwnd, ID_HOTKEY_F2);
			KillTimer(hwnd, ID_TIMER_PROGRESS);

			// Clean up if still running
			if (g_state->playing || g_state->liveMode)
				Stop(*g_state);
			PostQuitMessage(0);
			return 0;

			// Linter wants default. I dunno, man. I tried and it broke.
	}

	return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
	// Tracks our variables for us...
	AppState state{};
	g_state = &state;

	// Register window class
	WNDCLASSEXW wc{};
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = WndProc;
	wc.hInstance = hInstance;
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	// Screen scale, fonts and brushes for the look in Theme
	HDC screen = GetDC(nullptr);
	g_ui.dpi = GetDeviceCaps(screen, LOGPIXELSY);
	ReleaseDC(nullptr, screen);

	auto font = [](int px, int weight) {
		return CreateFontW(-S(px), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
			CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
	};
	g_ui.body = font(14, FW_NORMAL);
	g_ui.title = font(22, FW_SEMIBOLD);
	g_ui.section = font(12, FW_SEMIBOLD);
	g_ui.list = font(15, FW_NORMAL);
	g_ui.window = CreateSolidBrush(Theme::Window);
	g_ui.card = CreateSolidBrush(Theme::Card);

	wc.hbrBackground = g_ui.window;
	wc.lpszClassName = L"RobloxMidiPlayer";
	RegisterClassExW(&wc);

	// Create the window, sized so the inside is 600 x 648 at 96 DPI
	const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN; // Fixed size
	RECT size{0, 0, S(600), S(648)};
	AdjustWindowRect(&size, style, FALSE);

	HWND hwnd = CreateWindowExW(
		0,
		L"RobloxMidiPlayer",
		L"Roblox MIDI Player",
		style,
		CW_USEDEFAULT, CW_USEDEFAULT,
		size.right - size.left, size.bottom - size.top,
		nullptr, nullptr,
		hInstance, nullptr
	);

	ShowWindow(hwnd, nCmdShow);
	UpdateWindow(hwnd);

	// Message loop only wakes up when something actually happens. We hate polling in this household.
	MSG msg;
	while (GetMessage(&msg, nullptr, 0, 0)) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	// Wait for playback to finish stopping, since it reads `state`, which dies with this function.
	if (playThread.joinable())
		playThread.join();

	return 0;
}

// If input is main instead of WinMain, just use WinMain anyway.
int main() {
	WinMain(GetModuleHandle(nullptr), nullptr, nullptr, SW_SHOWDEFAULT);
}
