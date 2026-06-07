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
#include "base/debug_log.h"

#include <td/telegram/ClientInternal.h>
#include <td/telegram/Client.h>
#include <td/telegram/net/NetQuery.h>
#include <td/telegram/net/NetQueryDispatcher.h>

#include <QtCore/QMetaObject>

#include <mutex>
#include <optional>
#include <utility>

namespace TdBridge {
namespace {

constexpr uint32 kGzipPackedConstructor = 0x3072cfa1;

// upload.saveBigFilePart / upload.saveFilePart constructor ids. Both lay out
// file_id (int64) at byte offset 4 and file_part (int32) at offset 12, so an
// upload part can be keyed by (file_id, part) regardless of which is used.
constexpr uint32 kSaveBigFilePartConstructor = 0xde7b673d;
constexpr uint32 kSaveFilePartConstructor = 0xb304a621;

// How many times a transient transport failure on a single upload part is
// reported to TDLib as NetQuery::Error::Canceled -- which makes its
// FileUploader re-issue just that part instead of aborting the whole file --
// before we give up and let the real error through. Bounds the retry loop.
constexpr int kMaxUploadPartRetries = 5;

using UploadPartId = std::pair<td::int64, td::int32>;

// (file_id, part) of an upload-part query, for retry accounting; nullopt for
// anything that isn't a raw saveFilePart/saveBigFilePart.
[[nodiscard]] std::optional<UploadPartId> UploadPartKey(
		const td::ExternalQuery &data) {
	if (data.gzip || data.query.size() < 16) {
		return std::nullopt;
	}
	td::uint32 ctor = 0;
	memcpy(&ctor, data.query.data(), sizeof(ctor));
	if (ctor != kSaveBigFilePartConstructor
		&& ctor != kSaveFilePartConstructor) {
		return std::nullopt;
	}
	td::int64 fileId = 0;
	td::int32 part = 0;
	memcpy(&fileId, data.query.data() + 4, sizeof(fileId));
	memcpy(&part, data.query.data() + 12, sizeof(part));
	return std::make_pair(fileId, part);
}

// A transient transport-level failure that warrants retrying the part rather
// than failing the whole upload. Server application errors (400 family,
// including INPUT_FETCH_FAIL; 403; 420 flood -- which we must NOT hammer) are
// treated as permanent and pass through so the file aborts as it should.
[[nodiscard]] bool IsTransientUploadError(const MTP::Error &error) {
	const auto code = error.code();
	if (code < 0 || code == 500 || code == 503) {
		return true;
	}
	const auto type = error.type();
	return type.startsWith(u"TIMEOUT"_q)
		|| type.startsWith(u"RPC_CALL_FAIL"_q)
		|| type.startsWith(u"RPC_MCGET_FAIL"_q)
		|| type == u"MSG_WAIT_FAILED"_q;
}

// Build an MTP::details::SerializedRequest from raw TL bytes.
// The raw bytes are the serialized telegram_api::Function.
[[nodiscard]] MTP::details::SerializedRequest BuildSerializedRequest(
		td::Slice queryData,
		bool gzipOn) {
	const auto *src = reinterpret_cast<const char *>(queryData.data());
	const auto srcSize = static_cast<uint32>(queryData.size());

	if (gzipOn) {
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
		td::ExternalQuery data;
	};
	struct ClientState {
		MTP::Instance *mtp = nullptr;
		std::vector<PendingQuery> pendingBeforeMtp;
	};

	std::mutex mutex;
	base::flat_map<int, ClientState> clients;

	// requestId -> the in-flight query.  We never hold a NetQueryPtr here: the
	// query stays inside TDLib, keyed by its id, and completion crosses back as
	// a plain id.  uploadPart is set when this is an upload part, so a transient
	// transport failure can be retried per-part instead of failing the file.
	struct Sent {
		int clientId = 0;
		uint64 queryId = 0;
		std::optional<UploadPartId> uploadPart;
	};
	base::flat_map<mtpRequestId, Sent> sentQueries;

	// (file_id, part) -> transient-failure retries already requested from
	// TDLib's FileUploader. Dropped once the part finally succeeds or aborts.
	base::flat_map<UploadPartId, int> uploadPartAttempts;

	struct IncomingEntry {
		int clientId = 0;
		PendingQuery pending;
	};
	// Queries stashed between the TDLib scheduler thread and the Qt thread.
	// Only the plain ExternalQuery crosses; the owning NetQuery is held inside
	// TDLib.
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

	// Finish all stashed-but-not-yet-dispatched queries.  set_error and
	// delivery happen inside TDLib on a scheduler thread; we pass only the id.
	for (auto &[id, entry] : _d->incoming) {
		td::complete_external_query(entry.clientId, id, shuttingDown());
	}
	_d->incoming.clear();

	// Finish all queries waiting for the MTP instance.
	for (auto &[clientId, state] : _d->clients) {
		for (auto &p : state.pendingBeforeMtp) {
			td::complete_external_query(clientId, p.data.id, shuttingDown());
		}
		state.pendingBeforeMtp.clear();
	}

	// Finish all in-flight queries (sent to MTP, waiting for response).
	for (auto &[requestId, sent] : _d->sentQueries) {
		td::complete_external_query(sent.clientId, sent.queryId, shuttingDown());
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
		[this](td::int32 clientId, td::ExternalQuery query) {
			// Called from TDLib's scheduler thread.  We receive only plain data
			// here -- the NetQuery stays inside TDLib, keyed by query.id.  Pass
			// that id through the Qt event queue; nothing actor-owned crosses.
			const auto id = query.id;
			{
				std::lock_guard<std::mutex> lock(_d->mutex);
				_d->incoming.emplace(id, Private::IncomingEntry{
					clientId,
					Private::PendingQuery{ std::move(query) },
				});
			}

			QMetaObject::invokeMethod(this, [this, id]() {
				std::lock_guard<std::mutex> lock(_d->mutex);
				auto it = _d->incoming.find(id);
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

	const auto queryId = pending.data.id;
	auto queryType = static_cast<td::NetQuery::Type>(pending.data.type);

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

	const auto shiftedDcId = mapDcId(mtp, pending.data.raw_dc_id, queryType);

	auto serialized = BuildSerializedRequest(
		pending.data.query,
		pending.data.gzip);

	const auto requestId = MTP::details::GetNextRequestId();
	serialized->requestId = requestId;

	// Remember which TDLib query this MTP request belongs to.  Only the id is
	// held on this (Qt) thread; completion crosses back into TDLib by id.
	// Note the (file_id, part) for upload parts so a transient failure can be
	// retried per-part; identified from the query bytes, not the routed type.
	sentQueries.emplace(requestId, Sent{
		clientId,
		queryId,
		UploadPartKey(pending.data),
	});

	auto done = [this, requestId](const MTP::Response &response) -> bool {
		auto it = sentQueries.find(requestId);
		if (it == sentQueries.end()) {
			return true;
		}
		const auto cId = it->second.clientId;
		const auto qId = it->second.queryId;
		// Part succeeded -- drop any per-part retry accounting.
		if (it->second.uploadPart) {
			uploadPartAttempts.take(*it->second.uploadPart);
		}
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

		td::complete_external_query(cId, qId, std::move(result));
		return true;
	};

	auto fail = [this, requestId](
			const MTP::Error &error,
			const MTP::Response &response) -> bool {
		auto it = sentQueries.find(requestId);
		if (it == sentQueries.end()) {
			return true;
		}
		const auto cId = it->second.clientId;
		const auto qId = it->second.queryId;

		// Surface every failure (code + type, and the part for uploads) so
		// throttling (FLOOD_WAIT/SLOWMODE) vs. genuine errors (INPUT_FETCH_FAIL,
		// FILE_PART_*) is visible in the log instead of only as a TDLib send
		// failure with no context.
		::base::LogWriteMain(QString("TdBridge: query failed: code %1, type %2%3"
			).arg(error.code()
			).arg(error.type()
			).arg(it->second.uploadPart
				? u" [upload part %1]"_q.arg(it->second.uploadPart->second)
				: QString()));

		// Translate a transient transport failure on an upload part into
		// NetQuery::Error::Canceled, which makes TDLib's FileUploader re-issue
		// just that part instead of aborting the whole (multi-thousand-part)
		// upload. TDLib owns the retry; the bridge only reclassifies the error
		// and caps the attempts. Permanent errors -- including throttling's
		// 400 INPUT_FETCH_FAIL and 420 flood -- pass through so the file aborts.
		auto reportCanceled = false;
		if (it->second.uploadPart && IsTransientUploadError(error)) {
			auto &attempts = uploadPartAttempts[*it->second.uploadPart];
			if (attempts < kMaxUploadPartRetries) {
				++attempts;
				reportCanceled = true;
			} else {
				uploadPartAttempts.take(*it->second.uploadPart);
			}
		}
		sentQueries.erase(it);

		auto result = td::ExternalQueryResult();
		result.is_ok = false;
		if (reportCanceled) {
			result.error_code = td::NetQuery::Error::Canceled;
			result.error_message = "Upload part transient failure, retrying";
		} else {
			result.error_code = error.code();
			result.error_message = error.type().toStdString();
		}

		td::complete_external_query(cId, qId, std::move(result));
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
