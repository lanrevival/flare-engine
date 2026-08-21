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

#ifndef NULLSOUNDMANAGER_H
#define NULLSOUNDMANAGER_H

#include "SoundManager.h"

/** A SoundManager that opens no audio device.
 *
 * Used by the dedicated server. No SDL_mixer call is made from this file.
 *
 * load() returns SoundID 0, which is the engine's existing "no sound" value -- it is what
 * SDLSoundManager::load() already returns when settings->audio is false. Every call site
 * therefore already handles it: play() early-returns on !sid and unload() finds nothing.
 * Inventing a fake non-zero ID would take an untested path instead of a well-trodden one.
 *
 * @class NullSoundManager
 * @see SoundManager
 */
class NullSoundManager : public SoundManager {
public:
	NullSoundManager();
	virtual ~NullSoundManager();

	SoundID load(const std::string& filename, const std::string& errormessage);
	void unload(SoundID sid);
	void play(SoundID sid, const std::string& channel, const FPoint& pos, bool loop, bool cleanup = true);
	void pauseChannel(const std::string& channel);
	void pauseAll();
	void resumeAll();
	void setVolumeSFX(int value);

	void loadMusic(const std::string& filename);
	void unloadMusic();
	void playMusic();
	void stopMusic();
	void setVolumeMusic(int value);
	bool isPlayingMusic();

	void logic();
	void reset();

	SoundID getLastPlayedSID();
};

#endif // NULLSOUNDMANAGER_H
