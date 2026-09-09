#include "cards.h"

#include <filesystem>

#include "callbacks.h"
#include "commands.h"
#include "directx.h"
#include "game_functions.h"
#include "ui_manager.h"
#include "ui_skin.h"
#include "zeal.h"

// SidlScreenWnd vtable overrides. Same shapes as the map window in zone_map.cpp.

// Draw nothing behind the client area so the RenderUI callback owns every pixel inside the frame.
static int __fastcall DrawBackground(Zeal::GameUI::SidlWnd *wnd, int unusedEDX) { return 0; }

// The client does not call this for windows outside its own list; Zeal calls it on zone exit.
static void __fastcall Deactivate(Zeal::GameUI::SidlWnd *wnd, int unusedEDX) { wnd->show(0, false); }

bool Cards::is_window_visible() const { return wnd && wnd->IsVisible; }

void Cards::show_window() {
  if (wnd && !wnd->IsVisible) wnd->show(1, true);
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
  if (args.size() > 1 && args[1] == "version") {
    Zeal::Game::print_chat("Cards of Norrath %s", kVersion);
    return true;
  }
  toggle_window();
  return true;  // Handled, so the client does not treat /cards as an unknown command.
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
  if (!wnd) return;
  ZealService *zeal = ZealService::get_instance();
  if (zeal && zeal->ui) zeal->ui->DestroySidlScreenWnd(wnd);
  wnd = nullptr;  // Disables rendering until the next init_ui.
}

void Cards::callback_deactivate_ui() {
  if (wnd) Deactivate(wnd, 0);
}

// Paints the window's client area with a flat colour. Milestone 1 only needs to prove the
// frame, the client rect and the draw order; the board replaces this in milestone 2.
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

  // No texture bound, so take colour and alpha straight from the vertex diffuse.
  D3DTextureStateStash texture_state(*device);
  texture_state.store_and_modify({D3DTSS_COLOROP, D3DTOP_SELECTARG1});
  texture_state.store_and_modify({D3DTSS_COLORARG1, D3DTA_DIFFUSE});
  texture_state.store_and_modify({D3DTSS_ALPHAOP, D3DTOP_SELECTARG1});
  texture_state.store_and_modify({D3DTSS_ALPHAARG1, D3DTA_DIFFUSE});

  struct Vertex {
    float x, y, z, rhw;
    DWORD color;
  };
  const DWORD color = D3DCOLOR_ARGB(230, 24, 48, 32);  // Card table green, slightly translucent.
  Vertex vertices[] = {{left, top, 0.0f, 1.0f, color},
                       {right, top, 0.0f, 1.0f, color},
                       {left, bottom, 0.0f, 1.0f, color},
                       {right, bottom, 0.0f, 1.0f, color}};

  device->SetTexture(0, NULL);
  device->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
  device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(Vertex));

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
}
