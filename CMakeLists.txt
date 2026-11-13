cmake_minimum_required(VERSION 3.12)
project(PXEServer VERSION 1.0.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(WIN32)
    enable_language(RC)
endif()

file(GLOB SOURCES CONFIGURE_DEPENDS
    "src/main.cpp"
    "src/server/*.cpp"
    "src/utils/*.cpp"
    "src/protocols/*.cpp"
)

if(WIN32)
    list(APPEND SOURCES "${CMAKE_SOURCE_DIR}/resources/pxe.rc")
endif()

add_executable(pxe_server ${SOURCES})
target_include_directories(pxe_server PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(pxe_server pthread ws2_32)
set_target_properties(pxe_server PROPERTIES LINK_FLAGS "-Wl,--subsystem,console")
set_target_properties(pxe_server PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
# Some Windows security policies deny execution of the literal name
# "pxe_server.exe" / "pxe-server.exe" are blocked on this machine, so ship as pxesrv.exe.
set_target_properties(pxe_server PROPERTIES OUTPUT_NAME "pxesrv")

# Distribution layout: the executable plus everything it serves at runtime
# (config/ and boot/) is assembled under build/bin.
add_custom_command(TARGET pxe_server POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/bin/logs"
    # Mirror (not merge) so files deleted from the source tree disappear here too.
    COMMAND ${CMAKE_COMMAND} -E remove_directory "${CMAKE_BINARY_DIR}/bin/config"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/config" "${CMAKE_BINARY_DIR}/bin/config"
    COMMAND ${CMAKE_COMMAND} -E remove_directory "${CMAKE_BINARY_DIR}/bin/boot"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/boot" "${CMAKE_BINARY_DIR}/bin/boot"
    COMMENT "Distributing config/ and boot/ into ${CMAKE_BINARY_DIR}/bin"
    VERBATIM)

# Copy MinGW runtime DLLs (if present) so the binary runs standalone.
set(_mingw_bin "/d/App/msys64/mingw64/bin")
foreach(_dll libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll)
    if(EXISTS "${_mingw_bin}/${_dll}")
        add_custom_command(TARGET pxe_server POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_mingw_bin}/${_dll}" "${CMAKE_BINARY_DIR}/bin/${_dll}"
            VERBATIM)
    endif()
endforeach()

install(TARGETS pxe_server DESTINATION bin)
install(DIRECTORY config DESTINATION .)
install(DIRECTORY boot DESTINATION .)
