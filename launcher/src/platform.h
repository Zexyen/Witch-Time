// What differs between Linux and Windows for the launcher: file names, folders, the environment, and (Windows) starting
// programs with their output read through a pipe. Paths are UTF-8 with '/' on both; on Windows the program runs with
// UTF-8 as its code page (witch-time.manifest), so the C library's file functions take them as they are.
#pragma once
#include <string>
#include <vector>

#ifdef _WIN32
#define WT_EXE ".exe"
#else
#define WT_EXE ""
#endif
// the files the build makes in build/ (tools/wtbuild/src/target.rs)
#define BUILT_VERSION "built_version"    // in build/: the launcher release that last finished a build
inline const char* const GAME_EXE = "witch-time-game" WT_EXE;
#ifdef _WIN32
inline const char* const GAME_LIB = "game.dll";
#else
inline const char* const GAME_LIB = "game.so";
#endif

bool make_dir(const std::string& path);
// setenv(name, value, overwrite)
void set_env(const char* name, const std::string& value, bool overwrite);
void unset_env(const char* name);
bool change_dir(const std::string& path);
// a program file that can be started (on Windows: that exists)
bool is_program(const std::string& path);

#ifdef _WIN32
std::wstring widen(const std::string& s);
std::string narrow(const std::wstring& s);

// A program started with its standard output and error going into one pipe (or a file), without a console window, in
// a job of its own so that stop() also ends what it started (the compilers of a build).
class ChildProcess {
public:
	ChildProcess() = default;
	ChildProcess(const ChildProcess&) = delete;
	ChildProcess& operator=(const ChildProcess&) = delete;
	~ChildProcess();
	// argv[0] is the program; cwd may be empty. log_file: write the output there instead of the pipe.
	bool start(const std::vector<std::string>& argv, const std::string& cwd, const std::string& log_file = "");
	// what the pipe holds now, without waiting
	std::string read_available();
	// true once it has ended; its exit code in *code
	bool finished(int* code);
	bool wait(int* code, unsigned ms);
	void stop();
	bool running() const { return process != nullptr; }
	void reset();                        // lets go of it (its handles), running or not

private:
	void* process = nullptr;
	void* job = nullptr;
	void* pipe = nullptr;
};
#endif
