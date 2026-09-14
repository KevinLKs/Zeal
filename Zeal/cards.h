#pragma once

#include <array>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "card_collection.h"
#include "card_sources.h"
#include "directx.h"
#include "game_ui.h"

struct Vec3;  // vectors.h; only referenced by pointer/reference here.

// Cards of Norrath: an EQ-themed Triple Triad card game.
//
// Milestone 3: a second window, the battle board, alongside the milestone 1/2 collection window.
// A 3x3 grid, five-card hands dealt from the pool, click a hand card then click an open cell to
// place it, basic flip rule only, against the Addled (uniform random) AI. Hands are drawn FF8
// style: stacked and mostly covered, showing only the rank diamond (already in the art's top
// left corner, so no new art is needed) until hovered, when the card draws in full and shifts
// toward the board. Ownership colour (blue for the local side, red for the opponent) is a
// render-time decision, never stored on the card, so it still means the right thing once this is
// ever played PvP. No picking/deck-selection screen yet: hands are just dealt, like the browser
// prototype this was ported from.
class Cards {
 public:
  static constexpr char kVersion[] = "0.4.0";
  static constexpr char kWindowName[] = "ZealCards";  // Screen item name in the xml.
  static constexpr char kXmlFilename[] = "EQUI_ZealCards.xml";
  static constexpr char kBattleWindowName[] = "ZealCardsBattle";
  static constexpr char kBattleXmlFilename[] = "EQUI_ZealBattle.xml";
  static constexpr char kCardsSubDirectory[] = "cards";  // Within the zeal ui skin path.
  static constexpr char kPoolFilename[] = "pool.json";
  static constexpr char kSourcesFilename[] = "sources.json";
  static constexpr char kRankFontName[] = "arial_bold_20";
  static constexpr char kLabelFontName[] = "arial_10";
  static constexpr char kScoreFontName[] = "arial_bold_36";

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

  enum class Owner { None, You, Foe };

  // One card in play: a copy of its pool definition (not a pointer into `pool`) plus who owns it
  // right now, so a `/cards reload` mid-match cannot dangle a hand or board cell.
  struct BattleCard {
    CardDef def;
    Owner owner = Owner::None;
  };

  Cards(class ZealService *zeal);
  ~Cards();

  bool is_window_visible() const;
  void show_window();
  void hide_window();
  void toggle_window();

  bool is_battle_window_visible() const;
  void show_battle_window();
  void hide_battle_window();
  void toggle_battle_window();

  // Reads pool.json. Returns false and leaves the previous pool in place on failure.
  bool load_pool(bool verbose = true);

  // Reads sources.json, the kill-to-card matching rules. Optional: a missing file just means no
  // kill can be credited yet, which is exactly the state before the rules were authored. Called
  // by load_pool, so /cards reload picks up edits to both.
  bool load_sources(bool verbose = true);
  const CardSources &get_sources() const { return sources; }

  // Which card, if any, a dead creature belongs to. Every argument is readable off the client's
  // Entity at death; `name` should already be through Zeal::Game::trim_name. nullptr for no match,
  // which is the common case and never an error. See claude/kill-hook-matching.md.
  const CardDef *card_for_spawn(int race, int texture, int level, int zone_id, const std::string &name) const;
  const CardDef *find_by_art(const std::string &art) const;  // nullptr if no card has that art hash.

  // The per-character collection (held copies, kill counts, acquisition records). Loaded on
  // EnterZone for whichever character is playing; read-only from outside, mutate through the
  // methods below so the cap and the record fields are filled in one place.
  const CardCollection &get_collection() const { return collection; }
  const CardDef *find_by_dex(int dex) const;  // nullptr if no card has that dex number.
  // Copies a character may hold of this card: ten below level 7, one at 7 and above and for the
  // secret tier (awards). Rare cards will be one regardless once the pool carries a rare flag.
  static int copy_cap(const CardDef &card);
  // A creature carrying `card` died with this character credited. Flips unknown to encountered.
  void record_kill(const CardDef &card);
  // Adds a copy with the acquisition record filled from the current zone and time. `dropped_from`
  // is the dead creature's name, or empty for an award. Returns false at the cap. The copy's ranks
  // are the authored base for now; the below-level-7 roll is still an open design item and this
  // is the one place it will plug in.
  bool grant(const CardDef &card, const std::string &dropped_from);

  // Entry points for the battle window's vtable overrides (see cards.cpp). Public only because
  // the override functions are free functions outside the class, same shape as ZoneMap's.
  void handle_battle_click(int32_t mouse_x, int32_t mouse_y);

 private:
  bool parse_command(const std::vector<std::string> &args);

  // Window lifecycle. The windows are not in the client's own list, so Zeal callbacks drive them.
  void callback_init_ui();        // Creates both windows from xml after the client UI is built.
  void callback_clean_ui();       // Destroys the windows before the client tears its UI down.
  void callback_deactivate_ui();  // Hides both on zone exit.
  void callback_enter_zone();     // Loads the collection for the character entering the zone.
  void callback_render();         // Paints whichever window(s) are visible every frame.

  bool ensure_fonts(IDirect3DDevice8 &device);  // Creates all three fonts once, shared by both windows.
  void render_collection(IDirect3DDevice8 &device, const Zeal::GameUI::CXRect &rect);
  void render_battle(IDirect3DDevice8 &device, const Zeal::GameUI::CXRect &rect);

  // Direct3D resources. Released on device reset and clean up, re-created on demand.
  IDirect3DTexture8 *get_texture(IDirect3DDevice8 &device, const std::string &art);
  void release_resources();
  // v_bottom is the texture V coordinate sampled at the quad's bottom edge (default 1, the whole
  // texture). A cropped card (visible_height < full height) needs a matching crop of the art
  // texture too, or the sampler stretches the whole image into the shorter quad instead of
  // showing a true top slice of it.
  void draw_quad(IDirect3DDevice8 &device, float left, float top, float right, float bottom, D3DCOLOR color,
                 IDirect3DTexture8 *texture = nullptr, float v_bottom = 1.f);
  // Same shape as draw_quad but a top/bottom vertex-colour gradient (D3D8 interpolates it across
  // the quad), no texture. Used to feather the art into a flat colour instead of a hard cutoff.
  void draw_quad_gradient(IDirect3DDevice8 &device, float left, float top, float right, float bottom,
                          D3DCOLOR color_top, D3DCOLOR color_bottom);
  // border_color overrides the neutral collection-window border (battle mode passes a blue/red
  // tint so ownership reads as colour, never as data on the card). visible_height, when less than
  // the full card height, clips the card to just its top (border, start of the art, rank diamond)
  // and skips the label strip: the FF8-style "peek" for a covered hand card is this, not a
  // separate drawing path.
  void draw_card(IDirect3DDevice8 &device, const CardDef &card, float left, float top, D3DCOLOR border_color,
                 float visible_height);
  // Draws one rank glyph outlined (eight offset dark copies, then the real glyph), no backing tile.
  void queue_outlined_rank(const char *text, const Vec3 &position);

  // ---- battle rules and AI (ported from the browser prototype; see claude/state.md) ----
  void start_new_battle();                        // Shuffles the pool, deals two hands, picks who is first.
  std::vector<int> resolve_flips(int idx) const;  // Indices flipped by the card just placed at idx.
  // Places the card, resolves flips, and returns the indices it took (for the status line).
  std::vector<int> place_card(int idx, BattleCard card);
  void play_hand_card(int hand_index, int board_index);
  void foe_take_turn();  // Addled AI: uniform random empty cell, uniform random hand card.
  void finish_battle();
  int battle_score(Owner owner) const;
  void update_battle_hover(int32_t mouse_x, int32_t mouse_y);

  Zeal::GameUI::SidlWnd *wnd = nullptr;
  Zeal::GameUI::SidlWnd *battle_wnd = nullptr;
  std::vector<CardDef> pool;
  bool pool_loaded = false;
  CardSources sources;  // Kill matching rules, keyed by card art hash.
  CardCollection collection;
  std::unordered_map<std::string, IDirect3DTexture8 *> textures;  // nullptr entries cache failures.
  std::unique_ptr<class BitmapFont> rank_font;
  std::unique_ptr<class BitmapFont> label_font;
  std::unique_ptr<class BitmapFont> score_font;
  bool fonts_failed = false;  // Stops retrying font creation every frame.

  // Battle match state, reset by start_new_battle(). board_rects/hand_you_rects/hand_foe_rects
  // are recomputed every frame by render_battle() against the current client rect and reused
  // as-is by the click handler and the hover poll; the window does not resize, so this is safe.
  std::array<BattleCard, 9> board{};
  std::vector<BattleCard> hand_you;
  std::vector<BattleCard> hand_foe;
  int selected_hand_index = -1;  // Index into hand_you currently picked up, or -1.
  int hover_you = -1;            // Hand index under the mouse this frame, or -1.
  int hover_foe = -1;
  Owner turn = Owner::None;
  bool battle_started = false;  // False until the window has dealt its first hand.
  bool battle_over = false;
  bool waiting_on_foe = false;
  unsigned long foe_move_at_tick = 0;  // GetTickCount() deadline; mirrors the prototype's 620ms pause.
  std::string battle_status;           // One line under the board: last move, or the result.

  std::array<RECT, 9> board_rects{};
  std::vector<RECT> hand_you_rects;  // Index-aligned with hand_you.
  std::vector<RECT> hand_foe_rects;  // Index-aligned with hand_foe.

  // Hover tween state: each hand card's current horizontal offset toward the board, eased every
  // frame toward kHoverShift (hovered) or 0. Index-aligned with the hands; reset whenever a hand
  // changes size (a card played) or a new battle starts, which is a snap nobody will notice.
  std::vector<float> hand_you_shift;
  std::vector<float> hand_foe_shift;
  unsigned long last_render_tick = 0;  // GetTickCount() of the previous battle frame, 0 before the first.
};
