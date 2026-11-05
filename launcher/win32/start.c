/* witch-time.exe at the top of a Windows release: starts build\witch-time.exe beside it with the same arguments, waits
 * for it and ends with its exit code, so Steam, Playnite or LaunchBox see the game running until it ends (issue 143;
 * on Linux a symbolic link stands there). */
#include <windows.h>

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR args, int show)
{
	(void)inst;
	(void)prev;
	(void)show;
	static wchar_t dir[32768], cmd[65536];
	DWORD n = GetModuleFileNameW(NULL, dir, 32768);
	while (n > 0 && dir[n - 1] != L'\\' && dir[n - 1] != L'/')
		n--;
	dir[n] = 0;
	if (lstrlenW(dir) + lstrlenW(args) > 65000)
		return 1;
	lstrcpyW(cmd, L"\"");
	lstrcatW(cmd, dir);
	lstrcatW(cmd, L"build\\witch-time.exe\" ");
	lstrcatW(cmd, args);
	STARTUPINFOW si = {sizeof si};
	PROCESS_INFORMATION pi;
	if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
	{
		MessageBoxW(NULL, L"build\\witch-time.exe is missing beside this program.", L"Witch Time", MB_ICONERROR);
		return 1;
	}
	CloseHandle(pi.hThread);
	MSG msg;    /* one empty peek: this program is idle, so Windows drops its startup cursor */
	PeekMessageW(&msg, NULL, 0, 0, PM_NOREMOVE);
	DWORD code = 1;
	WaitForSingleObject(pi.hProcess, INFINITE);
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hProcess);
	return (int)code;
}
