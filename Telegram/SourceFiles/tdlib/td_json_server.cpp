/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tdlib/td_json_server.h"

#include "base/basic_types.h"
#include "export/export_controller.h"
#include "export/export_settings.h"
#include "export/output/export_output_abstract.h"
#include "main/main_domain.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "mtproto/mtp_instance.h"
#include "mtproto/core_types.h"
#include "mtproto/details/mtproto_serialized_request.h"
#include "mtproto/mtproto_response.h"
#include "mtproto/mtproto_config.h"
#include "lang/lang_keys.h"

#include <td/telegram/td_json_client.h>
#include <td/telegram/telegram_api.h>
#include <td/telegram/telegram_api.hpp>
#include <td/telegram/telegram_api_json.h>
#include <td/tl/tl_json.h>
#include <td/utils/JsonBuilder.h>
#include <td/utils/buffer.h>
#include <td/utils/tl_storers.h>
#include <td/utils/tl_parsers.h>

#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

namespace TdBridge {
namespace {

Export::Settings ParseExportSettings(const QJsonObject &settingsJson) {
	auto result = Export::Settings();

	result.path = settingsJson.value("path").toString();

	const auto formatStr = settingsJson.value("format").toString(u"json"_q);
	if (formatStr == u"html"_q) {
		result.format = Export::Output::Format::Html;
	} else if (formatStr == u"json"_q) {
		result.format = Export::Output::Format::Json;
	} else if (formatStr == u"html_and_json"_q) {
		result.format = Export::Output::Format::HtmlAndJson;
	} else {
		result.format = Export::Output::Format::Json;
	}

	const auto typesArray = settingsJson.value("types").toArray();
	if (!typesArray.isEmpty()) {
		result.types = Export::Settings::Types();
		for (const auto &val : typesArray) {
			const auto t = val.toString();
			if (t == u"personal_info"_q) {
				result.types |= Export::Settings::Type::PersonalInfo;
			} else if (t == u"userpics"_q) {
				result.types |= Export::Settings::Type::Userpics;
			} else if (t == u"contacts"_q) {
				result.types |= Export::Settings::Type::Contacts;
			} else if (t == u"sessions"_q) {
				result.types |= Export::Settings::Type::Sessions;
			} else if (t == u"other_data"_q) {
				result.types |= Export::Settings::Type::OtherData;
			} else if (t == u"personal_chats"_q) {
				result.types |= Export::Settings::Type::PersonalChats;
			} else if (t == u"bot_chats"_q) {
				result.types |= Export::Settings::Type::BotChats;
			} else if (t == u"private_groups"_q) {
				result.types |= Export::Settings::Type::PrivateGroups;
			} else if (t == u"public_groups"_q) {
				result.types |= Export::Settings::Type::PublicGroups;
			} else if (t == u"private_channels"_q) {
				result.types |= Export::Settings::Type::PrivateChannels;
			} else if (t == u"public_channels"_q) {
				result.types |= Export::Settings::Type::PublicChannels;
			} else if (t == u"stories"_q) {
				result.types |= Export::Settings::Type::Stories;
			} else if (t == u"profile_music"_q) {
				result.types |= Export::Settings::Type::ProfileMusic;
			}
		}
	}

	const auto mediaObj = settingsJson.value("media").toObject();
	if (!mediaObj.isEmpty()) {
		const auto mediaTypesArray = mediaObj.value("types").toArray();
		if (!mediaTypesArray.isEmpty()) {
			result.media.types = Export::MediaSettings::Types();
			for (const auto &val : mediaTypesArray) {
				const auto t = val.toString();
				if (t == u"photo"_q) {
					result.media.types |= Export::MediaSettings::Type::Photo;
				} else if (t == u"video"_q) {
					result.media.types |= Export::MediaSettings::Type::Video;
				} else if (t == u"voice"_q) {
					result.media.types |= Export::MediaSettings::Type::VoiceMessage;
				} else if (t == u"video_message"_q) {
					result.media.types |= Export::MediaSettings::Type::VideoMessage;
				} else if (t == u"sticker"_q) {
					result.media.types |= Export::MediaSettings::Type::Sticker;
				} else if (t == u"gif"_q) {
					result.media.types |= Export::MediaSettings::Type::GIF;
				} else if (t == u"file"_q) {
					result.media.types |= Export::MediaSettings::Type::File;
				}
			}
		}
		if (mediaObj.contains("size_limit")) {
			result.media.sizeLimit = int64(mediaObj.value("size_limit").toDouble());
		}
	}

	if (settingsJson.contains("from_date")) {
		result.singlePeerFrom = TimeId(settingsJson.value("from_date").toInt());
	}
	if (settingsJson.contains("till_date")) {
		result.singlePeerTill = TimeId(settingsJson.value("till_date").toInt());
	}

	return result;
}

Export::Environment PrepareEnvironment(Main::Session *session) {
	auto result = Export::Environment();
	if (session) {
		result.internalLinksDomain = session->serverConfig().internalLinksDomain;
	} else {
		result.internalLinksDomain = u"https://t.me/"_q;
	}
	result.aboutTelegram = tr::lng_export_about_telegram(tr::now).toUtf8();
	result.aboutContacts = tr::lng_export_about_contacts(tr::now).toUtf8();
	result.aboutFrequent = tr::lng_export_about_frequent(tr::now).toUtf8();
	result.aboutSessions = tr::lng_export_about_sessions(tr::now).toUtf8();
	result.aboutWebSessions = tr::lng_export_about_web_sessions(tr::now).toUtf8();
	result.aboutChats = tr::lng_export_about_chats(tr::now).toUtf8();
	result.aboutLeftChats = tr::lng_export_about_left_chats(tr::now).toUtf8();
	return result;
}

QString StepToString(Export::ProcessingState::Step step) {
	using Step = Export::ProcessingState::Step;
	switch (step) {
	case Step::Initializing: return u"Initializing"_q;
	case Step::DialogsList: return u"DialogsList"_q;
	case Step::PersonalInfo: return u"PersonalInfo"_q;
	case Step::Userpics: return u"Userpics"_q;
	case Step::Stories: return u"Stories"_q;
	case Step::ProfileMusic: return u"ProfileMusic"_q;
	case Step::Contacts: return u"Contacts"_q;
	case Step::Sessions: return u"Sessions"_q;
	case Step::OtherData: return u"OtherData"_q;
	case Step::Dialogs: return u"Dialogs"_q;
	case Step::Topic: return u"Topic"_q;
	}
	return u"Unknown"_q;
}

QString EntityTypeToString(Export::ProcessingState::EntityType type) {
	using Type = Export::ProcessingState::EntityType;
	switch (type) {
	case Type::Chat: return u"Chat"_q;
	case Type::SavedMessages: return u"SavedMessages"_q;
	case Type::RepliesMessages: return u"RepliesMessages"_q;
	case Type::VerifyCodes: return u"VerifyCodes"_q;
	case Type::Topic: return u"Topic"_q;
	case Type::Other: return u"Other"_q;
	}
	return u"Unknown"_q;
}

QJsonObject ExportStateToJson(
		int accountIndex,
		const Export::State &state,
		const QJsonValue &extra) {
	auto payload = QJsonObject{
		{ "command", "exportProgress" },
		{ "account", accountIndex },
	};

	v::match(state, [&](const Export::ProcessingState &s) {
		payload["state"] = "processing";
		payload["step"] = StepToString(s.step);
		payload["entity_type"] = EntityTypeToString(s.entityType);
		if (!s.entityName.isEmpty()) {
			payload["entity_name"] = s.entityName;
		}
		payload["entity_index"] = s.entityIndex;
		payload["entity_count"] = s.entityCount;
		payload["item_index"] = s.itemIndex;
		payload["item_count"] = s.itemCount;
		if (s.bytesCount > 0) {
			payload["bytes_loaded"] = qint64(s.bytesLoaded);
			payload["bytes_count"] = qint64(s.bytesCount);
			if (!s.bytesName.isEmpty()) {
				payload["bytes_name"] = s.bytesName;
			}
		}
	}, [&](const Export::FinishedState &s) {
		payload["state"] = "finished";
		payload["path"] = s.path;
		payload["files_count"] = s.filesCount;
		payload["bytes_count"] = qint64(s.bytesCount);
	}, [&](const Export::ApiErrorState &s) {
		payload["state"] = "error";
		payload["error_code"] = s.data.code();
		payload["message"] = s.data.type();
		if (!s.data.description().isEmpty()) {
			payload["description"] = s.data.description();
		}
	}, [&](const Export::OutputErrorState &s) {
		payload["state"] = "error";
		payload["message"] = u"Output error"_q;
		payload["path"] = s.path;
	}, [&](const Export::CancelledState &) {
		payload["state"] = "cancelled";
	}, [&](const Export::PasswordCheckState &) {
		payload["state"] = "password_check";
	}, [&](v::null_t) {
	});

	if (!extra.isUndefined()) {
		payload["@extra"] = extra;
	}

	return QJsonObject{
		{ "type", "tdesktop" },
		{ "payload", payload },
	};
}

} // namespace

ControlServer::ControlServer(
		const QString &socketPath,
		const QStringList &allowedAccountSpecs,
		QObject *parent)
: QObject(parent)
, _socketPath(socketPath)
, _allowedAccountSpecs(allowedAccountSpecs) {
	connect(&_server, &QLocalServer::newConnection,
		this, &ControlServer::onNewConnection);

	_pollTimer.setInterval(50);
	connect(&_pollTimer, &QTimer::timeout,
		this, &ControlServer::pollTdLib);
}

ControlServer::~ControlServer() {
	stop();
}

bool ControlServer::start() {
	// Remove stale socket file.
	QFile::remove(_socketPath);

	if (!_server.listen(_socketPath)) {
		return false;
	}
	_pollTimer.start();
	return true;
}

void ControlServer::stop() {
	_pollTimer.stop();

	_activeExports.clear();

	for (auto &[socket, info] : _clients) {
		socket->disconnectFromServer();
	}
	_clients.clear();
	_accounts.clear();
	_clientIdToAccount.clear();
	_server.close();

	QFile::remove(_socketPath);
}

void ControlServer::setDomain(not_null<Main::Domain*> domain) {
	_domain = domain;
}

bool ControlServer::isAccountAllowed(int accountIndex) const {
	for (const auto &spec : _allowedAccountSpecs) {
		if (spec == u"*"_q) {
			return true;
		}
		bool ok = false;
		if (spec.toInt(&ok) == accountIndex && ok) {
			return true;
		}
		auto it = _accounts.find(accountIndex);
		if (it != _accounts.end()
			&& !it->second.info.username.isEmpty()
			&& it->second.info.username.compare(
				spec, Qt::CaseInsensitive) == 0) {
			return true;
		}
	}
	return false;
}

void ControlServer::recomputeDefaultAccount() {
	for (const auto &[accountIndex, entry] : _accounts) {
		if (isAccountAllowed(accountIndex)) {
			_defaultAccount = accountIndex;
			return;
		}
	}
	_defaultAccount = 0;
}

void ControlServer::addAccountClient(
		int accountIndex,
		int tdlibClientId) {
	addAccountClient(accountIndex, tdlibClientId, AccountInfo());
}

void ControlServer::addAccountClient(
		int accountIndex,
		int tdlibClientId,
		const AccountInfo &info) {
	_accounts[accountIndex] = AccountEntry{ tdlibClientId, info };
	_clientIdToAccount[tdlibClientId] = accountIndex;
	recomputeDefaultAccount();
}

void ControlServer::updateAccountInfo(
		int accountIndex,
		const AccountInfo &info) {
	auto it = _accounts.find(accountIndex);
	if (it != _accounts.end()) {
		it->second.info = info;
		recomputeDefaultAccount();
	}
}

void ControlServer::removeAccountClient(int accountIndex) {
	auto it = _accounts.find(accountIndex);
	if (it != _accounts.end()) {
		_clientIdToAccount.erase(it->second.clientId);
		_accounts.erase(it);
	}
	_activeExports.erase(accountIndex);
	recomputeDefaultAccount();
}

void ControlServer::onNewConnection() {
	while (auto *socket = _server.nextPendingConnection()) {
		_clients[socket];  // create empty SocketInfo

		connect(socket, &QLocalSocket::readyRead,
			this, [this, socket] { onClientReadyRead(socket); });
		connect(socket, &QLocalSocket::disconnected,
			this, [this, socket] { onClientDisconnected(socket); });
	}
}

void ControlServer::onClientReadyRead(QLocalSocket *socket) {
	auto it = _clients.find(socket);
	if (it == _clients.end()) {
		return;
	}
	auto &info = it->second;

	info.readBuffer.append(socket->readAll());

	while (true) {
		const auto nlPos = info.readBuffer.indexOf('\n');
		if (nlPos < 0) {
			break;
		}
		auto line = info.readBuffer.left(nlPos).trimmed();
		info.readBuffer.remove(0, nlPos + 1);

		if (!line.isEmpty()) {
			processLine(socket, line);
		}
	}
}

void ControlServer::processLine(
		QLocalSocket *socket,
		const QByteArray &line) {
	auto doc = QJsonDocument::fromJson(line);
	if (doc.isNull() || !doc.isObject()) {
		sendJson(socket, QJsonObject{
			{ "type", "error" },
			{ "code", 400 },
			{ "message", "Invalid JSON" },
		});
		return;
	}

	auto obj = doc.object();
	const auto type = obj.value("type").toString();

	// Resolve account index and enforce access control.
	// For tdlib/mtp the account is at the top level; for tdesktop
	// export commands it's inside the payload. Account-less tdesktop
	// commands (ping, listAccounts) skip this check.
	auto accountIndex = _defaultAccount;
	auto needsAccountCheck = false;

	if (type == u"tdlib"_q || type == u"mtp"_q) {
		needsAccountCheck = true;
		if (obj.contains("account")) {
			accountIndex = obj.value("account").toInt(_defaultAccount);
		} else {
			obj["account"] = _defaultAccount;
		}
	} else if (type == u"tdesktop"_q) {
		const auto payload = obj.value("payload").toObject();
		const auto command = payload.value("command").toString();
		if (command == u"export"_q || command == u"cancelExport"_q) {
			needsAccountCheck = true;
			accountIndex = payload.value("account").toInt(_defaultAccount);
		}
	}

	if (needsAccountCheck && !isAccountAllowed(accountIndex)) {
		const auto errorPayload = QJsonObject{
			{ "@type", "error" },
			{ "code", 403 },
			{ "message",
				u"Account %1 not allowed"_q.arg(accountIndex) },
		};
		if (type == u"tdesktop"_q) {
			sendJson(socket, QJsonObject{
				{ "type", "tdesktop" },
				{ "payload", errorPayload },
			});
		} else {
			sendJson(socket, QJsonObject{
				{ "type", type },
				{ "account", accountIndex },
				{ "payload", errorPayload },
			});
		}
		return;
	}

	if (type == u"tdlib"_q) {
		handleTdLibRequest(socket, obj);
	} else if (type == u"tdesktop"_q) {
		handleControlRequest(socket, obj.value("payload").toObject());
	} else if (type == u"mtp"_q) {
		handleMtpRequest(socket, obj);
	} else {
		sendJson(socket, QJsonObject{
			{ "type", "error" },
			{ "code", 400 },
			{ "message",
				u"Unknown type: \"%1\". Expected \"tdlib\", \"tdesktop\", or \"mtp\"."_q
					.arg(type) },
		});
	}
}

void ControlServer::handleTdLibRequest(
		QLocalSocket *socket,
		const QJsonObject &obj) {
	const auto accountIndex = obj.value("account").toInt(0);
	const auto payload = obj.value("payload").toObject();

	auto it = _accounts.find(accountIndex);
	if (it == _accounts.end()) {
		sendJson(socket, QJsonObject{
			{ "type", "tdlib" },
			{ "account", accountIndex },
			{ "payload", QJsonObject{
				{ "@type", "error" },
				{ "code", 404 },
				{ "message",
					u"No TDLib client for account %1"_q.arg(accountIndex) },
			}},
		});
		return;
	}

	const auto json = QJsonDocument(payload).toJson(QJsonDocument::Compact);
	td_send(it->second.clientId, json.constData());
}

void ControlServer::handleMtpRequest(
		QLocalSocket *socket,
		const QJsonObject &obj) {
	const auto accountIndex = obj.value("account").toInt(0);
	const auto payloadValue = obj.value("payload");
	const auto extra = obj.value("@extra");

	if (!payloadValue.isObject()) {
		sendJson(socket, QJsonObject{
			{ "type", "mtp" },
			{ "account", accountIndex },
			{ "payload", QJsonObject{
				{ "@type", "error" },
				{ "code", 400 },
				{ "message", "Missing or invalid 'payload' object" },
			}},
		});
		return;
	}

	if (!_domain) {
		auto response = QJsonObject{
			{ "type", "mtp" },
			{ "account", accountIndex },
			{ "payload", QJsonObject{
				{ "@type", "error" },
				{ "code", 500 },
				{ "message", "Domain not available" },
			}},
		};
		if (!extra.isUndefined()) {
			response["@extra"] = extra;
		}
		sendJson(socket, response);
		return;
	}

	// Find the account.
	Main::Account *account = nullptr;
	for (const auto &[idx, acc] : _domain->accounts()) {
		if (idx == accountIndex) {
			account = acc.get();
			break;
		}
	}
	if (!account || !account->sessionExists()) {
		auto response = QJsonObject{
			{ "type", "mtp" },
			{ "account", accountIndex },
			{ "payload", QJsonObject{
				{ "@type", "error" },
				{ "code", 404 },
				{ "message",
					u"No active session for account %1"_q
						.arg(accountIndex) },
			}},
		};
		if (!extra.isUndefined()) {
			response["@extra"] = extra;
		}
		sendJson(socket, response);
		return;
	}

	// Parse the JSON payload into a telegram_api::Function.
	auto payloadJson = QJsonDocument(
		payloadValue.toObject()).toJson(QJsonDocument::Compact);
	auto r_json_value = td::json_decode(
		td::MutableSlice(payloadJson.data(), payloadJson.size()));
	if (r_json_value.is_error()) {
		auto response = QJsonObject{
			{ "type", "mtp" },
			{ "account", accountIndex },
			{ "payload", QJsonObject{
				{ "@type", "error" },
				{ "code", 400 },
				{ "message", u"JSON decode error: %1"_q.arg(
					QString::fromStdString(
						r_json_value.error().message().str())) },
			}},
		};
		if (!extra.isUndefined()) {
			response["@extra"] = extra;
		}
		sendJson(socket, response);
		return;
	}

	td::telegram_api::object_ptr<td::telegram_api::Function> func;
	auto status = td::telegram_api::from_json(
		func, r_json_value.move_as_ok());
	if (status.is_error() || !func) {
		auto response = QJsonObject{
			{ "type", "mtp" },
			{ "account", accountIndex },
			{ "payload", QJsonObject{
				{ "@type", "error" },
				{ "code", 400 },
				{ "message", u"Failed to parse telegram_api function: %1"_q
					.arg(QString::fromStdString(
						status.is_error()
							? status.error().message().str()
							: "null result")) },
			}},
		};
		if (!extra.isUndefined()) {
			response["@extra"] = extra;
		}
		sendJson(socket, response);
		return;
	}

	// Serialize the Function to TL binary.
	td::TlStorerCalcLength calcLength;
	func->store(calcLength);
	const auto tlSize = calcLength.get_length();

	auto tlBuffer = std::vector<unsigned char>(tlSize);
	td::TlStorerUnsafe storer(tlBuffer.data());
	func->store(storer);

	// Build a SerializedRequest from the raw TL bytes.
	const auto bodySizeInInts = static_cast<uint32>((tlSize + 3) / 4);
	auto serialized = MTP::details::SerializedRequest::Prepare(
		bodySizeInInts);
	auto &buf = *serialized;

	const auto *srcInts = reinterpret_cast<const mtpPrime *>(
		tlBuffer.data());
	for (uint32 i = 0; i < bodySizeInInts; ++i) {
		if ((i + 1) * sizeof(mtpPrime) <= tlSize) {
			buf.push_back(srcInts[i]);
		} else {
			mtpPrime last = 0;
			memcpy(&last, tlBuffer.data() + i * sizeof(mtpPrime),
				tlSize - i * sizeof(mtpPrime));
			buf.push_back(last);
		}
	}
	serialized->needsLayer = true;

	const auto requestId = MTP::details::GetNextRequestId();
	serialized->requestId = requestId;

	// Prevent socket and extra from dangling in the callbacks
	// by capturing copies/weak references.
	auto socketPtr = QPointer<QLocalSocket>(socket);
	auto extraCopy = extra;

	auto done = [this, socketPtr, accountIndex, extraCopy](
			const MTP::Response &response) -> bool {
		if (!socketPtr) {
			return true;
		}

		const auto &reply = response.reply;
		if (reply.isEmpty()) {
			auto resp = QJsonObject{
				{ "type", "mtp" },
				{ "account", accountIndex },
				{ "payload", QJsonObject{
					{ "@type", "error" },
					{ "code", 500 },
					{ "message", "Empty response" },
				}},
			};
			if (!extraCopy.isUndefined()) {
				resp["@extra"] = extraCopy;
			}
			sendJson(socketPtr.data(), resp);
			return true;
		}

		// Parse the raw MTP response into a telegram_api::Object.
		const auto *data = reinterpret_cast<const char *>(
			reply.constData());
		const auto size = static_cast<size_t>(
			reply.size() * sizeof(mtpPrime));

		// Check for Bool responses (boolTrue/boolFalse) which are
		// not part of the Object hierarchy.
		constexpr int32_t kBoolFalse = 0xbc799737;
		constexpr int32_t kBoolTrue = 0x997275b5;
		if (size >= sizeof(int32_t)) {
			int32_t constructorId = 0;
			memcpy(&constructorId, data, sizeof(int32_t));
			if (constructorId == kBoolTrue
				|| constructorId == kBoolFalse) {
				auto resp = QJsonObject{
					{ "type", "mtp" },
					{ "account", accountIndex },
					{ "payload", constructorId == kBoolTrue },
				};
				if (!extraCopy.isUndefined()) {
					resp["@extra"] = extraCopy;
				}
				sendJson(socketPtr.data(), resp);
				return true;
			}
		}

		auto bufSlice = td::BufferSlice(td::Slice(data, size));
		td::TlBufferParser parser(&bufSlice);
		auto resultObj = td::telegram_api::Object::fetch(parser);
		parser.fetch_end();

		if (parser.get_error() || !resultObj) {
			auto resp = QJsonObject{
				{ "type", "mtp" },
				{ "account", accountIndex },
				{ "payload", QJsonObject{
					{ "@type", "error" },
					{ "code", 500 },
					{ "message", u"Failed to parse response: %1"_q.arg(
						parser.get_error()
							? QString::fromUtf8(parser.get_error())
							: u"null"_q) },
				}},
			};
			if (!extraCopy.isUndefined()) {
				resp["@extra"] = extraCopy;
			}
			sendJson(socketPtr.data(), resp);
			return true;
		}

		// Serialize the result object to JSON.
		auto jsonStr = td::json_encode<std::string>(
			td::ToJson(*resultObj));
		auto jsonDoc = QJsonDocument::fromJson(
			QByteArray::fromRawData(jsonStr.data(), jsonStr.size()));

		auto resp = QJsonObject{
			{ "type", "mtp" },
			{ "account", accountIndex },
			{ "payload", jsonDoc.object() },
		};
		if (!extraCopy.isUndefined()) {
			resp["@extra"] = extraCopy;
		}
		sendJson(socketPtr.data(), resp);
		return true;
	};

	auto fail = [this, socketPtr, accountIndex, extraCopy](
			const MTP::Error &error,
			const MTP::Response &) -> bool {
		if (!socketPtr) {
			return true;
		}
		auto resp = QJsonObject{
			{ "type", "mtp" },
			{ "account", accountIndex },
			{ "payload", QJsonObject{
				{ "@type", "error" },
				{ "code", error.code() },
				{ "message", error.type() },
			}},
		};
		if (!extraCopy.isUndefined()) {
			resp["@extra"] = extraCopy;
		}
		sendJson(socketPtr.data(), resp);
		return true;
	};

	account->mtp().sendSerialized(
		requestId,
		std::move(serialized),
		MTP::ResponseHandler{ std::move(done), std::move(fail) },
		0,  // shiftedDcId: 0 = main DC
		0,  // msCanWait
		0); // afterRequestId
}

void ControlServer::handleControlRequest(
		QLocalSocket *socket,
		const QJsonObject &payload) {
	const auto command = payload.value("command").toString();
	const auto extra = payload.value("@extra");

	if (command == u"ping"_q) {
		auto responsePayload = QJsonObject{
			{ "command", "pong" },
		};
		if (!extra.isUndefined()) {
			responsePayload["@extra"] = extra;
		}
		sendJson(socket, QJsonObject{
			{ "type", "tdesktop" },
			{ "payload", responsePayload },
		});
	} else if (command == u"listAccounts"_q) {
		auto accounts = QJsonArray();
		for (const auto &[accountIndex, entry] : _accounts) {
			if (!isAccountAllowed(accountIndex)) {
				continue;
			}
			auto obj = QJsonObject{
				{ "index", accountIndex },
			};
			const auto &info = entry.info;
			if (!info.firstName.isEmpty()) {
				obj["first_name"] = info.firstName;
			}
			if (!info.lastName.isEmpty()) {
				obj["last_name"] = info.lastName;
			}
			if (!info.username.isEmpty()) {
				obj["username"] = info.username;
			}
			if (!info.phone.isEmpty()) {
				obj["phone"] = info.phone;
			}
			if (info.userId) {
				obj["user_id"] = qint64(info.userId);
			}
			accounts.append(obj);
		}
		auto responsePayload = QJsonObject{
			{ "command", "listAccounts" },
			{ "accounts", accounts },
		};
		if (!extra.isUndefined()) {
			responsePayload["@extra"] = extra;
		}
		sendJson(socket, QJsonObject{
			{ "type", "tdesktop" },
			{ "payload", responsePayload },
		});
	} else if (command == u"export"_q) {
		handleExportCommand(socket, payload, extra);
	} else if (command == u"cancelExport"_q) {
		handleCancelExportCommand(socket, payload, extra);
	} else {
		auto responsePayload = QJsonObject{
			{ "error", u"Unknown command: \"%1\""_q.arg(command) },
		};
		if (!extra.isUndefined()) {
			responsePayload["@extra"] = extra;
		}
		sendJson(socket, QJsonObject{
			{ "type", "tdesktop" },
			{ "payload", responsePayload },
		});
	}
}

void ControlServer::handleExportCommand(
		QLocalSocket *socket,
		const QJsonObject &payload,
		const QJsonValue &extra) {
	if (!_domain) {
		auto responsePayload = QJsonObject{
			{ "command", "exportProgress" },
			{ "state", "error" },
			{ "message", "Domain not available" },
		};
		if (!extra.isUndefined()) {
			responsePayload["@extra"] = extra;
		}
		sendJson(socket, QJsonObject{
			{ "type", "tdesktop" },
			{ "payload", responsePayload },
		});
		return;
	}

	const auto accountIndex = payload.value("account").toInt(0);

	if (_activeExports.contains(accountIndex)) {
		auto responsePayload = QJsonObject{
			{ "command", "exportProgress" },
			{ "account", accountIndex },
			{ "state", "error" },
			{ "message", "Export already running for this account" },
		};
		if (!extra.isUndefined()) {
			responsePayload["@extra"] = extra;
		}
		sendJson(socket, QJsonObject{
			{ "type", "tdesktop" },
			{ "payload", responsePayload },
		});
		return;
	}

	// Find the account by index.
	Main::Account *account = nullptr;
	for (const auto &[idx, acc] : _domain->accounts()) {
		if (idx == accountIndex) {
			account = acc.get();
			break;
		}
	}
	if (!account) {
		auto responsePayload = QJsonObject{
			{ "command", "exportProgress" },
			{ "account", accountIndex },
			{ "state", "error" },
			{ "message", u"Account %1 not found"_q.arg(accountIndex) },
		};
		if (!extra.isUndefined()) {
			responsePayload["@extra"] = extra;
		}
		sendJson(socket, QJsonObject{
			{ "type", "tdesktop" },
			{ "payload", responsePayload },
		});
		return;
	}

	if (!account->sessionExists()) {
		auto responsePayload = QJsonObject{
			{ "command", "exportProgress" },
			{ "account", accountIndex },
			{ "state", "error" },
			{ "message", u"Account %1 has no active session"_q.arg(accountIndex) },
		};
		if (!extra.isUndefined()) {
			responsePayload["@extra"] = extra;
		}
		sendJson(socket, QJsonObject{
			{ "type", "tdesktop" },
			{ "payload", responsePayload },
		});
		return;
	}

	const auto settingsJson = payload.value("settings").toObject();
	if (settingsJson.value("path").toString().isEmpty()) {
		auto responsePayload = QJsonObject{
			{ "command", "exportProgress" },
			{ "account", accountIndex },
			{ "state", "error" },
			{ "message", "Missing required field: settings.path" },
		};
		if (!extra.isUndefined()) {
			responsePayload["@extra"] = extra;
		}
		sendJson(socket, QJsonObject{
			{ "type", "tdesktop" },
			{ "payload", responsePayload },
		});
		return;
	}

	auto settings = ParseExportSettings(settingsJson);

	auto &activeExport = _activeExports[accountIndex];
	activeExport.extra = extra;
	activeExport.controller = std::make_unique<Export::Controller>(
		&account->mtp(),
		MTP_inputPeerEmpty());

	auto *session = account->maybeSession();
	auto environment = PrepareEnvironment(session);

	activeExport.controller->state(
	) | rpl::on_next([this, accountIndex](Export::State state) {
		auto it = _activeExports.find(accountIndex);
		if (it == _activeExports.end()) {
			return;
		}
		const auto &activeExtra = it->second.extra;
		const auto isTerminal = v::is<Export::FinishedState>(state)
			|| v::is<Export::ApiErrorState>(state)
			|| v::is<Export::OutputErrorState>(state)
			|| v::is<Export::CancelledState>(state);

		broadcastJson(ExportStateToJson(accountIndex, state, activeExtra));

		if (isTerminal) {
			_activeExports.erase(accountIndex);
		}
	}, activeExport.lifetime);

	activeExport.controller->startExport(settings, environment);

	// Send acknowledgment.
	auto responsePayload = QJsonObject{
		{ "command", "exportStarted" },
		{ "account", accountIndex },
	};
	if (!extra.isUndefined()) {
		responsePayload["@extra"] = extra;
	}
	sendJson(socket, QJsonObject{
		{ "type", "tdesktop" },
		{ "payload", responsePayload },
	});
}

void ControlServer::handleCancelExportCommand(
		QLocalSocket *socket,
		const QJsonObject &payload,
		const QJsonValue &extra) {
	const auto accountIndex = payload.value("account").toInt(0);

	auto it = _activeExports.find(accountIndex);
	if (it == _activeExports.end()) {
		auto responsePayload = QJsonObject{
			{ "command", "exportProgress" },
			{ "account", accountIndex },
			{ "state", "error" },
			{ "message", u"No active export for account %1"_q.arg(accountIndex) },
		};
		if (!extra.isUndefined()) {
			responsePayload["@extra"] = extra;
		}
		sendJson(socket, QJsonObject{
			{ "type", "tdesktop" },
			{ "payload", responsePayload },
		});
		return;
	}

	it->second.controller->cancelExportFast();

	auto responsePayload = QJsonObject{
		{ "command", "exportCancelled" },
		{ "account", accountIndex },
	};
	if (!extra.isUndefined()) {
		responsePayload["@extra"] = extra;
	}
	sendJson(socket, QJsonObject{
		{ "type", "tdesktop" },
		{ "payload", responsePayload },
	});
}

void ControlServer::sendJson(
		QLocalSocket *socket,
		const QJsonObject &obj) {
	if (socket->state() != QLocalSocket::ConnectedState) {
		return;
	}
	socket->write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
	socket->write("\n");
}

void ControlServer::broadcastJson(const QJsonObject &obj) {
	for (auto &[socket, info] : _clients) {
		sendJson(socket, obj);
	}
}

void ControlServer::onClientDisconnected(QLocalSocket *socket) {
	_clients.erase(socket);
	socket->deleteLater();
}

void ControlServer::pollTdLib() {
	while (true) {
		const char *result = td_receive(0);
		if (!result) {
			break;
		}

		// Parse to extract @client_id.
		auto doc = QJsonDocument::fromJson(QByteArray(result));
		if (doc.isNull()) {
			continue;
		}

		auto obj = doc.object();
		const auto clientId = obj.value("@client_id").toInt();

		// Look up account index for this client ID.
		auto accountIt = _clientIdToAccount.find(clientId);
		if (accountIt == _clientIdToAccount.end()) {
			continue;
		}
		const auto accountIndex = accountIt->second;

		// Remove @client_id, wrap in envelope with "type":"tdlib".
		obj.remove("@client_id");

		auto envelope = QJsonObject{
			{ "type", "tdlib" },
			{ "account", accountIndex },
			{ "payload", obj },
		};

		// Broadcast to all connected sockets.
		broadcastJson(envelope);
	}
}

} // namespace TdBridge
