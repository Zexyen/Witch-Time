// The files the launcher reads and writes: where things are, config/graphics.ini, config/mods.ini, the mods list and
// build/setup_state.json. Everything here is synchronous and small.
#pragma once
#include <string>
#include <vector>

struct Paths {
	std::string repo;          // the checkout: the launcher lives in <repo>/build
	std::string build, ui, config, mods, run;
	std::string self;          // this program
	static Paths find(const char* argv0);
};

// One key of an ini file; set() keeps every other line, comments included, as the runtime's own writer does.
std::string ini_get(const std::string& file, const std::string& section, const std::string& key, const std::string& def);
bool ini_set(const std::string& file, const std::string& section, const std::string& key, const std::string& value);

// build/setup_state.json, as the build (wtbuild setup) leaves it
struct SetupState {
	std::string dump;                  // the dump folder an earlier run used
	std::string mods_dir;              // a mods folder given with --mods (else <repo>/mods)
	std::vector<std::string> done;     // finished steps
	bool has(const std::string& step) const;
	static SetupState read(const std::string& build_dir);
};

struct ModEntry {
	std::string key, label, note;
	bool on = true;
	bool code = true;                   // patches code (a change rebuilds); false: data files only, read at start
	bool data = false;                  // also holds data files (romfs)
};
// wtbuild mods-tree: the mods the build would use. error: what went wrong, or empty.
std::vector<ModEntry> list_mods(const Paths& paths, const std::string& mods_dir, std::string& error);

bool file_exists(const std::string& path);
// The build tool and the arguments before a command's own: wtbuild (tools/wtbuild; the release ships it in build/).
// Empty in a checkout where it was never built: NO_BUILD_TOOL is what to tell the player. command: "setup", "mods-tree"...
std::vector<std::string> build_command(const std::string& repo, const std::string& command);
#define NO_BUILD_TOOL "The build tool is not built. In the Witch Time folder, run: cargo build --release --manifest-path tools/wtbuild/Cargo.toml"
std::string run_capture(const std::vector<std::string>& argv, const std::string& cwd, int* exit_code);
