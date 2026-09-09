#include "cards.h"

#include "commands.h"
#include "game_functions.h"
#include "zeal.h"

bool Cards::parse_command(const std::vector<std::string> &args) {
  Zeal::Game::print_chat("Cards of Norrath %s: milestone 0. No window yet.", kVersion);
  return true;  // Handled, so the client does not treat /cards as an unknown command.
}

Cards::Cards(ZealService *zeal) {
  // queue_chat_message defers the print until the UI is up and the character is in game.
  zeal->queue_chat_message(std::string("Cards of Norrath ") + kVersion + " loaded.");

  zeal->commands_hook->Add("/cards", {}, "Cards of Norrath card game",
                           [this](const std::vector<std::string> &args) { return parse_command(args); });
}
