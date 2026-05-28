target_sources(
  ruby
  PRIVATE #
)

target_sources(
  ruby
  PRIVATE #
)

find_package(OpenGL REQUIRED)
find_package(X11 REQUIRED)

target_link_libraries(ruby PRIVATE OpenGL::GL X11::X11 X11::Xrandr)

target_enable_feature(ruby "SDL3 OpenGL video driver" VIDEO_SDL3)

find_package(librashader)
if(librashader_FOUND AND ARES_ENABLE_LIBRASHADER)
  target_enable_feature(ruby "librashader OpenGL runtime" LIBRA_RUNTIME_OPENGL)
else()
  target_compile_definitions(ruby PRIVATE LIBRA_RUNTIME_OPENGL)
endif()

target_link_libraries(
  ruby
  PRIVATE
    SDL3::SDL3
    $<$<BOOL:TRUE>:librashader::librashader>
)
