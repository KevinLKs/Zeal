#include "card_collection.h"

#include <Windows.h>

#include <chrono>
#include <ctime>
#include <fstream>

#include "json.hpp"

// Declared here rather than including game_functions.h, same trick as io_ini.h, so this file
// stays a plain data module that only needs the chat line.
namespace Zeal::Game {
void print_chat(const char *format, ...);
}

namespace {

// File layout, all little endian:
//   char[4]  "CONC"                       Cards Of Norrath Collection.
//   uint32   format version               CardCollection::kFormatVersion.
//   uint32   payload length in bytes.
//   uint64   FNV-1a 64 of (plain payload || character name).
//   uint8[]  payload: MessagePack of the JSON document below, XORed with a fixed keystream.
//
// The check covers the character name so that copying another character's file over your own
// fails the check and gets flagged, without any of this pretending to be encryption. Anyone who
// reads this source can regenerate the file; that is the accepted trust model.
//
// JSON document (before packing):
//   { "character": "...", "saved": "...", "flagged": false,
//     "cards": { "<art>": { "kills": 0,
//                           "first": { "n","e","s","w","found","zone","from" },   (only if has_first)
//                           "copies": [ { "n","e","s","w","found","zone","from" }, ... ] } } }
constexpr char kMagic[4] = {'C', 'O', 'N', 'C'};
constexpr size_t kHeaderSize = 4 + 4 + 4 + 8;

uint64_t fnv1a64(const uint8_t *data, size_t length, uint64_t hash = 14695981039346656037ull) {
  for (size_t i = 0; i < length; ++i) {
    hash ^= data[i];
    hash *= 1099511628211ull;
  }
  return hash;
}

// xorshift32 keystream over the buffer, in place. Symmetric, so the same call decodes. The seed
// is a constant on purpose: the character name is part of the integrity check instead, so a file
// copied between characters still decodes and gets flagged (load-and-flag) rather than turning
// into an unreadable file that looks like corruption.
void obfuscate(std::vector<uint8_t> &bytes) {
  uint32_t state = 0x5EA1CA4Du;
  for (uint8_t &b : bytes) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    b ^= static_cast<uint8_t>(state);
  }
}

void put_u32(std::vector<uint8_t> &out, uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void put_u64(std::vector<uint8_t> &out, uint64_t v) {
  for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
uint32_t get_u32(const uint8_t *p) {
  return p[0] | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint64_t get_u64(const uint8_t *p) {
  return uint64_t(get_u32(p)) | (uint64_t(get_u32(p + 4)) << 32);
}

nlohmann::json copy_to_json(const CardCollection::Copy &c) {
  return nlohmann::json{{"n", c.north},     {"e", c.east},     {"s", c.south},       {"w", c.west},
                        {"found", c.found}, {"zone", c.zone}, {"from", c.dropped_from}};
}

CardCollection::Copy copy_from_json(const nlohmann::json &j) {
  CardCollection::Copy c;
  c.north = j.value("n", 0);
  c.east = j.value("e", 0);
  c.south = j.value("s", 0);
  c.west = j.value("w", 0);
  c.found = j.value("found", "");
  c.zone = j.value("zone", "");
  c.dropped_from = j.value("from", "");
  return c;
}

// Writes the whole buffer and flushes it to disk before returning. Win32 rather than fstream so
// the flush is a real FlushFileBuffers and not just a runtime buffer drain.
bool write_all(const std::filesystem::path &path, const std::vector<uint8_t> &bytes) {
  HANDLE h = CreateFileA(path.string().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                         nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  bool ok = WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
            written == bytes.size() && FlushFileBuffers(h);
  CloseHandle(h);
  return ok;
}

bool read_all(const std::filesystem::path &path, std::vector<uint8_t> &bytes) {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) return false;
  bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  return true;
}

}  // namespace

std::string CardCollection::now_string() {
  std::time_t t = std::time(nullptr);
  std::tm tm{};
  localtime_s(&tm, &t);
  char buffer[32];
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &tm);
  return buffer;
}

std::filesystem::path CardCollection::path_for(const std::string &character_name) {
  return std::filesystem::path(kDirectory) / (character_name + kExtension);
}

// ---- lifecycle ----

bool CardCollection::load(const std::string &name) {
  unload();
  character_name = name;
  loaded = true;
  flagged = false;
  dirty = false;
  entries.clear();

  const std::filesystem::path main_path = path_for(name);
  const std::filesystem::path backup_path = std::filesystem::path(kDirectory) / (name + kBackupExtension);

  // Prefer the main file. Fall back to the backup only when the main file is missing (a crash
  // between the rotate and the rename) or does not parse at all. A main file that parses but
  // fails its check is used as-is and flagged: load-and-flag, never refuse.
  bool integrity_ok = true;
  if (read_file(main_path, integrity_ok)) {
    if (!integrity_ok) flagged = true;
    if (flagged) {
      Zeal::Game::print_chat("Cards of Norrath: %s's collection failed its integrity check and is flagged.",
                             name.c_str());
    }
    return true;
  }
  const bool main_exists = std::filesystem::exists(main_path);
  entries.clear();
  if (read_file(backup_path, integrity_ok)) {
    if (!integrity_ok) flagged = true;
    Zeal::Game::print_chat("Cards of Norrath: restored %s's collection from its backup%s.", name.c_str(),
                           flagged ? " (flagged)" : "");
    dirty = true;  // Re-establish the main file straight away.
    save();
    return true;
  }
  entries.clear();
  if (main_exists) {
    Zeal::Game::print_chat("Cards of Norrath: could not read %s's collection file; starting empty. A save will overwrite it.",
                           name.c_str());
    return false;
  }
  return true;  // New character, nothing on disk yet. Nothing is written until something happens.
}

bool CardCollection::save() {
  if (!loaded) return false;
  std::error_code ec;
  std::filesystem::create_directories(kDirectory, ec);

  const std::filesystem::path main_path = path_for(character_name);
  const std::filesystem::path temp_path = std::filesystem::path(kDirectory) / (character_name + kTempExtension);
  const std::filesystem::path backup_path = std::filesystem::path(kDirectory) / (character_name + kBackupExtension);

  if (!write_file(temp_path)) {
    Zeal::Game::print_chat("Cards of Norrath: failed to write %s", temp_path.string().c_str());
    return false;
  }
  // Rotate the previous file to .bak, then swap the new one in. MOVEFILE_WRITE_THROUGH makes the
  // rename itself durable before the call returns.
  if (std::filesystem::exists(main_path))
    MoveFileExA(main_path.string().c_str(), backup_path.string().c_str(), MOVEFILE_REPLACE_EXISTING);
  if (!MoveFileExA(temp_path.string().c_str(), main_path.string().c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    Zeal::Game::print_chat("Cards of Norrath: failed to replace %s", main_path.string().c_str());
    return false;
  }
  dirty = false;
  return true;
}

bool CardCollection::save_if_dirty() { return dirty ? save() : true; }

void CardCollection::unload() {
  if (loaded && dirty) save();
  loaded = false;
  dirty = false;
  flagged = false;
  character_name.clear();
  entries.clear();
}

// ---- queries ----

CardCollection::State CardCollection::state(const std::string &key) const {
  const Entry *e = find(key);
  if (!e) return State::Unknown;
  if (!e->copies.empty()) return State::Held;
  return e->kills > 0 ? State::Encountered : State::Unknown;
}

const CardCollection::Entry *CardCollection::find(const std::string &key) const {
  auto it = entries.find(key);
  return it == entries.end() ? nullptr : &it->second;
}

int CardCollection::held(const std::string &key) const {
  const Entry *e = find(key);
  return e ? static_cast<int>(e->copies.size()) : 0;
}

int CardCollection::kills(const std::string &key) const {
  const Entry *e = find(key);
  return e ? e->kills : 0;
}

// ---- mutations ----

void CardCollection::record_kill(const std::string &key) {
  if (!loaded || key.empty()) return;
  entries[key].kills++;
  dirty = true;
  save();
}

bool CardCollection::grant(const std::string &key, const Copy &copy, int cap) {
  if (!loaded || key.empty()) return false;
  Entry &e = entries[key];
  if (cap > 0 && static_cast<int>(e.copies.size()) >= cap) return false;
  e.copies.push_back(copy);
  if (!e.has_first) {
    e.first = copy;
    e.has_first = true;
  }
  dirty = true;
  save();
  return true;
}

bool CardCollection::remove_copy(const std::string &key, int index) {
  if (!loaded) return false;
  auto it = entries.find(key);
  if (it == entries.end() || index < 0 || index >= static_cast<int>(it->second.copies.size())) return false;
  it->second.copies.erase(it->second.copies.begin() + index);
  if (it->second.copies.empty() && it->second.kills == 0 && !it->second.has_first) entries.erase(it);
  dirty = true;
  save();
  return true;
}

void CardCollection::reset() {
  if (!loaded) return;
  entries.clear();
  flagged = false;
  dirty = true;
  save();
}

// ---- encoding ----

std::vector<uint8_t> CardCollection::encode() const {
  nlohmann::json cards = nlohmann::json::object();
  for (const auto &[key, e] : entries) {
    nlohmann::json entry;
    entry["kills"] = e.kills;
    if (e.has_first) entry["first"] = copy_to_json(e.first);
    nlohmann::json copies = nlohmann::json::array();
    for (const Copy &c : e.copies) copies.push_back(copy_to_json(c));
    entry["copies"] = std::move(copies);
    cards[key] = std::move(entry);
  }
  nlohmann::json root;
  root["character"] = character_name;
  root["saved"] = now_string();
  root["flagged"] = flagged;
  root["cards"] = std::move(cards);

  std::vector<uint8_t> payload = nlohmann::json::to_msgpack(root);
  uint64_t check = fnv1a64(payload.data(), payload.size());
  check = fnv1a64(reinterpret_cast<const uint8_t *>(character_name.data()), character_name.size(), check);
  obfuscate(payload);

  std::vector<uint8_t> out;
  out.reserve(kHeaderSize + payload.size());
  out.insert(out.end(), kMagic, kMagic + 4);
  put_u32(out, kFormatVersion);
  put_u32(out, static_cast<uint32_t>(payload.size()));
  put_u64(out, check);
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

bool CardCollection::decode(const std::vector<uint8_t> &bytes, bool &integrity_ok) {
  integrity_ok = false;
  if (bytes.size() < kHeaderSize) return false;
  if (memcmp(bytes.data(), kMagic, 4) != 0) return false;
  const uint32_t version = get_u32(bytes.data() + 4);
  const uint32_t length = get_u32(bytes.data() + 8);
  const uint64_t stored_check = get_u64(bytes.data() + 12);
  if (version == 0 || version > kFormatVersion) return false;
  if (bytes.size() != kHeaderSize + length) return false;

  std::vector<uint8_t> payload(bytes.begin() + kHeaderSize, bytes.end());
  obfuscate(payload);
  uint64_t check = fnv1a64(payload.data(), payload.size());
  check = fnv1a64(reinterpret_cast<const uint8_t *>(character_name.data()), character_name.size(), check);
  integrity_ok = (check == stored_check);

  nlohmann::json root;
  try {
    root = nlohmann::json::from_msgpack(payload);
  } catch (const std::exception &) {
    return false;
  }
  if (!root.is_object()) return false;

  entries.clear();
  flagged = root.value("flagged", false);
  if (root.value("character", character_name) != character_name) integrity_ok = false;
  if (root.contains("cards") && root["cards"].is_object()) {
    for (auto it = root["cards"].begin(); it != root["cards"].end(); ++it) {
      const nlohmann::json &j = it.value();
      if (!j.is_object()) continue;
      Entry e;
      e.kills = j.value("kills", 0);
      if (j.contains("first") && j["first"].is_object()) {
        e.first = copy_from_json(j["first"]);
        e.has_first = true;
      }
      if (j.contains("copies") && j["copies"].is_array())
        for (const auto &c : j["copies"]) e.copies.push_back(copy_from_json(c));
      if (!e.has_first && !e.copies.empty()) {
        e.first = e.copies.front();
        e.has_first = true;
      }
      entries[it.key()] = std::move(e);
    }
  }
  return true;
}

bool CardCollection::read_file(const std::filesystem::path &path, bool &integrity_ok) {
  std::vector<uint8_t> bytes;
  if (!read_all(path, bytes)) return false;
  return decode(bytes, integrity_ok);
}

bool CardCollection::write_file(const std::filesystem::path &path) const { return write_all(path, encode()); }
