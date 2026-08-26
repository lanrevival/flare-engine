/*
Copyright © 2026 Flare LAN Revival contributors

This file is part of FLARE.

FLARE is free software: you can redistribute it and/or modify it under the terms
of the GNU General Public License as published by the Free Software Foundation,
either version 3 of the License, or (at your option) any later version.

FLARE is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE.  See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
FLARE.  If not, see http://www.gnu.org/licenses/
*/

#ifndef FONTDEVICELIST_H
#define FONTDEVICELIST_H

#include "CommonIncludes.h"

class FontEngine;
class MessageEngine;

// Split out of DeviceList.cpp (P1.4d): getRenderDevice()/getSoundManager()/getInputManager()
// each construct their concrete SDL class in the same function body as their null/headless
// branch, so linking DeviceList.cpp at all -- even for a headless build that only ever takes
// that branch -- pulls in SDL2_image and SDL2_mixer. getFontEngine() has no headless branch to
// begin with (MenuConfig::refreshFont() always wants a real, working font renderer), and
// createRenderDeviceList() is pure string data with no device dependency at all -- neither
// belongs in that problem, so both live here instead, in their own translation unit that
// flare-server links (accepting SDL2_ttf, since MenuConfig.cpp needs SDLFontEngine to exist for
// linking even though nothing server-side ever constructs a MenuConfig) without also pulling in
// SDL2_image/SDL2_mixer.
FontEngine* getFontEngine();
void createRenderDeviceList(MessageEngine* msg, std::vector<std::string> &rd_name, std::vector<std::string> &rd_desc);

#endif
