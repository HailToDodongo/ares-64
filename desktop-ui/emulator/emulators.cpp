#ifdef CORE_N64
  namespace ares::Nintendo64 {
    auto load(Node::System& node, string name) -> bool;
    auto option(string name, string value) -> bool;
  }
  #include "nintendo-64.cpp"
  #include "nintendo-64dd.cpp"
#endif

auto Emulator::construct() -> void {
  #ifdef CORE_N64
  emulators.push_back(std::make_shared<Nintendo64>());
  emulators.push_back(std::make_shared<Nintendo64DD>());
  #endif

  std::ranges::sort(emulators, [](std::shared_ptr<Emulator> a, std::shared_ptr<Emulator> b) {
    return (a->manufacturer < b->manufacturer);
  });
}
