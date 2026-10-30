add_library(libSceNKWebKit SHARED EXCLUDE_FROM_ALL Export.cpp LegacyExports.cpp)
target_include_directories(libSceNKWebKit PRIVATE ${LIBS_INCLUDE_DIR})
configure_windows_unwind(libSceNKWebKit)
