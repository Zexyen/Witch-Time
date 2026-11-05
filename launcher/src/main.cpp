// Witch Time: the launcher (see app.h). witch-time [--play] [--returned <exit code>]: --play starts the game at once and
// ends when it does (a Steam shortcut, Playnite, a desktop entry), unless an update is out or the game needs a build;
// --returned is how the launcher starts itself again after the game ended in its window (game_host.cpp).
#include "app.h"

int main(int argc, char** argv)
{
	App app;
	if (!app.init(argc, argv))
		return 1;
	return app.run();
}
