#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

// Cards of Norrath: per-character collection state. This is everything the collection book shows
// that is not in pool.json: which cards this character holds (and each copy's ranks and where it
// came from), and how many times they have killed something that carries a card they do not hold
// yet. Nothing here knows how to draw; the Cards class owns one of these and reads it.
//
// Identity. Entries are keyed by the card's hashed art stem (CardDef::art), which the forge derives
// from identity that never changes (expansion plus dex), so renaming a creature never orphans a
// player's copies, and secret cards (no dex) key the same way as everything else.
//
// Three states, per decisions.md: Unknown (nothing recorded), Encountered (killed it, never had
// it drop: kills > 0 and no copies), Held (one or more copies). A card with copies is Held even if
// kills is 0, since a secret card is awarded rather than dropped.
//
// Storage, per the locked decisions: one file per character, obfuscated binary with an integrity
// check (friction, not security), atomic replace on every change with a rotating .bak, and
// load-and-flag rather than refuse when the check fails. Losing a collection to a crash is worse
// than any amount of cheating.
class CardCollection {
 public:
  static constexpr char kDirectory[] = ".\\cards_of_norrath";  // Relative to the EQ directory, like zeal.ini.
  static constexpr char kExtension[] = ".collection";
  static constexpr char kBackupExtension[] = ".collection.bak";
  static constexpr char kTempExtension[] = ".collection.tmp";
  static constexpr uint32_t kFormatVersion = 1;

  // One physical copy of a card. Ranks are stored per copy because below level 7 they are rolled
  // (the roll itself is not designed yet; see CardCollection::grant). The record fields are the
  // acquisition record the book shows: when, what zone, and what died for it.
  struct Copy {
    int north = 0, east = 0, south = 0, west = 0;
    std::string found;         // Local time, "2026-09-13 14:27".
    std::string zone;          // Full zone name at the time of the drop.
    std::string dropped_from;  // Name of the creature that died. Empty for an award (secret cards).
  };

  struct Entry {
    int kills = 0;            // Deaths credited to this character of creatures carrying this card.
    std::vector<Copy> copies; // In acquisition order.
    // The first-found record, set on the first grant and never changed afterwards, so it survives
    // copies being traded away later.
    Copy first;
    bool has_first = false;
  };

  enum class State { Unknown, Encountered, Held };

  // ---- lifecycle ----

  // Switches to this character's file, saving the previous character first if anything is
  // unsaved. Starts empty if the file does not exist. Returns false only when a file existed and
  // could not be read at all (the collection is then empty in memory and a save would overwrite
  // it, so callers should print something).
  bool load(const std::string &character_name);

  // Writes the current state. Called by every mutation, so callers normally never need it; it is
  // public for the shutdown paths. Returns false if nothing is loaded or the write failed.
  bool save();
  bool save_if_dirty();

  // Saves if needed and forgets everything. Safe to call when nothing is loaded.
  void unload();

  bool is_loaded() const { return loaded; }
  const std::string &character() const { return character_name; }
  // True when an integrity check failed on load at any point in this collection's history. Sticky
  // and persisted. Nothing is refused because of it; it is information.
  bool is_flagged() const { return flagged; }
  std::filesystem::path path() const { return path_for(character_name); }

  // ---- queries ----
  State state(const std::string &key) const;
  const Entry *find(const std::string &key) const;  // nullptr when Unknown.
  int held(const std::string &key) const;           // Copies held, 0 when not held.
  int kills(const std::string &key) const;
  int entry_count() const { return static_cast<int>(entries.size()); }
  const std::unordered_map<std::string, Entry> &all() const { return entries; }

  // ---- mutations (each one saves) ----

  // A creature carrying this card died with this character credited. Flips Unknown to
  // Encountered; also counted while Held (the book can show it as "killed N").
  void record_kill(const std::string &key);

  // Adds a copy. Returns false and changes nothing when the entry already holds `cap` copies.
  // The copy's ranks are whatever the caller passes; the roll below level 7 is the caller's
  // problem (decisions.md, still open item 1) so this class never invents numbers.
  bool grant(const std::string &key, const Copy &copy, int cap);

  // Removes the copy at `index` (trading, eventually). Returns false if there is no such copy. An
  // entry with no copies and no kills is dropped entirely.
  bool remove_copy(const std::string &key, int index);

  // Empties the collection and saves the empty state. For the /cards wipe debug command.
  void reset();

  // Local time formatted the way Copy::found expects.
  static std::string now_string();

 private:
  static std::filesystem::path path_for(const std::string &character_name);
  // Reads and decodes one file into `entries`/`flagged`. `integrity_ok` reports the check; the
  // content is loaded either way when it parses. Returns false when the file is missing or does
  // not parse at all.
  bool read_file(const std::filesystem::path &path, bool &integrity_ok);
  bool write_file(const std::filesystem::path &path) const;
  std::vector<uint8_t> encode() const;
  bool decode(const std::vector<uint8_t> &bytes, bool &integrity_ok);

  std::string character_name;
  std::unordered_map<std::string, Entry> entries;
  bool loaded = false;
  bool flagged = false;
  bool dirty = false;
};
