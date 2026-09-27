/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <td/telegram/ClientInternal.h>
#include <td/telegram/telegram_api.h>
#include <td/utils/tl_helpers.h>

namespace TdBridge {

inline void RequestDifference(int clientId) {
	// updatesTooLong has no fields; its boxed constructor requests cursor-based recovery.
	td::push_external_updates(
		clientId,
		td::serialize(td::telegram_api::updatesTooLong::ID));
}

} // namespace TdBridge
