#include <any>
namespace nall::vfs {

struct attribute {
  attribute(const string& name, const std::any& value = {}) : name(name), value(value) {}
  auto operator==(const attribute& source) const -> bool { return name == source.name; }
  auto operator< (const attribute& source) const -> bool { return name <  source.name; }

  string name;
  std::any value;
};

}
