/*
Copyright © 2011-2012 Clint Bellanger
Copyright © 2013 Kurt Rinnert
Copyright © 2014 Henrik Andersson
Copyright © 2012-2016 Justin Jacobs

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

/**
 * class LabelInfo
 *
 * Text positioning config (x/y, justify/valign, font style). Split out of WidgetLabel.h in P1.4b
 * because it's pure data with no rendering dependency, but WidgetLabel.h -- a genuine rendering
 * widget -- pulled in FontEngine/RenderDevice, and sim classes (EngineSettings, UtilsParsing's
 * Parse::popLabelInfo) need to own/return a LabelInfo by value.
 */

#ifndef LABEL_INFO_H
#define LABEL_INFO_H

#include "CommonIncludes.h"
#include "Utils.h"

class LabelInfo {
public:
	enum {
		VALIGN_CENTER = 0,
		VALIGN_TOP = 1,
		VALIGN_BOTTOM = 2
	};

	int x,y;
	int justify,valign;
	bool hidden;
	std::string font_style;

	LabelInfo();
};

#endif
