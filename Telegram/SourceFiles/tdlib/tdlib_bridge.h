/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QObject>

#include <memory>

namespace MTP {
class Instance;
} // namespace MTP

namespace TdBridge {

class TdLibBridge final : public QObject {
	Q_OBJECT

public:
	TdLibBridge();
	~TdLibBridge();

	void setMtpInstance(not_null<MTP::Instance*> instance);

	// Registers this bridge as the TDLib external dispatch handler.
	void registerExternalDispatch();

private:
	struct Private;
	const std::unique_ptr<Private> _d;
};

} // namespace TdBridge
