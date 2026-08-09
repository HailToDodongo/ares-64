target_sources(ares PRIVATE cmake/os-macos.cmake)

if(ARES_ENABLE_VULKAN)
  find_package(MoltenVK)

  if(MoltenVK_FOUND)
    target_link_libraries(ares PRIVATE "$<LINK_LIBRARY:WEAK_LIBRARY,MoltenVK::MoltenVK>")
  endif()

  if(MoltenVK_FOUND)
    set(VULKAN_FOUND true)
  endif()
endif()
