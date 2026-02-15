# This file is part of Telegram Desktop,
# the official desktop application for the Telegram messaging service.
#
# For license and copyright information please follow this link:
# https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL

add_library(td_tdlib_bridge OBJECT)
init_non_host_target(td_tdlib_bridge)
add_library(tdesktop::td_tdlib_bridge ALIAS td_tdlib_bridge)

set_target_properties(td_tdlib_bridge PROPERTIES AUTOMOC ON)

nice_target_sources(td_tdlib_bridge ${src_loc}
PRIVATE
    tdlib/tdlib_bridge.cpp
    tdlib/tdlib_bridge.h
    tdlib/td_json_server.cpp
    tdlib/td_json_server.h
)

target_include_directories(td_tdlib_bridge
PUBLIC
    ${src_loc}
)

target_link_libraries(td_tdlib_bridge
PUBLIC
    tdesktop::td_mtproto
    desktop-app::lib_base
    tdjson_static
PRIVATE
    desktop-app::external_qt
)
