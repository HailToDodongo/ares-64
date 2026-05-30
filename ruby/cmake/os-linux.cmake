target_sources(
  ruby
  PRIVATE #
)

target_sources(
  ruby
  PRIVATE #
)

find_package(X11 REQUIRED)

target_link_libraries(ruby PRIVATE X11::X11 X11::Xrandr)

target_enable_feature(ruby "SDL3 GPU video driver" VIDEO_SDL3)

target_link_libraries(ruby PRIVATE SDL3::SDL3)
