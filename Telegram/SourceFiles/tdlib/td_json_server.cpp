/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tdlib/td_json_server.h"

#include "base/basic_types.h"

#include <td/telegram/td_json_client.h>

#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

namespace TdBridge {

ControlServer::ControlServer(const QString &socketPath, QObject *parent)
: QObject(parent)
, _socketPath(socketPath) {
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

	for (auto &[socket, info] : _clients) {
		socket->disconnectFromServer();
	}
	_clients.clear();
	_accounts.clear();
	_clientIdToAccount.clear();
	_server.close();

	QFile::remove(_socketPath);
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
}

void ControlServer::updateAccountInfo(
		int accountIndex,
		const AccountInfo &info) {
	auto it = _accounts.find(accountIndex);
	if (it != _accounts.end()) {
		it->second.info = info;
	}
}

void ControlServer::removeAccountClient(int accountIndex) {
	auto it = _accounts.find(accountIndex);
	if (it != _accounts.end()) {
		_clientIdToAccount.erase(it->second.clientId);
		_accounts.erase(it);
	}
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

	if (type == u"tdlib"_q) {
		handleTdLibRequest(socket, obj);
	} else if (type == u"tdesktop"_q) {
		handleControlRequest(socket, obj.value("payload").toObject());
	} else {
		sendJson(socket, QJsonObject{
			{ "type", "error" },
			{ "code", 400 },
			{ "message",
				u"Unknown type: \"%1\". Expected \"tdlib\" or \"tdesktop\"."_q
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
			{ "type", "error" },
			{ "account", accountIndex },
			{ "code", 404 },
			{ "message",
				u"No TDLib client for account %1"_q.arg(accountIndex) },
		});
		return;
	}

	const auto json = QJsonDocument(payload).toJson(QJsonDocument::Compact);
	td_send(it->second.clientId, json.constData());
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

void ControlServer::sendJson(
		QLocalSocket *socket,
		const QJsonObject &obj) {
	if (socket->state() != QLocalSocket::ConnectedState) {
		return;
	}
	socket->write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
	socket->write("\n");
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
		for (auto &[socket, info] : _clients) {
			sendJson(socket, envelope);
		}
	}
}

} // namespace TdBridge
