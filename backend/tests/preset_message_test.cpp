#include <cstdio>
#include <string>

#include "preset_message.h"

namespace {

using namespace molink::preset;

int checks = 0;
int failures = 0;

void expect(bool value, const char* description) {
  ++checks;
  if (value) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n", description);
}

void expect_eq(const std::string& actual, const std::string& expected,
               const char* description) {
  ++checks;
  if (actual == expected) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n  expected: %s\n  actual:   %s\n",
               description, expected.c_str(), actual.c_str());
}

/// The fixed tiers' expected payloads, pinned from the Web's
/// `presetMessage()` / `PRESET_PLANS`. Key order matters: these are compared
/// byte for byte.
void test_six_tiers_match_web_json() {
  expect_eq(
      preset_message_json(preset_id::turbo, 0),
      R"({"type":"session-preset","preset":"turbo","fps":10,"quality":0,"bitrate_kbps":2500,"rate_mode":"auto","format":"preset","chroma":"gray","codec":"auto"})",
      "turbo payload matches the Web");
  expect_eq(
      preset_message_json(preset_id::smooth, 0),
      R"({"type":"session-preset","preset":"smooth","fps":24,"quality":0,"bitrate_kbps":6000,"rate_mode":"auto","format":"preset","chroma":"420","codec":"auto"})",
      "smooth payload matches the Web");
  expect_eq(
      preset_message_json(preset_id::balanced, 0),
      R"({"type":"session-preset","preset":"balanced","fps":30,"quality":0,"bitrate_kbps":0,"rate_mode":"auto","format":"preset","chroma":"420","codec":"auto"})",
      "balanced payload matches the Web");
  expect_eq(
      preset_message_json(preset_id::hd, 0),
      R"({"type":"session-preset","preset":"hd","fps":45,"quality":0,"bitrate_kbps":0,"rate_mode":"auto","format":"preset","chroma":"420","codec":"auto"})",
      "hd payload matches the Web");
  expect_eq(
      preset_message_json(preset_id::truecolor, 0),
      R"({"type":"session-preset","preset":"truecolor","fps":18,"quality":0,"bitrate_kbps":0,"rate_mode":"cq","format":"bgra","chroma":"444","codec":"auto"})",
      "truecolor payload matches the Web");
  expect_eq(
      preset_message_json(preset_id::auto_, 0),
      R"({"type":"session-preset","preset":"auto","fps":30,"quality":0,"bitrate_kbps":0,"rate_mode":"auto","format":"preset","chroma":"auto","codec":"auto"})",
      "auto payload matches the Web");
}

void test_fixed_fps_override_and_clamp() {
  expect_eq(
      preset_message_json(preset_id::smooth, 12),
      R"({"type":"session-preset","preset":"smooth","fps":12,"quality":0,"bitrate_kbps":6000,"rate_mode":"auto","format":"preset","chroma":"420","codec":"auto"})",
      "the viewer's target fps replaces the plan rate");
  expect(preset_message_json(preset_id::smooth, -3).find("\"fps\":1,") !=
             std::string::npos,
         "negative fps clamps to 1");
  expect(preset_message_json(preset_id::smooth, 90).find("\"fps\":60,") !=
             std::string::npos,
         "fps above 60 clamps to 60");
}

void test_plan_table_matches_web() {
  expect(plan_count == 6, "there are exactly six tiers");
  expect(std::string(plans()[0].name) == "turbo" &&
             std::string(plans()[1].name) == "smooth" &&
             std::string(plans()[2].name) == "balanced" &&
             std::string(plans()[3].name) == "hd" &&
             std::string(plans()[4].name) == "truecolor" &&
             std::string(plans()[5].name) == "auto",
         "tier order is turbo/smooth/balanced/hd/truecolor/auto");
  expect(std::string(plans()[0].label) == "极速" &&
             std::string(plans()[1].label) == "流畅" &&
             std::string(plans()[2].label) == "标准" &&
             std::string(plans()[3].label) == "高清" &&
             std::string(plans()[4].label) == "真彩色" &&
             std::string(plans()[5].label) == "自动",
         "labels match the zh-CN locale");
  expect(plan(preset_id::hd).fps == 45 &&
             std::string(plan(preset_id::hd).mode) == "quality",
         "hd is the host's quality mode at 45 fps");
  expect(plan(preset_id::truecolor).fps == 18 &&
             std::string(plan(preset_id::truecolor).mode) == "lossless" &&
             std::string(plan(preset_id::truecolor).format) == "bgra" &&
             std::string(plan(preset_id::truecolor).chroma) == "444" &&
             std::string(plan(preset_id::truecolor).rate_mode) == "cq",
         "truecolor is lossless BGRA 4:4:4 CQ at 18 fps");
  expect(plan(preset_id::turbo).fps == 10 &&
             std::string(plan(preset_id::turbo).chroma) == "gray" &&
             plan(preset_id::turbo).bitrate_kbps == 2500,
         "turbo is 10 fps grayscale at 2500 kbit/s");
  expect(default_preset() == preset_id::smooth,
         "the default tier is smooth");
  expect(default_advanced_params(preset_id::turbo).chroma == "gray" &&
             default_advanced_params(preset_id::truecolor).format == "bgra",
         "advanced defaults come from the selected plan");
}

void test_custom_message_fields() {
  advanced_params params;
  params.fps = 20;
  params.quality = 40;
  params.bitrate_kbps = 8000;
  params.rate_mode = "cbr";
  params.format = "rgb24";
  params.chroma = "444";
  params.codec = "auto";
  expect_eq(
      advanced_preset_message_json(params),
      R"({"type":"session-preset","preset":"custom","fps":20,"quality":40,"bitrate_kbps":8000,"rate_mode":"cbr","format":"rgb24","chroma":"444","codec":"auto"})",
      "custom keeps every explicit field");

  // 4:4:4 without an explicit format resolves to the BGRA tile path.
  params.format = "preset";
  expect(advanced_preset_message_json(params).find("\"format\":\"bgra\"") !=
             std::string::npos,
         "custom 4:4:4 defaults to BGRA");
  expect(advanced_preset_message_json(params).find("\"chroma\":\"444\"") !=
             std::string::npos,
         "custom 4:4:4 keeps its chroma with the tile path");

  // A pinned stream encoder is 4:2:0: the chroma is reduced and an explicit
  // tile format goes back to the profile.
  params.codec = "vp9";
  expect_eq(
      advanced_preset_message_json(params),
      R"({"type":"session-preset","preset":"custom","fps":20,"quality":40,"bitrate_kbps":8000,"rate_mode":"cbr","format":"preset","chroma":"420","codec":"vp9"})",
      "an explicit stream encoder forces 4:2:0");

  // The explicit tiles codec keeps 4:4:4.
  params.codec = "tiles";
  expect(advanced_preset_message_json(params).find("\"chroma\":\"444\"") !=
             std::string::npos,
         "tiles keeps 4:4:4");
  expect(advanced_preset_message_json(params).find("\"format\":\"bgra\"") !=
             std::string::npos,
         "tiles 4:4:4 uses BGRA");
}

void test_custom_clamps() {
  advanced_params params;
  params.fps = 2;
  params.quality = 70;
  params.bitrate_kbps = 60000;
  params.rate_mode = "auto";
  params.format = "preset";
  params.chroma = "420";
  params.codec = "auto";
  const std::string json = advanced_preset_message_json(params);
  expect(json.find("\"fps\":5,") != std::string::npos,
         "custom fps clamps to 5");
  expect(json.find("\"quality\":63,") != std::string::npos,
         "custom quality clamps to 63");
  expect(json.find("\"bitrate_kbps\":50000,") != std::string::npos,
         "custom bitrate clamps to 50000");

  params.fps = 90;
  params.quality = -5;
  params.bitrate_kbps = -1;
  const std::string high = advanced_preset_message_json(params);
  expect(high.find("\"fps\":60,") != std::string::npos,
         "custom fps clamps to 60");
  expect(high.find("\"quality\":0,") != std::string::npos,
         "custom quality clamps to 0");
  expect(high.find("\"bitrate_kbps\":0,") != std::string::npos,
         "custom bitrate clamps to 0");
}

void test_legacy_ids_migrate() {
  expect(migrate_preset_id("turbo") == preset_id::turbo,
         "turbo stays turbo");
  expect(migrate_preset_id("quality") == preset_id::hd,
         "legacy quality becomes hd");
  expect(migrate_preset_id("ultra") == preset_id::truecolor,
         "legacy ultra becomes truecolor");
  expect(migrate_preset_id("lossless") == preset_id::truecolor,
         "legacy lossless becomes truecolor");
  expect(migrate_preset_id("game") == preset_id::smooth,
         "legacy game becomes smooth");
  expect(migrate_preset_id("hd") == preset_id::hd, "hd stays hd");
  expect(migrate_preset_id("auto") == preset_id::auto_, "auto stays auto");
  expect(migrate_preset_id("nonsense") == preset_id::smooth,
         "an unknown stored id falls back to the default");
  expect(migrate_preset_id("") == preset_id::smooth,
         "an empty stored id falls back to the default");

  preset_id parsed = preset_id::turbo;
  expect(!parse_preset_id("lossless", &parsed),
         "legacy ids are not current wire ids");
  expect(parse_preset_id("truecolor", &parsed) && parsed == preset_id::truecolor,
         "current ids parse");
}

void test_applied_reply() {
  applied_reply reply;
  expect(parse_session_preset_reply(
             R"({"type":"session-preset","applied":true,"preset":"balanced","actual":{"fps":30,"quality":"balanced","format":"bgra","chroma":"420","codec":"vp9"}})",
             &reply),
         "a session-preset reply parses");
  expect(reply.applied, "applied:true is recorded");
  expect(reply.preset == "balanced", "the applied preset is recorded");
  expect(reply.actual.fps == 30 && reply.actual.quality == "balanced" &&
             reply.actual.format == "bgra" && reply.actual.chroma == "420" &&
             reply.actual.codec == "vp9",
         "every actual field is recorded");

  expect(parse_session_preset_reply(
             R"({"type":"session-preset","applied":false,"preset":"hd"})",
             &reply),
         "a non-applied reply still parses");
  expect(!reply.applied, "applied:false does not move the actual line");
  expect(reply.actual.fps == 0 && reply.actual.codec.empty(),
         "a missing actual object leaves the defaults");

  expect(parse_session_preset_reply(
             R"({"type":"session-preset","applied":true,"preset":"truecolor","actual":{"fps":18,"chroma":"444","codec":"tiles"}})",
             &reply),
         "a partial actual object parses");
  expect(reply.actual.chroma == "444" && reply.actual.codec == "tiles" &&
             reply.actual.format.empty(),
         "omitted actual fields stay empty");

  // A JSON string with an escape must not break the rest of the object.
  expect(parse_session_preset_reply(
             R"({"type":"session-preset","preset":"custom","applied":true,"note":"a\"b","actual":{"fps":12}})",
             &reply),
         "escaped strings parse");
  expect(reply.preset == "custom" && reply.actual.fps == 12,
         "fields after an escaped string are still read");

  applied_reply ignored;
  expect(!parse_session_preset_reply(R"({"type":"quality"})", &ignored),
         "a different message type is not treated as session-preset");
  expect(!parse_session_preset_reply("not json", &ignored),
         "malformed JSON is rejected");
}

void test_legacy_quality_reply() {
  applied_reply reply;
  expect(parse_quality_reply(
             R"({"type":"quality","mode":"smooth","format":"rgb565","qp":0,"bitrate_kbps":0,"rate_mode":"auto","scope":"viewer","fps":24})",
             &reply),
         "a legacy quality acknowledgement parses");
  expect(reply.applied, "the legacy acknowledgement counts as applied");
  expect(reply.preset == "smooth" && reply.actual.fps == 24 &&
             reply.actual.format == "rgb565" &&
             reply.actual.chroma == "565" && reply.actual.codec.empty(),
         "legacy fields map onto the actual line");

  expect(parse_quality_reply(
             R"({"type":"quality","mode":"lossless","format":"bgra"})",
             &reply),
         "a legacy BGRA reply parses");
  expect(reply.actual.chroma == "444",
         "legacy BGRA reports 4:4:4");
}

void test_labels() {
  expect(std::string(chroma_label("444")) == "4:4:4" &&
             std::string(chroma_label("420")) == "4:2:0" &&
             std::string(chroma_label("gray")) == "8-bit" &&
             std::string(chroma_label("565")) == "16-bit",
         "chroma labels match the Web");
  expect(std::string(format_label("bgra")) == "BGRA" &&
             std::string(format_label("rgb24")) == "RGB24" &&
             std::string(format_label("rgb565")) == "RGB565" &&
             std::string(format_label("gray8")) == "Gray8",
         "format labels match the Web");
  expect(std::string(codec_label("tiles")) == "平铺" &&
             std::string(codec_label("h264")) == "H.264" &&
             std::string(codec_label("vp9")) == "VP9",
         "codec labels match the panel");
}

}  // namespace

int main() {
  test_six_tiers_match_web_json();
  test_fixed_fps_override_and_clamp();
  test_plan_table_matches_web();
  test_custom_message_fields();
  test_custom_clamps();
  test_legacy_ids_migrate();
  test_applied_reply();
  test_legacy_quality_reply();
  test_labels();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
