namespace Systems {
  #include "nintendo-64.cpp"
  #include "nintendo-64dd.cpp"
}

auto System::create(string name) -> std::shared_ptr<Pak> {
  if(name == "Nintendo 64") return std::make_shared<Systems::Nintendo64>();
  if(name == "Nintendo 64DD") return std::make_shared<Systems::Nintendo64DD>();
  return {};
}

auto System::locate() -> string {
  string location = {mia::homeLocation(), name(), ".sys/"};
  directory::create(location);
  return location;
}
