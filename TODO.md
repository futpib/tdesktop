## Add a tdesktop socket command to trigger export

The tdjson socket (`td_json_server`) supports `{"type":"tdesktop", ...}` commands.
Add a new tdesktop-type command to drive the existing export controller programmatically.

### Why not use TDLib directly?

- TDLib's high-level `td_api` does not expose takeout/export functions
- The takeout primitives (`account.initTakeoutSession`, `invokeWithTakeout`, etc.) only exist in the low-level MTProto layer
- tdesktop's export already uses raw MTP calls through its own `export_api_wrap`

### Approach

- Reuse the existing export machinery in `Telegram/SourceFiles/export/` (controller, api wrap, writers)
- Expose it via a `{"type":"tdesktop", "command":"export", ...}` socket message
- Accept export settings (types, media filters, format, output path, date range) as JSON parameters
- Stream progress/status updates back over the socket

### Relevant code

- Socket server: `Telegram/SourceFiles/tdlib/td_json_server.cpp`
- Export manager: `Telegram/SourceFiles/export/export_manager.cpp`
- Export controller: `Telegram/SourceFiles/export/export_controller.cpp`
- Export settings: `Telegram/SourceFiles/export/export_settings.cpp`
- API wrapper: `Telegram/SourceFiles/export/export_api_wrap.cpp`

## Generate a JSON codec for `telegram_api.tl`

TDLib only generates a JSON codec for `td_api.tl` (the high-level API), not for `telegram_api.tl` (the raw MTProto layer). Both are TL-defined, but only `td_api` gets JSON serialization — that's what `td_send`/`td_receive` use.

Extend TDLib's codegen to also produce a JSON serializer/deserializer for `telegram_api.tl` types, then expose raw `telegram_api` calls over the tdjson socket. This would allow socket clients to call any Telegram MTProto function directly (e.g. takeout, without needing `td_api` wrappers).

### Approach

- Study how TDLib's codegen produces the JSON codec for `td_api` (in `futpib/td` fork)
- Extend/duplicate that codegen to also cover `telegram_api.tl`
- Add a new socket message type (e.g. `{"type":"telegram_api", ...}`) that accepts JSON-encoded `telegram_api` functions
- Serialize to TL binary, send via the existing `TdLibBridge` → MTP path, deserialize response back to JSON

### Relevant code

- TDLib fork: `Telegram/ThirdParty/td/` (submodule `futpib/td`)
- TDLib codegen: `td/generate/` in the TD submodule
- `td_api.tl`: `td/generate/scheme/td_api.tl`
- `telegram_api.tl`: `td/generate/scheme/telegram_api.tl`
- TDLib bridge: `Telegram/SourceFiles/tdlib/tdlib_bridge.cpp`
- Socket server: `Telegram/SourceFiles/tdlib/td_json_server.cpp`
