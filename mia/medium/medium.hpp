struct Database {
  string name;
  Markup::Node list;
};

struct Medium : Pak {
  static auto create(string name) -> std::shared_ptr<Pak>;
  auto loadDatabase() -> bool;
  auto database() -> Database;
  auto manifestDatabase(string sha256) -> string;
  auto manifestDatabaseArcade(string name) -> string;

  string sha256;
};

struct Cartridge : Medium {
  auto type() -> string override { return "Cartridge"; }
};

struct FloppyDisk : Medium {
  auto type() -> string override { return "Floppy Disk"; }
};
