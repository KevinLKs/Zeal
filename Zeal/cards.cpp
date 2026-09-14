#include "cards.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>

#include "bitmap_font.h"
#include "callbacks.h"
#include "commands.h"
#include "game_addresses.h"
#include "game_functions.h"
#include "json.hpp"
#include "ui_manager.h"
#include "ui_skin.h"
#include "zeal.h"

// Layout of the collection window's card grid, inside the 630x840 window (client area is shorter
// by the title bar). Scaled 1.5x from the milestone 2 layout (120x160 cards) so the card frame
// reads clearly at normal resolution; the rank and label fonts are unscaled bitmap fonts, so this
// is the only lever. The battle window (milestone 3) reuses the same card size.
static constexpr int kColumns = 3;
static constexpr int kRows = 3;
static constexpr float kGap = 15.f;
static constexpr float kCardWidth = 180.f;
static constexpr float kCardHeight = 240.f;
static constexpr float kArtHeight = 180.f;  // Square art at the top, label strip below.
static constexpr float kBorder = 3.f;
// Diamond of ranks in the top left of the art, pulled in from the border by these two (separate
// axes since Kevin wants it snugged into the corner rather than centred by a single inset).
// Measured 2026-09-13 off a screenshot against his reference marks: shift left ~18px, up ~10px
// from the milestone-2 placement (which used a single 9px inset on both axes).
static constexpr float kRankInsetX = -6.f;
static constexpr float kRankInsetY = -1.f;
static constexpr float kRankSpacing = 21.f;
static constexpr float kRankBacking = 72.f;  // Square behind the diamond.
// Height of art, in pixels, blended into the label strip colour at the seam instead of a hard
// cutoff (draw_card feathers this in with a vertex-alpha gradient quad).
static constexpr float kFeatherHeight = 8.f;
// Vertical distance from the label strip's centre to each of the two text lines (name above,
// level below).
static constexpr float kLabelLineOffset = 11.f;

static constexpr D3DCOLOR kTableColor = D3DCOLOR_ARGB(230, 24, 48, 32);
static constexpr D3DCOLOR kCardBorderColor = D3DCOLOR_ARGB(255, 109, 95, 61);       // #6d5f3d
static constexpr D3DCOLOR kCardBackColor = D3DCOLOR_ARGB(255, 42, 37, 25);          // #2a2519
static constexpr D3DCOLOR kCardBackColorTransparent = D3DCOLOR_ARGB(0, 42, 37, 25);  // Same, alpha 0.
static constexpr D3DCOLOR kMissingArtColor = D3DCOLOR_ARGB(255, 70, 60, 80);
static constexpr D3DCOLOR kWhite = D3DCOLOR_ARGB(255, 255, 255, 255);
static constexpr D3DCOLOR kNameColor = D3DCOLOR_ARGB(255, 232, 223, 198);  // #e8dfc6
static constexpr D3DCOLOR kLevelColor = D3DCOLOR_ARGB(255, 109, 95, 61);   // #6d5f3d, matches border

// Rank numbers carry their own outline instead of sitting on a backing tile: eight offset copies
// in a near-black colour behind the real white glyph, like a hand-drawn thick text outline.
static constexpr D3DCOLOR kRankOutlineColor = D3DCOLOR_ARGB(255, 0, 0, 0);
static constexpr float kRankOutlineThickness = 2.f;

// ---- battle window layout (milestone 3) ----
// A hand card not currently moused over draws only its top kHandPeekHeight pixels: border, the
// start of the art, and the rank diamond (which already lives inside that region by construction,
// see draw_card). That is the whole "FF8 stack" effect, no separate peek asset or clip rect
// needed. First-guess numbers below; expect to tune all of them once Kevin screenshots this.
static constexpr float kBattleMargin = 20.f;
static constexpr float kHandBoardGap = 30.f;    // Between a hand stack and the board.
static constexpr float kHandPeekHeight = 120.f;  // "Half the card", per Kevin's own description.
static constexpr float kInfoBarHeight = 40.f;
static constexpr float kInfoBarGap = 12.f;
static constexpr float kScoreGap = 14.f;
static constexpr float kScoreBlockHeight = 40.f;  // Room for the arial_bold_36 numeral above each stack.
static constexpr float kHoverShift = 24.f;  // How far a hovered hand card shifts toward the board.
// The hover shift is tweened, not snapped: each card's offset eases toward its target (kHoverShift
// when hovered, 0 otherwise) with this time constant, so it reaches ~95% of the way in about three
// times this. Frame-rate independent since it's driven by the tick delta.
static constexpr float kHoverTweenMs = 55.f;
static constexpr unsigned long kMaxFrameDeltaMs = 100;  // Clamp after a hitch so the tween doesn't jump.
static constexpr unsigned long kFoeMoveDelayMs = 620;  // Matches the browser prototype's pause.

// Ownership colour, not card data: whichever side is local always reads blue, the opponent always
// red, so this still means the right thing if Cards is ever played PvP. Hex values match the
// validated browser prototype's --you-edge / --foe-edge.
static constexpr D3DCOLOR kBattleYouColor = D3DCOLOR_ARGB(255, 0x2f, 0x7a, 0x74);
static constexpr D3DCOLOR kBattleFoeColor = D3DCOLOR_ARGB(255, 0xb8, 0x54, 0x3f);
static constexpr D3DCOLOR kBoardCellColor = D3DCOLOR_ARGB(140, 20, 40, 30);
static constexpr D3DCOLOR kBoardOpenTargetColor = D3DCOLOR_ARGB(90, 220, 200, 120);

static std::filesystem::path get_cards_path() {
  return UISkin::get_zeal_resources_path() / std::filesystem::path(Cards::kCardsSubDirectory);
}

static bool point_in_rect(int32_t x, int32_t y, const RECT &r) {
  return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

// SidlScreenWnd vtable overrides. Same shapes as the map window in zone_map.cpp. Shared by both
// the collection window and the battle window; only the click handler differs between them.

// Draw nothing behind the client area so the RenderUI callback owns every pixel inside the frame.
static int __fastcall DrawBackground(Zeal::GameUI::SidlWnd *wnd, int unusedEDX) { return 0; }

// The client does not call this for windows outside its own list; Zeal calls it on zone exit.
static void __fastcall Deactivate(Zeal::GameUI::SidlWnd *wnd, int unusedEDX) { wnd->show(0, false); }

// Use the mouse left button down event since it has undergone proper zlayer window filtering
// (same reasoning as zone_map.cpp's HandleLButtonDown).
static int __fastcall BattleHandleLButtonDown(Zeal::GameUI::SidlWnd *wnd, int unusedEDX, int32_t mouse_x,
                                              int32_t mouse_y, uint32_t unused3) {
  ZealService *zeal = ZealService::get_instance();
  if (zeal && zeal->cards) zeal->cards->handle_battle_click(mouse_x, mouse_y);
  return 0;
}

bool Cards::is_window_visible() const { return wnd && wnd->IsVisible; }

void Cards::show_window() {
  if (!wnd) return;
  if (!pool_loaded) load_pool();
  if (!wnd->IsVisible) wnd->show(1, true);
}

void Cards::hide_window() {
  if (wnd && wnd->IsVisible) wnd->show(0, false);
}

void Cards::toggle_window() {
  if (!wnd) {
    Zeal::Game::print_chat("Cards of Norrath: window is not available (missing %s?)", kXmlFilename);
    return;
  }
  if (wnd->IsVisible)
    hide_window();
  else
    show_window();
}

bool Cards::is_battle_window_visible() const { return battle_wnd && battle_wnd->IsVisible; }

void Cards::show_battle_window() {
  if (!battle_wnd) return;
  if (!pool_loaded) load_pool();
  if (!battle_started) start_new_battle();
  if (!battle_wnd->IsVisible) battle_wnd->show(1, true);
}

void Cards::hide_battle_window() {
  if (battle_wnd && battle_wnd->IsVisible) battle_wnd->show(0, false);
}

void Cards::toggle_battle_window() {
  if (!battle_wnd) {
    Zeal::Game::print_chat("Cards of Norrath: battle window is not available (missing %s?)", kBattleXmlFilename);
    return;
  }
  if (battle_wnd->IsVisible)
    hide_battle_window();
  else
    show_battle_window();
}

bool Cards::parse_command(const std::vector<std::string> &args) {
  if (args.size() > 1) {
    if (args[1] == "version") {
      Zeal::Game::print_chat("Cards of Norrath %s", kVersion);
      return true;
    }
    if (args[1] == "reload") {
      release_resources();  // Drop textures and fonts so edited art is picked up too.
      if (load_pool())
        Zeal::Game::print_chat("Cards of Norrath: reloaded %d cards, %d source rules",
                               static_cast<int>(pool.size()), static_cast<int>(sources.rule_count()));
      return true;
    }
    if (args[1] == "battle") {
      toggle_battle_window();
      return true;
    }
    // Collection debug commands. These stand in for the drop and kill hooks until those exist, so
    // the collection book can be built and tested against real per-character state.
    if (args[1] == "collection") {
      if (!collection.is_loaded()) {
        Zeal::Game::print_chat("Cards of Norrath: no collection loaded (not in game yet?)");
        return true;
      }
      int held_cards = 0, copies = 0, encountered = 0;
      for (const auto &[key, entry] : collection.all()) {
        if (!entry.copies.empty()) {
          held_cards++;
          copies += static_cast<int>(entry.copies.size());
        } else if (entry.kills > 0) {
          encountered++;
        }
      }
      Zeal::Game::print_chat("Cards of Norrath: %s holds %d cards (%d copies), %d encountered, %d in pool%s",
                             collection.character().c_str(), held_cards, copies, encountered,
                             static_cast<int>(pool.size()), collection.is_flagged() ? " [flagged]" : "");
      Zeal::Game::print_chat("  file: %s", collection.path().string().c_str());
      return true;
    }
    if (args[1] == "grant" || args[1] == "kill") {
      if (!pool_loaded) load_pool();
      const CardDef *card = (args.size() > 2) ? find_by_dex(atoi(args[2].c_str())) : nullptr;
      if (!card) {
        Zeal::Game::print_chat("Usage: /cards %s <dex> %s", args[1].c_str(),
                               args[1] == "grant" ? "[dropped from...]" : "");
        return true;
      }
      if (args[1] == "kill") {
        record_kill(*card);
        Zeal::Game::print_chat("Cards of Norrath: killed for #%d %s (%d so far, %d held)", card->dex,
                               card->name.c_str(), collection.kills(card->art), collection.held(card->art));
        return true;
      }
      std::string dropped_from;
      for (size_t i = 3; i < args.size(); ++i) dropped_from += (i > 3 ? " " : "") + args[i];
      if (dropped_from.empty()) {
        Zeal::GameStructures::Entity *target = Zeal::Game::get_target();
        dropped_from = target ? Zeal::Game::trim_name(target->Name) : card->name;
      }
      if (grant(*card, dropped_from))
        Zeal::Game::print_chat("Cards of Norrath: granted #%d %s (%d of %d) from %s", card->dex, card->name.c_str(),
                               collection.held(card->art), copy_cap(*card), dropped_from.c_str());
      else
        Zeal::Game::print_chat("Cards of Norrath: #%d %s is at its cap of %d", card->dex, card->name.c_str(),
                               copy_cap(*card));
      return true;
    }
    // Prints the identity fields the client can actually see on a spawn. bodytype is server side and
    // never reaches the client, so (race, texture, level, size, name) is the whole vocabulary the kill
    // hook gets to match a card with. Use this to check the rules in kill-hook-matching.md against the
    // real client, and to confirm Texture here equals PQDI's texture column.
    if (args[1] == "probe") {
      Zeal::GameStructures::Entity *target = Zeal::Game::get_target();
      if (!target) {
        Zeal::Game::print_chat("Usage: /cards probe  (target a mob first)");
        return true;
      }
      Zeal::GameStructures::Entity *self = Zeal::Game::get_self();
      // Avoid %f in print_chat: no precedent for it in this codebase, so size is printed as tenths.
      int size_whole = static_cast<int>(target->Height);
      int size_tenth = static_cast<int>(target->Height * 10.f) - size_whole * 10;
      if (size_tenth < 0) size_tenth = 0;
      Zeal::Game::print_chat("Cards probe: \"%s\"", Zeal::Game::trim_name(target->Name));
      Zeal::Game::print_chat("  race %d   texture %d   level %d   size %d.%d", static_cast<int>(target->Race),
                             static_cast<int>(target->Texture), static_cast<int>(target->Level), size_whole,
                             size_tenth);
      Zeal::Game::print_chat("  class %d   gender %d   type %d   head_material %d",
                             static_cast<int>(target->Class), static_cast<int>(target->Gender),
                             static_cast<int>(target->Type),
                             static_cast<int>(target->EquipmentMaterialType[0]));
      Zeal::Game::print_chat("  spawn_id %d   zone_id %d   hp %d/%d", static_cast<int>(target->SpawnId),
                             self ? static_cast<int>(self->ZoneId) : -1, target->HpCurrent, target->HpMax);
      if (!pool_loaded) load_pool();
      const int zone_id = self ? static_cast<int>(self->ZoneId) : -1;
      const std::string probe_name = Zeal::Game::trim_name(target->Name);
      const std::vector<std::string> all = sources.match_all(target->Race, target->Texture, target->Level, zone_id,
                                                             probe_name);
      if (all.empty()) {
        Zeal::Game::print_chat("  -> no card%s", sources.is_loaded() ? "" : " (sources.json not loaded)");
      } else {
        for (const std::string &art : all) {
          const CardDef *card = find_by_art(art);
          Zeal::Game::print_chat("  -> card #%d %s", card ? card->dex : -1,
                                 card ? card->name.c_str() : art.c_str());
        }
        // More than one card claiming the same creature is an authoring bug, not a tie to break.
        if (all.size() > 1) Zeal::Game::print_chat("  *** %d cards claim this spawn ***", static_cast<int>(all.size()));
      }
      return true;
    }
    if (args[1] == "sources") {
      if (!pool_loaded) load_pool();
      if (!sources.is_loaded()) {
        Zeal::Game::print_chat("Cards of Norrath: no source rules loaded (%s missing?)", kSourcesFilename);
        return true;
      }
      int with_rules = 0;
      for (const CardSources::Entry &entry : sources.all())
        if (!entry.rules.empty()) with_rules++;
      Zeal::Game::print_chat("Cards of Norrath: %d source rules over %d of %d cards", static_cast<int>(sources.rule_count()),
                             with_rules, static_cast<int>(sources.card_count()));
      Zeal::Game::print_chat("  file: %s", (get_cards_path() / kSourcesFilename).string().c_str());
      return true;
    }
    if (args[1] == "wipe") {
      if (args.size() > 2 && args[2] == "confirm" && collection.is_loaded()) {
        collection.reset();
        Zeal::Game::print_chat("Cards of Norrath: %s's collection wiped", collection.character().c_str());
      } else {
        Zeal::Game::print_chat("Cards of Norrath: /cards wipe confirm  (erases this character's whole collection)");
      }
      return true;
    }
    Zeal::Game::print_chat("Usage: /cards [version | reload | battle | probe | sources | collection | grant <dex> | kill <dex> | wipe]");
    return true;
  }
  toggle_window();
  return true;  // Handled, so the client does not treat /cards as an unknown command.
}

// Reads the forge export. Field names match cards-of-norrath-pool.json format version 1.
bool Cards::load_pool(bool verbose) {
  std::filesystem::path path = get_cards_path() / kPoolFilename;
  std::ifstream file(path);
  if (!file.is_open()) {
    if (verbose) Zeal::Game::print_chat("Cards of Norrath: cannot open %s", path.string().c_str());
    return false;
  }

  std::vector<CardDef> loaded;
  try {
    nlohmann::json root = nlohmann::json::parse(file);
    for (const auto &entry : root.at("cards")) {
      CardDef card;
      card.dex = (entry.contains("dex") && entry.at("dex").is_number()) ? entry.at("dex").get<int>() : -1;
      card.expansion = entry.value("expansion", "");
      card.name = entry.value("name", "");
      card.level = entry.value("level", 0);  // Level 0 is the secret tier, never a default.
      card.secret = entry.value("secret", false);
      card.element =
          (entry.contains("element") && entry.at("element").is_string()) ? entry.at("element").get<std::string>() : "";
      const auto &base = entry.at("base");
      card.north = base.value("n", 0);
      card.east = base.value("e", 0);
      card.south = base.value("s", 0);
      card.west = base.value("w", 0);
      card.art = entry.value("art", "");
      card.flair = entry.value("flair", "");
      if (card.name.empty()) continue;  // The forge refuses to save these; be consistent.
      loaded.push_back(card);
    }
  } catch (const std::exception &ex) {
    if (verbose) Zeal::Game::print_chat("Cards of Norrath: bad pool file: %s", ex.what());
    return false;
  }

  // Dex order, with secret cards (no dex) after everything else. Stable so ties keep file order.
  std::stable_sort(loaded.begin(), loaded.end(), [](const CardDef &a, const CardDef &b) {
    if ((a.dex < 0) != (b.dex < 0)) return b.dex < 0;
    return a.dex < b.dex;
  });
  pool = std::move(loaded);
  pool_loaded = true;
  load_sources(verbose);
  if (verbose) Zeal::Game::print_chat("Cards of Norrath: loaded %d cards from %s", static_cast<int>(pool.size()),
                                      kPoolFilename);
  return true;
}

// Reads the kill-to-card matching rules. A missing file is not an error: before the rules were
// authored the addon simply credits nothing, which is the state milestones 0 through 4 ran in.
bool Cards::load_sources(bool verbose) {
  std::filesystem::path path = get_cards_path() / kSourcesFilename;
  std::string error;
  if (!sources.load(path, error)) {
    if (verbose) Zeal::Game::print_chat("Cards of Norrath: no source rules (%s)", error.c_str());
    return false;
  }
  if (verbose)
    Zeal::Game::print_chat("Cards of Norrath: loaded %d source rules from %s",
                           static_cast<int>(sources.rule_count()), kSourcesFilename);
  return true;
}

// ---- collection ----

const Cards::CardDef *Cards::find_by_art(const std::string &art) const {
  if (art.empty()) return nullptr;
  for (const CardDef &card : pool)
    if (card.art == art) return &card;
  return nullptr;
}

// The lookup the kill hook will call once it can catch a death. Kept separate from the hook itself
// so it is testable on its own: card_sources.cpp has no Windows or Direct3D dependency and is
// exercised off-target against the full PQDI scrape.
const Cards::CardDef *Cards::card_for_spawn(int race, int texture, int level, int zone_id,
                                            const std::string &name) const {
  const std::string art = sources.match(race, texture, level, zone_id, name);
  const CardDef *card = find_by_art(art);
  // A rule can name an art hash the pool no longer has (a card deleted in the forge). Treat that
  // as no match rather than crediting nothing-shaped state into the collection.
  if (!card || card->secret) return nullptr;
  return card;
}

const Cards::CardDef *Cards::find_by_dex(int dex) const {
  if (dex < 0) return nullptr;  // Secret cards have no dex and are never addressable by number.
  for (const CardDef &card : pool)
    if (card.dex == dex) return &card;
  return nullptr;
}

int Cards::copy_cap(const CardDef &card) {
  if (card.secret || card.level >= 7) return 1;
  return 10;
}

void Cards::record_kill(const CardDef &card) {
  if (card.secret) return;  // Secret cards are never dropped, so nothing counts toward them.
  collection.record_kill(card.art);
}

bool Cards::grant(const CardDef &card, const std::string &dropped_from) {
  CardCollection::Copy copy;
  copy.north = card.north;
  copy.east = card.east;
  copy.south = card.south;
  copy.west = card.west;
  copy.found = CardCollection::now_string();
  Zeal::GameStructures::Entity *self = Zeal::Game::get_self();
  copy.zone = self ? Zeal::Game::get_full_zone_name(self->ZoneId) : "";
  copy.dropped_from = card.secret ? "" : dropped_from;
  return collection.grant(card.art, copy, copy_cap(card));
}

void Cards::callback_enter_zone() {
  Zeal::GameStructures::GAMECHARINFO *char_info = Zeal::Game::get_char_info();
  const char *name = (char_info && char_info->Name[0]) ? char_info->Name : nullptr;
  if (!name) return;  // Keep whatever is loaded; a later EnterZone will sort it out.
  if (collection.is_loaded() && collection.character() == name) return;  // Same character, new zone.
  collection.load(name);
}

void Cards::callback_init_ui() {
  ZealService *zeal = ZealService::get_instance();

  std::filesystem::path xml_file = UISkin::get_zeal_xml_path() / kXmlFilename;
  if (!wnd && std::filesystem::exists(xml_file) && zeal && zeal->ui) wnd = zeal->ui->CreateSidlScreenWnd(kWindowName);
  if (!wnd) {
    Zeal::Game::print_chat("Cards of Norrath: failed to load %s", xml_file.string().c_str());
  } else {
    auto *vtbl = static_cast<Zeal::GameUI::SidlScreenWndVTable *>(wnd->vtbl);
    vtbl->DrawBackground = DrawBackground;
    vtbl->Deactivate = Deactivate;
    wnd->show(0, false);  // Start hidden. /cards opens it.
  }

  std::filesystem::path battle_xml_file = UISkin::get_zeal_xml_path() / kBattleXmlFilename;
  if (!battle_wnd && std::filesystem::exists(battle_xml_file) && zeal && zeal->ui)
    battle_wnd = zeal->ui->CreateSidlScreenWnd(kBattleWindowName);
  if (!battle_wnd) {
    Zeal::Game::print_chat("Cards of Norrath: failed to load %s", battle_xml_file.string().c_str());
  } else {
    auto *vtbl = static_cast<Zeal::GameUI::SidlScreenWndVTable *>(battle_wnd->vtbl);
    vtbl->DrawBackground = DrawBackground;
    vtbl->Deactivate = Deactivate;
    vtbl->HandleLButtonDown = BattleHandleLButtonDown;
    battle_wnd->show(0, false);  // Start hidden. /cards battle opens it.
  }
}

void Cards::callback_clean_ui() {
  release_resources();
  ZealService *zeal = ZealService::get_instance();
  if (wnd) {
    if (zeal && zeal->ui) zeal->ui->DestroySidlScreenWnd(wnd);
    wnd = nullptr;  // Disables rendering until the next init_ui.
  }
  if (battle_wnd) {
    if (zeal && zeal->ui) zeal->ui->DestroySidlScreenWnd(battle_wnd);
    battle_wnd = nullptr;
  }
}

void Cards::callback_deactivate_ui() {
  if (wnd) Deactivate(wnd, 0);
  if (battle_wnd) Deactivate(battle_wnd, 0);
}

// Loads <art>.tga on first use. A failed load is cached as nullptr so it is reported once.
IDirect3DTexture8 *Cards::get_texture(IDirect3DDevice8 &device, const std::string &art) {
  auto it = textures.find(art);
  if (it != textures.end()) return it->second;

  IDirect3DTexture8 *texture = nullptr;
  std::filesystem::path path = get_cards_path() / std::filesystem::path(art + ".tga");
  if (art.empty() || FAILED(D3DXCreateTextureFromFileA(&device, path.string().c_str(), &texture))) {
    texture = nullptr;
    Zeal::Game::print_chat("Cards of Norrath: no art at %s", path.string().c_str());
  }
  textures[art] = texture;
  return texture;
}

void Cards::release_resources() {
  for (auto &pair : textures)
    if (pair.second) pair.second->Release();
  textures.clear();
  rank_font.reset();
  label_font.reset();
  score_font.reset();
  fonts_failed = false;
}

// Screen-space quad. Pre-transformed vertices, so no matrices are involved. v_bottom lets a
// caller sample only the top slice of the texture (see the header comment); it's ignored for
// untextured quads.
void Cards::draw_quad(IDirect3DDevice8 &device, float left, float top, float right, float bottom, D3DCOLOR color,
                      IDirect3DTexture8 *texture, float v_bottom) {
  struct Vertex {
    float x, y, z, rhw;
    DWORD color;
    float u, v;
  };
  Vertex vertices[] = {{left, top, 0.f, 1.f, color, 0.f, 0.f},
                       {right, top, 0.f, 1.f, color, 1.f, 0.f},
                       {left, bottom, 0.f, 1.f, color, 0.f, v_bottom},
                       {right, bottom, 0.f, 1.f, color, 1.f, v_bottom}};

  // With a texture, modulate it by the vertex colour. Without one, take the vertex colour alone.
  device.SetTexture(0, texture);
  device.SetTextureStageState(0, D3DTSS_COLOROP, texture ? D3DTOP_MODULATE : D3DTOP_SELECTARG2);
  device.SetTextureStageState(0, D3DTSS_ALPHAOP, texture ? D3DTOP_MODULATE : D3DTOP_SELECTARG2);
  device.SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
  device.DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(Vertex));
  if (texture) device.SetTexture(0, NULL);
}

// Same shape as draw_quad, but the top and bottom edges get different vertex colours (including
// alpha) and the D3D8 rasteriser linearly interpolates between them across the quad. No texture:
// used to feather the art into a flat colour at a seam instead of drawing a hard cutoff line.
void Cards::draw_quad_gradient(IDirect3DDevice8 &device, float left, float top, float right, float bottom,
                               D3DCOLOR color_top, D3DCOLOR color_bottom) {
  struct Vertex {
    float x, y, z, rhw;
    DWORD color;
    float u, v;
  };
  Vertex vertices[] = {{left, top, 0.f, 1.f, color_top, 0.f, 0.f},
                       {right, top, 0.f, 1.f, color_top, 1.f, 0.f},
                       {left, bottom, 0.f, 1.f, color_bottom, 0.f, 1.f},
                       {right, bottom, 0.f, 1.f, color_bottom, 1.f, 1.f}};

  device.SetTexture(0, NULL);
  device.SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
  device.SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
  device.SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
  device.DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(Vertex));
}

static const char *rank_text(int rank) {
  static const char *kText[] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "A"};
  if (rank < 0) return "?";
  return rank > 10 ? "A" : kText[rank];
}

// Draws one rank glyph with a thick outline: eight near-black copies at a small pixel offset in
// every direction, then the real white glyph on top. No backing tile needed underneath.
void Cards::queue_outlined_rank(const char *text, const Vec3 &position) {
  if (!rank_font) return;
  static constexpr float t = kRankOutlineThickness;
  static const Vec3 kOffsets[8] = {
      Vec3(-t, -t, 0.f), Vec3(0.f, -t, 0.f), Vec3(t, -t, 0.f), Vec3(-t, 0.f, 0.f),
      Vec3(t, 0.f, 0.f),  Vec3(-t, t, 0.f),  Vec3(0.f, t, 0.f), Vec3(t, t, 0.f),
  };
  for (const Vec3 &offset : kOffsets)
    rank_font->queue_string(text, Vec3(position.x + offset.x, position.y + offset.y, position.z), true,
                             kRankOutlineColor);
  rank_font->queue_string(text, position, true, kWhite);
}

// border_color lets the same drawing code serve the collection window (a neutral border) and the
// battle window (blue for the local side, red for the opponent) without the card data itself ever
// knowing which side it's on. visible_height less than kCardHeight is the FF8-style "peek" for a
// covered hand card: only the border and the start of the art draw, which is enough to show the
// rank diamond (it lives inside that region by construction) and nothing else; no separate peek
// asset or clip rect needed.
void Cards::draw_card(IDirect3DDevice8 &device, const CardDef &card, float left, float top, D3DCOLOR border_color,
                      float visible_height) {
  const bool full = visible_height >= kCardHeight - 0.5f;
  const float right = left + kCardWidth;
  const float bottom = top + (full ? kCardHeight : visible_height);

  draw_quad(device, left, top, right, bottom, border_color);
  draw_quad(device, left + kBorder, top + kBorder, right - kBorder, bottom - (full ? kBorder : 0.f), kCardBackColor);

  IDirect3DTexture8 *texture = get_texture(device, card.art);
  const float art_bottom = full ? (top + kBorder + kArtHeight) : bottom;
  // When cropped, sample only the matching top fraction of the texture (its full height is
  // kArtHeight), so a peeked card shows a true top slice of the art instead of the whole image
  // squeezed into a shorter box.
  const float art_v_bottom = full ? 1.f : max(0.f, min(1.f, (art_bottom - (top + kBorder)) / kArtHeight));
  draw_quad(device, left + kBorder, top + kBorder, right - kBorder, art_bottom, texture ? kWhite : kMissingArtColor,
            texture, art_v_bottom);

  if (full) {
    // Feather the art into the label strip colour at the seam instead of a hard cutoff: a
    // gradient quad over the bottom of the art, transparent at its top edge fading to the label
    // colour (already opaque) by art_bottom, where the label strip itself begins.
    draw_quad_gradient(device, left + kBorder, art_bottom - kFeatherHeight, right - kBorder, art_bottom,
                        kCardBackColorTransparent, kCardBackColor);
  }

  // Rank diamond in the top left of the art. No backing tile; each glyph carries its own outline
  // (queue_outlined_rank) so the numbers read against the art underneath on their own. Drawn even
  // when peeking; that's the entire point of the peek.
  const float rank_left = left + kBorder + kRankInsetX;
  const float rank_top = top + kBorder + kRankInsetY;
  const float rank_size = kRankBacking;  // Spacing reference only; nothing draws this square.

  if (rank_font) {
    const float cx = rank_left + rank_size / 2.f;
    const float cy = rank_top + rank_size / 2.f;
    queue_outlined_rank(rank_text(card.north), Vec3(cx, cy - kRankSpacing, 0.f));
    queue_outlined_rank(rank_text(card.west), Vec3(cx - kRankSpacing, cy, 0.f));
    queue_outlined_rank(rank_text(card.east), Vec3(cx + kRankSpacing, cy, 0.f));
    queue_outlined_rank(rank_text(card.south), Vec3(cx, cy + kRankSpacing, 0.f));
  }

  if (full && label_font) {
    const float label_cx = left + kCardWidth / 2.f;
    const float label_cy = art_bottom + (bottom - kBorder - art_bottom) / 2.f;
    label_font->queue_string(card.name.c_str(), Vec3(label_cx, label_cy - kLabelLineOffset, 0.f), true, kNameColor);
    std::string sub = "LEVEL " + std::to_string(card.level);
    label_font->queue_string(sub.c_str(), Vec3(label_cx, label_cy + kLabelLineOffset, 0.f), true, kLevelColor);
  }
}

bool Cards::ensure_fonts(IDirect3DDevice8 &device) {
  if (rank_font && label_font && score_font) return true;
  if (fonts_failed) return false;

  rank_font = BitmapFont::create_bitmap_font(device, kRankFontName);
  label_font = BitmapFont::create_bitmap_font(device, kLabelFontName);
  score_font = BitmapFont::create_bitmap_font(device, kScoreFontName);
  if (!rank_font || !label_font || !score_font) {
    fonts_failed = true;
    Zeal::Game::print_chat("Cards of Norrath: font load failed (%s, %s, %s)", kRankFontName, kLabelFontName,
                           kScoreFontName);
    return false;
  }
  // Rank numbers draw their own outline (queue_outlined_rank); the label and score fonts use the
  // engine's built-in drop shadow, which is enough for text that isn't sitting on card art.
  label_font->set_drop_shadow(true);
  score_font->set_drop_shadow(true);
  return true;
}

// Paints the collection window's client area: table colour, then the first nine cards of the
// pool as a grid, at full size with the neutral border colour.
void Cards::render_collection(IDirect3DDevice8 &device, const Zeal::GameUI::CXRect &rect) {
  const float left = static_cast<float>(rect.Left);
  const float top = static_cast<float>(rect.Top);
  const float right = static_cast<float>(rect.Right);
  const float bottom = static_cast<float>(rect.Bottom);

  draw_quad(device, left, top, right, bottom, kTableColor);

  const float grid_width = kColumns * kCardWidth + (kColumns - 1) * kGap;
  const float origin_x = left + max(kGap, (right - left - grid_width) / 2.f);
  const float origin_y = top + kGap;
  int slot = 0;
  for (const CardDef &card : pool) {
    if (slot >= kColumns * kRows) break;
    const float card_left = origin_x + (slot % kColumns) * (kCardWidth + kGap);
    const float card_top = origin_y + (slot / kColumns) * (kCardHeight + kGap);
    if (card_top + kCardHeight > bottom) break;  // Clipped by the window; stop rather than spill.
    draw_card(device, card, card_left, card_top, kCardBorderColor, kCardHeight);
    ++slot;
  }
}

// Paints the battle window: table colour, the 3x3 board, both hand stacks (foe on the left in
// red, you on the right in blue, always, regardless of who is actually playing), the two big
// score numerals, and an info bar under the board naming whichever card is currently hovered (or
// the last status line, when nothing is). See claude/state.md for what's ported from the browser
// prototype versus new for this milestone.
void Cards::render_battle(IDirect3DDevice8 &device, const Zeal::GameUI::CXRect &rect) {
  const float left = static_cast<float>(rect.Left);
  const float top = static_cast<float>(rect.Top);
  const float right = static_cast<float>(rect.Right);
  const float bottom = static_cast<float>(rect.Bottom);

  draw_quad(device, left, top, right, bottom, kTableColor);

  if (!battle_started) start_new_battle();

  const float board_width = kColumns * kCardWidth + (kColumns - 1) * kGap;
  const float board_height = kRows * kCardHeight + (kRows - 1) * kGap;
  // A stack is (n-1) peeked cards plus the last card in full, FF8 style: the bottom card is never
  // covered by anything, so there's nothing to peek from under. Both stacks use the larger hand
  // so the two columns stay aligned as cards get played.
  const size_t stack_count = max(hand_you.size(), hand_foe.size());
  const float stack_span = stack_count == 0 ? 0.f : (stack_count - 1) * kHandPeekHeight + kCardHeight;
  // The score numeral sits on top of its stack, then the stack; the whole column is centred on the
  // board vertically but never starts above the window's own margin.
  const float column_span = kScoreBlockHeight + kScoreGap + stack_span;

  const float board_left =
      left + max(kBattleMargin + kCardWidth + kHandBoardGap, (right - left - board_width) / 2.f);
  const float board_top = top + kBattleMargin;
  const float hand_you_left = board_left + board_width + kHandBoardGap;
  const float hand_foe_left = board_left - kHandBoardGap - kCardWidth;
  const float column_top = max(top + 4.f, board_top + (board_height - column_span) / 2.f);
  const float score_cy = column_top + kScoreBlockHeight / 2.f;
  const float hand_top = column_top + kScoreBlockHeight + kScoreGap;

  // ---- hover tween: ease every card's horizontal offset toward its target this frame ----
  const unsigned long now = GetTickCount();
  const unsigned long delta = last_render_tick == 0 ? 0 : min(now - last_render_tick, kMaxFrameDeltaMs);
  last_render_tick = now;
  const float ease = 1.f - expf(-static_cast<float>(delta) / kHoverTweenMs);
  if (hand_you_shift.size() != hand_you.size()) hand_you_shift.assign(hand_you.size(), 0.f);
  if (hand_foe_shift.size() != hand_foe.size()) hand_foe_shift.assign(hand_foe.size(), 0.f);
  for (size_t i = 0; i < hand_you_shift.size(); ++i) {
    const float target = static_cast<int>(i) == hover_you ? kHoverShift : 0.f;
    hand_you_shift[i] += (target - hand_you_shift[i]) * ease;
    if (fabsf(target - hand_you_shift[i]) < 0.25f) hand_you_shift[i] = target;
  }
  for (size_t i = 0; i < hand_foe_shift.size(); ++i) {
    const float target = static_cast<int>(i) == hover_foe ? kHoverShift : 0.f;
    hand_foe_shift[i] += (target - hand_foe_shift[i]) * ease;
    if (fabsf(target - hand_foe_shift[i]) < 0.25f) hand_foe_shift[i] = target;
  }

  // ---- board cells ----
  for (int i = 0; i < 9; ++i) {
    const float cell_left = board_left + (i % kColumns) * (kCardWidth + kGap);
    const float cell_top = board_top + (i / kColumns) * (kCardHeight + kGap);
    board_rects[i] = RECT{static_cast<LONG>(cell_left), static_cast<LONG>(cell_top),
                          static_cast<LONG>(cell_left + kCardWidth), static_cast<LONG>(cell_top + kCardHeight)};

    const BattleCard &cell = board[i];
    if (cell.owner == Owner::None) {
      const bool open_target = !battle_over && turn == Owner::You && !waiting_on_foe && selected_hand_index >= 0;
      draw_quad(device, cell_left, cell_top, cell_left + kCardWidth, cell_top + kCardHeight,
                open_target ? kBoardOpenTargetColor : kBoardCellColor);
    } else {
      draw_card(device, cell.def, cell_left, cell_top, cell.owner == Owner::You ? kBattleYouColor : kBattleFoeColor,
                kCardHeight);
    }
  }

  // ---- hands: stacked and mostly covered, the last card in full, the hovered card drawn last
  // (in front), in full, and shifted toward the board. Rects are sized to the peek slot (or the
  // full card for the last one) regardless of hover state, since that's the fixed hit-test area;
  // only the drawing changes with hover. Cards sliding back after a hover keep their partial
  // shift while peeked, which is what makes the tween read as a slide rather than a snap. ----
  hand_you_rects.assign(hand_you.size(), RECT{});
  for (size_t i = 0; i < hand_you.size(); ++i) {
    const float slot_top = hand_top + i * kHandPeekHeight;
    const bool last = i + 1 == hand_you.size();
    const float slot_height = last ? kCardHeight : kHandPeekHeight;
    hand_you_rects[i] = RECT{static_cast<LONG>(hand_you_left), static_cast<LONG>(slot_top),
                             static_cast<LONG>(hand_you_left + kCardWidth),
                             static_cast<LONG>(slot_top + slot_height)};
    if (static_cast<int>(i) == hover_you) continue;
    const bool picked_up = static_cast<int>(i) == selected_hand_index;
    draw_card(device, hand_you[i].def, hand_you_left - hand_you_shift[i], slot_top,
              picked_up ? kWhite : kBattleYouColor, slot_height);
  }

  hand_foe_rects.assign(hand_foe.size(), RECT{});
  for (size_t i = 0; i < hand_foe.size(); ++i) {
    const float slot_top = hand_top + i * kHandPeekHeight;
    const bool last = i + 1 == hand_foe.size();
    const float slot_height = last ? kCardHeight : kHandPeekHeight;
    hand_foe_rects[i] = RECT{static_cast<LONG>(hand_foe_left), static_cast<LONG>(slot_top),
                             static_cast<LONG>(hand_foe_left + kCardWidth),
                             static_cast<LONG>(slot_top + slot_height)};
    if (static_cast<int>(i) == hover_foe) continue;
    draw_card(device, hand_foe[i].def, hand_foe_left + hand_foe_shift[i], slot_top, kBattleFoeColor, slot_height);
  }

  // Flush the rank-number text queued so far (board cells and every peeked card) to the screen
  // now, before drawing the hovered card. Text is normally queued and flushed once, at the very
  // end of callback_render, but a covered card's queued numbers would then paint on top of
  // *everything* drawn later in the frame, including the hovered card's own quad where it
  // overlaps the peek slot behind it (this is the "old card's numbers show through" bug) since
  // quads are drawn immediately but text is deferred. Committing them here first means the
  // hovered card's quad, drawn next, correctly covers whatever peeked numbers it overlaps.
  // draw_quad always resets the texture/shader state it needs, so this is safe mid-frame.
  if (rank_font) rank_font->flush_queue_to_screen();

  if (hover_you >= 0 && hover_you < static_cast<int>(hand_you.size())) {
    const float slot_top = hand_top + hover_you * kHandPeekHeight;
    const bool picked_up = hover_you == selected_hand_index;
    draw_card(device, hand_you[hover_you].def, hand_you_left - hand_you_shift[hover_you], slot_top,
              picked_up ? kWhite : kBattleYouColor, kCardHeight);
  }
  if (hover_foe >= 0 && hover_foe < static_cast<int>(hand_foe.size())) {
    const float slot_top = hand_top + hover_foe * kHandPeekHeight;
    draw_card(device, hand_foe[hover_foe].def, hand_foe_left + hand_foe_shift[hover_foe], slot_top, kBattleFoeColor,
              kCardHeight);
  }

  // ---- scores: big numerals above each stack (Kevin's call after seeing them below; top keeps
  // them put as the stack shrinks, instead of climbing the column as cards get played). ----
  if (score_font) {
    score_font->queue_string(std::to_string(battle_score(Owner::You)).c_str(),
                             Vec3(hand_you_left + kCardWidth / 2.f, score_cy, 0.f), true, kBattleYouColor);
    score_font->queue_string(std::to_string(battle_score(Owner::Foe)).c_str(),
                             Vec3(hand_foe_left + kCardWidth / 2.f, score_cy, 0.f), true, kBattleFoeColor);
  }

  // ---- info bar: whichever card is hovered or picked up, else the last status line ----
  if (label_font) {
    std::string info = battle_status;
    if (hover_you >= 0 && hover_you < static_cast<int>(hand_you.size()))
      info = hand_you[hover_you].def.name;
    else if (hover_foe >= 0 && hover_foe < static_cast<int>(hand_foe.size()))
      info = hand_foe[hover_foe].def.name;
    else if (selected_hand_index >= 0 && selected_hand_index < static_cast<int>(hand_you.size()))
      info = hand_you[selected_hand_index].def.name;
    const float info_cy = board_top + board_height + kInfoBarGap + kInfoBarHeight / 2.f;
    label_font->queue_string(info.c_str(), Vec3(board_left + board_width / 2.f, info_cy, 0.f), true, kNameColor);
  }
}

// Paints whichever window(s) are visible every frame; both share one device state setup so
// switching between them (or having both open) costs nothing extra.
void Cards::callback_render() {
  const bool show_collection = wnd && wnd->IsVisible && !wnd->IsMinimized;
  const bool show_battle = battle_wnd && battle_wnd->IsVisible && !battle_wnd->IsMinimized;
  if ((!show_collection && !show_battle) || !Zeal::Game::is_in_game()) return;

  ZealService *zeal = ZealService::get_instance();
  IDirect3DDevice8 *device = (zeal && zeal->dx) ? zeal->dx->GetDevice() : nullptr;
  if (!device) return;

  // Size of the whole render target. Ignores the game's /viewport setting on purpose.
  IDirect3DSurface8 *surface = nullptr;
  D3DSURFACE_DESC description = {};
  if (FAILED(device->GetRenderTarget(&surface)) || !surface) return;
  surface->GetDesc(&description);
  surface->Release();

  if (!ensure_fonts(*device)) return;

  if (show_battle) {
    // Hover uses the previous frame's hand rects (the window is a fixed size, so they don't
    // move), and the AI's delayed move is checked here too: both are per-frame housekeeping that
    // belongs before drawing, not inside render_battle itself.
    update_battle_hover(*Zeal::Game::mouse_client_x, *Zeal::Game::mouse_client_y);
    if (waiting_on_foe && GetTickCount() >= foe_move_at_tick) {
      waiting_on_foe = false;
      foe_take_turn();
    }
  }

  // Full-target viewport so pre-transformed pixel coordinates are not clipped by /viewport.
  D3DVIEWPORT8 original_viewport;
  device->GetViewport(&original_viewport);
  D3DVIEWPORT8 viewport = {0, 0, description.Width, description.Height, 0.0f, 1.0f};
  device->SetViewport(&viewport);

  D3DRenderStateStash render_state(*device);
  render_state.store_and_modify({D3DRS_CULLMODE, D3DCULL_NONE});
  render_state.store_and_modify({D3DRS_ALPHABLENDENABLE, TRUE});
  render_state.store_and_modify({D3DRS_SRCBLEND, D3DBLEND_SRCALPHA});
  render_state.store_and_modify({D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA});
  render_state.store_and_modify({D3DRS_ZENABLE, FALSE});
  render_state.store_and_modify({D3DRS_ZWRITEENABLE, FALSE});
  render_state.store_and_modify({D3DRS_LIGHTING, FALSE});
  render_state.store_and_modify({D3DRS_FOGENABLE, FALSE});

  D3DTextureStateStash texture_state(*device);
  texture_state.store_and_modify({D3DTSS_COLOROP, D3DTOP_MODULATE});
  texture_state.store_and_modify({D3DTSS_COLORARG1, D3DTA_TEXTURE});
  texture_state.store_and_modify({D3DTSS_COLORARG2, D3DTA_DIFFUSE});
  texture_state.store_and_modify({D3DTSS_ALPHAOP, D3DTOP_MODULATE});
  texture_state.store_and_modify({D3DTSS_ALPHAARG1, D3DTA_TEXTURE});
  texture_state.store_and_modify({D3DTSS_ALPHAARG2, D3DTA_DIFFUSE});
  texture_state.store_and_modify({D3DTSS_MINFILTER, D3DTEXF_LINEAR});
  texture_state.store_and_modify({D3DTSS_MAGFILTER, D3DTEXF_LINEAR});
  texture_state.store_and_modify({D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP});
  texture_state.store_and_modify({D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP});

  auto clamped_rect = [&](Zeal::GameUI::SidlWnd *w) {
    Zeal::GameUI::CXRect rect;
    Zeal::Game::GameInternal::CXWndGetClientRect(w, 0, &rect);
    rect.Left = max(0, rect.Left);
    rect.Top = max(0, rect.Top);
    rect.Right = min(static_cast<int>(description.Width), rect.Right);
    rect.Bottom = min(static_cast<int>(description.Height), rect.Bottom);
    return rect;
  };

  if (show_collection) {
    Zeal::GameUI::CXRect rect = clamped_rect(wnd);
    if (rect.Right > rect.Left && rect.Bottom > rect.Top) render_collection(*device, rect);
  }
  if (show_battle) {
    Zeal::GameUI::CXRect rect = clamped_rect(battle_wnd);
    if (rect.Right > rect.Left && rect.Bottom > rect.Top) render_battle(*device, rect);
  }

  // Text last: the font flush replaces stream, shader and texture state, which the stashes then restore.
  if (rank_font) rank_font->flush_queue_to_screen();
  if (label_font) label_font->flush_queue_to_screen();
  if (score_font) score_font->flush_queue_to_screen();

  texture_state.restore_state();
  render_state.restore_state();
  device->SetViewport(&original_viewport);
}

// ---- battle rules and AI, ported from the browser prototype (claude/state.md has the details) ----

// Which of the placed card's four ranks faces a given neighbour, and which of the neighbour's
// ranks faces back. 0=north, 1=east, 2=south, 3=west, matching CardDef's own fields.
static int rank_in_direction(const Cards::CardDef &c, int direction) {
  switch (direction) {
    case 0:
      return c.north;
    case 1:
      return c.east;
    case 2:
      return c.south;
    default:
      return c.west;
  }
}

std::vector<int> Cards::resolve_flips(int idx) const {
  std::vector<int> flipped;
  const BattleCard &placed = board[idx];

  struct Dir {
    int delta;
    bool valid;
    int mine, theirs;
  };
  const Dir dirs[4] = {
      {-kColumns, idx >= kColumns, 0, 2},                     // North neighbour: my north vs their south.
      {1, idx % kColumns != kColumns - 1, 1, 3},               // East neighbour: my east vs their west.
      {kColumns, idx < kColumns * (kRows - 1), 2, 0},          // South neighbour: my south vs their north.
      {-1, idx % kColumns != 0, 3, 1},                         // West neighbour: my west vs their east.
  };
  for (const Dir &d : dirs) {
    if (!d.valid) continue;
    const int at = idx + d.delta;
    const BattleCard &other = board[at];
    if (other.owner == Owner::None || other.owner == placed.owner) continue;
    if (rank_in_direction(placed.def, d.mine) > rank_in_direction(other.def, d.theirs)) flipped.push_back(at);
  }
  return flipped;
}

std::vector<int> Cards::place_card(int idx, BattleCard card) {
  board[idx] = card;
  std::vector<int> flipped = resolve_flips(idx);
  for (int at : flipped) board[at].owner = card.owner;
  return flipped;
}

int Cards::battle_score(Owner owner) const {
  int score = static_cast<int>((owner == Owner::You ? hand_you : hand_foe).size());
  for (const auto &cell : board)
    if (cell.owner == owner) ++score;
  return score;
}

void Cards::start_new_battle() {
  battle_started = true;
  board = {};
  hand_you.clear();
  hand_foe.clear();
  selected_hand_index = -1;
  hover_you = -1;
  hover_foe = -1;
  hand_you_shift.clear();
  hand_foe_shift.clear();
  battle_over = false;
  waiting_on_foe = false;

  if (pool.empty()) {
    battle_status = "No cards loaded. Try /cards reload once pool.json has some.";
    return;
  }

  static thread_local std::mt19937 rng{std::random_device{}()};
  std::vector<CardDef> shuffled = pool;
  std::shuffle(shuffled.begin(), shuffled.end(), rng);
  while (shuffled.size() < 10) {  // Small pools: reshuffle in another copy rather than repeat a run.
    std::vector<CardDef> extra = pool;
    std::shuffle(extra.begin(), extra.end(), rng);
    shuffled.insert(shuffled.end(), extra.begin(), extra.end());
  }

  for (int i = 0; i < 5; ++i) hand_you.push_back({shuffled[i], Owner::None});
  for (int i = 0; i < 5; ++i) hand_foe.push_back({shuffled[5 + i], Owner::None});

  turn = (std::uniform_int_distribution<int>(0, 1)(rng) == 0) ? Owner::You : Owner::Foe;
  if (turn == Owner::You) {
    battle_status = "Your turn. Pick a card, then a square.";
  } else {
    battle_status = "Addled goes first.";
    waiting_on_foe = true;
    foe_move_at_tick = GetTickCount() + kFoeMoveDelayMs;
  }
}

void Cards::play_hand_card(int hand_index, int board_index) {
  if (battle_over || waiting_on_foe || turn != Owner::You) return;
  if (hand_index < 0 || hand_index >= static_cast<int>(hand_you.size())) return;
  if (board_index < 0 || board_index >= 9 || board[board_index].owner != Owner::None) return;

  BattleCard card = hand_you[hand_index];
  card.owner = Owner::You;
  hand_you.erase(hand_you.begin() + hand_index);
  selected_hand_index = -1;

  std::vector<int> flipped = place_card(board_index, card);
  battle_status =
      "You played " + card.def.name + (flipped.empty() ? "." : ", taking " + std::to_string(flipped.size()) + ".");

  bool full = true;
  for (const auto &cell : board)
    if (cell.owner == Owner::None) { full = false; break; }
  if (full || hand_foe.empty()) {
    finish_battle();
    return;
  }
  turn = Owner::Foe;
  waiting_on_foe = true;
  foe_move_at_tick = GetTickCount() + kFoeMoveDelayMs;
}

void Cards::foe_take_turn() {
  if (battle_over || turn != Owner::Foe) return;

  std::vector<int> empties;
  for (int i = 0; i < 9; ++i)
    if (board[i].owner == Owner::None) empties.push_back(i);
  if (empties.empty() || hand_foe.empty()) {
    finish_battle();
    return;
  }

  static thread_local std::mt19937 rng{std::random_device{}()};
  const int hand_index = std::uniform_int_distribution<int>(0, static_cast<int>(hand_foe.size()) - 1)(rng);
  const int board_index = empties[std::uniform_int_distribution<int>(0, static_cast<int>(empties.size()) - 1)(rng)];

  BattleCard card = hand_foe[hand_index];
  card.owner = Owner::Foe;
  hand_foe.erase(hand_foe.begin() + hand_index);

  std::vector<int> flipped = place_card(board_index, card);
  battle_status = "Addled played " + card.def.name +
                  (flipped.empty() ? "." : ", taking " + std::to_string(flipped.size()) + ".");

  bool full = true;
  for (const auto &cell : board)
    if (cell.owner == Owner::None) { full = false; break; }
  if (full || hand_you.empty()) {
    finish_battle();
    return;
  }
  turn = Owner::You;
}

void Cards::finish_battle() {
  battle_over = true;
  const int you = battle_score(Owner::You);
  const int foe = battle_score(Owner::Foe);
  if (you > foe)
    battle_status = "You win, " + std::to_string(you) + " to " + std::to_string(foe) + ". Click to play again.";
  else if (foe > you)
    battle_status = "Addled wins, " + std::to_string(foe) + " to " + std::to_string(you) + ". Click to play again.";
  else
    battle_status = "Draw at " + std::to_string(you) + " each. Click to play again.";
}

void Cards::update_battle_hover(int32_t mouse_x, int32_t mouse_y) {
  hover_you = -1;
  hover_foe = -1;
  for (size_t i = 0; i < hand_you_rects.size(); ++i)
    if (point_in_rect(mouse_x, mouse_y, hand_you_rects[i])) hover_you = static_cast<int>(i);
  for (size_t i = 0; i < hand_foe_rects.size(); ++i)
    if (point_in_rect(mouse_x, mouse_y, hand_foe_rects[i])) hover_foe = static_cast<int>(i);
}

void Cards::handle_battle_click(int32_t mouse_x, int32_t mouse_y) {
  if (!battle_wnd || !battle_wnd->IsVisible) return;

  if (battle_over) {
    start_new_battle();
    return;
  }
  if (turn != Owner::You || waiting_on_foe) return;

  for (size_t i = 0; i < hand_you_rects.size(); ++i) {
    if (point_in_rect(mouse_x, mouse_y, hand_you_rects[i])) {
      selected_hand_index = (selected_hand_index == static_cast<int>(i)) ? -1 : static_cast<int>(i);
      return;
    }
  }
  if (selected_hand_index < 0) return;
  for (int i = 0; i < 9; ++i) {
    if (board[i].owner == Owner::None && point_in_rect(mouse_x, mouse_y, board_rects[i])) {
      play_hand_card(selected_hand_index, i);
      return;
    }
  }
}

Cards::Cards(ZealService *zeal) {
  if (!Zeal::Game::is_new_ui()) return;  // Old UI not supported, same as the map.

  // queue_chat_message defers the print until the UI is up and the character is in game.
  zeal->queue_chat_message(std::string("Cards of Norrath ") + kVersion +
                           " loaded. /cards to open, /cards battle for the board.");

  zeal->commands_hook->Add("/cards", {}, "Cards of Norrath card game (toggles the window)",
                           [this](const std::vector<std::string> &args) { return parse_command(args); });

  zeal->callbacks->AddGeneric([this]() { callback_init_ui(); }, callback_type::InitUI);
  zeal->callbacks->AddGeneric([this]() { callback_clean_ui(); }, callback_type::CleanUI);
  zeal->callbacks->AddGeneric([this]() { callback_deactivate_ui(); }, callback_type::DeactivateUI);
  zeal->callbacks->AddGeneric([this]() { callback_enter_zone(); }, callback_type::EnterZone);
  // Every mutation already saves; this is the belt to that suspenders on the way out of a zone.
  zeal->callbacks->AddGeneric([this]() { collection.save_if_dirty(); }, callback_type::EndMainLoop);
  zeal->callbacks->AddGeneric([this]() { callback_render(); }, callback_type::RenderUI);
  zeal->callbacks->AddGeneric([this]() { release_resources(); }, callback_type::DXReset);
  zeal->callbacks->AddGeneric([this]() { release_resources(); }, callback_type::DXCleanDevice);
}

Cards::~Cards() {
  collection.unload();  // Saves if anything is unsaved.
  release_resources();
}
