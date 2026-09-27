# TDLib recovery after a disconnect

The embedded TDLib client shares Telegram Desktop's MTProto transport, but
maintains its own updates cursor. Live pushes reach both clients. Telegram
Desktop's `updates.getDifference` replies do not enter the live-push handler,
so they cannot repair TDLib's cursor after an outage.

Main-DC connection recovery, session reset, and `new_session_created` now
inject a boxed `updatesTooLong` into the corresponding TDLib clients. TDLib
performs its own difference request and emits the recovered messages through
the existing control socket. Its update manager coalesces concurrent requests
and advances its cursor, preventing duplicate delivery on repeated signals.

## Integration test

In a configured development build:

```sh
cmake -S . -B out -DTDESKTOP_BUILD_TDLIB_TESTS=ON
cmake --build out --target test_tdlib_recovery
out/test_tdlib_recovery
```

The test runs the fork's actual TDLib engine with an isolated temporary
database and an in-process external transport. It verifies that a missed
message remains absent without a recovery signal, that recovery uses the old
cursor, that concurrent signals coalesce, and that a second recovery uses the
new cursor without delivering the message again. No Telegram account or
network access is used. This covers the recovery protocol, not the desktop's
connection callbacks.

## Live regression

Use an isolated client profile and control socket, keeping the regular client
and its monitors untouched. Start `tdctl listen` before disconnecting the test
client's transport. From another authorized client, send a uniquely labelled
message to the test chat while the test client is offline. Restore its
connection without restarting the listener, querying history, or sending a
second message. The existing listener must receive the missed message once.
Disconnect and reconnect again and check that it is not repeated. Delete the
test message and remove the isolated profile afterward. Saved Messages can be
used with `tdctl listen --include-outgoing`; sending even a test message needs
the account owner's authorization.
