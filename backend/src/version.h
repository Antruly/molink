#pragma once

// Keep this in sync with the git tag and the packaging scripts. The wire protocol version is separate: kProtocolVersion in
// protocol/frame_packet.h only changes when the byte layout changes.
#define MOLINK_VERSION "0.9.19"
#define MOLINK_PRODUCT_NAME "moLink"
#define MOLINK_CORE_SERVICE "moLink Core"

// Built-in directory used when the user has not configured a custom URL and
// the remote directory switch is on.
#define MOLINK_DEFAULT_SERVER_URL "https://molink.ssvr.top:28430"
