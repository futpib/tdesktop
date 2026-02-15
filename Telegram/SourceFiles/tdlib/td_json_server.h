/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtNetwork/QLocalServer>
#include <QtNetwork/QLocalSocket>
#include <QtCore/QTimer>

#include "base/flat_map.h"

namespace TdBridge {

// Unix socket server exposing a line-delimited JSON protocol.
//
// Each request line is a JSON object with a "type" field:
//   {"type":"tdlib", ...}      — forwarded to TDLib (tdjson API)
//   {"type":"tdesktop", ...}   — tdesktop-specific control commands
//
// Responses are JSON objects with the same "type" prefix.
// TDLib responses/updates carry "type":"tdlib".
// Control responses carry "type":"tdesktop".
class ControlServer final : public QObject {
	Q_OBJECT

public:
	explicit ControlServer(const QString &socketPath,
		QObject *parent = nullptr);
	~ControlServer();

	bool start();
	void stop();

private:
	void onNewConnection();
	void onClientReadyRead(QLocalSocket *socket);
	void onClientDisconnected(QLocalSocket *socket);
	void pollTdLib();
	void processLine(QLocalSocket *socket, const QByteArray &line);
	void handleTdLibRequest(QLocalSocket *socket, const QJsonObject &obj);
	void handleControlRequest(QLocalSocket *socket, const QJsonObject &obj);
	void sendJson(QLocalSocket *socket, const QJsonObject &obj);

	QString _socketPath;
	QLocalServer _server;
	QTimer _pollTimer;

	struct ClientInfo {
		int tdlibClientId = 0;
		QByteArray readBuffer;
	};
	base::flat_map<QLocalSocket*, ClientInfo> _clients;
	base::flat_map<int, QLocalSocket*> _tdlibClientIdToSocket;
};

} // namespace TdBridge
