#pragma once

#include <string>
#include <vector>

// Cards of Norrath: an EQ-themed Triple Triad card game.
//
// Milestone 0: prove the module is built, injected and constructed by printing one
// line to the chat window on login and answering a /cards command. No UI yet.
class Cards {
 public:
  static constexpr char kVersion[] = "0.0.1";

  Cards(class ZealService *zeal);
  ~Cards() {};

 private:
  bool parse_command(const std::vector<std::string> &args);
};
