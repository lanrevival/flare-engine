cmake_minimum_required (VERSION 3.10)
Project (Flare)

Set (PACKAGE "FLARE")
Set (VERSION "1.15")

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake/")

# can't use add_compile_options here. Seems to have something to do with add_compile_options appending it after the "-O3 -DNDEBUG" flags
if (MINGW AND CMAKE_BUILD_TYPE STREQUAL "Release")
	set(CMAKE_CXX_FLAGS_RELEASE "-Wl,-subsystem,windows ${CMAKE_CXX_FLAGS_RELEASE}")
endif()

# Default definitions
if (MSVC)
	add_definitions(-D_CRT_SECURE_NO_WARNINGS)
	add_definitions(-D_CRT_SECURE_NO_DEPRECATE)
	add_definitions(-D_CRT_NONSTDC_NO_DEPRECATE)
else ()
	if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
		add_compile_options(-Wno-psabi)
	endif()

	add_compile_options(
		-Wall -Wextra -Wunused -Wshadow -Woverloaded-virtual -Wunreachable-code -Wconversion -Wno-sign-conversion -Wformat-security
		-fno-math-errno -fno-exceptions
		-std=c++98
	)
endif ()

add_compile_options(
	"$<$<CONFIG:RELEASE>:-O2>"
	"$<$<CONFIG:RELEASE>:-g0>"

	"$<$<CONFIG:RELWITHDEBINFO>:-O2>"
	"$<$<CONFIG:RELWITHDEBINFO>:-g>"

	"$<$<CONFIG:MINSIZEREL>:-Os>"
	"$<$<CONFIG:MINSIZEREL>:-g0>"

	"$<$<CONFIG:DEBUG>:-O0>"
	"$<$<CONFIG:DEBUG>:-g3>"
	"$<$<CONFIG:DEBUG>:-pg>"
)

# Not supported by CMake 3.12!
# Not supported by CMake < 3.13, so we have to stick to modifying the strings...
# add_link_options(
# 	"$<$<CONFIG:DEBUG>:-pg>"
# )

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
	set(CMAKE_EXE_LINKER_FLAGS_DEBUG "-pg ${CMAKE_EXE_LINKER_FLAGS_DEBUG}")
	set(CMAKE_SHARED_LINKER_FLAGS_DEBUG "-pg ${CMAKE_SHARED_LINKER_FLAGS_DEBUG}")
	set(CMAKE_MODULE_LINKER_FLAGS_DEBUG "-pg ${CMAKE_MODULE_LINKER_FLAGS_DEBUG}")
endif()

set(BINDIR  "games"             CACHE STRING "Directory from CMAKE_INSTALL_PREFIX where game executable will be installed.")
set(DATADIR "share/games/flare" CACHE STRING "Directory from CMAKE_INSTALL_PREFIX where game data files will be installed.")
set(MANDIR  "share/man"         CACHE STRING "Directory from CMAKE_INSTALL_PREFIX where manual pages will be installed.")

If(NOT IS_ABSOLUTE "${BINDIR}")
	set(FLARE_EXECUTABLE_PATH ${CMAKE_INSTALL_PREFIX}/${BINDIR}/flare)
Else(NOT IS_ABSOLUTE "${BINDIR}")
	set(FLARE_EXECUTABLE_PATH ${BINDIR}/flare)
EndIf(NOT IS_ABSOLUTE "${BINDIR}")

If(NOT IS_ABSOLUTE "${DATADIR}")
	add_definitions(-DDATA_INSTALL_DIR="${CMAKE_INSTALL_PREFIX}/${DATADIR}")
Else(NOT IS_ABSOLUTE "${DATADIR}")
	add_definitions(-DDATA_INSTALL_DIR="${DATADIR}")
EndIf(NOT IS_ABSOLUTE "${DATADIR}")


# desktop file
If(NOT IS_ABSOLUTE "${BINDIR}")
	set(FLARE_EXECUTABLE_PATH ${CMAKE_INSTALL_PREFIX}/${BINDIR}/flare)
Else(NOT IS_ABSOLUTE "${BINDIR}")
	set(FLARE_EXECUTABLE_PATH ${BINDIR}/flare)
EndIf(NOT IS_ABSOLUTE "${BINDIR}")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/distribution/flare.desktop.in" "${CMAKE_CURRENT_BINARY_DIR}/flare.desktop")


# Detect missing dependencies
# First, search for SDL2 modules

Find_Package(SDL2)
If (NOT SDL2_FOUND)
	Message (FATAL_ERROR "Couldn't find SDL2 development files. On Debian-based systems (such as Ubuntu) you should install the 'libsdl2-dev' package.")
Else (NOT SDL2_FOUND)
	INCLUDE_DIRECTORIES(${SDL2_INCLUDE_DIR})
EndIf (NOT SDL2_FOUND)

Find_Package(SDL2_image)
If (NOT SDL2IMAGE_FOUND)
	Message (FATAL_ERROR "Couldn't find SDL2-image development files. On Debian-based systems (such as Ubuntu) you should install the 'libsdl2-image-dev' package.")
Else (NOT SDL2IMAGE_FOUND)
	INCLUDE_DIRECTORIES(${SDL2IMAGE_INCLUDE_DIR})
EndIf (NOT SDL2IMAGE_FOUND)

Find_Package(SDL2_mixer)
If (NOT SDL2MIXER_FOUND)
	Message (FATAL_ERROR "Couldn't find SDL2-mixer development files. On Debian-based systems (such as Ubuntu) you should install the 'libsdl2-mixer-dev' package.")
Else (NOT SDL2MIXER_FOUND)
	INCLUDE_DIRECTORIES(${SDL2MIXER_INCLUDE_DIR})
EndIf (NOT SDL2MIXER_FOUND)

Find_Package(SDL2_ttf)
If (NOT SDL2TTF_FOUND)
	Message (FATAL_ERROR "Couldn't find SDL2-ttf development files. On Debian-based systems (such as Ubuntu) you should install the 'libsdl2-ttf-dev' package.")
Else (NOT SDL2TTF_FOUND)
	INCLUDE_DIRECTORIES(${SDL2TTF_INCLUDE_DIR})
EndIf (NOT SDL2TTF_FOUND)


# Sources

# Defines FLARE_SIM_SOURCES (no SDL2_image/SDL2_mixer/SDL2_ttf dependency, see P1.4b) and
# FLARE_PRESENTATION_SOURCES. FLARE_SOURCES below must stay their exact union, or flare's own
# build silently drops files.
Include (FlareSimSources)

Set (FLARE_SOURCES
	${FLARE_SIM_SOURCES}
	${FLARE_PRESENTATION_SOURCES}
	./src/main.cpp
)

Set (FLARE_HEADERS
	./src/ActionData.h
	./src/Animation.h
	./src/AnimationMedia.h
	./src/AnimationManager.h
	./src/AnimationSet.h
	./src/AStarContainer.h
	./src/AStarNode.h
	./src/ActionBarState.h
	./src/Avatar.h
	./src/Camera.h
	./src/CampaignManager.h
	./src/CombatText.h
	./src/CommonIncludes.h
	./src/CursorManager.h
	./src/DeviceList.h
	./src/EffectManager.h
	./src/EnemyGroupManager.h
	./src/EngineSettings.h
	./src/Entity.h
	./src/EntityBehavior.h
	./src/EntityManager.h
	./src/EventManager.h
	./src/FileParser.h
	./src/FogOfWar.h
	./src/FontDeviceList.h
	./src/FontEngine.h
	./src/GameSlotPreview.h
	./src/GameState.h
	./src/GameStateConfig.h
	./src/GameStateCutscene.h
	./src/GameStateTitle.h
	./src/GameStateLoad.h
	./src/GameStatePlay.h
	./src/GameStateNew.h
	./src/GameSwitcher.h
	./src/GetText.h
	./src/Hazard.h
	./src/HazardManager.h
	./src/IconManager.h
	./src/InputState.h
	./src/ItemManager.h
	./src/ItemStorage.h
	./src/LabelInfo.h
	./src/Loot.h
	./src/LootManager.h
	./src/Map.h
	./src/MapCollision.h
	./src/MapParallax.h
	./src/MapRenderer.h
	./src/MapSaver.h
	./src/Menu.h
	./src/MenuActionBar.h
	./src/MenuActiveEffects.h
	./src/MenuBook.h
	./src/MenuCharacter.h
	./src/MenuConfig.h
	./src/MenuConfirm.h
	./src/MenuDevConsole.h
	./src/MenuEnemy.h
	./src/MenuExit.h
	./src/MenuGameOver.h
	./src/MenuHUDLog.h
	./src/MenuInventory.h
	./src/MenuItemStorage.h
	./src/MenuLog.h
	./src/MenuManager.h
	./src/MenuMiniMap.h
	./src/MenuNumPicker.h
	./src/MenuPowers.h
	./src/MenuRegionTitle.h
	./src/MenuStash.h
	./src/MenuStatBar.h
	./src/MenuTalker.h
	./src/MenuTouchControls.h
	./src/MenuVendor.h
	./src/MessageEngine.h
	./src/ModManager.h
	./src/NPC.h
	./src/NullFontEngine.h
	./src/NullInputState.h
	./src/NullRenderDevice.h
	./src/NullSoundManager.h
	./src/NPCManager.h
	./src/PlayerCommand.h
	./src/PlayerInventory.h
	./src/PowerBonusState.h
	./src/PowerManager.h
	./src/QuestLog.h
	./src/RenderDevice.h
	./src/Replay.h
	./src/Rng.h
	./src/SDLInputState.h
	./src/SDLSoftwareRenderDevice.h
	./src/SDLSoundManager.h
	./src/SDLHardwareRenderDevice.h
	./src/SDLFontEngine.h
	./src/Settings.h
	./src/SimEvents.h
	./src/SharedGameResources.h
	./src/SharedResources.h
	./src/StatBlock.h
	./src/Stats.h
	./src/SoundManager.h
	./src/Subtitles.h
	./src/TileSet.h
	./src/TooltipData.h
	./src/TooltipManager.h
	./src/Utils.h
	./src/UtilsDebug.h
	./src/UtilsFileSystem.h
	./src/UtilsMath.h
	./src/UtilsParsing.h
	./src/Version.h
	./src/Widget.h
	./src/WorldHash.h
	./src/WidgetCheckBox.h
	./src/WidgetButton.h
	./src/WidgetHorizontalList.h
	./src/WidgetInput.h
	./src/WidgetLabel.h
	./src/WidgetListBox.h
	./src/WidgetLog.h
	./src/WidgetScrollBar.h
	./src/WidgetScrollBox.h
	./src/WidgetSlider.h
	./src/WidgetSlot.h
	./src/WidgetTabControl.h
	./src/WidgetTooltip.h
	./src/XPScaling.h
)

# Add icon and file info to executable for Windows systems
IF (WIN32)
	SET(FLARE_SOURCES
	${FLARE_SOURCES}
	./src/Flare.rc
	)
ENDIF (WIN32)

# flare-server = FLARE_SIM_SOURCES + main_server.cpp + the parts of FLARE_PRESENTATION_SOURCES
# main_server.cpp's own reach (through SaveLoad.cpp, and the if(menu)/if(mapr)-guarded calls
# P1.4a/c/d left in EventManager.cpp, StatBlock.cpp, ItemManager.cpp, FogOfWar.cpp, Avatar.cpp)
# still needs a symbol for, at link time, even though menu and mapr are always NULL server-side.
# P1.4d measured this: those guarded reach-throughs need Menu*/Widget*/MapRenderer/Camera/
# SaveLoad/CombatText/TooltipManager to link. Almost all of them only go through the abstract
# RenderDevice/InputState/SoundManager/FontEngine interfaces, so including them costs no forbidden
# dependency -- the AC this actually has to satisfy (see P1.4d's rewritten acceptance criteria) is
# "no SDL2_image/SDL2_mixer, no SDL_Render*/Mix_/IMG_ symbols" -- not "flare_sim alone" -- and the
# inherited plan's stronger "flare_sim + main_server.cpp only" version is exactly the thing
# P1.4d's own investigation found isn't achievable without a much larger untangling job than
# either the inherited plan or that untangling would be worth. One exception survived: MenuConfig
# (the options screen) is a real, structural dependency of MenuManager -- MenuManager's
# constructor unconditionally builds a MenuExit, which unconditionally owns a live MenuConfig (the
# pause menu embeds the settings screen) -- and MenuManager.cpp itself is unavoidable, because
# MenuActionBar::addPower() (needed: MenuInventory.cpp and MenuPowers.cpp, both genuinely required
# by SaveLoad.cpp, call it) itself calls MenuManager::isDragging()/defocusLeft()/defocusRight().
# MenuConfig::refreshFont() has no headless branch -- it exists purely to hot-swap the live
# SDLFontEngine when the player changes font/language -- so accepting MenuConfig.cpp into the link
# means accepting SDL2_ttf. getFontEngine() and createRenderDeviceList() were split out of
# DeviceList.cpp into FontDeviceList.cpp (P1.4d) specifically so accepting that one, unavoidable,
# dead-code-only SDL2_ttf dependency wouldn't also drag in the getRenderDevice()/getSoundManager()/
# getInputManager() factories' own SDL2_image/SDL2_mixer needs. Excluded, deliberately:
#  - DeviceList.cpp: getRenderDevice()/getSoundManager()/getInputManager() each construct their
#    concrete SDL class in the same function body as their null/headless branch, so linking it at
#    all requires linking SDLSoftwareRenderDevice/SDLHardwareRenderDevice/SDLSoundManager/
#    SDLInputState too, regardless of which branch runs. main_server.cpp constructs
#    NullRenderDevice/NullInputState/NullSoundManager/NullFontEngine directly instead (P1.4d).
#  - The four concrete SDL backends behind that: SDLHardwareRenderDevice/SDLSoftwareRenderDevice
#    (SDL2_image, SDL_Render*), SDLSoundManager (SDL2_mixer), SDLInputState (no image/mixer/ttf
#    dependency of its own, but nothing left in the required closure still calls it). SDLFontEngine
#    is the one concrete backend NOT excluded -- see above.
#  - GameState*.cpp and GameSwitcher.cpp: P1.4c's whole point was a main_server.cpp that never
#    references GameState*/MenuManager's owning game-state machine. Nothing in FLARE_SIM_SOURCES
#    or the rest of FLARE_PRESENTATION_SOURCES calls into these (SaveLoad.cpp #includes
#    GameStatePlay.h but never uses a GameStatePlay symbol), so excluding them costs nothing and
#    keeps that property true at the link-set level, not just in main_server.cpp's own source.
Set (FLARE_SERVER_EXCLUDED_PRESENTATION_SOURCES
	./src/DeviceList.cpp
	./src/SDLHardwareRenderDevice.cpp
	./src/SDLSoftwareRenderDevice.cpp
	./src/SDLInputState.cpp
	./src/SDLSoundManager.cpp
	./src/GameState.cpp
	./src/GameStateConfig.cpp
	./src/GameStateCutscene.cpp
	./src/GameStateLoad.cpp
	./src/GameStateNew.cpp
	./src/GameStatePlay.cpp
	./src/GameStateTitle.cpp
	./src/GameSwitcher.cpp
)

Set (FLARE_SERVER_SOURCES ${FLARE_SIM_SOURCES} ${FLARE_PRESENTATION_SOURCES})
List (REMOVE_ITEM FLARE_SERVER_SOURCES ${FLARE_SERVER_EXCLUDED_PRESENTATION_SOURCES})
List (APPEND FLARE_SERVER_SOURCES ./src/main_server.cpp)

# The simulation, in isolation: no SDL2_image, SDL2_mixer, or SDL2_ttf dependency. flare-server
# is not literally built by linking this library (it has its own broader FLARE_SERVER_SOURCES,
# see above) -- this target exists purely so FLARE_SIM_SOURCES' own boundary (compiles and links
# standalone, no presentation dependency at all) stays verified in isolation, per P1.4b.
Add_Library (flare_sim STATIC ${FLARE_SIM_SOURCES})
Target_Link_Libraries (flare_sim ${CMAKE_LD_FLAGS} ${SDL2_LIBRARY})

Add_Executable (flare ${FLARE_SOURCES} ${FLARE_HEADERS})
Add_Executable (flare-server ${FLARE_SERVER_SOURCES} ${FLARE_HEADERS})

# libSDLMain comes with libSDL if needed on certain platforms
If (NOT SDL2MAIN_LIBRARY)
	Set (SDL2MAIN_LIBRARY "")
EndIf (NOT SDL2MAIN_LIBRARY)

Target_Link_Libraries (flare ${CMAKE_LD_FLAGS} ${SDL2_LIBRARY} ${SDL2IMAGE_LIBRARY} ${SDL2MIXER_LIBRARY} ${SDL2TTF_LIBRARY} ${SDL2MAIN_LIBRARY})
# No SDL2_image/SDL2_mixer: FLARE_SERVER_SOURCES excludes every file that calls into them (see
# the comment above FLARE_SERVER_EXCLUDED_PRESENTATION_SOURCES). SDL2_ttf stays -- MenuConfig.cpp
# needs SDLFontEngine to exist for linking even though nothing server-side ever constructs a
# MenuConfig (same comment). SDL2_LIBRARY (core) stays too -- e.g. SDL_GetTicks and the SDL type
# aliases CommonIncludes.h pulls in are still used outside the excluded files, and the AC block
# explicitly expects libSDL2-2 still linked.
Target_Link_Libraries (flare-server ${CMAKE_LD_FLAGS} ${SDL2_LIBRARY} ${SDL2TTF_LIBRARY} ${SDL2MAIN_LIBRARY})

# Link-time guard (P1.4d): if flare-server ever gains a real SDL_Render*/SDL_CreateWindow call,
# a Mix_ audio call, or an IMG_ image-loading call, fail the build immediately rather than
# discovering it from a shipped binary. SDL2_ttf's TTF_ symbols are deliberately absent from this
# list -- see the FLARE_SERVER_EXCLUDED_PRESENTATION_SOURCES comment above for why flare-server
# accepts that one dependency. `nm` is used directly (not `ldd`/`otool -L`, which only see shared
# library *names*, not which symbols a binary actually calls) so a forbidden call would be caught
# even if it came from a statically-linked source.
Add_Custom_Command (TARGET flare-server POST_BUILD
	COMMAND sh -c "if nm $<TARGET_FILE:flare-server> 2>/dev/null | grep -qE 'SDL_Render|SDL_CreateWindow|Mix_OpenAudio|IMG_Load'; then echo 'flare-server: forbidden presentation symbol found (SDL_Render*/SDL_CreateWindow/Mix_OpenAudio/IMG_Load) -- see CMakeLists.txt FLARE_SERVER_EXCLUDED_PRESENTATION_SOURCES' >&2; exit 1; fi"
	COMMENT "flare-server: checking for forbidden SDL render/audio/image symbols"
	VERBATIM
)


# installing to the proper places
install(PROGRAMS
	${CMAKE_CURRENT_BINARY_DIR}/flare
	DESTINATION ${BINDIR})
install(DIRECTORY
	"${CMAKE_CURRENT_SOURCE_DIR}/mods"
	DESTINATION ${DATADIR})
install(FILES
	"${CMAKE_CURRENT_SOURCE_DIR}/distribution/flare.man"
	DESTINATION ${MANDIR}/man6
	RENAME flare.6)
install(FILES
	"${CMAKE_CURRENT_BINARY_DIR}/flare.desktop"
	DESTINATION ${CMAKE_INSTALL_PREFIX}/share/applications)
install(FILES
	"${CMAKE_CURRENT_SOURCE_DIR}/distribution/flare_logo_icon.svg"
	DESTINATION ${CMAKE_INSTALL_PREFIX}/share/icons/hicolor/scalable/apps
	RENAME flare.svg)
