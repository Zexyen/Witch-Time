#include "game_host.h"
#include "platform.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#ifndef _WIN32
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

// program 0 is the game; program N (the picture book's story is 1) has its own folder in targets/ and build/
std::string program_dir(int program) { return program ? "/program" + std::to_string(program) : ""; }
std::string main_elf(const Paths& paths, int program = 0) { return paths.repo + "/targets" + program_dir(program) + "/exefs/main.elf"; }
std::string program_build(const Paths& paths, int program) { return paths.build + program_dir(program); }

// the save folder, where the runtime leaves the switch file (runtime/b3_imports.c switch_path; ..\userdata from build/run)
std::string switch_file(const Paths& paths)
{
	const char* e = std::getenv("WT_PROGRAM_SWITCH");
	if (e && e[0])
		return e;
	const char* u = std::getenv("WT_USERDATA");
	return (u && u[0] ? std::string(u) : paths.build + "/userdata") + "/program_switch.bin";
}

#ifndef _WIN32
std::string g_self;
volatile int32_t* g_display_close = nullptr;  // runtime/display.c (a LONG): the player closed the window
bool g_relaunch = true;                       // the launcher starts again when the game ends (not with --play)
int g_out = -1, g_err = -1;                   // the launcher's own stdout / stderr while the game writes to its log

// The game's output goes to build/run/witch-time-game.log, as on Windows: a launcher started from a desktop entry has no
// terminal, and the log is what a bug report needs
void log_to_file(const Paths& paths)
{
	std::string log = paths.run + "/witch-time-game.log";
	std::fprintf(stderr, "witch-time: the game's output goes to %s\n", log.c_str());
	std::fflush(nullptr);
	int fd = open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
		return;
	g_out = fcntl(1, F_DUPFD_CLOEXEC, 3);
	g_err = fcntl(2, F_DUPFD_CLOEXEC, 3);
	dup2(fd, 1);
	dup2(fd, 2);
	close(fd);
}

// runtime/plat_posix.c calls this from ExitProcess, on whichever thread ends the game
void on_game_exit(int code)
{
	const bool to_program = code == PROGRAM_SWITCH_EXIT;   // ExecuteProgram: the launcher starts the other program at once
	if ((g_display_close && *g_display_close) || (!g_relaunch && !to_program))
		return;                                // closing the window ends everything, as it always did
	std::fflush(nullptr);
	if (g_out >= 0)                            // the launcher that starts again writes where this one did
		dup2(g_out, 1);
	if (g_err >= 0)
		dup2(g_err, 2);
	std::string c = std::to_string(code);
	if (to_program && !g_relaunch)             // started with --play: it plays the next program the same way
		execl(g_self.c_str(), g_self.c_str(), "--play", "--returned", c.c_str(), (char*)nullptr);
	execl(g_self.c_str(), g_self.c_str(), "--returned", c.c_str(), (char*)nullptr);
}
#endif

} // namespace

int program_switch_target(const Paths& paths)
{
	unsigned char h[12];
	FILE* f = std::fopen(switch_file(paths).c_str(), "rb");
	if (!f)
		return -1;
	bool ok = std::fread(h, 1, sizeof h, f) == sizeof h && !std::memcmp(h, "WTPS", 4);
	std::fclose(f);
	uint32_t index = 0;
	std::memcpy(&index, h + 4, 4);
	return ok && index < 8 ? (int)index : -1;
}

void program_switch_drop(const Paths& paths) { std::remove(switch_file(paths).c_str()); }

GameStart prepare_game(const Paths& paths, int program, bool after_switch)
{
	GameStart g;
	g.program = program;
	const std::string build = program_build(paths, program);
	if (!file_exists(main_elf(paths, program)) || !file_exists(build + "/" + GAME_LIB) || !file_exists(build + "/" + GAME_EXE))
	{
		g.error = program ? "The Old Picture Book opens a second program of the game, which is not built: Rebuild with a dump that has it (README)."
			: "The game is not built yet.";
		return g;
	}
	make_dir(paths.run);
	// as docs/STATUS.md runs it: the dump's data through targets/romfs, saves in build/userdata (..\userdata from build/run).
	// After another program ran, by a switch or before it crashed, the variables it had are set again (Linux starts the
	// launcher again in the same environment)
	const char* last = std::getenv("WT_PROGRAM");
	const bool other = after_switch || (last && std::strcmp(last, std::to_string(program).c_str()) != 0);
	set_env("WT_ROMFS", paths.repo + "/targets" + program_dir(program) + "/romfs", other);
	set_env("WT_PROGRAM", std::to_string(program), true);
	if (program || other)
	{                                            // each program its own shaders: the container store is tied to its root_000.pkz
		const std::string own = program ? "-program" + std::to_string(program) : "";
		set_env("WT_VK_CACHE", paths.build + "/cache/vk" + own, true);
		set_env("WT_VK_CONTAINERS", paths.build + "/cache/vk_containers2" + own, true);
	}
	// program N's exe is in build/programN, where the runtime's <exe>/../config would be build/config
	const std::string config = paths.config + "/controllers.ini";
	const char* c = std::getenv("WT_CONFIG");
	if (program)
		set_env("WT_CONFIG", config, false);
	else if (c && config == c)
		unset_env("WT_CONFIG");                  // ours, from program N: program 0 finds config/ itself, and refreshes graphics.ini
	set_env("WT_TITLE", "Witch Time", false);
	set_env("WT_VK_MISSING", "warn", false);
	// Linux runs the game in this process, named witch-time, which nnaudio.c's test-copy rule would leave silent
	set_env("WT_VOLUME", "100", false);
	if (!change_dir(paths.run))
	{
		g.error = "Cannot open " + paths.run + ".";
		return g;
	}
#ifdef _WIN32
	// Windows: the runtime makes a window of its own (runtime/display.c), so the game always runs as a program of its own
	return g;
#else
	const std::string lib = build + "/libwitchtime.so";
	if (file_exists(lib))
	{
		g.host = dlopen(lib.c_str(), RTLD_NOW | RTLD_GLOBAL);
		if (!g.host)
			std::fprintf(stderr, "witch-time: %s; starting witch-time-game instead\n", dlerror());
	}
	g.in_process = g.host && dlsym(g.host, "wt_host_main") && dlsym(g.host, "plat_exit_hook");
	return g;
#endif
}

#ifdef _WIN32
void run_in_process(GameStart&, const Paths&, SDL_Window*, bool) { std::abort(); }   // prepare_game never offers it

int run_child(const Paths& paths, SDL_Window* window, int program)
{
	SDL_HideWindow(window);
	// its output goes to build/run/witch-time-game.log: this program has no console to show it in
	ChildProcess game;
	int code = -1;
	if (game.start({program_build(paths, program) + "/" + GAME_EXE, main_elf(paths, program)}, paths.run, paths.run + "/witch-time-game.log"))
		while (!game.wait(&code, 50))
			SDL_PumpEvents();
	SDL_ShowWindow(window);
	SDL_RaiseWindow(window);
	return code;
}
#else
void run_in_process(GameStart& start, const Paths& paths, SDL_Window* window, bool relaunch)
{
	g_relaunch = relaunch;
	using HostMain = int (*)(int, char**);
	auto host_main = reinterpret_cast<HostMain>(dlsym(start.host, "wt_host_main"));
	auto exit_hook = static_cast<void (**)(int)>(dlsym(start.host, "plat_exit_hook"));
	g_display_close = static_cast<volatile int32_t*>(dlsym(start.host, "g_display_close"));
	g_self = paths.self;
	*exit_hook = on_game_exit;
	SDL_SetPointerProperty(SDL_GetGlobalProperties(), "witchtime.window", window);
	std::string exe = program_build(paths, start.program) + "/witch-time-game", elf = main_elf(paths, start.program);
	char* argv[] = {exe.data(), elf.data(), nullptr};
	log_to_file(paths);
	int code = host_main(2, argv);
	on_game_exit(code);
	std::fflush(nullptr);
	_exit(code);
}

int run_child(const Paths& paths, SDL_Window* window, int program)
{
	SDL_HideWindow(window);
	std::string exe = program_build(paths, program) + "/witch-time-game", elf = main_elf(paths, program);
	std::fflush(nullptr);                      // the child must not write the launcher's pending output again
	pid_t pid = fork();
	if (pid == 0)
	{
		log_to_file(paths);
		execl(exe.c_str(), exe.c_str(), elf.c_str(), (char*)nullptr);
		_exit(127);
	}
	int status = 0;
	while (pid > 0 && waitpid(pid, &status, WNOHANG) == 0)
	{
		SDL_PumpEvents();
		SDL_Delay(50);
	}
	SDL_ShowWindow(window);
	SDL_RaiseWindow(window);
	return pid < 0 ? -1 : WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
#endif
