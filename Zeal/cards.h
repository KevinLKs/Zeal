#pragma once

#include <string>
#include <vector>

#include "game_ui.h"

// Cards of Norrath: an EQ-themed Triple Triad card game.
//
// Milestone 1: a native SIDL window (EQUI_ZealCards.xml) that /cards opens and closes,
// with its client area painted by Direct3D in the RenderUI callback. The window frame,
// title bar, close box, drag and z-order come from the client; everything inside the
// frame is ours. This is the same split the Zeal map window uses.
class Cards {
 public:
  static constexpr char kVersion[] = "0.1.0";
  static constexpr char kWindowName[] = "ZealCards";  // Screen item name in the xml.
  static constexpr char kXmlFilename[] = "EQUI_ZealCards.xml";

  Cards(class ZealService *zeal);
  ~Cards() {};

  bool is_window_visible() const;
  void show_window();
  void hide_window();
  void toggle_window();

 private:
  bool parse_command(const std::vector<std::string> &args);

  // Window lifecycle. The window is not in the client's own list, so Zeal callbacks drive it.
  void callback_init_ui();        // Creates the window from xml after the client UI is built.
  void callback_clean_ui();       // Destroys the window before the client tears its UI down.
  void callback_deactivate_ui();  // Hides on zone exit.
  void callback_render();         // Paints the client area every frame it is visible.

  Zeal::GameUI::SidlWnd *wnd = nullptr;
};
