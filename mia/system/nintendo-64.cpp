struct Nintendo64 : System {
  auto name() -> string override { return "Nintendo 64"; }
  auto load(string location) -> LoadResult override;
  auto save(string location) -> bool override;
};

auto Nintendo64::load(string location) -> LoadResult {
  this->location = locate();
  pak = std::make_shared<vfs::directory>();

  auto pif_ntsc = file::read(mia::locate("Firmware/Nintendo 64/pif.ntsc.rom"));
  auto pif_pal  = file::read(mia::locate("Firmware/Nintendo 64/pif.pal.rom"));
  auto pif_sm5  = file::read(mia::locate("Firmware/Nintendo 64/pif.sm5.rom"));

  if(pif_ntsc.empty() || pif_pal.empty() || pif_sm5.empty()) return noFirmware;

  pak->append("pif.ntsc.rom", pif_ntsc);
  pak->append("pif.pal.rom",  pif_pal);
  pak->append("pif.sm5.rom",  pif_sm5);
  return successful;
}

auto Nintendo64::save(string location) -> bool {
  return true;
}
