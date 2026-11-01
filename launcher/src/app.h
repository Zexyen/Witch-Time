// The launcher: one SDL window, RmlUi screens on SDL's GPU API (ui/launcher.rml), the build (BuildRunner) and the start
// of the game (game_host). Screens: title menu, first run (the dump), building, mods, settings.
#pragma once
#include "build_runner.h"
#include "config.h"
#include "RmlUi_Platform_SDL.h"
#include "ui_render.h"
#include <RmlUi/Core.h>
#include <SDL3/SDL.h>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct CheckRow { Rml::String text, right, state; };
struct StepRow { Rml::String num, name, note, right, state; };
struct ModRow { Rml::String name, note, kind; bool on = true; };
struct SettingRow { Rml::String name, note, value; int index = 0; };   // index: the row in setting_defs()
struct MenuRow { Rml::String label, tag; std::string action; bool dim = false; };   // a bar of the title's staircase

class App {
public:
	bool init(int argc, char** argv);
	int run();

private:
	Paths paths;
	SDL_Window* window = nullptr;
	SDL_GPUDevice* gpu = nullptr;
	std::unique_ptr<UiRender> render;
	std::unique_ptr<SystemInterface_SDL> system;
	TextInputMethodEditor_SDL ime;
	Rml::Context* ctx = nullptr;
	Rml::DataModelHandle model;
	std::vector<SDL_Gamepad*> pads;
	int prompts = 4;                // footer hints: 1 Switch, 2 PlayStation, 3 Xbox, 4 keyboard
	bool prompts_fixed = false;     // [general] button_prompts names one; else they follow what was used last
	void prompts_read();
	void prompts_use(int family);
	void prompts_use(SDL_Gamepad* pad);
	std::vector<std::vector<Rml::byte>> font_data;

	// what the screens show (the data model "wt")
	Rml::String screen = "title";
	bool built = false, status_bad = false;
	float hour_deg = 0, minute_deg = 0;   // the title dial's hands, at this machine's local time
	Rml::String status_title, status_line, footer_info, version;
	std::vector<MenuRow> menu;
	Rml::String update_tag;            // a newer release to install, or empty
	bool stale = false;                // built by another release than this launcher
	Rml::String dump;
	bool checking = false, verified = false;
	bool move_dump = false;   // setup moves the dump's game data into the build folder instead of copying it
	bool dump_modified = false;    // the last check found files that differ from the supported version
	bool accept_modified = false;  // setup runs with --accept-modified: such a dump is used anyway
	std::vector<CheckRow> checks;
	bool building = false;
	float progress = 0;
	int percent = 0;
	Rml::String phase, build_aside, build_line, build_error;
	bool show_log = false;             // the build screen's log box
	std::vector<StepRow> steps;
	std::vector<Rml::String> log;
	std::vector<ModRow> mods;
	std::vector<ModEntry> mod_entries;
	Rml::String mods_summary, mods_error;
	bool mods_rebuild = false;
	std::vector<SettingRow> settings;  // the rows of the tab shown
	int tab = 0;                       // settings: 0 Display, 1 Graphics, 2 Controls
	Rml::String tex_note, tex_action;  // settings: the converted textures (#54)
	bool tex_remove = false;

	// work for the main loop
	BuildRunner runner;
	enum class Job { none, verify, build, textures } job = Job::none;
	bool running = true, want_play = false, want_focus = true;
	bool play_only = false;            // --play: the game starts at once, and the launcher ends with it
	int returned_code = -1;            // --returned <code>: the game just ended
	std::mutex picked_mutex;
	std::string picked_folder;         // from the folder dialog (another thread)
	bool picked = false;
	int update_seen = 0;               // the update's state and line as the title last showed them
	Rml::String update_message;

	bool create_window();
	bool create_ui();
	void bind_model();
	void handle(SDL_Event& ev);
	bool key(Rml::Input::KeyIdentifier k);
	void back();
	void tick();
	void tick_clock();
	void draw();
	void rescale();
	void focus_first();

	void go(const Rml::String& to);
	void refresh_title();
	void refresh_build();
	void refresh_mods();
	void refresh_settings();
	void set_tab(int t);
	void step_setting(int index, int dir);
	void convert_textures();
	void remove_textures();
	void toggle_mod(int index);
	void check_dump();
	void start_build();
	void browse();
	void play();
	void release_window();
};
