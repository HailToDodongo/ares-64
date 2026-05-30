struct Attribute {
  Attribute(const string& name, const std::any& value = {}) : name(name), value(value) {}
  auto operator==(const Attribute& source) const -> bool { return name == source.name; }
  auto operator< (const Attribute& source) const -> bool { return name <  source.name; }

  string name;
  std::any value;
};
