/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/launcher.h"
#include "crl/crl.h"
#include <cstdio>

int main(int argc, char *argv[]) {
	fprintf(stderr, "[%.3f] main() entered\n", crl::profile() / 1000.0);
	const auto launcher = Core::Launcher::Create(argc, argv);
	return launcher ? launcher->exec() : 1;
}
