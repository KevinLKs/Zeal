#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "directx.h"
#include "game_ui.h"

// Cards of Norrath: an EQ-themed Triple Triad card game.
//
// Milestone 2: the card pool is read from uifiles/zeal/cards/pool.json (the forge export),
// card art is loaded on demand from uifiles/zeal/cards/<art>.tga through D3DX with a small
// cache, and the cards are drawn inside the window as textured quads with their four ranks
// in a bitmap font. No board and no rules yet.
class Cards {
 public:
  static constexpr char kVersion[] = "0.2.0";
  static constexpr char kWindowName[] = "ZealCards";  // Screen item name in the xml.
  static constexpr char kXmlFilename[] = "EQUI_ZealCards.xml";
  static constexpr char kCardsSubDirectory[] = "cards";  // Within the zeal ui skin path.
  static constexpr char kPoolFilename[] = "pool.json";
  static constexpr char kRankFontName[] = "arial_bold_20";
  static constexpr char kLabelFontName[] = "arial_10";

  // One authored card as exported by the forge. Level 0 is the secret tier and is a real value.
  struct CardDef {
    int dex = -1;  // -1 when the export has null (secret cards have no dex number).
    std::string expansion;
    std::string name;
    int level = 0;
    bool secret = false;
    std::string element;  // Empty when null.
    int north = 0, east = 0, south = 0, west = 0;
    std::string art;  // Hashed texture stem, no extension.
    std::string flair;
  };

  Cards(class ZealService *zeal);
  ~Cards();

  bool is_window_visible() const;
  void show_window();
  void hide_window();
  void toggle_window();

  // Reads pool.json. Returns false and leaves the previous pool in place on failure.
  bool load_pool(bool verbose = true);

 private:
  bool parse_command(const std::vector<std::string> &args);

  // Window lifecycle. The window is not in the client's own list, so Zeal callbacks drive it.
  void callback_init_ui();        // Creates the window from xml after the client UI is built.
  void callback_clean_ui();       // Destroys the window before the client tears its UI down.
  void callback_deactivate_ui();  // Hides on zone exit.
  void callback_render();         // Paints the client area every frame it is visible.

  // Direct3D resources. Released on device reset and clean up, re-created on demand.
  IDirect3DTexture8 *get_texture(IDirect3DDevice8 &device, const std::string &art);
  void release_resources();
  void draw_quad(IDirect3DDevice8 &device, float left, float top, float right, float bottom, D3DCOLOR color,
                 IDirect3DTexture8 *texture = nullptr);
  void draw_card(IDirect3DDevice8 &device, const CardDef &card, float left, float top);

  Zeal::GameUI::SidlWnd *wnd = nullptr;
  std::vector<CardDef> pool;
  bool pool_loaded = false;
  std::unordered_map<std::string, IDirect3DTexture8 *> textures;  // nullptr entries cache failures.
  std::unique_ptr<class BitmapFont> rank_font;
  std::unique_ptr<class BitmapFont> label_font;
  bool fonts_failed = false;  // Stops retrying font creation every frame.
};
