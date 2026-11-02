#include "game_host.h"
#include "platform.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#ifndef _WIN32
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

std::string main_elf(const Paths& paths) { return paths.repo + "/targets/exefs/main.elf"; }

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
	if ((g_display_close && *g_display_close) || !g_relaunch)
		return;                                // closing the window ends everything, as it always did
	std::fflush(nullptr);
	if (g_out >= 0)                            // the launcher that starts again writes where this one did
		dup2(g_out, 1);
	if (g_err >= 0)
		dup2(g_err, 2);
	std::string c = std::to_string(code);
	execl(g_self.c_str(), g_self.c_str(), "--returned", c.c_str(), (char*)nullptr);
}
#endif

} // namespace

GameStart prepare_game(const Paths& paths)
{
	GameStart g;
	if (!file_exists(main_elf(paths)) || !file_exists(paths.build + "/" + GAME_LIB) || !file_exists(paths.build + "/" + GAME_EXE))
	{
		g.error = "The game is not built yet.";
		return g;
	}
	make_dir(paths.run);
	// as docs/STATUS.md runs it: the dump's data through targets/romfs, saves in build/userdata (..\userdata from build/run)
	set_env("WT_ROMFS", paths.repo + "/targets/romfs", false);
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
	const std::string lib = paths.build + "/libwitchtime.so";
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

int run_child(const Paths& paths, SDL_Window* window)
{
	SDL_HideWindow(window);
	// its output goes to build/run/witch-time-game.log: this program has no console to show it in
	ChildProcess game;
	int code = -1;
	if (game.start({paths.build + "/" + GAME_EXE, main_elf(paths)}, paths.run, paths.run + "/witch-time-game.log"))
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
	std::string exe = paths.build + "/witch-time-game", elf = main_elf(paths);
	char* argv[] = {exe.data(), elf.data(), nullptr};
	log_to_file(paths);
	int code = host_main(2, argv);
	on_game_exit(code);
	std::fflush(nullptr);
	_exit(code);
}

int run_child(const Paths& paths, SDL_Window* window)
{
	SDL_HideWindow(window);
	std::string exe = paths.build + "/witch-time-game", elf = main_elf(paths);
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
