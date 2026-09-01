/*
Copyright © 2011-2012 Clint Bellanger
Copyright © 2012 Igor Paliychuk
Copyright © 2012 Stefan Beller
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
 * Settings
 */

#ifndef SETTINGS_H
#define SETTINGS_H

#include "CommonIncludes.h"

#include <typeinfo>

class Settings {
public:
	/** The simulation rate, in Hz. Fixed and shared: two peers must agree on what tick N means.
	 *
	 * This is NOT max_frames_per_sec, which is a render frame limit and a user preference.
	 * Anything converting between seconds and ticks -- durations, speeds, regen, timers --
	 * uses this. Only the render pacing and the frame-limit setting itself use max_frames_per_sec.
	 *
	 * unsigned short deliberately, matching max_frames_per_sec, so the integer timer arithmetic
	 * throughout the engine stays integer.
	 */
	static const unsigned short SIM_TICK_HZ;

	enum {
		LOOT_TIPS_DEFAULT = 0,
		LOOT_TIPS_SHOW_ALL = 1,
		LOOT_TIPS_HIDE_ALL = 2
	};

	enum {
		MINIMAP_NORMAL = 0,
		MINIMAP_2X = 1,
		MINIMAP_HIDDEN = 2
	};

	enum {
		LHP_WARN_NONE = 0,
		LHP_WARN_ALL = 1,
		LHP_WARN_TEXT_CURSOR = 2,
		LHP_WARN_TEXT_SOUND = 3,
		LHP_WARN_CURSOR_SOUND = 4,
		LHP_WARN_TEXT = 5,
		LHP_WARN_CURSOR = 6,
		LHP_WARN_SOUND = 7
	};

	static const int JOY_DEADZONE_MIN = 8000;
	static const int JOY_DEADZONE_MAX = 32767;

	Settings();
	void loadSettings();
	void saveSettings();
	void loadDefaults();
	void logSettings();
	void setCustomPathData();
	void setGame();

	// Video Settings
	bool fullscreen;
	unsigned short screen_w;
	unsigned short screen_h;
	bool hwsurface;
	bool vsync;
	bool texture_filter;
	bool dpi_scaling;
	unsigned short max_frames_per_sec;
	std::string render_device_name;
	bool change_gamma;
	float gamma;
	bool parallax_layers;
	unsigned short min_render_size;
	unsigned short max_render_size;
	bool fade_walls;

	// Audio Settings
	unsigned short music_volume;
	unsigned short sound_volume;
	bool mute_on_focus_loss;
	unsigned int audio_freq;

	// Input Settings
	bool mouse_move;
	bool mouse_move_swap;
	bool mouse_move_attack;
	bool enable_joystick;
	int joystick_device;
	bool mouse_aim;
	bool no_mouse;
	int joy_deadzone;
	float touch_scale;
	bool joystick_rumble;

	// Game Settings
	bool auto_equip;
	int auto_loot;
	int low_hp_warning_type;
	int low_hp_threshold;

	// Interface Settings
	bool combat_text;
	bool show_fps;
	bool colorblind;
	bool hardware_cursor;
	bool dev_mode;
	bool dev_hud;
	int loot_tooltips;
	bool statbar_labels;
	bool statbar_autohide;
	bool subtitles;
	int minimap_mode;
	bool entity_markers;
	bool item_compare_tips;
	bool pause_on_focus_loss;

	// Language Settings
	std::string language;

	// Misc
	int prev_save_slot;
	bool setup_language;
	bool setup_mousemove;
	bool enable_threaded_image_load;

	// Dev console: shortcut commands
	std::string dev_cmd_1;
	std::string dev_cmd_2;
	std::string dev_cmd_3;

	/**
	 * NOTE Everything below is not part of the user's settings.txt, but somehow ended up here
	 * TODO Move these to more appropriate locations?
	 */

	// Path info
	std::string path_conf; // user-configurable settings files
	std::string path_user; // important per-user data (saves)
	std::string path_data; // common game data
	std::string custom_path_data; // user-defined replacement for PATH_DATA
	bool custom_path_data_clear;
	bool custom_path_data_save;
	bool custom_path_data_ignore;
	std::string game; // if set, a sub-dir will be used in PATH_CONF. The config menu's mod list will also be filtered to match.

	// Command-line settings
	std::string load_slot;
	std::string load_script;
	std::string net_connect_target; // "<host>:<port>" -- empty means not a network client. See P3.4b.
	unsigned short net_host_port; // 0 means not an embedded host. See P3.4c. Mutually exclusive
	                               // with net_connect_target -- main.cpp refuses both being set.
	int net_max_players; // only meaningful with net_host_port set. D3: 2-8. See P3.4c.

	// P3.7: reuses the existing 'headless' field below (originally "true only for flare-server",
	// from the P0.2a/P1.4b era when main_server.cpp still drove GameSwitcher/GameStatePlay
	// headlessly -- see GameSwitcher.cpp:68's intro-cutscene skip and GameStatePlay.cpp's
	// drainSimEvents()/isPaused() checks, both still live and both exactly what a headless flare
	// client needs, just unreachable from flare-server since P1.4c excluded those two files from
	// its link entirely). --headless now sets it for flare too. Mirrors ServerCmdLineArgs' other
	// field names in main_server.cpp deliberately, since tests/run-net.sh compares both processes'
	// output.
	unsigned long max_ticks; // 0 = unbounded. See main.cpp's mainLoop().
	std::string script_path; // --script=<file>; empty with --headless means NullInputState, not scripted.
	bool hash_replicated; // --hash-replicated: WorldHash::computeReplicated() instead of compute().
	unsigned long hash_every; // 0 = never print. See main.cpp's mainLoop().

	// Misc
	unsigned short view_w;
	unsigned short view_h;
	unsigned short view_w_half;
	unsigned short view_h_half;
	float view_scaling;

	bool audio;

	bool touchscreen;
	bool mouse_scaled; // mouse position is automatically scaled to view_w * view_h resolution

	bool show_hud;

	bool soft_reset;

	bool safe_video;

	bool no_lock_file;

	// True only for flare-server. Suppresses presentation-only work that a headless
	// process should never do, such as playing the intro cutscene in real time.
	bool headless;

private:
	class ConfigEntry {
	public:
		std::string name;
		const std::type_info *type;
		std::string default_val;
		void *storage;
		std::string comment;
	};
	std::vector<ConfigEntry> config;

	void setConfigDefault(size_t index, const std::string& name, const std::type_info *type, const std::string& default_val, void *storage, const std::string& comment);
	size_t getConfigEntry(const std::string& name);
	void loadMobileDefaults();
	std::string configValueToString(const std::type_info &type, void *storage);
};
#endif
