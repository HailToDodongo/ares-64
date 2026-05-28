#ifdef CORE_N64
  namespace ares::Nintendo64 {
    auto load(Node::System& node, string name) -> bool;
    auto option(string name, string value) -> bool;
  }
  #include "nintendo-64.cpp"
  #include "nintendo-64dd.cpp"
#endif

#include "arcade.cpp"

auto Emulator::construct() -> void {
  if(Arcade::available()) emulators.push_back(std::make_shared<Arcade>());

  #ifdef CORE_N64
  emulators.push_back(std::make_shared<Nintendo64>());
  emulators.push_back(std::make_shared<Nintendo64DD>());
  #endif

  std::ranges::sort(emulators, [](std::shared_ptr<Emulator> a, std::shared_ptr<Emulator> b) {
    return (a->manufacturer < b->manufacturer);
  });
}
