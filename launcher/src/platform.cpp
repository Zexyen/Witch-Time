#include "platform.h"
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

#ifdef _WIN32
#include <algorithm>
#include <direct.h>
#include <windows.h>
#include <shlobj.h>

std::wstring widen(const std::string& s)
{
	int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
	std::wstring w(size_t(n), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
	return w;
}

std::string narrow(const std::wstring& w)
{
	int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
	std::string s(size_t(n), '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
	return s;
}

bool make_dir(const std::string& path) { return CreateDirectoryW(widen(path).c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS; }

void set_env(const char* name, const std::string& value, bool overwrite)
{
	std::wstring n = widen(name);
	if (!overwrite && GetEnvironmentVariableW(n.c_str(), nullptr, 0))
		return;
	SetEnvironmentVariableW(n.c_str(), widen(value).c_str());   // what a started program inherits
	_putenv_s(name, value.c_str());                              // and getenv() here
}

void unset_env(const char* name)
{
	SetEnvironmentVariableW(widen(name).c_str(), nullptr);
	_putenv_s(name, "");                                         // an empty value removes it
}

bool change_dir(const std::string& path) { return SetCurrentDirectoryW(widen(path).c_str()) != 0; }

bool is_program(const std::string& path)
{
	DWORD a = GetFileAttributesW(widen(path).c_str());
	return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

namespace {

// one argument as CommandLineToArgvW reads it back
std::wstring quote(const std::wstring& a)
{
	if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos)
		return a;
	std::wstring q = L"\"";
	size_t slashes = 0;
	for (wchar_t c : a)
	{
		if (c == L'\\')
			slashes++;
		else
		{
			if (c == L'"')
				q.append(slashes + 1, L'\\');
			slashes = 0;
		}
		q += c;
	}
	q.append(slashes, L'\\');
	return q + L"\"";
}

} // namespace

ChildProcess::~ChildProcess() { reset(); }

void ChildProcess::reset()
{
	for (void** h : {&process, &job, &pipe})
		if (*h)
		{
			CloseHandle(*h);
			*h = nullptr;
		}
}

bool ChildProcess::start(const std::vector<std::string>& argv, const std::string& cwd, const std::string& log_file)
{
	reset();
	SECURITY_ATTRIBUTES sa = {sizeof sa, nullptr, TRUE};
	HANDLE rd = nullptr, wr = nullptr;
	if (log_file.empty())
	{
		if (!CreatePipe(&rd, &wr, &sa, 0))
			return false;
		SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
	}
	else
	{
		wr = CreateFileW(widen(log_file).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL, nullptr);
		if (wr == INVALID_HANDLE_VALUE)
			return false;
	}
	HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
	STARTUPINFOW si = {};
	si.cb = sizeof si;
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdInput = nul;
	si.hStdOutput = si.hStdError = wr;
	std::wstring cmd;
	for (const std::string& a : argv)
		cmd += (cmd.empty() ? L"" : L" ") + quote(widen(a));
	std::wstring dir = widen(cwd);
	PROCESS_INFORMATION pi = {};
	BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED |
		CREATE_UNICODE_ENVIRONMENT, nullptr, dir.empty() ? nullptr : dir.c_str(), &si, &pi);
	CloseHandle(wr);
	if (nul != INVALID_HANDLE_VALUE)
		CloseHandle(nul);
	if (!ok)
	{
		if (rd)
			CloseHandle(rd);
		return false;
	}
	job = CreateJobObjectW(nullptr, nullptr);
	if (job)
		AssignProcessToJobObject(job, pi.hProcess);
	ResumeThread(pi.hThread);
	CloseHandle(pi.hThread);
	process = pi.hProcess;
	pipe = rd;
	return true;
}

std::string ChildProcess::read_available()
{
	std::string out;
	char buf[4096];
	DWORD avail = 0, n = 0;
	while (pipe && PeekNamedPipe(pipe, nullptr, 0, nullptr, &avail, nullptr) && avail > 0 &&
		ReadFile(pipe, buf, avail < sizeof buf ? avail : DWORD(sizeof buf), &n, nullptr) && n > 0)
		out.append(buf, n);
	return out;
}

bool ChildProcess::wait(int* code, unsigned ms)
{
	if (!process || WaitForSingleObject(process, ms) != WAIT_OBJECT_0)
		return false;
	DWORD c = 0;
	GetExitCodeProcess(process, &c);
	if (code)
		*code = int(c);
	return true;
}

bool ChildProcess::finished(int* code) { return wait(code, 0); }

void ChildProcess::stop()
{
	if (job)
		TerminateJobObject(job, 1);
	else if (process)
		TerminateProcess(process, 1);
}

bool create_shortcut(const std::string& desktop, const std::string& program, const std::string&, std::string* said)
{
	std::wstring exe = widen(program), lnk = widen(desktop + "Witch Time.lnk");
	std::replace(exe.begin(), exe.end(), L'/', L'\\');
	std::replace(lnk.begin(), lnk.end(), L'/', L'\\');
	HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);    // S_FALSE where SDL already did; both are undone below
	IShellLinkW* link = nullptr;
	IPersistFile* file = nullptr;
	HRESULT hr = desktop.empty() ? E_FAIL : CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&link);
	if (SUCCEEDED(hr))
	{
		link->SetPath(exe.c_str());
		link->SetArguments(L"--play");
		link->SetWorkingDirectory(exe.substr(0, exe.find_last_of(L'\\')).c_str());
		link->SetDescription(L"Starts the game without the launcher's screens");
		hr = link->QueryInterface(IID_IPersistFile, (void**)&file);
	}
	if (SUCCEEDED(hr))
		hr = file->Save(lnk.c_str(), TRUE);
	if (file)
		file->Release();
	if (link)
		link->Release();
	if (SUCCEEDED(com))
		CoUninitialize();
	*said = SUCCEEDED(hr) ? "On your desktop now" : desktop.empty() ? "Windows names no desktop folder" : "Could not write to the desktop folder";
	return SUCCEEDED(hr);
}

#else
#include <cstdio>
#include <unistd.h>

bool make_dir(const std::string& path) { return mkdir(path.c_str(), 0755) == 0; }

void set_env(const char* name, const std::string& value, bool overwrite) { setenv(name, value.c_str(), overwrite); }

void unset_env(const char* name) { unsetenv(name); }

bool change_dir(const std::string& path) { return chdir(path.c_str()) == 0; }

bool is_program(const std::string& path) { return access(path.c_str(), X_OK) == 0; }

#ifdef __APPLE__
bool create_shortcut(const std::string& desktop, const std::string& program, const std::string&, std::string* said)
{
	// macOS: a .command file is a shell script Finder runs in a Terminal window; the program's name inside single quotes
	std::string quoted;
	for (char c : program)
		quoted += c == '\'' ? std::string("'\\''") : std::string(1, c);
	const std::string path = desktop + "Witch Time.command";
	FILE* f = desktop.empty() ? nullptr : std::fopen(path.c_str(), "w");
	bool ok = f && std::fprintf(f, "#!/bin/sh\n# Witch Time, without the launcher's screens\nexec '%s' --play\n", quoted.c_str()) > 0;
	ok = f && std::fclose(f) == 0 && ok && chmod(path.c_str(), 0755) == 0;
	*said = ok ? "On your desktop now" : desktop.empty() ? "macOS names no desktop folder" : "Could not write to the desktop folder";
	return ok;
}
#else
bool create_shortcut(const std::string& desktop, const std::string& program, const std::string& icon, std::string* said)
{
	// Desktop Entry Specification: a string value doubles its backslashes; Exec's quoted program also takes one before
	// " ` $ and \ (so two in the file), and % is doubled
	// shortcut: GLib (GNOME, gio launch) looks for the program before it undoubles %, so a game folder with % in its name
	// gets an entry that does not start there; write a wrapper script if anyone reports it
	auto value = [](const std::string& s, bool exec) {
		std::string o;
		for (char c : s)
		{
			if (exec && std::strchr("\"`$\\", c))
				o += "\\\\";
			if (c == '\\' || (exec && c == '%'))
				o += c;
			o += c;
		}
		return o;
	};
	const std::string entry = "[Desktop Entry]\nType=Application\nName=Witch Time\nComment=Bayonetta 3, without the launcher's screens\n"
		"Exec=\"" + value(program, true) + "\" --play\nPath=" + value(program.substr(0, program.find_last_of('/')), false) +
		"\nIcon=" + value(icon, false) + "\nTerminal=false\nCategories=Game;\nStartupWMClass=witchtime\n";   // witchtime: the windows' app id
	auto write = [&entry](const std::string& path) {
		FILE* f = std::fopen(path.c_str(), "w");
		if (!f)
			return false;
		bool ok = std::fputs(entry.c_str(), f) >= 0;
		return std::fclose(f) == 0 && ok && chmod(path.c_str(), 0755) == 0;   // executable: Plasma starts it from the desktop unasked
	};
	const char *data = std::getenv("XDG_DATA_HOME"), *home = std::getenv("HOME");
	std::string share = data && data[0] == '/' ? data : std::string(home ? home : "") + "/.local/share";
	if (program.find('\n') != std::string::npos)
		return *said = "The game folder's name has a line break", false;
	for (const std::string& dir : {share.substr(0, share.find_last_of('/')), share, share + "/applications"})    // ~/.local first
		make_dir(dir);
	bool menu = write(share + "/applications/witchtime.desktop"), desk = !desktop.empty() && write(desktop + "witchtime.desktop");
	*said = menu && desk ? "In your app menu and on your desktop now" : menu ? "In your app menu now" :
		desk ? "On your desktop now" : "Could not write to the app menu's folder";
	return menu || desk;
}
#endif
#endif
