#pragma once

#include <optional>
#include <nall/traits.hpp>

namespace nall {

using nothing_t = std::nullopt_t;
inline constexpr nothing_t nothing = std::nullopt;
struct else_t {};

template<typename T>
struct maybe {
  maybe() = default;
  maybe(nothing_t) : _value(std::nullopt) {}
  maybe(const T& source) : _value(source) {}
  maybe(T&& source) : _value(std::move(source)) {}
  maybe(const maybe& source) : _value(source._value) {}
  maybe(maybe&& source) : _value(std::move(source._value)) {}
  ~maybe() = default;

  auto operator=(nothing_t) -> maybe& { _value.reset(); return *this; }
  auto operator=(const T& source) -> maybe& { _value.emplace(source); return *this; }
  auto operator=(T&& source) -> maybe& { _value.emplace(std::move(source)); return *this; }
  auto operator=(const maybe& source) -> maybe& { _value = source._value; return *this; }
  auto operator=(maybe&& source) -> maybe& { _value = std::move(source._value); return *this; }

  explicit operator bool() const { return _value.has_value(); }
  auto reset() -> void { _value.reset(); }
  auto data() -> T* { return _value.has_value() ? &*_value : nullptr; }
  auto get() -> T& { return _value.value(); }
  auto data() const -> const T* { return _value.has_value() ? &*_value : nullptr; }
  auto get() const -> const T& { return _value.value(); }
  auto operator->() -> T* { return data(); }
  auto operator->() const -> const T* { return data(); }
  auto operator*() -> T& { return get(); }
  auto operator*() const -> const T& { return get(); }
  auto operator()() -> T& { return get(); }
  auto operator()() const -> const T& { return get(); }
  auto operator()(const T& invalid) const -> const T& { return _value.has_value() ? get() : invalid; }

private:
  std::optional<T> _value;
};

template<typename T>
struct maybe<T&> {
  maybe() : _value(nullptr) {}
  maybe(nothing_t) : _value(nullptr) {}
  maybe(const T& source) : _value((T*)&source) {}
  maybe(const maybe& source) : _value(source._value) {}

  auto operator=(nothing_t) -> maybe& { _value = nullptr; return *this; }
  auto operator=(const T& source) -> maybe& { _value = (T*)&source; return *this; }
  auto operator=(const maybe& source) -> maybe& { _value = source._value; return *this; }

  explicit operator bool() const { return _value; }
  auto reset() -> void { _value = nullptr; }
  auto data() -> T* { return _value; }
  auto get() -> T& { return *_value; }
  auto data() const -> const T* { return const_cast<maybe*>(this)->data(); }
  auto get() const -> const T& { return const_cast<maybe*>(this)->get(); }
  auto operator->() -> T* { return data(); }
  auto operator->() const -> const T* { return data(); }
  auto operator*() -> T& { return get(); }
  auto operator*() const -> const T& { return get(); }
  auto operator()() -> T& { return get(); }
  auto operator()() const -> const T& { return get(); }
  auto operator()(const T& invalid) const -> const T& { return _value ? get() : invalid; }

private:
  T* _value;
};

}
