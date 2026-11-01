// Runs the build from the player's dump (wtbuild setup; scripts/setup.py where wtbuild was not built) as a child
// process and follows its output: which of its steps runs, which are done, how far the compile step is, and the last
// lines of the log. poll() never blocks.
#pragma once
#include "platform.h"
#include <ios>
#include <map>
#include <string>
#include <vector>
#include <sys/types.h>

class BuildRunner {
public:
	// the build's steps, in order (tools/wtbuild/src/setup.rs STEPS, scripts/setup.py STEPS)
	static const std::vector<std::string>& steps();

	bool start(const std::string& repo, const std::vector<std::string>& args, const std::string& command = "setup");
	void stop();                         // ends the build; it resumes from the step it was in next time
	bool poll();                         // reads what is new; true when anything changed
#ifdef _WIN32
	bool running() const { return child.running(); }
#else
	bool running() const { return pid > 0; }
#endif

	int exit_code = 0;                   // when it has ended
	std::string current;                 // the step running now
	std::vector<std::string> done;       // steps finished in this run
	std::map<std::string, std::string> took;   // and how long each took, as the build says it
	std::string failed;                  // the step that failed
	std::vector<std::string> lines;      // the output, without the step lines
	int compiled = 0, to_compile = 0;    // the compile step's progress ([n/total] in build/logs/compile.log), or which archive `textures` is at
	double started_at = 0;

private:
#ifdef _WIN32
	ChildProcess child;
#else
	pid_t pid = -1;
	int fd = -1;
#endif
	std::string repo, partial;
	double last_log_check = 0;
	long long compile_log_start = 0;
	void line(const std::string& l);
	void read_compile_log();
};

double now_seconds();
