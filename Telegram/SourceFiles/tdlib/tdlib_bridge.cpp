/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tdlib/tdlib_bridge.h"

#include "mtproto/details/mtproto_serialized_request.h"
#include "mtproto/mtproto_response.h"
#include "mtproto/core_types.h"

#include <td/telegram/net/NetQuery.h>
#include <td/telegram/net/NetQueryDispatcher.h>

#include <QtCore/QMetaObject>

namespace TdBridge {
namespace {

constexpr uint32 kGzipPackedConstructor = 0x3072cfa1;

// Build an MTP::details::SerializedRequest from raw TL bytes.
// The raw bytes are the serialized telegram_api::Function.
[[nodiscard]] MTP::details::SerializedRequest BuildSerializedRequest(
		const td::BufferSlice &queryData,
		td::NetQuery::GzipFlag gzipFlag) {
	const auto *src = reinterpret_cast<const char *>(queryData.data());
	const auto srcSize = static_cast<uint32>(queryData.size());

	if (gzipFlag == td::NetQuery::GzipFlag::On) {
		// Wrap in gzip_packed TL constructor:
		// constructor_id (4 bytes) + TL string (length-prefixed)
		const auto tlStringPadding = ((srcSize + 3) & ~3u) - srcSize;
		uint32 tlStringHeaderSize;
		if (srcSize < 254) {
			tlStringHeaderSize = 1;
		} else {
			tlStringHeaderSize = 4;
		}
		const auto totalTlStringSize = tlStringHeaderSize + srcSize
			+ tlStringPadding;
		// gzip_packed = constructor(4) + tl_string(totalTlStringSize)
		const auto bodySize = 4 + totalTlStringSize;
		const auto bodySizeInInts = (bodySize + 3) / 4;

		auto request = MTP::details::SerializedRequest::Prepare(
			bodySizeInInts);
		auto &buf = *request;

		// Write constructor ID.
		buf.push_back(static_cast<mtpPrime>(kGzipPackedConstructor));

		// Write TL string: length prefix + data + padding.
		QByteArray tlStringBuf;
		tlStringBuf.reserve(static_cast<int>(totalTlStringSize));
		if (srcSize < 254) {
			tlStringBuf.append(static_cast<char>(srcSize));
		} else {
			tlStringBuf.append(static_cast<char>(254));
			tlStringBuf.append(static_cast<char>(srcSize & 0xFF));
			tlStringBuf.append(static_cast<char>((srcSize >> 8) & 0xFF));
			tlStringBuf.append(static_cast<char>((srcSize >> 16) & 0xFF));
		}
		tlStringBuf.append(src, static_cast<int>(srcSize));
		while (tlStringBuf.size() % 4 != 0) {
			tlStringBuf.append('\0');
		}

		const auto *tlStringData = reinterpret_cast<const mtpPrime *>(
			tlStringBuf.constData());
		const auto tlStringInts = static_cast<int>(
			tlStringBuf.size() / sizeof(mtpPrime));
		for (int i = 0; i < tlStringInts; ++i) {
			buf.push_back(tlStringData[i]);
		}

		request->needsLayer = true;
		return request;
	}

	// No gzip: copy raw bytes directly.
	const auto bodySizeInInts = (srcSize + 3) / 4;
	auto request = MTP::details::SerializedRequest::Prepare(bodySizeInInts);
	auto &buf = *request;

	const auto *srcInts = reinterpret_cast<const mtpPrime *>(src);
	for (uint32 i = 0; i < bodySizeInInts; ++i) {
		if ((i + 1) * sizeof(mtpPrime) <= srcSize) {
			buf.push_back(srcInts[i]);
		} else {
			// Last partial int - zero-pad.
			mtpPrime last = 0;
			memcpy(&last, src + i * sizeof(mtpPrime),
				srcSize - i * sizeof(mtpPrime));
			buf.push_back(last);
		}
	}

	request->needsLayer = true;
	return request;
}

} // namespace

TdLibBridge::TdLibBridge() {
}

TdLibBridge::~TdLibBridge() = default;

void TdLibBridge::setMtpInstance(not_null<MTP::Instance*> instance) {
	std::lock_guard<std::mutex> lock(_mutex);
	_mtp = instance;

	// Flush any queries that arrived before MTP was ready.
	auto pending = std::move(_pendingBeforeMtp);
	_pendingBeforeMtp.clear();

	for (auto &p : pending) {
		sendToMtp(std::move(p));
	}
}

void TdLibBridge::onExternalDispatch(td::NetQueryPtr query) {
	// Called from TDLib's scheduler thread.
	// Extract what we need, then marshal to Qt main thread.
	const auto rawDcId = query->dc_id().is_main()
		? 0
		: query->dc_id().get_raw_id();
	const auto type = query->type();

	PendingQuery pending;
	pending.query = std::move(query);
	pending.rawDcId = rawDcId;
	pending.type = type;

	QMetaObject::invokeMethod(this, [this, p = std::move(pending)]() mutable {
		std::lock_guard<std::mutex> lock(_mutex);
		if (!_mtp) {
			_pendingBeforeMtp.push_back(std::move(p));
			return;
		}
		sendToMtp(std::move(p));
	}, Qt::QueuedConnection);
}

void TdLibBridge::sendToMtp(PendingQuery &&pending) {
	auto &query = pending.query;
	const auto shiftedDcId = mapDcId(pending.rawDcId, pending.type);

	auto serialized = BuildSerializedRequest(
		query->query(),
		query->gzip_flag());

	const auto requestId = MTP::details::GetNextRequestId();
	serialized->requestId = requestId;

	// Store the TDLib query so we can complete it when the response arrives.
	_sentQueries.emplace(requestId, std::move(query));

	auto done = [this, requestId](const MTP::Response &response) -> bool {
		auto it = _sentQueries.find(requestId);
		if (it == _sentQueries.end()) {
			return true;
		}
		auto query = std::move(it->second);
		_sentQueries.erase(it);

		// Convert the MTP response buffer to a td::BufferSlice.
		// The reply buffer starts at position 0 and is the raw TL response.
		const auto &reply = response.reply;
		if (!reply.isEmpty()) {
			const auto *data = reinterpret_cast<const char *>(
				reply.constData());
			const auto size = static_cast<size_t>(
				reply.size() * sizeof(mtpPrime));
			query->set_ok(td::BufferSlice(td::Slice(data, size)));
		} else {
			query->set_error(td::Status::Error(500, "Empty response"));
		}

		completeQuery(std::move(query));
		return true;
	};

	auto fail = [this, requestId](
			const MTP::Error &error,
			const MTP::Response &response) -> bool {
		auto it = _sentQueries.find(requestId);
		if (it == _sentQueries.end()) {
			return true;
		}
		auto query = std::move(it->second);
		_sentQueries.erase(it);

		const auto code = error.code();
		const auto message = error.type().toStdString();
		query->set_error(td::Status::Error(code, message));

		completeQuery(std::move(query));
		return true;
	};

	_mtp->sendSerialized(
		requestId,
		std::move(serialized),
		MTP::ResponseHandler{ std::move(done), std::move(fail) },
		shiftedDcId,
		0,  // msCanWait
		0); // afterRequestId
}

void TdLibBridge::completeQuery(td::NetQueryPtr query) {
	td::NetQueryDispatcher::complete_net_query(std::move(query));
}

MTP::ShiftedDcId TdLibBridge::mapDcId(
		int32 rawDcId,
		td::NetQuery::Type type) const {
	if (rawDcId == 0) {
		// Main DC - use ShiftedDcId 0 which means "current main DC".
		switch (type) {
		case td::NetQuery::Type::Upload:
			return MTP::ShiftDcId(_mtp->mainDcId(), MTP::kBaseUploadDcShift);
		case td::NetQuery::Type::Download:
		case td::NetQuery::Type::DownloadSmall:
			return MTP::ShiftDcId(
				_mtp->mainDcId(),
				MTP::kBaseDownloadDcShift);
		default:
			return 0; // Main DC, no shift.
		}
	}

	switch (type) {
	case td::NetQuery::Type::Upload:
		return MTP::ShiftDcId(rawDcId, MTP::kBaseUploadDcShift);
	case td::NetQuery::Type::Download:
	case td::NetQuery::Type::DownloadSmall:
		return MTP::ShiftDcId(rawDcId, MTP::kBaseDownloadDcShift);
	default:
		return rawDcId; // Bare DC ID.
	}
}

} // namespace TdBridge
