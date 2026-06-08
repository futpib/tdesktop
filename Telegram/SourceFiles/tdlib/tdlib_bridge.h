/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QObject>
#include <QtCore/QByteArray>

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

	void addClient(int tdlibClientId, not_null<MTP::Instance*> instance);
	void removeClient(int tdlibClientId);

	// Inject server-pushed updates received on `instance` into every TDLib
	// client bound to it.  Inbound counterpart to registerExternalDispatch:
	// external dispatch carries our queries out, this carries the server's
	// pushes back in, so the client's update state no longer freezes.
	void pushUpdates(
		not_null<MTP::Instance*> instance,
		const QByteArray &serialized);

	// Registers this bridge as the TDLib external dispatch handler.
	void registerExternalDispatch();

	// Complete all held NetQueryPtrs with errors and return them to
	// TDLib's object pool.  Must be called before TDLib is shut down.
	void releaseAllQueries();

private:
	struct Private;
	const std::unique_ptr<Private> _d;
};

} // namespace TdBridge
