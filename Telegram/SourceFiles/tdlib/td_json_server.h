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

#include <rpl/lifetime.h>

namespace Main {
class Domain;
class Account;
} // namespace Main

namespace Export {
class Controller;
} // namespace Export

namespace TdBridge {

// Unix socket server exposing a line-delimited JSON protocol.
//
// Each request line is a JSON object with a "type" field:
//   {"type":"tdlib", "account":0, "payload":{...}} — forwarded to TDLib
//   {"type":"tdesktop", ...}             — tdesktop-specific control commands
//   {"type":"mtp", "account":0, "payload":{...}}   — raw MTP API calls
//
// Responses are JSON objects with the same "type" prefix.
// TDLib responses/updates carry "type":"tdlib" with TDLib JSON in "payload"
// and "account" indicating which account the response belongs to.
// Control responses carry "type":"tdesktop".
// MTP responses carry "type":"mtp" with the raw telegram_api result in "payload".
class ControlServer final : public QObject {
	Q_OBJECT

public:
	explicit ControlServer(const QString &socketPath,
		const QStringList &allowedAccountSpecs,
		QObject *parent = nullptr);
	~ControlServer();

	bool start();
	void stop();

	void setDomain(not_null<Main::Domain*> domain);

	struct AccountInfo {
		QString firstName;
		QString lastName;
		QString username;
		QString phone;
		uint64 userId = 0;
	};

	void addAccountClient(int accountIndex, int tdlibClientId);
	void addAccountClient(int accountIndex, int tdlibClientId,
		const AccountInfo &info);
	void updateAccountInfo(int accountIndex, const AccountInfo &info);
	void removeAccountClient(int accountIndex);

private:
	void onNewConnection();
	void onClientReadyRead(QLocalSocket *socket);
	void onClientDisconnected(QLocalSocket *socket);
	void pollTdLib();
	void processLine(QLocalSocket *socket, const QByteArray &line);
	void handleTdLibRequest(QLocalSocket *socket, const QJsonObject &obj);
	void handleControlRequest(QLocalSocket *socket, const QJsonObject &obj);
	void handleMtpRequest(QLocalSocket *socket, const QJsonObject &obj);
	void handleExportCommand(
		QLocalSocket *socket,
		const QJsonObject &payload,
		const QJsonValue &extra);
	void handleCancelExportCommand(
		QLocalSocket *socket,
		const QJsonObject &payload,
		const QJsonValue &extra);
	void sendJson(QLocalSocket *socket, const QJsonObject &obj);
	void broadcastJson(const QJsonObject &obj);

	bool isAccountAllowed(int accountIndex) const;
	void recomputeDefaultAccount();

	QString _socketPath;
	QStringList _allowedAccountSpecs;
	int _defaultAccount = 0;
	QLocalServer _server;
	QTimer _pollTimer;

	Main::Domain *_domain = nullptr;

	struct SocketInfo {
		QByteArray readBuffer;
	};
	base::flat_map<QLocalSocket*, SocketInfo> _clients;

	struct AccountEntry {
		int clientId = 0;
		AccountInfo info;
		// Set true after the one-shot loadChats warm-up is fired on the
		// first updateAuthorizationState -> authorizationStateReady.
		// See pollTdLib(); prevents out-of-order chat-list requests from
		// hitting a fatal CHECK in TDLib's MessagesManager.
		bool warmed = false;
	};
	base::flat_map<int, AccountEntry> _accounts;
	base::flat_map<int, int> _clientIdToAccount;

	struct ActiveExport {
		std::unique_ptr<Export::Controller> controller;
		rpl::lifetime lifetime;
		QJsonValue extra;
	};
	base::flat_map<int, ActiveExport> _activeExports;
};

} // namespace TdBridge
