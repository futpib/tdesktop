/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tdlib/tdlib_bridge.h"

#include "mtproto/mtp_instance.h"
#include "mtproto/core_types.h"
#include "mtproto/details/mtproto_serialized_request.h"
#include "mtproto/mtproto_response.h"

#include <td/telegram/ClientInternal.h>
#include <td/telegram/Client.h>
#include <td/telegram/net/NetQuery.h>
#include <td/telegram/net/NetQueryDispatcher.h>

#include <QtCore/QMetaObject>

#include <mutex>

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

struct TdLibBridge::Private {
	struct PendingQuery {
		td::NetQueryPtr query;
		int32 rawDcId = 0;
		td::NetQuery::Type type = td::NetQuery::Type::Common;
		MTP::details::SerializedRequest serialized;
	};
	struct ClientState {
		MTP::Instance *mtp = nullptr;
		std::vector<PendingQuery> pendingBeforeMtp;
	};

	std::mutex mutex;
	base::flat_map<int, ClientState> clients;
	uint64 nextBridgeId = 1;
	base::flat_map<mtpRequestId, std::pair<int, td::NetQueryPtr>> sentQueries;

	// Incoming queries stashed here so that the Qt queued lambda
	// only carries a plain uint64 id — no NetQueryPtr.  This avoids
	// crashes when Qt destroys pending events during QObject teardown
	// (the NetQueryPtr destructor would touch TDLib's actor system
	// which may already be shut down).
	uint64 nextIncomingId = 1;
	struct IncomingEntry {
		td::int32 clientId;
		PendingQuery pending;
	};
	base::flat_map<uint64, IncomingEntry> incoming;

	void sendToMtp(TdLibBridge *bridge, int clientId, PendingQuery &&pending);
	[[nodiscard]] static MTP::ShiftedDcId mapDcId(
		MTP::Instance *mtp,
		int32 rawDcId,
		td::NetQuery::Type type);
};

TdLibBridge::TdLibBridge()
: _d(std::make_unique<Private>()) {
}

TdLibBridge::~TdLibBridge() {
	// Clear the external dispatch callback so TDLib doesn't call us
	// after we're destroyed.
	td::set_external_dispatch(nullptr);
}

void TdLibBridge::releaseAllQueries() {
	// Stop accepting new queries from TDLib.
	td::set_external_dispatch(nullptr);

	std::lock_guard<std::mutex> lock(_d->mutex);

	const auto shuttingDown = [] {
		auto result = td::ExternalQueryResult();
		result.is_ok = false;
		result.error_code = 500;
		result.error_message = "Bridge shutting down";
		return result;
	};

	// Finish all incoming (stashed but not yet dispatched) queries.  set_error
	// and delivery run on the owning client's scheduler thread.
	for (auto &[id, entry] : _d->incoming) {
		td::complete_external_query(
			entry.clientId,
			std::move(entry.pending.query),
			shuttingDown());
	}
	_d->incoming.clear();

	// Finish all queries waiting for the MTP instance.
	for (auto &[clientId, state] : _d->clients) {
		for (auto &p : state.pendingBeforeMtp) {
			td::complete_external_query(
				clientId,
				std::move(p.query),
				shuttingDown());
		}
		state.pendingBeforeMtp.clear();
	}

	// Finish all in-flight queries (sent to MTP, waiting for response).
	for (auto &[requestId, pair] : _d->sentQueries) {
		td::complete_external_query(
			pair.first,
			std::move(pair.second),
			shuttingDown());
	}
	_d->sentQueries.clear();
}

void TdLibBridge::addClient(
		int tdlibClientId,
		not_null<MTP::Instance*> instance) {
	std::lock_guard<std::mutex> lock(_d->mutex);
	auto &state = _d->clients[tdlibClientId];
	state.mtp = instance;

	// Flush any queries that arrived before MTP was ready.
	auto pending = std::move(state.pendingBeforeMtp);
	state.pendingBeforeMtp.clear();

	for (auto &p : pending) {
		_d->sendToMtp(this, tdlibClientId, std::move(p));
	}
}

void TdLibBridge::removeClient(int tdlibClientId) {
	std::lock_guard<std::mutex> lock(_d->mutex);
	_d->clients.erase(tdlibClientId);
}

void TdLibBridge::registerExternalDispatch() {
	td::set_external_dispatch(
		[this](td::int32 clientId, td::NetQueryPtr query) {
			// Called from TDLib's scheduler thread.  Serialize the request and
			// read everything we need from the NetQuery here, on the scheduler
			// thread; only plain data crosses to the Qt side.  The NetQueryPtr
			// is stashed and never touched off the scheduler thread until it is
			// finished on one.
			Private::PendingQuery pending;
			pending.rawDcId = query->dc_id().is_main()
				? 0
				: query->dc_id().get_raw_id();
			pending.type = query->type();
			pending.serialized = BuildSerializedRequest(
				query->query(),
				query->gzip_flag());
			pending.query = std::move(query);

			// Stash the query (which owns a NetQueryPtr) in a
			// mutex-protected map and only pass a plain uint64 id
			// through the Qt event queue.  This way, if the queued
			// event is never delivered (e.g. during shutdown) its
			// destructor won't touch TDLib's actor system.
			uint64 incomingId;
			{
				std::lock_guard<std::mutex> lock(_d->mutex);
				incomingId = _d->nextIncomingId++;
				_d->incoming.emplace(incomingId, Private::IncomingEntry{
					clientId,
					std::move(pending),
				});
			}

			QMetaObject::invokeMethod(this, [this, incomingId]() {
				std::lock_guard<std::mutex> lock(_d->mutex);
				auto it = _d->incoming.find(incomingId);
				if (it == _d->incoming.end()) {
					return; // Already released during shutdown.
				}
				const auto cId = it->second.clientId;
				auto p = std::move(it->second.pending);
				_d->incoming.erase(it);

				auto clientIt = _d->clients.find(cId);
				if (clientIt == _d->clients.end()) {
					// Client not registered yet - create entry and queue.
					_d->clients[cId].pendingBeforeMtp.push_back(std::move(p));
					return;
				}
				if (!clientIt->second.mtp) {
					clientIt->second.pendingBeforeMtp.push_back(std::move(p));
					return;
				}
				_d->sendToMtp(this, cId, std::move(p));
			}, Qt::QueuedConnection);
		});
}

void TdLibBridge::Private::sendToMtp(
		TdLibBridge *bridge,
		int clientId,
		PendingQuery &&pending) {
	auto it = clients.find(clientId);
	if (it == clients.end() || !it->second.mtp) {
		return;
	}
	auto *mtp = it->second.mtp;

	auto queryType = pending.type;

	// Route uploads on the main connection instead of the shifted upload
	// connection.  The shifted upload connection is unreliable for the queries
	// we inject:
	//   * small files: an upload.saveFilePart part saved there is not found by
	//     the messages.sendMedia that references it -> INPUT_FETCH_FAIL;
	//   * large files: after ~40MB the shifted connection stops delivering
	//     responses; the in-flight parts never complete (no reply, no error),
	//     so the upload stalls forever and the message eventually fails.
	// The main connection is kept alive by the rest of the session's traffic
	// and delivers reliably, so send upload parts as common queries.  Downloads
	// keep their dedicated connection.
	if (queryType == td::NetQuery::Type::Upload) {
		queryType = td::NetQuery::Type::Common;
	}

	const auto shiftedDcId = mapDcId(mtp, pending.rawDcId, queryType);

	// The request was serialized on the scheduler thread when the query was
	// dispatched; here we only attach a request id and hand plain bytes to MTP.
	auto serialized = std::move(pending.serialized);
	const auto requestId = MTP::details::GetNextRequestId();
	serialized->requestId = requestId;

	// Hold the TDLib query until the response arrives.  It is never touched on
	// this (Qt) thread: it is moved into complete_external_query, which finishes
	// it on the owning client's scheduler thread.
	sentQueries.emplace(requestId, std::make_pair(clientId, std::move(pending.query)));

	auto done = [this, requestId](const MTP::Response &response) -> bool {
		auto it = sentQueries.find(requestId);
		if (it == sentQueries.end()) {
			return true;
		}
		const auto cId = it->second.first;
		auto query = std::move(it->second.second);
		sentQueries.erase(it);

		auto result = td::ExternalQueryResult();
		const auto &reply = response.reply;
		if (!reply.isEmpty()) {
			const auto *data = reinterpret_cast<const char *>(
				reply.constData());
			const auto size = static_cast<size_t>(
				reply.size() * sizeof(mtpPrime));
			result.is_ok = true;
			result.ok_data.assign(data, size);
		} else {
			result.is_ok = false;
			result.error_code = 500;
			result.error_message = "Empty response";
		}

		td::complete_external_query(cId, std::move(query), std::move(result));
		return true;
	};

	auto fail = [this, requestId](
			const MTP::Error &error,
			const MTP::Response &response) -> bool {
		auto it = sentQueries.find(requestId);
		if (it == sentQueries.end()) {
			return true;
		}
		const auto cId = it->second.first;
		auto query = std::move(it->second.second);
		sentQueries.erase(it);

		auto result = td::ExternalQueryResult();
		result.is_ok = false;
		result.error_code = error.code();
		result.error_message = error.type().toStdString();

		td::complete_external_query(cId, std::move(query), std::move(result));
		return true;
	};

	mtp->sendSerialized(
		requestId,
		std::move(serialized),
		MTP::ResponseHandler{ std::move(done), std::move(fail) },
		shiftedDcId,
		0,  // msCanWait
		0); // afterRequestId
}

MTP::ShiftedDcId TdLibBridge::Private::mapDcId(
		MTP::Instance *mtp,
		int32 rawDcId,
		td::NetQuery::Type type) {
	if (rawDcId == 0) {
		// Main DC - use ShiftedDcId 0 which means "current main DC".
		switch (type) {
		case td::NetQuery::Type::Upload:
			return MTP::ShiftDcId(mtp->mainDcId(), MTP::kBaseUploadDcShift);
		case td::NetQuery::Type::Download:
		case td::NetQuery::Type::DownloadSmall:
			return MTP::ShiftDcId(
				mtp->mainDcId(),
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
