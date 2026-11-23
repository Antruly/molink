// Viewer-owned picture presets, shared by the Windows shell and the tests.
//
// This file deliberately has no Windows dependency: the JSON the viewer puts
// on the wire is the contract with the host, so it is built and tested on
// Linux with the same source the native client compiles.
//
// The six tiers, their field values and the message shape mirror
// `frontend/src/lib/session-presets.ts` byte for byte. `custom` follows the
// advanced panel's rules; legacy ids from older native builds migrate to the
// closest current tier instead of failing a session.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace molink::preset {

/// Wire ids, in the order the toolbar and settings show them.
enum class preset_id {
  turbo,
  smooth,
  balanced,
  hd,
  truecolor,
  auto_,
};

inline constexpr std::size_t plan_count = 6;

struct preset_plan {
  preset_id id;
  /// Wire value sent as `preset` (auto is `"auto"`).
  const char* name;
  /// zh-CN display strings from `frontend/src/locales/zh-CN.json`.
  const char* label;
  const char* description;
  /// Host quality mode (`balanced`, `quality`, `lossless`, ...).
  const char* mode;
  /// Resting capture rate; `preset_message_json` may use the viewer's target.
  int fps;
  int min_fps;
  int max_fps;
  /// QP sent to the host, 0 = follow the quality profile.
  int qp;
  /// Stream bitrate ceiling in kbit/s, 0 = follow the profile.
  int bitrate_kbps;
  const char* rate_mode;
  const char* format;
  const char* chroma;
};

/// The six plans, in `SESSION_PRESETS` order.
const preset_plan* plans();

/// The plan for a current id, or the default (smooth) for anything else.
const preset_plan& plan(preset_id id);

/// The default tier: Web's `DEFAULT_SESSION_PRESET`.
preset_id default_preset();

/// True only for the six current wire ids.
bool parse_preset_id(std::string_view value, preset_id* out);

/// Resolves a stored value to a current id. Old native builds stored
/// `quality`/`ultra`/`lossless` (and the settings panel could write `game`);
/// unknown or malformed values fall back to the default like the Web's
/// `normalizePresetId`.
preset_id migrate_preset_id(std::string_view value);

const char* preset_name(preset_id id);

/// The advanced panel's parameters, exactly the fields
/// `advancedPresetMessage()` sends.
struct advanced_params {
  int fps = 30;
  int quality = 0;
  int bitrate_kbps = 0;
  std::string rate_mode = "auto";
  std::string format = "preset";
  std::string chroma = "420";
  std::string codec = "auto";
};

/// The value the advanced controls start from for a tier: the plan, exactly
/// (`defaultAdvancedParams`).
advanced_params default_advanced_params(preset_id id);

/// True for the four real stream encoders (auto/tiles are modes).
bool is_explicit_stream_codec(std::string_view codec);

/// `advancedFormatForChroma`: the tile format that carries a chroma request.
const char* format_for_chroma(std::string_view chroma,
                              std::string_view current_format);

/// The exact `session-preset` payload for a fixed tier. Field order matches
/// the Web object literal so the bytes are identical.
std::string preset_message_json(preset_id id, int fps,
                                std::string_view codec = "auto");

/// The exact payload for `preset:"custom"`. Applies the Web's clamps and the
/// "explicit stream encoder forces 4:2:0" rule.
std::string advanced_preset_message_json(const advanced_params& params);

/// What the host actually applied, from its `session-preset` reply.
struct applied_actual {
  int fps = 0;
  std::string quality;
  std::string format;
  std::string chroma;
  std::string codec;
};

struct applied_reply {
  /// True only when the host said `applied:true`; a host echo or another
  /// viewer's choice never moves the selector.
  bool applied = false;
  std::string preset;
  applied_actual actual;
};

/// Parses a `{"type":"session-preset",...}` reply. Returns true when the
/// message is a session-preset object (whether or not `applied` is true).
bool parse_session_preset_reply(std::string_view json, applied_reply* out);

/// Parses the retired host-global `quality` acknowledgement as a fallback,
/// so an older core still feeds the panel's actual-values line.
bool parse_quality_reply(std::string_view json, applied_reply* out);

/// Chroma/format/codec labels used by the panel, matching
/// `chromaLabel()`/`formatLabel()` in the Web code.
const char* chroma_label(std::string_view chroma);
const char* format_label(std::string_view format);
const char* codec_label(std::string_view codec);

}  // namespace molink::preset
