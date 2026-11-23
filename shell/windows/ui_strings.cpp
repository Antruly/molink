#include "ui_strings.h"

#include <cstdio>
#include <cstring>
#include <cwchar>

namespace molink::ui_strings {
namespace {

const entry kEntries[] = {
    // ---- Application shell -------------------------------------------------
    {"app.tagline", L"Remote desktop", L"远程桌面"},
    {"tabs.home", L"Home", L"首页"},
    {"tabs.new", L"New session", L"新连接"},
    {"home.connect", L"Connect", L"连接"},
    {"home.requestAccess", L"Request access", L"发送远程请求"},
    {"home.recent", L"Recent connections", L"最近连接"},
    {"home.title", L"Connect your devices", L"连接你的设备"},

    // ---- Preferences -------------------------------------------------------
    {"settings.title", L"Preferences", L"偏好设置"},
    {"settings.theme", L"Theme", L"主题"},
    {"settings.themeLight", L"Light", L"亮色"},
    {"settings.themeDark", L"Dark", L"暗色"},
    {"settings.autoLock", L"Auto lock", L"自动锁定"},
    {"settings.autoLockHint",
     L"Locks the interface after a while with no remote session and no input.",
     L"在没有任何远程会话且长时间无操作后锁定界面。"},
    {"settings.language", L"Language", L"语言"},
    {"settings.saved", L"Saved.", L"已保存。"},
    {"settings.watermark", L"Connection quality watermark", L"连接质量水印"},
    {"settings.watermarkHint", L"Show frame rate, latency and bandwidth in the session corner; off by default.", L"在会话角落显示帧率、延迟和带宽，默认关闭。"},

    // ---- Session toolbar and overlays -------------------------------------
    {"session.connecting", L"Connecting…", L"连接中…"},
    {"session.disconnected", L"Connection lost", L"连接已断开"},
    {"session.reconnect", L"Reconnect", L"重新连接"},
    {"session.retryIn", L"Reconnecting in {{seconds}}s", L"{{seconds}} 秒后重连"},
    {"session.waitingApproval", L"Waiting for the host to approve…", L"已发送连接请求，等待主机确认…"},
    {"session.disconnect", L"Disconnect", L"断开"},
    {"session.quality", L"Quality", L"画质"},
    {"session.files", L"Files", L"文件"},
    {"session.clipboard", L"Clipboard", L"剪贴板"},
    {"session.chat", L"Chat", L"聊天"},
    {"session.controlOn", L"INPUT LIVE", L"控制中"},
    {"session.startControl", L"Control", L"控制"},
    {"session.stopControl", L"Stop control", L"停止控制"},
    {"session.pinToolbar", L"Pin toolbar", L"固定工具栏"},
    {"session.unpinToolbar", L"Unpin toolbar", L"取消固定"},
    {"session.collapse", L"Hide toolbar", L"收起"},
    {"session.fullscreen", L"Fullscreen", L"全屏"},
    {"session.exitFullscreen", L"Exit fullscreen", L"退出全屏"},
    {"session.fit", L"Fit", L"适应"},
    {"session.actual", L"1:1", L"1:1"},
    {"session.fill", L"Stretch", L"拉伸"},
    {"session.frameRate", L"Frame rate", L"帧率"},
    {"session.audioOn", L"Enable remote audio", L"开启远程声音"},
    {"session.audioOff", L"Disable remote audio", L"关闭远程声音"},
    {"session.audioVolume", L"Volume", L"音量"},
    {"session.clipboardOn", L"Enable clipboard sync", L"开启剪贴板同步"},
    {"session.clipboardOff", L"Disable clipboard sync", L"关闭剪贴板同步"},
    {"session.clipboardGet", L"Copy remote clipboard", L"复制远端剪贴板"},
    {"session.clipboardSet", L"Send clipboard", L"发送剪贴板"},
    {"session.tools", L"Tools", L"工具"},
    {"session.appearance", L"Appearance", L"外观"},
    {"session.transport", L"Transport", L"传输"},
    {"session.path", L"Path", L"通路"},
    {"session.protocol", L"Protocol", L"协议"},
    {"session.resolution", L"Resolution", L"分辨率"},
    {"session.codec", L"Codec", L"编码"},
    {"session.ctrlAltDel", L"Ctrl+Alt+Del", L"Ctrl+Alt+Del"},
    {"session.comboKeys", L"Key combos", L"组合键"},
    {"session.watermarkTiles", L"TILES", L"平铺"},
    {"session.watermarkLink", L"LINK", L"传输"},
    {"session.presetTitle", L"Picture preset", L"画质挡位"},
    {"session.presetTurbo", L"Turbo", L"极速"},
    {"session.presetSmooth", L"Smooth", L"流畅"},
    {"session.presetBalanced", L"Standard", L"标准"},
    {"session.presetHd", L"HD", L"高清"},
    {"session.presetTruecolor", L"True colour", L"真彩色"},
    {"session.presetAuto", L"Auto", L"自动"},
    {"session.qualityCustom", L"Custom", L"自定义"},
    {"session.renameOpening", L"Opening rename…", L"正在打开重命名…"},

    // ---- Window and close behaviour ---------------------------------------
    {"window.minimize", L"Minimize", L"最小化"},
    {"window.fullscreen", L"Fullscreen", L"全屏"},
    {"window.close", L"Close", L"关闭"},
    {"window.closePromptTitle", L"Close window", L"关闭窗口"},
    {"window.closePromptBody",
     L"1 remote session tab is open. Close the session tab, or hide the "
     L"window to the tray and keep it running?",
     L"当前有 1 个远程画面标签页。要关闭会话标签页，还是把窗口隐藏到"
     L"托盘（会话继续）？"},
    {"window.closeTab", L"Close tab", L"关闭标签页"},
    {"window.minimizeToTray", L"Minimize to tray", L"最小化到托盘"},
    {"window.rememberChoice", L"Remember my choice, do not ask again",
     L"记住我的选择，下次不再提示"},
    {"window.cancel", L"Cancel", L"取消"},
    {"window.closeBehavior", L"When closing the window", L"关闭窗口时"},
    {"window.closeBehaviorHint",
     L"Hiding keeps the local core and any remote session running. Quitting "
     L"ends a core that this window started.",
     L"隐藏到托盘会保留本机 core 和远程会话；退出会结束本窗口启动的 core。"},
    {"window.hideToTray", L"Hide to tray", L"隐藏到托盘"},
    {"window.quit", L"Quit moLink", L"退出 moLink"},

    // ---- Notification area -------------------------------------------------
    {"tray.show", L"Show moLink", L"显示 moLink"},
    {"tray.recent", L"Recent sessions", L"最近会话"},
    {"tray.recentEmpty", L"No recent sessions", L"暂无最近会话"},
    {"tray.exit", L"Exit", L"退出"},
    {"tray.prefilled", L"Filled the form; double-click to connect",
     L"已填入连接框，双击直接连接"},

    // ---- Toasts (shell-only wording where the Web has no key) --------------
    {"toast.connecting", L"Connecting…", L"正在连接中…"},
    {"toast.connected", L"Connected", L"已连接"},
    {"toast.reconnected", L"Reconnected", L"已重新连接"},
    {"toast.disconnected", L"Session disconnected", L"会话已断开"},
    {"toast.connectionLost", L"Connection lost; reconnecting…",
     L"连接已断开，正在重连…"},
    {"toast.reconnecting", L"Reconnecting…", L"正在重新连接…"},
    {"toast.retryStopped", L"Stopped retrying", L"已停止重连"},
    {"toast.codeRequired", L"Enter the code and retry", L"请输入验证码后重试"},
    {"toast.accessRequested", L"Access request sent; waiting for approval",
     L"已发送连接请求，等待对方确认"},
    {"toast.clipboardRequested", L"Requested the remote clipboard",
     L"已请求远端剪贴板"},
    {"toast.clipboardEmpty", L"The local clipboard is empty",
     L"本地剪贴板为空"},
    {"toast.clipboardSent", L"Sent to the remote clipboard",
     L"已发送到远端剪贴板"},
    {"toast.clipboardReceived", L"Received the remote clipboard",
     L"已接收远端剪贴板"},
    {"toast.clipboardTooLarge",
     L"Clipboard text exceeds 192 KiB and was not synced",
     L"剪贴板文本超过 192 KiB，未同步"},
    {"toast.clipboardFailed", L"Could not sync the clipboard",
     L"剪贴板同步发送失败"},
    {"toast.ctrlAltDelSent", L"Sent Ctrl+Alt+Del", L"已发送 Ctrl+Alt+Del"},
    {"toast.fileNeedsSession", L"Connect a session before sending files",
     L"请先连接会话后再发送文件"},
    {"toast.fileUnsupported",
     L"The remote only supports whole-file uploads; clipboard files were not "
     L"sent",
     L"远端只支持整文件上传，剪贴板文件未发送"},
    {"toast.receiving", L"Receiving", L"开始接收"},
    {"toast.savedTo", L"Saved to", L"已保存到"},
    {"toast.sent", L"Sent", L"已发送"},
    {"toast.idCopied", L"Device ID copied", L"已复制 ID"},
    {"toast.codeCopied", L"Verification code copied", L"已复制验证码"},
    {"toast.idNotReady", L"Device ID is not ready: the local core did not answer",
     L"ID 尚未就绪：本机核心没有响应"},
    {"toast.codeNotReady",
     L"Verification code is not ready: the local core did not answer",
     L"验证码尚未就绪：本机核心没有响应"},
    {"toast.codeChanged", L"Verification code changed", L"验证码已更换"},
    {"toast.codeChangeFailed", L"Could not change the verification code",
     L"验证码更换失败"},
    {"toast.recentRemoved", L"Removed from recents", L"已从最近连接中移除"},
    {"toast.videoModeNeedsSession",
     L"Connect a session before switching the video mode",
     L"请先连接会话再切换视频模式"},
    {"toast.videoModeFailed", L"Could not switch the video mode",
     L"视频模式切换失败"},
    {"toast.videoStream", L"Switched to video-stream mode",
     L"已切换到视频流模式"},
    {"toast.videoTiles", L"Switched to tile mode",
     L"已切换到瓦片（静态）模式"},
    {"toast.coreFallback",
     L"Core connection failed; using a direct tile connection",
     L"通过本机 core 连接失败，改用直连（平铺）"},
    {"toast.coreSurfaceConnected", L"Local core picture connected",
     L"本机核心画面已连接"},
    {"toast.updateFound", L"New version found", L"发现新版本"},
    {"toast.updateLatest", L"Already up to date", L"已是最新版本"},
    {"toast.updateFailed",
     L"Could not check for updates; check the network and retry",
     L"检查更新失败，请检查网络后重试"},

    // ---- Host/session errors ----------------------------------------------
    {"error.invalid_code", L"The code is wrong or has expired",
     L"验证码错误或已失效"},
    {"error.locked", L"The host is locked; connecting is not possible right now",
     L"主机已锁定，暂时无法连接"},
    {"error.access_denied", L"The other side denied the request",
     L"对方拒绝了本次连接请求"},
    {"error.too_many_attempts", L"Too many attempts; try again later",
     L"尝试次数过多，请稍后再试"},
    {"error.ip_banned", L"This IP address is blocked",
     L"当前 IP 已被禁止连接"},
    {"error.handshake_failed",
     L"Could not complete the handshake; retry later",
     L"与主机握手失败，请稍后重试"},
    {"error.hello_timeout", L"Could not complete the handshake; retry later",
     L"与主机握手失败，请稍后重试"},
    {"error.display_mode_busy",
     L"The host cannot switch display modes right now",
     L"主机当前无法切换显示模式"},
    {"error.display_mode_failed", L"Display mode change failed",
     L"显示模式切换失败"},
    {"error.host", L"The host returned an error", L"主机返回了错误"},
};

constexpr std::size_t kCount = sizeof(kEntries) / sizeof(kEntries[0]);
constexpr std::size_t kMaxFileBytes = 64u * 1024u;

FILE* open_language_file(const std::wstring& path, const wchar_t* mode) {
#if defined(_WIN32)
  return _wfopen(path.c_str(), mode);
#else
  // The tests use ASCII paths; the shell only calls this through _wfopen.
  std::string narrow(path.begin(), path.end());
  std::string narrow_mode(mode, mode + std::wcslen(mode));
  return std::fopen(narrow.c_str(), narrow_mode.c_str());
#endif
}

}  // namespace

const entry* entries(std::size_t* count) {
  if (count != nullptr) *count = kCount;
  return kEntries;
}

const wchar_t* text(language lang, std::string_view key) {
  for (std::size_t i = 0; i < kCount; ++i) {
    if (key == kEntries[i].key) {
      return lang == language::zh_cn ? kEntries[i].zh_cn : kEntries[i].en;
    }
  }
  return L"";
}

bool has_key(std::string_view key) {
  for (std::size_t i = 0; i < kCount; ++i) {
    if (key == kEntries[i].key) return true;
  }
  return false;
}

const char* tag(language lang) {
  return lang == language::zh_cn ? "zh-CN" : "en";
}

bool parse_tag(std::string_view value, language* out) {
  if (out == nullptr || value.empty()) return false;
  std::string lowered;
  lowered.reserve(value.size());
  for (char c : value) {
    if (c >= 'A' && c <= 'Z') {
      lowered.push_back(static_cast<char>(c - 'A' + 'a'));
    } else {
      lowered.push_back(c);
    }
  }
  if (lowered.rfind("zh", 0) == 0) {
    *out = language::zh_cn;
    return true;
  }
  if (lowered.rfind("en", 0) == 0) {
    *out = language::en;
    return true;
  }
  return false;
}

language language_or(std::string_view value, language fallback) {
  language parsed{};
  return parse_tag(value, &parsed) ? parsed : fallback;
}

bool load_file(const std::wstring& path, language* out) {
  if (path.empty() || out == nullptr) return false;
  FILE* file = open_language_file(path, L"rb");
  if (file == nullptr) return false;
  std::string body;
  char buffer[4096];
  while (body.size() < kMaxFileBytes) {
    const std::size_t got = std::fread(buffer, 1, sizeof(buffer), file);
    if (got == 0) break;
    body.append(buffer, got);
  }
  std::fclose(file);
  if (body.empty()) return false;

  const std::string needle = "\"language\"";
  std::size_t at = body.find(needle);
  if (at == std::string::npos) return false;
  at += needle.size();
  while (at < body.size() &&
         (body[at] == ' ' || body[at] == '\t' || body[at] == '\n' ||
          body[at] == '\r')) {
    ++at;
  }
  if (at >= body.size() || body[at] != ':') return false;
  ++at;
  while (at < body.size() &&
         (body[at] == ' ' || body[at] == '\t' || body[at] == '\n' ||
          body[at] == '\r')) {
    ++at;
  }
  if (at >= body.size() || body[at] != '"') return false;
  ++at;
  const std::size_t end = body.find('"', at);
  if (end == std::string::npos) return false;
  return parse_tag(std::string_view(body).substr(at, end - at), out);
}

bool save_file(const std::wstring& path, language lang) {
  if (path.empty()) return false;
  FILE* file = open_language_file(path, L"wb");
  if (file == nullptr) return false;
  const std::string body =
      std::string("{\n  \"language\": \"") + tag(lang) + "\"\n}\n";
  const std::size_t written =
      std::fwrite(body.data(), 1, body.size(), file);
  const bool ok = written == body.size();
  if (std::fclose(file) != 0) return false;
  return ok;
}

}  // namespace molink::ui_strings
