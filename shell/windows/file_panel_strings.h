// Chinese copy for the native file panel, kept in one place so the i18n pass
// (batch 9) only has to replace this header. The strings mirror
// `frontend/src/locales/zh-CN.json` -> `files` where the Web has one.
#pragma once

#include <cstddef>
#include <string>

namespace molink::files::text {

// Toolbar / path bar
inline constexpr wchar_t kRoots[] = L"盘符 / 根目录";
inline constexpr wchar_t kRootsShort[] = L"根";
inline constexpr wchar_t kUp[] = L"上一级";
inline constexpr wchar_t kHomeRemote[] = L"回到远端主目录（home）";
inline constexpr wchar_t kHomeLocal[] = L"回到本机主目录（home）";
inline constexpr wchar_t kRefresh[] = L"刷新";
inline constexpr wchar_t kBrowse[] = L"浏览";
inline constexpr wchar_t kPathPlaceholderRemote[] =
    L"C:\\Windows、/var/log 或 ~/Documents";
inline constexpr wchar_t kPathPlaceholderLocal[] =
    L"绝对路径或子目录，回车跳转";

// Lists / selection
inline constexpr wchar_t kSelectAll[] = L"全选";
inline constexpr wchar_t kEmptyFolder[] = L"此文件夹为空";
inline constexpr wchar_t kLoading[] = L"加载中…";
inline constexpr wchar_t kRemoteLabel[] = L"远程";
inline constexpr wchar_t kLocalLabel[] = L"本地";
inline constexpr wchar_t kOpenDir[] = L"打开目录";

// Actions
inline constexpr wchar_t kSendTo[] = L"传过去 →";
inline constexpr wchar_t kSendToHint[] = L"把本地选中项传到远程当前目录";
inline constexpr wchar_t kReceiveFrom[] = L"← 传回来";
inline constexpr wchar_t kReceiveFromHint[] = L"把远程选中项拉回本地当前目录";

// Block reasons (Web files.noSelection / busy / need*)
inline constexpr wchar_t kBusy[] = L"已有传输正在进行。";
inline constexpr wchar_t kNoSelection[] = L"请至少选择一项。";
inline constexpr wchar_t kNeedRemoteTarget[] = L"请先打开远程目录。";
inline constexpr wchar_t kNeedLocalSource[] = L"请先选择本地目录或拖入文件。";

// Limits / directories (F8 / F5 guard)
inline constexpr wchar_t kFileTooLarge[] =
    L"文件超过 8 GiB 上限，无法发送";
inline constexpr wchar_t kFileTooLargeReceive[] =
    L"文件超过 8 GiB 上限，无法接收";
inline constexpr wchar_t kTruncated[] = L"条目过多，仅显示前一部分";
inline constexpr wchar_t kNotAFile[] = L"这不是文件。";
inline constexpr wchar_t kTooManyActive[] =
    L"同时传输的文件已达上限（8 个），请稍后再试。";
inline constexpr wchar_t kFolderUnsupported[] = L"暂不支持整个文件夹";

inline constexpr wchar_t kFileReadFailed[] = L"无法读取文件。";
inline constexpr wchar_t kFileOpenFailed[] = L"无法打开文件。";
inline constexpr wchar_t kSendFailed[] = L"无法发送选中的文件";

// Listing errors (host messages are English codes; these are the readable
// Chinese variants, with the raw text kept for unknown reasons)
inline constexpr wchar_t kInvalidPath[] = L"路径无效，请检查输入。";
inline constexpr wchar_t kNoSuchFolder[] = L"没有这个文件夹。";
inline constexpr wchar_t kNotAFolder[] = L"这不是文件夹。";
inline constexpr wchar_t kPermissionDenied[] = L"没有权限读取该文件夹。";
inline constexpr wchar_t kReadFolderFailed[] = L"无法读取该文件夹。";
inline constexpr wchar_t kHostUnreachable[] = L"无法读取主机文件夹。";

// Volume kinds (Web files.driveRemovable / driveNetwork / ...)
inline constexpr wchar_t kDriveRemovable[] = L"可移动";
inline constexpr wchar_t kDriveNetwork[] = L"网络";
inline constexpr wchar_t kDriveCdrom[] = L"光盘";
inline constexpr wchar_t kDriveRam[] = L"内存盘";
inline constexpr wchar_t kDriveHome[] = L"主目录";
inline constexpr wchar_t kDriveUnknown[] = L"其他";

// Task rows
inline constexpr wchar_t kTransfers[] = L"传输";
inline constexpr wchar_t kDone[] = L"已完成";
inline constexpr wchar_t kSending[] = L"传输中…";
inline constexpr wchar_t kFailed[] = L"失败";
inline constexpr wchar_t kCancelled[] = L"传输已取消";
inline constexpr wchar_t kDetails[] = L"详情";
inline constexpr wchar_t kCollapse[] = L"收起";
inline constexpr wchar_t kClearFinished[] = L"清除已完成";
inline constexpr wchar_t kCancel[] = L"取消";

inline std::wstring SelectedCount(std::size_t count) {
  return L"已选 " + std::to_wstring(count) + L" 项";
}

inline std::wstring Summary(int done, int failed, int cancelled) {
  return L"成功 " + std::to_wstring(done) + L" · 失败 " +
         std::to_wstring(failed) + L" · 取消 " + std::to_wstring(cancelled);
}

inline std::wstring SkippedCount(int skipped) {
  return L"跳过 " + std::to_wstring(skipped) + L" 项";
}

inline std::wstring FolderSkipped(int skipped) {
  return std::wstring(kFolderUnsupported) + L"（已跳过 " +
         std::to_wstring(skipped) + L" 个）";
}

inline std::wstring FileCount(std::size_t count) {
  return std::to_wstring(count) + L" 个文件";
}

inline std::wstring TaskCount(std::size_t count) {
  return std::to_wstring(count) + L" 个";
}

inline std::wstring FileCountWithRest(const std::wstring& first,
                                      std::size_t count) {
  return first + L" 等 " + std::to_wstring(count) + L" 个文件";
}

}  // namespace molink::files::text
