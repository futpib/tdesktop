/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tdlib/td_json_server.h"

#include <td/telegram/td_json_client.h>

#include <QtCore/QFile>
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
	_tdlibClientIdToSocket.clear();
	_server.close();

	QFile::remove(_socketPath);
}

void ControlServer::onNewConnection() {
	while (auto *socket = _server.nextPendingConnection()) {
		const auto clientId = td_create_client_id();

		auto &info = _clients[socket];
		info.tdlibClientId = clientId;
		_tdlibClientIdToSocket[clientId] = socket;

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
		handleControlRequest(socket, obj);
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
	auto it = _clients.find(socket);
	if (it == _clients.end()) {
		return;
	}

	// Extract the TDLib payload (everything except "type").
	auto payload = obj;
	payload.remove("type");

	const auto json = QJsonDocument(payload).toJson(QJsonDocument::Compact);
	td_send(it->second.tdlibClientId, json.constData());
}

void ControlServer::handleControlRequest(
		QLocalSocket *socket,
		const QJsonObject &obj) {
	const auto command = obj.value("command").toString();
	const auto extra = obj.value("@extra");

	if (command == u"ping"_q) {
		auto response = QJsonObject{
			{ "type", "tdesktop" },
			{ "command", "pong" },
		};
		if (!extra.isUndefined()) {
			response["@extra"] = extra;
		}
		sendJson(socket, response);
	} else {
		auto response = QJsonObject{
			{ "type", "tdesktop" },
			{ "error", u"Unknown command: \"%1\""_q.arg(command) },
		};
		if (!extra.isUndefined()) {
			response["@extra"] = extra;
		}
		sendJson(socket, response);
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
	auto it = _clients.find(socket);
	if (it == _clients.end()) {
		return;
	}

	const auto clientId = it->second.tdlibClientId;
	_tdlibClientIdToSocket.erase(clientId);

	// Send close request to TDLib for this client.
	const auto closeJson = QByteArray("{\"@type\":\"close\"}");
	td_send(clientId, closeJson.constData());

	_clients.erase(it);
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

		auto it = _tdlibClientIdToSocket.find(clientId);
		if (it == _tdlibClientIdToSocket.end()) {
			continue;
		}

		auto *socket = it->second;

		// Remove @client_id, wrap with "type":"tdlib".
		obj.remove("@client_id");
		obj["type"] = "tdlib";

		sendJson(socket, obj);
	}
}

} // namespace TdBridge
