#include "config.h"
#include "platform.h"
#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

std::string dirname(const std::string& p)
{
	size_t s = p.find_last_of('/');
	return s == std::string::npos ? "." : s == 0 ? "/" : p.substr(0, s);
}

std::string trim(const std::string& s)
{
	size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
	return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

std::string lower(std::string s)
{
	for (char& c : s)
		c = char(std::tolower((unsigned char)c));
	return s;
}

std::vector<std::string> read_lines(const std::string& file)
{
	std::vector<std::string> lines;
	std::ifstream f(file);
	for (std::string l; std::getline(f, l);)
		lines.push_back(l);
	return lines;
}

// the key of a "key = value" line, or empty for a comment, a section or anything else
std::string line_key(const std::string& line)
{
	std::string t = trim(line);
	if (t.empty() || t[0] == ';' || t[0] == '#' || t[0] == '[')
		return "";
	size_t eq = t.find('=');
	return eq == std::string::npos ? "" : lower(trim(t.substr(0, eq)));
}

std::string section_of(const std::string& line)
{
	std::string t = trim(line);
	return t.size() > 1 && t[0] == '[' && t.back() == ']' ? lower(trim(t.substr(1, t.size() - 2))) : "";
}

// the JSON string value after "key": in text (enough for setup_state.json: plain paths, \\ and \" escapes)
std::string json_string(const std::string& text, const std::string& key)
{
	size_t k = text.find("\"" + key + "\"");
	if (k == std::string::npos)
		return "";
	size_t q = text.find('"', text.find(':', k) + 1);
	if (q == std::string::npos || text.find_first_not_of(" \t\r\n", text.find(':', k) + 1) != q)
		return "";
	std::string out;
	for (size_t i = q + 1; i < text.size() && text[i] != '"'; i++)
		out += text[i] == '\\' && i + 1 < text.size() ? text[++i] : text[i];
	return out;
}

} // namespace

bool file_exists(const std::string& path)
{
	struct stat st;
	return stat(path.c_str(), &st) == 0;
}

std::vector<std::string> build_command(const std::string& repo, const std::string& command)
{
	for (const std::string& exe : {repo + "/build/wtbuild" WT_EXE, repo + "/tools/wtbuild/target/release/wtbuild" WT_EXE})
		if (is_program(exe))
			return {exe, command, "--repo", repo};
	return {};
}

Paths Paths::find(const char* argv0)
{
	Paths p;
#ifdef _WIN32
	std::wstring buf(32768, L'\0');
	DWORD n = GetModuleFileNameW(nullptr, buf.data(), DWORD(buf.size()));
	p.self = n > 0 ? narrow(buf.substr(0, n)) : std::string(argv0);
	std::replace(p.self.begin(), p.self.end(), '\\', '/');    // '/' throughout, which Windows takes too
#else
	char buf[PATH_MAX];
	ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
	p.self = n > 0 ? std::string(buf, size_t(n)) : std::string(argv0);
#endif
	const char* env = std::getenv("WITCHTIME_REPO");
	p.repo = env && env[0] ? env : dirname(dirname(p.self));
	p.build = p.repo + "/build";
	p.ui = p.repo + "/launcher/ui";
	p.config = p.repo + "/config";
	p.mods = p.repo + "/mods";
	p.run = p.build + "/run";
	return p;
}

std::string ini_get(const std::string& file, const std::string& section, const std::string& key, const std::string& def)
{
	std::string cur, want = lower(key), sec = lower(section);
	for (const std::string& l : read_lines(file))
	{
		if (std::string s = section_of(l); !s.empty())
			cur = s;
		else if ((sec.empty() || cur == sec) && line_key(l) == want)
			return trim(l.substr(l.find('=') + 1));
	}
	return def;
}

bool ini_set(const std::string& file, const std::string& section, const std::string& key, const std::string& value)
{
	std::vector<std::string> lines = read_lines(file);
	std::string cur, want = lower(key), sec = lower(section);
	bool done = false;
	int insert_at = sec.empty() ? int(lines.size()) : -1;   // after the section's last line that is not empty
	for (size_t i = 0; i < lines.size() && !done; i++)
	{
		if (std::string s = section_of(lines[i]); !s.empty())
		{
			cur = s;
			if (cur == sec)
				insert_at = int(i) + 1;
		}
		else if (sec.empty() || cur == sec)
		{
			if (line_key(lines[i]) == want)
			{
				lines[i] = key + " = " + value;
				done = true;
			}
			else if (!sec.empty() && !trim(lines[i]).empty())
				insert_at = int(i) + 1;
		}
	}
	if (!done)
	{
		if (insert_at < 0)
		{
			lines.push_back("[" + section + "]");
			insert_at = int(lines.size());
		}
		lines.insert(lines.begin() + insert_at, key + " = " + value);
	}
	std::string tmp = file + ".tmp";
	{
		std::ofstream f(tmp, std::ios::binary);
		for (const std::string& l : lines)
			f << l << '\n';
		if (!f)
			return false;
	}
	std::error_code ec;   // std::rename will not replace an existing file on Windows
	std::filesystem::rename(tmp, file, ec);
	return !ec;
}

bool SetupState::has(const std::string& step) const { return std::find(done.begin(), done.end(), step) != done.end(); }

SetupState SetupState::read(const std::string& build_dir)
{
	SetupState st;
	std::ifstream f(build_dir + "/setup_state.json");
	std::stringstream ss;
	ss << f.rdbuf();
	const std::string text = ss.str();
	st.dump = json_string(text, "dump");
	st.mods_dir = json_string(text, "mods_dir");
	size_t d = text.find("\"done\"");
	size_t open = d == std::string::npos ? d : text.find('{', d), close = open == std::string::npos ? open : text.find('}', open);
	if (close != std::string::npos)
		for (size_t i = open; (i = text.find('"', i + 1)) < close;)
		{
			size_t e = text.find('"', i + 1);
			st.done.push_back(text.substr(i + 1, e - i - 1));
			i = e;
		}
	return st;
}

std::string run_capture(const std::vector<std::string>& argv, const std::string& cwd, int* exit_code)
{
#ifdef _WIN32
	ChildProcess child;
	std::string out;
	int code = -1;
	if (child.start(argv, cwd))
		while (true)
		{
			bool done = child.wait(&code, 20);
			out += child.read_available();
			if (done)
				break;
		}
	if (exit_code)
		*exit_code = code;
	return out;
#else
	int fd[2];
	if (pipe(fd))
		return "";
	pid_t pid = fork();
	if (pid == 0)
	{
		dup2(fd[1], 1);
		dup2(fd[1], 2);
		close(fd[0]);
		close(fd[1]);
		if (chdir(cwd.c_str()))
			_exit(127);
		std::vector<char*> args;
		for (const std::string& a : argv)
			args.push_back(const_cast<char*>(a.c_str()));
		args.push_back(nullptr);
		execvp(args[0], args.data());
		_exit(127);
	}
	close(fd[1]);
	std::string out;
	char buf[4096];
	for (ssize_t n; (n = read(fd[0], buf, sizeof buf)) > 0;)
		out.append(buf, size_t(n));
	close(fd[0]);
	int status = 0;
	waitpid(pid, &status, 0);
	if (exit_code)
		*exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
	return out;
#endif
}

std::vector<ModEntry> list_mods(const Paths& paths, const std::string& mods_dir, std::string& error)
{
	std::vector<ModEntry> mods;
	error.clear();
	if (!file_exists(mods_dir))
		return mods;
	int code = 0;
	std::vector<std::string> argv = build_command(paths.repo, "mods-tree");
	if (argv.empty())
	{
		error = NO_BUILD_TOOL;
		return mods;
	}
	argv.push_back(mods_dir);
	std::string out = run_capture(argv, paths.repo, &code);
	if (code)
	{
		error = trim(out.substr(out.size() > 400 ? out.size() - 400 : 0));
		return mods;
	}
	std::istringstream in(out);
	for (std::string line; std::getline(in, line);)
	{
		std::vector<std::string> f;
		for (size_t i = 0;;)
		{
			size_t t = line.find('\t', i);
			f.push_back(line.substr(i, t == std::string::npos ? std::string::npos : t - i));
			if (t == std::string::npos)
				break;
			i = t + 1;
		}
		// mod <key> <on> <label> <note> <romfs>
		if (f.size() >= 6 && f[0] == "mod")
		{
			ModEntry m;
			m.key = f[1];
			m.on = f[2] == "1";
			m.label = f[3];
			m.note = f[4];
			m.code = f[5] != "romfs-only";
			m.data = !f[5].empty();
			mods.push_back(m);
		}
		else if (f.size() >= 3 && f[0] == "state" && f[1] == "error")
			error = f[2];
	}
	return mods;
}
