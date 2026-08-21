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

#include "NullRenderDevice.h"
#include "CursorManager.h"
#include "IconManager.h"
#include "Settings.h"
#include "SharedResources.h"
#include "Utils.h"

NullImage::NullImage(RenderDevice *device, int width, int height)
	: Image(device)
	, w(width)
	, h(height)
{
	// A zero-sized image causes divide-by-zero in animation and layout code that computes
	// frame counts from image dimensions, so never allow one.
	if (w < 1) w = 1;
	if (h < 1) h = 1;
}

NullImage::~NullImage() {
}

int NullImage::getWidth() const {
	return w;
}

int NullImage::getHeight() const {
	return h;
}

void NullImage::fillWithColor(const Color& color) {
	(void)color;
}

void NullImage::drawPixel(int x, int y, const Color& color) {
	(void)x; (void)y; (void)color;
}

void NullImage::drawLine(int x0, int y0, int x1, int y1, const Color& color) {
	(void)x0; (void)y0; (void)x1; (void)y1; (void)color;
}

void NullImage::drawFilledRect(int x, int y, int w_, int h_, const Color& color) {
	(void)x; (void)y; (void)w_; (void)h_; (void)color;
}

Image* NullImage::resize(int width, int height) {
	// Matches the SDL devices: resize() hands back a new image and does not touch this one.
	return new NullImage(device, width, height);
}


NullRenderDevice::NullRenderDevice() {
	Utils::logInfo("NullRenderDevice: no window, no GPU, nothing is drawn.");
}

NullRenderDevice::~NullRenderDevice() {
}

int NullRenderDevice::createContextInternal() {
	// There is no context to create. Reporting success is correct: the base class
	// createContext() retries with degraded video settings on -1 and calls Utils::Exit(1)
	// if every attempt fails.
	is_initialized = true;

	// The SDL devices create these here, and the rest of the engine assumes they exist --
	// CursorManager::logic() is called every frame by GameSwitcher::logic() and dereferences
	// 'curs' without a null check. Both load their images through this device, so they cost
	// nothing here. Mirrors SDLSoftwareRenderDevice.cpp:344-349.
	delete icons;
	icons = new IconManager();
	delete curs;
	curs = new CursorManager();

	return 0;
}

void NullRenderDevice::createContextError() {
	Utils::logError("NullRenderDevice: createContextError() called, which should be impossible.");
}

void NullRenderDevice::destroyContext() {
	// Graphics are tied to the context in the SDL devices, and the base class asserts the
	// image cache is empty. Mirror that here so the server exercises the same teardown path.
	RenderDevice::cacheRemoveAll();
	reload_graphics = true;
}

void NullRenderDevice::getWindowSize(short unsigned *screen_w, short unsigned *screen_h) {
	if (screen_w) *screen_w = static_cast<short unsigned>(settings->screen_w);
	if (screen_h) *screen_h = static_cast<short unsigned>(settings->screen_h);
}

int NullRenderDevice::render(Renderable& r, Rect& dest) {
	(void)r; (void)dest;
	return 0;
}

int NullRenderDevice::render(Sprite* r) {
	(void)r;
	return 0;
}

int NullRenderDevice::renderToImage(Image* src_image, Rect& src, Image* dest_image, Rect& dest) {
	(void)src_image; (void)src; (void)dest_image; (void)dest;
	return 0;
}

Image* NullRenderDevice::renderTextToImage(FontStyle* font_style, const std::string& text, const Color& color, bool blended) {
	(void)font_style; (void)text; (void)color; (void)blended;
	// Must not return NULL: callers wrap the result in a Sprite without checking.
	return new NullImage(this, 1, 1);
}

void NullRenderDevice::drawPixel(int x, int y, const Color& color) {
	(void)x; (void)y; (void)color;
}

void NullRenderDevice::drawLine(int x0, int y0, int x1, int y1, const Color& color) {
	(void)x0; (void)y0; (void)x1; (void)y1; (void)color;
}

void NullRenderDevice::drawRectangle(const Point& p0, const Point& p1, const Color& color) {
	(void)p0; (void)p1; (void)color;
}

void NullRenderDevice::blankScreen() {
}

void NullRenderDevice::commitFrame() {
}

void NullRenderDevice::windowResize() {
}

void NullRenderDevice::setBackgroundColor(Color color) {
	(void)color;
}

void NullRenderDevice::setFullscreen(bool enable_fullscreen) {
	(void)enable_fullscreen;
}

unsigned short NullRenderDevice::getRefreshRate() {
	// Reported as the logic rate so that anything deriving a frame budget from this gets a
	// sane number rather than zero.
	return static_cast<unsigned short>(Settings::LOGIC_FPS);
}

Image *NullRenderDevice::createImage(int width, int height) {
	// Not cached, matching the SDL devices: createImage() has no filename to key on.
	return new NullImage(this, width, height);
}

Image *NullRenderDevice::loadImage(const std::string& filename, int error_type) {
	(void)error_type;

	Image *img = cacheLookup(filename);
	if (img != NULL)
		return img;

	// Deliberately never fails. The file is not opened at all -- the server has no use for
	// the pixels, and reporting failure would send required images down the ERROR_EXIT path.
	NullImage *image = new NullImage(this, 1, 1);
	cacheStore(filename, image);
	return image;
}

void NullRenderDevice::loadQueuedImages() {
	cleanupQueuedImages();
}

void NullRenderDevice::setGamma(float g) {
	(void)g;
}

void NullRenderDevice::resetGamma() {
}

void NullRenderDevice::updateTitleBar() {
}
