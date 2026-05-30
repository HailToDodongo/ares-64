target_enable_feature(ruby "SDL3 GPU video driver" VIDEO_SDL3)
target_enable_feature(ruby "SDL3 input driver" INPUT_SDL3)
target_enable_feature(ruby "SDL3 audio driver" AUDIO_SDL)

target_link_libraries(ruby PRIVATE SDL3::SDL3)
