// Witch Time: the launcher (see app.h). witch-time [--play] [--returned <exit code>]: --play starts the game at once and
// ends when it does (a Steam shortcut, Playnite, a desktop entry), unless an update is out or the game needs a build;
// --returned is how the launcher starts itself again after the game ended in its window (game_host.cpp).
// witch-time --prepare <wtbuild setup's arguments> builds with no window, for RecompLauncher (recomplauncher.json).
#include "app.h"
#include <cstring>

int main(int argc, char** argv)
{
	if (argc > 1 && !std::strcmp(argv[1], "--prepare"))
		return prepare_main(argc, argv);
	App app;
	if (!app.init(argc, argv))
		return 1;
	return app.run();
}
