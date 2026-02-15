/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "mtproto/mtp_instance.h"
#include "mtproto/core_types.h"

#include <td/telegram/Client.h>
#include <td/telegram/net/NetQuery.h>
#include <td/telegram/net/NetQueryDispatcher.h>

#include <QtCore/QObject>

#include <mutex>

namespace TdBridge {

class TdLibBridge final : public QObject {
	Q_OBJECT

public:
	TdLibBridge();
	~TdLibBridge();

	void setMtpInstance(not_null<MTP::Instance*> instance);

	// Called from TDLib's scheduler thread.
	void onExternalDispatch(td::NetQueryPtr query);

private:
	struct PendingQuery {
		td::NetQueryPtr query;
		int32 rawDcId = 0;
		td::NetQuery::Type type = td::NetQuery::Type::Common;
	};

	void sendToMtp(PendingQuery &&pending);
	void completeQuery(td::NetQueryPtr query);

	[[nodiscard]] MTP::ShiftedDcId mapDcId(
		int32 rawDcId,
		td::NetQuery::Type type) const;

	MTP::Instance *_mtp = nullptr;
	std::mutex _mutex;
	std::vector<PendingQuery> _pendingBeforeMtp;
	uint64 _nextBridgeId = 1;
	base::flat_map<mtpRequestId, td::NetQueryPtr> _sentQueries;
};

} // namespace TdBridge
