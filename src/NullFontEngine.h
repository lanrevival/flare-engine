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

#ifndef NULLFONTENGINE_H
#define NULLFONTENGINE_H

#include "FontEngine.h"

/** A FontEngine that renders nothing and loads no font files.
 *
 * Used by the dedicated server. SDLFontEngine calls TTF_Init, TTF_OpenFont and
 * TTF_RenderUTF8_Blended, all of which come from SDL2_ttf; this class makes no such call at
 * all, so linking it in never requires SDL2_ttf.
 *
 * getColor() and calcSizeWrapped() are inherited unmodified from FontEngine -- both are pure
 * data/arithmetic with no rendering dependency, same reasoning as NullInputState::handle().
 *
 * @class NullFontEngine
 * @see FontEngine
 */
class NullFontEngine : public FontEngine {
protected:
	void renderInternal(const std::string& text, int x, int y, int justify, Image *target, const Color& color, bool shadow);

public:
	NullFontEngine();
	~NullFontEngine();

	int getLineHeight();
	int getFontHeight();

	void setFont(const std::string& _font);
	Point calcSize(const std::string& text);
	std::string trimTextToWidth(const std::string& text, const int width, const bool use_ellipsis, size_t left_pos);
};

#endif // NULLFONTENGINE_H
