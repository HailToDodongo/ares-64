namespace Media {
  std::vector<Database> databases;
  #include "nintendo-64.cpp"
  #include "nintendo-64dd.cpp"
}

auto Medium::create(string name) -> std::shared_ptr<Pak> {
  if(name == "Nintendo 64") return std::make_shared<Media::Nintendo64>();
  if(name == "Nintendo 64DD") return std::make_shared<Media::Nintendo64DD>();
  return {};
}

auto Medium::loadDatabase() -> bool {
  for(auto& database : Media::databases) {
    if(database.name == name()) return true;
  }
  Database database;
  database.name = name();
  auto databaseFile = locate({"Database/", name(), ".bml"});
  if(inode::exists(databaseFile)) {
    database.list = BML::unserialize(file::read(databaseFile));
    Media::databases.push_back(std::move(database));
    return true;
  }
  return false;
}

auto Medium::database() -> Database {
  loadDatabase();
  for(auto& database : Media::databases) {
    if (database.name == name()) return database;
  }
  return {};
}

auto Medium::manifestDatabase(string sha256) -> string {
  loadDatabase();
  for(auto& database : Media::databases) {
    if(database.name == name()) {
      for(auto node : database.list) {
        if(node["sha256"].string() == sha256) {
          return BML::serialize(node);
        }
      }
    }
  }
  return {};
}

auto Medium::manifestDatabaseArcade(string rom) -> string {
  loadDatabase();
  for(auto& database : Media::databases) {
    if(database.name == name()) {
      for(auto node : database.list) {
        if(node["name"].string().iequals(rom)) {
          return BML::serialize(node);
        }
      }
    }
  }
  return {};
}
