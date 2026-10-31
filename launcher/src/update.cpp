#include "update.h"
#include "platform.h"
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace update {
namespace {

const char* const LATEST = "https://api.github.com/repos/Zexyen/Witch-Time/releases/latest";
#ifdef _WIN32
const char* const ASSET = "-windows-x86_64.zip";
#else
const char* const ASSET = "-linux-x86_64.tar.gz";
#endif

// one update per run, shared with its thread; never freed, so a thread still downloading at exit finds it
struct Shared {
	std::mutex m;
	State state = State::none;
	std::string tag, url, sha256, message;
};
Shared& shared()
{
	static Shared* s = new Shared;
	return *s;
}
void set(State st, const std::string& message)
{
	std::lock_guard<std::mutex> l(shared().m);
	shared().state = st;
	shared().message = message;
}

fs::path to_path(const std::string& s)
{
#ifdef _WIN32
	return fs::path(widen(s));
#else
	return fs::path(s);
#endif
}

// "v1.2.3" as a number to compare; -1 when it is not one (a dev build)
long long version_number(const std::string& v)
{
	int a, b, c;
	char end;
	if (std::sscanf(v.c_str(), "v%d.%d.%d%c", &a, &b, &c, &end) != 3)
		return -1;
	return (a * 100000LL + b) * 100000LL + c;
}

// the string values of `key` in GitHub's JSON, which never escapes a quote in a tag or a download URL
std::vector<std::string> json_strings(const std::string& json, const std::string& key)
{
	std::vector<std::string> out;
	for (size_t i = 0; (i = json.find("\"" + key + "\"", i)) != std::string::npos;)
	{
		size_t a = json.find('"', json.find(':', i) + 1), b = a == std::string::npos ? a : json.find('"', a + 1);
		if (b == std::string::npos)
			break;
		out.push_back(json.substr(a + 1, b - a - 1));
		i = b;
	}
	return out;
}

bool ends_with(const std::string& s, const std::string& end)
{
	return s.size() >= end.size() && s.compare(s.size() - end.size(), end.size(), end) == 0;
}

// the files of the unpacked release moved over the installed ones; those they replace go to old
std::string install(const fs::path& from, const fs::path& to, const fs::path& old)
{
	std::error_code ec;
	std::vector<fs::path> files;    // listed first: the moves change the folders being walked
	for (fs::recursive_directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec))
		if (it->is_symlink(ec) || !it->is_directory(ec))
			files.push_back(it->path().lexically_relative(from));
	if (ec)
		return "Cannot read the new files: " + ec.message();
	for (const fs::path& rel : files)
	{
		fs::path dst = to / rel;
		bool exists = fs::exists(fs::symlink_status(dst, ec));
		if (exists && *rel.begin() == "config")
			continue;                    // the player's settings
		fs::create_directories(dst.parent_path(), ec);
		if (exists)
		{
			fs::create_directories((old / rel).parent_path(), ec);
			fs::rename(dst, old / rel, ec);       // a running program can be moved, on Windows too, not overwritten
			if (ec)
				return "Cannot replace " + rel.generic_string() + ": " + ec.message();
		}
		fs::rename(from / rel, dst, ec);
		if (ec)
			return "Cannot install " + rel.generic_string() + ": " + ec.message();
	}
	return "";
}

void run_update(Paths paths, std::string tag, std::string url, std::string sha256)
{
	const std::string dir = paths.build + "/update", archive = dir + "/" + url.substr(url.find_last_of('/') + 1);
	std::error_code ec;
	fs::remove_all(to_path(dir), ec);
	make_dir(dir);
	make_dir(dir + "/new");
	int code = -1;
	set(State::working, "Downloading " + tag + "...");
	std::string out = run_capture({"curl", "-sSfL", "--connect-timeout", "20", "-o", archive, url}, paths.build, &code);
	if (code)
		return set(State::failed, "Download failed: " + (out.empty() ? "is curl installed?" : out));
	set(State::working, "Checking " + tag + "...");
	std::string got = run_capture({paths.build + "/wtbuild" WT_EXE, "sha256", archive}, paths.build, &code).substr(0, 64);
	if (code || got != sha256)
		return set(State::failed, "The download is damaged (checksum mismatch); try again");
	set(State::working, "Unpacking " + tag + "...");
	out = run_capture({"tar", "-xf", archive, "-C", dir + "/new"}, paths.build, &code);
	if (code)
		return set(State::failed, "Cannot unpack the download: " + out);
	fs::path top;    // the archive holds one folder, WitchTime-<tag>-<os>
	for (const auto& e : fs::directory_iterator(to_path(dir + "/new"), ec))
		top = e.path();
	if (top.empty())
		return set(State::failed, "The download is empty");
	set(State::working, "Installing " + tag + "...");
	std::string err = install(top, to_path(paths.repo), to_path(dir + "/old"));
	if (!err.empty())
		return set(State::failed, err);
	fs::remove(to_path(archive), ec);
	set(State::done, "");
}

} // namespace

void check(const Paths& paths)
{
	std::error_code ec;
	fs::remove_all(to_path(paths.build + "/update"), ec);    // what the last update replaced (fails harmlessly if busy)
	const long long mine = version_number(WT_VERSION);
	if (mine < 0)
		return;
	std::thread([mine] {
		int code = -1;
		std::string json = run_capture({"curl", "-sSfL", "--max-time", "20", "-H", "Accept: application/vnd.github+json", LATEST}, ".", &code);
		std::vector<std::string> tags = json_strings(json, "tag_name");
		if (code || tags.empty() || version_number(tags[0]) <= mine)
			return;
		std::string url;
		for (const std::string& u : json_strings(json, "browser_download_url"))
			if (ends_with(u, ASSET))
				url = u;
		if (url.empty())
			return;
		// GitHub lists an asset's digest ("sha256:<hash>") ahead of its download URL
		size_t at = json.rfind("\"digest\"", json.find(url));
		std::vector<std::string> digests = at == std::string::npos ? std::vector<std::string>{} : json_strings(json.substr(at), "digest");
		if (digests.empty() || digests[0].size() != 71 || digests[0].compare(0, 7, "sha256:") != 0)
			return;
		std::lock_guard<std::mutex> l(shared().m);
		shared().tag = tags[0];
		shared().url = url;
		shared().sha256 = digests[0].substr(7);
		shared().state = State::available;
	}).detach();
}

void start(const Paths& paths)
{
	std::string tag, url, sha256;
	{
		std::lock_guard<std::mutex> l(shared().m);
		if (shared().state != State::available && shared().state != State::failed)
			return;
		shared().state = State::working;
		tag = shared().tag;
		url = shared().url;
		sha256 = shared().sha256;
	}
	std::thread(run_update, paths, tag, url, sha256).detach();
}

State state(std::string& tag, std::string& message)
{
	std::lock_guard<std::mutex> l(shared().m);
	tag = shared().tag;
	message = shared().message;
	return shared().state;
}

void restart(const Paths& paths)
{
#ifdef _WIN32
	std::wstring cmd = L"\"" + widen(paths.self) + L"\"";
	STARTUPINFOW si{sizeof si};
	PROCESS_INFORMATION pi{};
	if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
	{
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
	}
#else
	execl(paths.self.c_str(), paths.self.c_str(), (char*)nullptr);
#endif
}

}
