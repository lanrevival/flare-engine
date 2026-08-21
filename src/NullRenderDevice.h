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

#ifndef NULLRENDERDEVICE_H
#define NULLRENDERDEVICE_H

#include "RenderDevice.h"

/** A RenderDevice that draws nothing and owns no window.
 *
 * Used by the dedicated server, which runs the simulation with no display and no GPU.
 * Every drawing operation is discarded. No SDL video call is made from this file.
 *
 * NullImage carries a width and height but no pixels. Sizes still matter because the
 * engine lays out menus and computes source rectangles from them even when nothing is
 * ever drawn, and a zero-sized image causes divide-by-zero in some of that layout code.
 *
 * @class NullRenderDevice
 * @see RenderDevice
 */

class NullImage : public Image {
public:
	explicit NullImage(RenderDevice *device, int width, int height);
	virtual ~NullImage();

	// NOTE: these two are NOT pure virtual in the base class. C++98 has no 'override',
	// so a signature that does not match exactly silently becomes a new function and the
	// base implementation keeps being called. Do not edit these without re-checking
	// RenderDevice.h:111-112.
	int getWidth() const;
	int getHeight() const;

	void fillWithColor(const Color& color);
	void drawPixel(int x, int y, const Color& color);
	void drawLine(int x0, int y0, int x1, int y1, const Color& color);
	void drawFilledRect(int x, int y, int w, int h, const Color& color);
	Image* resize(int width, int height);

private:
	int w;
	int h;
};

class NullRenderDevice : public RenderDevice {
public:
	NullRenderDevice();
	virtual ~NullRenderDevice();

	int render(Renderable& r, Rect& dest);
	int render(Sprite* r);
	int renderToImage(Image* src_image, Rect& src, Image* dest_image, Rect& dest);
	Image* renderTextToImage(FontStyle* font_style, const std::string& text, const Color& color, bool blended);

	void drawPixel(int x, int y, const Color& color);
	void drawLine(int x0, int y0, int x1, int y1, const Color& color);
	void drawRectangle(const Point& p0, const Point& p1, const Color& color);
	void blankScreen();
	void commitFrame();
	void destroyContext();
	void windowResize();

	// NOTE: these three are NOT pure virtual in the base class -- see the note on
	// NullImage above. RenderDevice.h:255-257. getRefreshRate returns 'unsigned short',
	// not 'int'.
	void setBackgroundColor(Color color);
	void setFullscreen(bool enable_fullscreen);
	unsigned short getRefreshRate();

	Image *createImage(int width, int height);
	Image *loadImage(const std::string& filename, int error_type);
	void loadQueuedImages();

	void setGamma(float g);
	void resetGamma();
	void updateTitleBar();

protected:
	int createContextInternal();
	void createContextError();

private:
	// Private pure virtual in the base class. It still has to be implemented here.
	void getWindowSize(short unsigned *screen_w, short unsigned *screen_h);
};

#endif // NULLRENDERDEVICE_H
