#include "card_sources.h"

#include <algorithm>
#include <cctype>
#include <fstream>

#include "json.hpp"

std::string CardSources::to_lower(const std::string &s) {
  std::string out = s;
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

namespace {

std::vector<int> read_int_array(const nlohmann::json &parent, const char *key) {
  std::vector<int> out;
  if (!parent.contains(key) || !parent.at(key).is_array()) return out;
  for (const auto &v : parent.at(key))
    if (v.is_number_integer()) out.push_back(v.get<int>());
  return out;
}

std::string read_lower_string(const nlohmann::json &parent, const char *key) {
  if (!parent.contains(key) || !parent.at(key).is_string()) return std::string();
  return CardSources::to_lower(parent.at(key).get<std::string>());
}

}  // namespace

bool CardSources::load(const std::filesystem::path &path, std::string &error) {
  std::ifstream file(path);
  if (!file.is_open()) {
    error = "cannot open " + path.string();
    return false;
  }

  std::vector<Entry> parsed;
  try {
    nlohmann::json root = nlohmann::json::parse(file);
    if (!root.contains("cards") || !root.at("cards").is_object()) {
      error = "no \"cards\" object";
      return false;
    }
    // nlohmann's default object type is std::map, so this iterates in key order rather than file
    // order. That is still deterministic, which is all match() promises.
    for (const auto &[art, node] : root.at("cards").items()) {
      Entry entry;
      entry.art = art;
      entry.name = node.value("name", "");
      entry.secret = node.value("secret", false);
      for (const std::string &n : node.value("exclude_names", std::vector<std::string>()))
        entry.exclude_names.push_back(to_lower(n));
      if (node.contains("sources") && node.at("sources").is_array()) {
        for (const auto &src : node.at("sources")) {
          Rule rule;
          // A rule with no race can never be satisfied, so drop it rather than let it match all.
          if (!src.contains("race") || !src.at("race").is_number_integer()) continue;
          rule.race = src.at("race").get<int>();
          rule.textures = read_int_array(src, "textures");
          rule.zones = read_int_array(src, "zones");
          const std::vector<int> level = read_int_array(src, "level");
          if (level.size() == 2) {
            rule.level_min = level[0];
            rule.level_max = level[1];
          }
          rule.name_contains = read_lower_string(src, "name_contains");
          rule.name_not_contains = read_lower_string(src, "name_not_contains");
          rule.name_prefix = read_lower_string(src, "name_prefix");
          rule.name_exact = read_lower_string(src, "name_exact");
          entry.rules.push_back(rule);
        }
      }
      parsed.push_back(entry);
    }
  } catch (const std::exception &ex) {
    error = ex.what();
    return false;
  }

  entries = std::move(parsed);
  loaded = true;
  error.clear();
  return true;
}

size_t CardSources::rule_count() const {
  size_t n = 0;
  for (const Entry &e : entries) n += e.rules.size();
  return n;
}

bool CardSources::rule_accepts(const Rule &rule, int race, int texture, int level, int zone_id,
                               const std::string &lower_name) {
  if (rule.race != race) return false;
  if (!rule.textures.empty() &&
      std::find(rule.textures.begin(), rule.textures.end(), texture) == rule.textures.end())
    return false;
  if (!rule.zones.empty() && std::find(rule.zones.begin(), rule.zones.end(), zone_id) == rule.zones.end())
    return false;
  // level_max 0 means the rule does not care. Level 0 spawns do exist, so an explicit band that
  // starts at 0 still works; it is only the absent band that is encoded as 0/0.
  if (rule.level_max > 0 && (level < rule.level_min || level > rule.level_max)) return false;
  if (!rule.name_contains.empty() && lower_name.find(rule.name_contains) == std::string::npos) return false;
  if (!rule.name_not_contains.empty() && lower_name.find(rule.name_not_contains) != std::string::npos)
    return false;
  if (!rule.name_prefix.empty() && lower_name.rfind(rule.name_prefix, 0) != 0) return false;
  if (!rule.name_exact.empty() && lower_name != rule.name_exact) return false;
  return true;
}

std::vector<std::string> CardSources::match_all(int race, int texture, int level, int zone_id,
                                                const std::string &name) const {
  std::vector<std::string> hits;
  const std::string lower_name = to_lower(name);
  for (const Entry &entry : entries) {
    if (entry.rules.empty()) continue;  // Secret cards and unauthored cards never drop from a kill.
    if (std::find(entry.exclude_names.begin(), entry.exclude_names.end(), lower_name) !=
        entry.exclude_names.end())
      continue;
    for (const Rule &rule : entry.rules) {
      if (rule_accepts(rule, race, texture, level, zone_id, lower_name)) {
        hits.push_back(entry.art);
        break;
      }
    }
  }
  return hits;
}

std::string CardSources::match(int race, int texture, int level, int zone_id, const std::string &name) const {
  const std::string lower_name = to_lower(name);
  for (const Entry &entry : entries) {
    if (entry.rules.empty()) continue;
    if (std::find(entry.exclude_names.begin(), entry.exclude_names.end(), lower_name) !=
        entry.exclude_names.end())
      continue;
    for (const Rule &rule : entry.rules)
      if (rule_accepts(rule, race, texture, level, zone_id, lower_name)) return entry.art;
  }
  return std::string();
}

const CardSources::Entry *CardSources::find(const std::string &art) const {
  for (const Entry &entry : entries)
    if (entry.art == art) return &entry;
  return nullptr;
}
