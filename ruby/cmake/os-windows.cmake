target_sources(
  ruby
  PRIVATE #
    video/sdl3.cpp
)

target_sources(
  ruby
  PRIVATE #
    audio/sdl.cpp
)

target_sources(
  ruby
  PRIVATE #
    input/sdl3.cpp
)

target_enable_feature(ruby "SDL3 OpenGL video driver" VIDEO_SDL3)
target_enable_feature(ruby "SDL3 input driver" INPUT_SDL3)
target_enable_feature(ruby "SDL3 audio driver" AUDIO_SDL)

target_link_libraries(
  ruby
  PRIVATE
    SDL3::SDL3
    $<$<BOOL:TRUE>:librashader::librashader>
)
