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

#include "NullSoundManager.h"
#include "Utils.h"

NullSoundManager::NullSoundManager() {
	Utils::logInfo("NullSoundManager: no audio device, nothing is played.");
}

NullSoundManager::~NullSoundManager() {
}

SoundID NullSoundManager::load(const std::string& filename, const std::string& errormessage) {
	(void)filename; (void)errormessage;
	return 0;
}

void NullSoundManager::unload(SoundID sid) {
	(void)sid;
}

void NullSoundManager::play(SoundID sid, const std::string& channel, const FPoint& pos, bool loop, bool cleanup) {
	(void)sid; (void)channel; (void)pos; (void)loop; (void)cleanup;
}

void NullSoundManager::pauseChannel(const std::string& channel) {
	(void)channel;
}

void NullSoundManager::pauseAll() {
}

void NullSoundManager::resumeAll() {
}

void NullSoundManager::setVolumeSFX(int value) {
	(void)value;
}

void NullSoundManager::loadMusic(const std::string& filename) {
	(void)filename;
}

void NullSoundManager::unloadMusic() {
}

void NullSoundManager::playMusic() {
}

void NullSoundManager::stopMusic() {
}

void NullSoundManager::setVolumeMusic(int value) {
	(void)value;
}

bool NullSoundManager::isPlayingMusic() {
	return false;
}

void NullSoundManager::logic() {
}

void NullSoundManager::reset() {
}

SoundID NullSoundManager::getLastPlayedSID() {
	// Used for subtitles. Nothing is ever played, so nothing was played last.
	return 0;
}
