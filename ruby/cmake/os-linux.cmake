target_sources(
  ruby
  PRIVATE #
)

target_sources(
  ruby
  PRIVATE #
)

target_enable_feature(ruby "SDL3 GPU video driver" VIDEO_SDL3)

target_link_libraries(ruby PRIVATE SDL3::SDL3)
