// Updates a release in place from the latest GitHub release: checked at start, installed when the player asks. The
// archive is downloaded with the system's curl, checked against the SHA-256 digest GitHub lists for it (wtbuild sha256) and unpacked
// with the system's tar (Windows 10 and 11 ship curl and tar). Its files are then moved over this folder's, keeping
// config/; the files they replace go to build/update and are deleted at the next start. The build then redoes the
// steps the new sources feed (tools/wtbuild/src/setup.rs REDO_ON_UPDATE).
#pragma once
#include "config.h"
#include <string>

namespace update {

enum class State { none, checking, available, working, failed, done };   // checking: check() has not heard from GitHub yet

// in the background (State::checking until GitHub answers, at most 20 s); only in a release build (WT_VERSION vX.Y.Z)
void check(const Paths& paths);
// downloads and installs the release check() found, in the background
void start(const Paths& paths);
// where it is: the release's tag, and a line for the screen
State state(std::string& tag, std::string& message);
// starts this program again (the new one); on Windows it waits for it, so whatever started this launcher sees it run on
void restart(const Paths& paths);

}
