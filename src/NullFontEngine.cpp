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

#include "NullFontEngine.h"

NullFontEngine::NullFontEngine()
	: FontEngine() {
}

NullFontEngine::~NullFontEngine() {
}

void NullFontEngine::renderInternal(const std::string& /*text*/, int /*x*/, int /*y*/, int /*justify*/, Image* /*target*/, const Color& /*color*/, bool /*shadow*/) {
}

int NullFontEngine::getLineHeight() {
	return 0;
}

int NullFontEngine::getFontHeight() {
	return 0;
}

void NullFontEngine::setFont(const std::string& /*_font*/) {
}

Point NullFontEngine::calcSize(const std::string& /*text*/) {
	return Point();
}

std::string NullFontEngine::trimTextToWidth(const std::string& text, const int /*width*/, const bool /*use_ellipsis*/, size_t /*left_pos*/) {
	return text;
}
