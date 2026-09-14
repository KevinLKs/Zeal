#pragma once

#include <filesystem>
#include <string>
#include <vector>

// Cards of Norrath: which card, if any, does a dead creature belong to.
//
// Deliberately free of Windows, Direct3D and Zeal headers so it can be unit tested off-target.
// Everything it matches on is readable from the client's Entity struct at kill time:
//
//   race     Entity::Race     0x00AA   game_structures.h:1324
//   texture  Entity::Texture  0x00EC   game_structures.h:1338
//   level    Entity::Level    0x00AD   game_structures.h:1326
//   zone     get_self()->ZoneId
//   name     Entity::Name, run through Zeal::Game::trim_name first
//
// bodytype is NOT here on purpose. The server never sends it, so the client cannot see whether a
// rat is undead. bodytype was used offline, against the PQDI scrape, only to work out which names
// need excluding; the exclusions it produced are expressed above in terms the client can check.
// See claude/kill-hook-matching.md.
class CardSources {
 public:
  // One matching clause. race is required; every other constraint is optional and is skipped when
  // left at its empty/zero default. All name comparisons are case-insensitive.
  struct Rule {
    int race = -1;
    std::vector<int> textures;  // Empty means any texture.
    std::vector<int> zones;     // Empty means any zone.
    int level_min = 0;          // 0/0 means any level.
    int level_max = 0;
    std::string name_contains;      // Lowercase substring that must appear.
    std::string name_not_contains;  // Lowercase substring that must NOT appear.
    std::string name_prefix;        // Lowercase prefix.
    std::string name_exact;         // Lowercase whole name.
  };

  struct Entry {
    std::string art;   // Hash stem; the same key the collection store uses.
    std::string name;  // Card name, for chat output only.
    bool secret = false;
    std::vector<Rule> rules;
    std::vector<std::string> exclude_names;  // Lowercase; a hit here vetoes the whole card.
  };

  // Replaces the current rules only on success, so a bad edit leaves the old set running.
  // On failure `error` is filled and false is returned.
  bool load(const std::filesystem::path &path, std::string &error);

  bool is_loaded() const { return loaded; }
  size_t card_count() const { return entries.size(); }
  size_t rule_count() const;

  // Art hash of the first card whose rules accept this spawn, or an empty string for no match.
  // Entries are held in file order, so the answer is stable across runs.
  std::string match(int race, int texture, int level, int zone_id, const std::string &name) const;

  // Every match, not just the first. Only used by the debug command and the tests; a spawn
  // matching more than one card is an authoring bug worth seeing rather than silently resolving.
  std::vector<std::string> match_all(int race, int texture, int level, int zone_id,
                                     const std::string &name) const;

  const Entry *find(const std::string &art) const;
  const std::vector<Entry> &all() const { return entries; }

  static std::string to_lower(const std::string &s);

 private:
  static bool rule_accepts(const Rule &rule, int race, int texture, int level, int zone_id,
                           const std::string &lower_name);

  std::vector<Entry> entries;
  bool loaded = false;
};
