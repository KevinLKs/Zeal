#include "cards.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "bitmap_font.h"
#include "callbacks.h"
#include "commands.h"
#include "game_functions.h"
#include "json.hpp"
#include "ui_manager.h"
#include "ui_skin.h"
#include "zeal.h"

// Layout of the hand display inside the 420x560 window (client area is shorter by the title bar).
static constexpr int kColumns = 3;
static constexpr int kRows = 3;
static constexpr float kGap = 10.f;
static constexpr float kCardWidth = 120.f;
static constexpr float kCardHeight = 160.f;
static constexpr float kArtHeight = 120.f;  // Square art at the top, label strip below.
static constexpr float kBorder = 2.f;
static constexpr float kRankInset = 6.f;    // Diamond of ranks in the top left of the art.
static constexpr float kRankSpacing = 14.f;
static constexpr float kRankBacking = 48.f;  // Square behind the diamond.

static constexpr D3DCOLOR kTableColor = D3DCOLOR_ARGB(230, 24, 48, 32);
static constexpr D3DCOLOR kCardBorderColor = D3DCOLOR_ARGB(255, 196, 172, 110);
static constexpr D3DCOLOR kCardBackColor = D3DCOLOR_ARGB(255, 28, 24, 20);
static constexpr D3DCOLOR kMissingArtColor = D3DCOLOR_ARGB(255, 70, 60, 80);
static constexpr D3DCOLOR kRankBackingColor = D3DCOLOR_ARGB(150, 0, 0, 0);
static constexpr D3DCOLOR kWhite = D3DCOLOR_ARGB(255, 255, 255, 255);
static constexpr D3DCOLOR kLabelColor = D3DCOLOR_ARGB(255, 225, 215, 190);

static std::filesystem::path get_cards_path() {
  return UISkin::get_zeal_resources_path() / std::filesystem::path(Cards::kCardsSubDirectory);
}

// SidlScreenWnd vtable overrides. Same shapes as the map window in zone_map.cpp.

// Draw nothing behind the client area so the RenderUI callback owns every pixel inside the frame.
static int __fastcall DrawBackground(Zeal::GameUI::SidlWnd *wnd, int unusedEDX) { return 0; }

// The client does not call this for windows outside its own list; Zeal calls it on zone exit.
static void __fastcall Deactivate(Zeal::GameUI::SidlWnd *wnd, int unusedEDX) { wnd->show(0, false); }

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

bool Cards::parse_command(const std::vector<std::string> &args) {
  if (args.size() > 1) {
    if (args[1] == "version") {
      Zeal::Game::print_chat("Cards of Norrath %s", kVersion);
      return true;
    }
    if (args[1] == "reload") {
      release_resources();  // Drop textures and fonts so edited art is picked up too.
      if (load_pool()) Zeal::Game::print_chat("Cards of Norrath: reloaded %d cards", static_cast<int>(pool.size()));
      return true;
    }
    Zeal::Game::print_chat("Usage: /cards [version | reload]");
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
  if (verbose) Zeal::Game::print_chat("Cards of Norrath: loaded %d cards from %s", static_cast<int>(pool.size()),
                                      kPoolFilename);
  return true;
}

void Cards::callback_init_ui() {
  std::filesystem::path xml_file = UISkin::get_zeal_xml_path() / kXmlFilename;
  ZealService *zeal = ZealService::get_instance();
  if (!wnd && std::filesystem::exists(xml_file) && zeal && zeal->ui) wnd = zeal->ui->CreateSidlScreenWnd(kWindowName);

  if (!wnd) {
    Zeal::Game::print_chat("Cards of Norrath: failed to load %s", xml_file.string().c_str());
    return;
  }

  auto *vtbl = static_cast<Zeal::GameUI::SidlScreenWndVTable *>(wnd->vtbl);
  vtbl->DrawBackground = DrawBackground;
  vtbl->Deactivate = Deactivate;

  wnd->show(0, false);  // Start hidden. /cards opens it.
}

void Cards::callback_clean_ui() {
  release_resources();
  if (!wnd) return;
  ZealService *zeal = ZealService::get_instance();
  if (zeal && zeal->ui) zeal->ui->DestroySidlScreenWnd(wnd);
  wnd = nullptr;  // Disables rendering until the next init_ui.
}

void Cards::callback_deactivate_ui() {
  if (wnd) Deactivate(wnd, 0);
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
  fonts_failed = false;
}

// Screen-space quad. Pre-transformed vertices, so no matrices are involved.
void Cards::draw_quad(IDirect3DDevice8 &device, float left, float top, float right, float bottom, D3DCOLOR color,
                      IDirect3DTexture8 *texture) {
  struct Vertex {
    float x, y, z, rhw;
    DWORD color;
    float u, v;
  };
  Vertex vertices[] = {{left, top, 0.f, 1.f, color, 0.f, 0.f},
                       {right, top, 0.f, 1.f, color, 1.f, 0.f},
                       {left, bottom, 0.f, 1.f, color, 0.f, 1.f},
                       {right, bottom, 0.f, 1.f, color, 1.f, 1.f}};

  // With a texture, modulate it by the vertex colour. Without one, take the vertex colour alone.
  device.SetTexture(0, texture);
  device.SetTextureStageState(0, D3DTSS_COLOROP, texture ? D3DTOP_MODULATE : D3DTOP_SELECTARG2);
  device.SetTextureStageState(0, D3DTSS_ALPHAOP, texture ? D3DTOP_MODULATE : D3DTOP_SELECTARG2);
  device.SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
  device.DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(Vertex));
  if (texture) device.SetTexture(0, NULL);
}

static const char *rank_text(int rank) {
  static const char *kText[] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "A"};
  if (rank < 0) return "?";
  return rank > 10 ? "A" : kText[rank];
}

void Cards::draw_card(IDirect3DDevice8 &device, const CardDef &card, float left, float top) {
  const float right = left + kCardWidth;
  const float bottom = top + kCardHeight;

  draw_quad(device, left, top, right, bottom, kCardBorderColor);
  draw_quad(device, left + kBorder, top + kBorder, right - kBorder, bottom - kBorder, kCardBackColor);

  IDirect3DTexture8 *texture = get_texture(device, card.art);
  const float art_bottom = top + kBorder + kArtHeight;
  draw_quad(device, left + kBorder, top + kBorder, right - kBorder, art_bottom, texture ? kWhite : kMissingArtColor,
            texture);

  // Rank diamond backing in the top left of the art.
  const float rank_left = left + kBorder + kRankInset;
  const float rank_top = top + kBorder + kRankInset;
  const float rank_size = kRankBacking;
  draw_quad(device, rank_left, rank_top, rank_left + rank_size, rank_top + rank_size, kRankBackingColor);

  if (rank_font) {
    const float cx = rank_left + rank_size / 2.f;
    const float cy = rank_top + rank_size / 2.f;
    rank_font->queue_string(rank_text(card.north), Vec3(cx, cy - kRankSpacing, 0.f), true, kWhite);
    rank_font->queue_string(rank_text(card.west), Vec3(cx - kRankSpacing, cy, 0.f), true, kWhite);
    rank_font->queue_string(rank_text(card.east), Vec3(cx + kRankSpacing, cy, 0.f), true, kWhite);
    rank_font->queue_string(rank_text(card.south), Vec3(cx, cy + kRankSpacing, 0.f), true, kWhite);
  }

  if (label_font) {
    const float label_cx = left + kCardWidth / 2.f;
    const float label_cy = art_bottom + (bottom - kBorder - art_bottom) / 2.f;
    label_font->queue_string(card.name.c_str(), Vec3(label_cx, label_cy - 7.f, 0.f), true, kLabelColor);
    std::string sub = card.dex >= 0 ? ("#" + std::to_string(card.dex) + "  L" + std::to_string(card.level))
                                    : ("secret  L" + std::to_string(card.level));
    label_font->queue_string(sub.c_str(), Vec3(label_cx, label_cy + 7.f, 0.f), true, kLabelColor);
  }
}

// Paints the window's client area: table colour, then the first nine cards of the pool as a grid.
void Cards::callback_render() {
  if (!wnd || !wnd->IsVisible || wnd->IsMinimized || !Zeal::Game::is_in_game()) return;
  ZealService *zeal = ZealService::get_instance();
  IDirect3DDevice8 *device = (zeal && zeal->dx) ? zeal->dx->GetDevice() : nullptr;
  if (!device) return;

  // Size of the whole render target. Ignores the game's /viewport setting on purpose.
  IDirect3DSurface8 *surface = nullptr;
  D3DSURFACE_DESC description = {};
  if (FAILED(device->GetRenderTarget(&surface)) || !surface) return;
  surface->GetDesc(&description);
  surface->Release();

  // Client rect in screen pixels, clamped to the render target.
  Zeal::GameUI::CXRect rect;
  Zeal::Game::GameInternal::CXWndGetClientRect(wnd, 0, &rect);
  float left = static_cast<float>(max(0, rect.Left));
  float top = static_cast<float>(max(0, rect.Top));
  float right = static_cast<float>(min(static_cast<int>(description.Width), rect.Right));
  float bottom = static_cast<float>(min(static_cast<int>(description.Height), rect.Bottom));
  if (right <= left || bottom <= top) return;

  if (!rank_font && !label_font && !fonts_failed) {
    rank_font = BitmapFont::create_bitmap_font(*device, kRankFontName);
    label_font = BitmapFont::create_bitmap_font(*device, kLabelFontName);
    if (!rank_font || !label_font) {
      fonts_failed = true;
      Zeal::Game::print_chat("Cards of Norrath: font load failed (%s, %s)", kRankFontName, kLabelFontName);
    } else {
      rank_font->set_drop_shadow(true);
      label_font->set_drop_shadow(true);
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

  draw_quad(*device, left, top, right, bottom, kTableColor);

  // Grid of cards, centred horizontally in the client area.
  const float grid_width = kColumns * kCardWidth + (kColumns - 1) * kGap;
  const float origin_x = left + max(kGap, (right - left - grid_width) / 2.f);
  const float origin_y = top + kGap;
  int slot = 0;
  for (const CardDef &card : pool) {
    if (slot >= kColumns * kRows) break;
    const float card_left = origin_x + (slot % kColumns) * (kCardWidth + kGap);
    const float card_top = origin_y + (slot / kColumns) * (kCardHeight + kGap);
    if (card_top + kCardHeight > bottom) break;  // Clipped by the window; stop rather than spill.
    draw_card(*device, card, card_left, card_top);
    ++slot;
  }

  // Text last: the font flush replaces stream, shader and texture state, which the stashes then restore.
  if (rank_font) rank_font->flush_queue_to_screen();
  if (label_font) label_font->flush_queue_to_screen();

  texture_state.restore_state();
  render_state.restore_state();
  device->SetViewport(&original_viewport);
}

Cards::Cards(ZealService *zeal) {
  if (!Zeal::Game::is_new_ui()) return;  // Old UI not supported, same as the map.

  // queue_chat_message defers the print until the UI is up and the character is in game.
  zeal->queue_chat_message(std::string("Cards of Norrath ") + kVersion + " loaded. /cards to open.");

  zeal->commands_hook->Add("/cards", {}, "Cards of Norrath card game (toggles the window)",
                           [this](const std::vector<std::string> &args) { return parse_command(args); });

  zeal->callbacks->AddGeneric([this]() { callback_init_ui(); }, callback_type::InitUI);
  zeal->callbacks->AddGeneric([this]() { callback_clean_ui(); }, callback_type::CleanUI);
  zeal->callbacks->AddGeneric([this]() { callback_deactivate_ui(); }, callback_type::DeactivateUI);
  zeal->callbacks->AddGeneric([this]() { callback_render(); }, callback_type::RenderUI);
  zeal->callbacks->AddGeneric([this]() { release_resources(); }, callback_type::DXReset);
  zeal->callbacks->AddGeneric([this]() { release_resources(); }, callback_type::DXCleanDevice);
}

Cards::~Cards() { release_resources(); }
