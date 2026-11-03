#include "platform.h"
#include <cstdlib>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>

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

#else
#include <unistd.h>

bool make_dir(const std::string& path) { return mkdir(path.c_str(), 0755) == 0; }

void set_env(const char* name, const std::string& value, bool overwrite) { setenv(name, value.c_str(), overwrite); }

void unset_env(const char* name) { unsetenv(name); }

bool change_dir(const std::string& path) { return chdir(path.c_str()) == 0; }

bool is_program(const std::string& path) { return access(path.c_str(), X_OK) == 0; }
#endif
