add_library(libSceFreeType SHARED EXCLUDE_FROM_ALL
        Export.cpp
        src/GuestObjects.cpp
        src/Names.cpp
)
target_include_directories(libSceFreeType PRIVATE ${LIBS_INCLUDE_DIR})
target_link_libraries(libSceFreeType PRIVATE freetype)
target_sources(libSceFreeType PRIVATE src/SizeRequest.cpp)

if(BUILD_TESTING)
    add_test_executable(guest_freetype_request_size_tests ${LIBS_INCLUDE_DIR}/tests/GuestFreeTypeRequestSize.cpp)
    target_include_directories(guest_freetype_request_size_tests PRIVATE ${LIBS_INCLUDE_DIR})
    target_link_libraries(guest_freetype_request_size_tests PRIVATE libSceFreeType freetype libc)
    configure_windows_unwind(guest_freetype_request_size_tests)
    add_test(NAME guest_freetype_request_size COMMAND guest_freetype_request_size_tests)
    if(WIN32)
        set_tests_properties(guest_freetype_request_size PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libSceFreeType>")
    endif()
endif()

if(MINGW OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(libSceFreeType PRIVATE LINKER:--exclude-libs,ALL)
endif()
configure_windows_unwind(libSceFreeType)

if(BUILD_TESTING)
    add_test_executable(guest_freetype_tests ${LIBS_INCLUDE_DIR}/tests/GuestFreeType.cpp)
    target_include_directories(guest_freetype_tests PRIVATE ${LIBS_INCLUDE_DIR})
    target_link_libraries(guest_freetype_tests PRIVATE libSceFreeType freetype libc)
    configure_windows_unwind(guest_freetype_tests)
    add_test(NAME guest_freetype COMMAND guest_freetype_tests)
    add_test_executable(guest_freetype_charmap_tests ${LIBS_INCLUDE_DIR}/tests/GuestFreeTypeCharmap.cpp)
    target_include_directories(guest_freetype_charmap_tests PRIVATE ${LIBS_INCLUDE_DIR})
    target_link_libraries(guest_freetype_charmap_tests PRIVATE libSceFreeType freetype libc)
    configure_windows_unwind(guest_freetype_charmap_tests)
    add_test(NAME guest_freetype_charmap COMMAND guest_freetype_charmap_tests)
    add_test_executable(guest_freetype_names_tests ${LIBS_INCLUDE_DIR}/tests/GuestFreeTypeNames.cpp)
    target_include_directories(guest_freetype_names_tests PRIVATE ${LIBS_INCLUDE_DIR})
    target_link_libraries(guest_freetype_names_tests PRIVATE libSceFreeType freetype libc)
    configure_windows_unwind(guest_freetype_names_tests)
    add_test(NAME guest_freetype_names COMMAND guest_freetype_names_tests)
    add_test_executable(guest_freetype_sfnt_names_tests ${LIBS_INCLUDE_DIR}/tests/GuestFreeTypeSfntNames.cpp)
    target_include_directories(guest_freetype_sfnt_names_tests PRIVATE ${LIBS_INCLUDE_DIR})
    target_link_libraries(guest_freetype_sfnt_names_tests PRIVATE libSceFreeType freetype libc)
    configure_windows_unwind(guest_freetype_sfnt_names_tests)
    add_test(NAME guest_freetype_sfnt_names COMMAND guest_freetype_sfnt_names_tests)
    if(WIN32)
        set_tests_properties(guest_freetype_sfnt_names PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libSceFreeType>")
        set_tests_properties(guest_freetype_names PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libSceFreeType>")
        set_tests_properties(guest_freetype PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libSceFreeType>")
        set_tests_properties(guest_freetype_charmap PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libSceFreeType>")
    endif()
endif()
