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

# The server reuses the whole engine, but swaps the client entry point for a headless one.
# TODO(P1.4c): once main_server.cpp is rewritten as a thin sim loop, this becomes
# flare_sim + FLARE_NET_SOURCES + main_server.cpp instead.
Set (FLARE_SERVER_SOURCES ${FLARE_SOURCES})
List (REMOVE_ITEM FLARE_SERVER_SOURCES ./src/main.cpp)
List (APPEND FLARE_SERVER_SOURCES ./src/main_server.cpp)

# The simulation, in isolation: no SDL2_image, SDL2_mixer, or SDL2_ttf dependency. Not yet linked
# into flare-server (P1.4c does that) -- this target exists so the boundary can be verified now,
# per P1.4b.
Add_Library (flare_sim STATIC ${FLARE_SIM_SOURCES})
Target_Link_Libraries (flare_sim ${CMAKE_LD_FLAGS} ${SDL2_LIBRARY})

Add_Executable (flare ${FLARE_SOURCES} ${FLARE_HEADERS})
Add_Executable (flare-server ${FLARE_SERVER_SOURCES} ${FLARE_HEADERS})

# libSDLMain comes with libSDL if needed on certain platforms
If (NOT SDL2MAIN_LIBRARY)
	Set (SDL2MAIN_LIBRARY "")
EndIf (NOT SDL2MAIN_LIBRARY)

Target_Link_Libraries (flare ${CMAKE_LD_FLAGS} ${SDL2_LIBRARY} ${SDL2IMAGE_LIBRARY} ${SDL2MIXER_LIBRARY} ${SDL2TTF_LIBRARY} ${SDL2MAIN_LIBRARY})
Target_Link_Libraries (flare-server ${CMAKE_LD_FLAGS} ${SDL2_LIBRARY} ${SDL2IMAGE_LIBRARY} ${SDL2MIXER_LIBRARY} ${SDL2TTF_LIBRARY} ${SDL2MAIN_LIBRARY})


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
