# dummy GitHub target used to enable editing of GitHub files within generated IDE projects

add_library(.github INTERFACE)

set(.github_sources
)

target_sources(
  .github
  PRIVATE
  ${.github_sources}
  ../.gitignore
  ../.gitattributes
  ../README.md
  ../LICENSE
)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${.github_sources})

