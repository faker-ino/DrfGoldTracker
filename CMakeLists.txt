cmake_minimum_required(VERSION 3.21)
project(DrfGoldTracker LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Match vcpkg's x64-windows-static triplet (static CRT) so the addon links
# against static versions of ixwebsocket/mbedtls/zlib and ships as a single
# self-contained DLL - no companion .dll files need to sit next to it in
# <Guild Wars 2>/addons/, unlike the dynamic triplet's z.dll/mbedtls.dll etc.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Guild Wars 2 / Nexus are 64-bit only - fail fast instead of shipping a
# DLL Nexus can load-fail or calling-convention-mismatch on.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "DrfGoldTracker must be built x64 - select the x64 CMake preset/configuration.")
endif()

find_package(nlohmann_json CONFIG REQUIRED)
find_package(ixwebsocket CONFIG REQUIRED)

# Embeds resources/icons/quickaccess_{normal,hover}.png as C++ byte arrays
# (see cmake/EmbedIcon.cmake) for Textures_GetOrCreateFromMemory, so the
# QuickAccess shortcut icon ships baked into the DLL with no loose-file
# deploy step. Deliberately not the Win32-resource route
# (Textures_GetOrCreateFromResource + RCDATA) - that route was tried in an
# earlier addon and Nexus's loader couldn't find a resource Win32 itself
# confirmed was present in the built DLL.
set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")

function(embed_icon name in_file)
    set(header "${GENERATED_DIR}/${name}.h")
    set(cpp "${GENERATED_DIR}/${name}.cpp")
    add_custom_command(
        OUTPUT "${header}" "${cpp}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${GENERATED_DIR}"
        COMMAND ${CMAKE_COMMAND}
            -DIN_FILE=${in_file}
            -DOUT_HEADER=${header}
            -DOUT_CPP=${cpp}
            -DSYMBOL_NAME=k${name}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedIcon.cmake"
        DEPENDS "${in_file}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedIcon.cmake"
        COMMENT "Embedding ${name} as a C++ byte array"
    )
    set(${name}_GENERATED_HEADER "${header}" PARENT_SCOPE)
    set(${name}_GENERATED_CPP "${cpp}" PARENT_SCOPE)
endfunction()

embed_icon(QuickAccessIconNormal "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/quickaccess_normal.png")
embed_icon(QuickAccessIconHover "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/quickaccess_hover.png")

# Gold/silver/copper coin icons (from wiki.guildwars2.com's "highres" coin
# renders) - used by RenderCoin's icon-only mode instead of a drawn circle
# glyph, and by the icon-grid view's Coin denomination tiles (see
# RenderCoinDenominationIcon/RenderCoinDenominationTile in main.cpp).
embed_icon(GoldCoinIcon "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/gold_coin.png")
embed_icon(SilverCoinIcon "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/silver_coin.png")
embed_icon(CopperCoinIcon "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/copper_coin.png")

add_library(DrfGoldTracker SHARED
    src/main.cpp
    src/DrfClient.cpp
    src/SessionTracker.cpp
    src/Gw2Api.cpp
    src/IconCache.cpp
    src/Profit.cpp
    src/TrackerSettings.cpp
    src/ResetScheduler.cpp
    vendor/imgui/imgui.cpp
    vendor/imgui/imgui_draw.cpp
    vendor/imgui/imgui_tables.cpp
    vendor/imgui/imgui_widgets.cpp
    "${QuickAccessIconNormal_GENERATED_HEADER}"
    "${QuickAccessIconNormal_GENERATED_CPP}"
    "${QuickAccessIconHover_GENERATED_HEADER}"
    "${QuickAccessIconHover_GENERATED_CPP}"
    "${GoldCoinIcon_GENERATED_HEADER}"
    "${GoldCoinIcon_GENERATED_CPP}"
    "${SilverCoinIcon_GENERATED_HEADER}"
    "${SilverCoinIcon_GENERATED_CPP}"
    "${CopperCoinIcon_GENERATED_HEADER}"
    "${CopperCoinIcon_GENERATED_CPP}"
)

target_include_directories(DrfGoldTracker PRIVATE src vendor/nexus vendor/imgui "${GENERATED_DIR}")
target_link_libraries(DrfGoldTracker PRIVATE nlohmann_json::nlohmann_json ixwebsocket::ixwebsocket bcrypt)
target_compile_definitions(DrfGoldTracker PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

# Nexus loads addons from <GW2>/addons/<DrfGoldTracker>/<DrfGoldTracker>.dll - the
# output name must match the folder name exactly.
set_target_properties(DrfGoldTracker PROPERTIES OUTPUT_NAME "DrfGoldTracker")

# DLL stays loaded inside the running GW2 process while iterating - GW2/Nexus
# can hold the matching .pdb open for symbol lookup, which makes the linker's
# Debug-config /DEBUG step fail with LNK1201 on rebuild. Skip linker PDB
# generation in Debug so rebuilds don't depend on closing the game.
target_link_options(DrfGoldTracker PRIVATE $<$<CONFIG:Debug>:/DEBUG:NONE>)
