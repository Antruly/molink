#include "input/keymap.h"

#include "input/key_alias.h"

#include <cstring>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace molink {
namespace {

// Every key resolves to a physical identifier, not a character:
//
//   * Windows uses the set-1 scan code (with the E0 bit for extended keys).
//   * X11 uses the X key code, which is the Linux evdev code plus eight.
//   * macOS keeps its Carbon key code, which is already position-based.
//
// A layout (US, German, French, ...) only changes which character the host
// produces for that position. It never changes these numbers, so the same
// physical key on the client reaches the same physical key on the host.
struct key_entry {
  const char* code;
  unsigned int value;   // Windows VK, or macOS CGKeyCode
  unsigned int scan;    // Windows set-1 scan code (0 = inject by VK)
  unsigned int keycode; // X11 key code (0 = no physical key)
  const char* keysym;   // X11 keysym name, for the layout compatibility path
  bool extended;        // Windows E0 prefix
  bool unsupported;     // known DOM code, no key on this platform
};

constexpr key_entry win(const char* code, unsigned int vk, unsigned int scan,
                        bool extended = false) {
  return key_entry{code, vk, scan, 0, nullptr, extended, false};
}

/// A Windows key with no reliable set-1 scan code. The injector sends the
/// virtual key instead; this is the compatibility path, not the norm.
constexpr key_entry win_vk(const char* code, unsigned int vk,
                           bool extended = false) {
  return key_entry{code, vk, 0, 0, nullptr, extended, false};
}

constexpr key_entry x11(const char* code, const char* keysym,
                        unsigned int keycode) {
  // A zero key code means the host has no physical key for this DOM code.
  return key_entry{code, 0, 0, keycode, keysym, false, keycode == 0};
}

constexpr key_entry mac(const char* code, unsigned int keycode) {
  return key_entry{code, keycode, 0, 0, nullptr, false, false};
}

constexpr key_entry unsupported(const char* code) {
  return key_entry{code, 0, 0, 0, nullptr, false, true};
}

// Windows virtual-key values. Keeping them here (instead of including
// <windows.h> in the table) lets the Linux CI build exercise the Windows
// table too. On Windows every value is checked against the real VK_ constant
// below, so a typo cannot survive the windows-core job. Scan-code injection
// derives its compatibility VK with MapVirtualKeyW; these values are only
// used by the `MOLINK_KEYMAP=layout` compatibility path and by keys that have
// no scan code.
namespace vk {
constexpr unsigned int Cancel = 0x03;
constexpr unsigned int Back = 0x08;
constexpr unsigned int Tab = 0x09;
constexpr unsigned int Clear = 0x0C;
constexpr unsigned int Return = 0x0D;
constexpr unsigned int Pause = 0x13;
constexpr unsigned int Capital = 0x14;
constexpr unsigned int Kana = 0x15;      // VK_KANA / VK_HANGUL
constexpr unsigned int Hangul = 0x15;
constexpr unsigned int Hanja = 0x19;     // VK_HANJA / VK_KANJI
constexpr unsigned int Escape = 0x1B;
constexpr unsigned int Convert = 0x1C;
constexpr unsigned int NonConvert = 0x1D;
constexpr unsigned int Space = 0x20;
constexpr unsigned int Prior = 0x21;
constexpr unsigned int Next = 0x22;
constexpr unsigned int End = 0x23;
constexpr unsigned int Home = 0x24;
constexpr unsigned int Left = 0x25;
constexpr unsigned int Up = 0x26;
constexpr unsigned int Right = 0x27;
constexpr unsigned int Down = 0x28;
constexpr unsigned int Select = 0x29;
constexpr unsigned int Execute = 0x2B;
constexpr unsigned int Snapshot = 0x2C;
constexpr unsigned int Insert = 0x2D;
constexpr unsigned int Delete = 0x2E;
constexpr unsigned int Help = 0x2F;
constexpr unsigned int LWin = 0x5B;
constexpr unsigned int RWin = 0x5C;
constexpr unsigned int Apps = 0x5D;
constexpr unsigned int Sleep = 0x5F;
constexpr unsigned int Multiply = 0x6A;
constexpr unsigned int Add = 0x6B;
constexpr unsigned int Separator = 0x6C;
constexpr unsigned int Subtract = 0x6D;
constexpr unsigned int Decimal = 0x6E;
constexpr unsigned int Divide = 0x6F;
constexpr unsigned int F1 = 0x70;
constexpr unsigned int F24 = 0x87;
constexpr unsigned int NumLock = 0x90;
constexpr unsigned int Scroll = 0x91;
constexpr unsigned int OemNecEqual = 0x92;
constexpr unsigned int LShift = 0xA0;
constexpr unsigned int RShift = 0xA1;
constexpr unsigned int LControl = 0xA2;
constexpr unsigned int RControl = 0xA3;
constexpr unsigned int LMenu = 0xA4;
constexpr unsigned int RMenu = 0xA5;
constexpr unsigned int BrowserBack = 0xA6;
constexpr unsigned int BrowserForward = 0xA7;
constexpr unsigned int BrowserRefresh = 0xA8;
constexpr unsigned int BrowserStop = 0xA9;
constexpr unsigned int BrowserSearch = 0xAA;
constexpr unsigned int BrowserFavorites = 0xAB;
constexpr unsigned int BrowserHome = 0xAC;
constexpr unsigned int VolumeMute = 0xAD;
constexpr unsigned int VolumeDown = 0xAE;
constexpr unsigned int VolumeUp = 0xAF;
constexpr unsigned int MediaNextTrack = 0xB0;
constexpr unsigned int MediaPrevTrack = 0xB1;
constexpr unsigned int MediaStop = 0xB2;
constexpr unsigned int MediaPlayPause = 0xB3;
constexpr unsigned int LaunchMail = 0xB4;
constexpr unsigned int LaunchApp1 = 0xB6;
constexpr unsigned int LaunchApp2 = 0xB7;
constexpr unsigned int Oem1 = 0xBA;
constexpr unsigned int OemPlus = 0xBB;
constexpr unsigned int OemComma = 0xBC;
constexpr unsigned int OemMinus = 0xBD;
constexpr unsigned int OemPeriod = 0xBE;
constexpr unsigned int Oem2 = 0xBF;
constexpr unsigned int Oem3 = 0xC0;
constexpr unsigned int Oem4 = 0xDB;
constexpr unsigned int Oem5 = 0xDC;
constexpr unsigned int Oem6 = 0xDD;
constexpr unsigned int Oem7 = 0xDE;
constexpr unsigned int Oem102 = 0xE2;
}  // namespace vk

#if defined(_WIN32)
static_assert(vk::Cancel == VK_CANCEL, "VK_CANCEL changed");
static_assert(vk::Back == VK_BACK, "VK_BACK changed");
static_assert(vk::Tab == VK_TAB, "VK_TAB changed");
static_assert(vk::Clear == VK_CLEAR, "VK_CLEAR changed");
static_assert(vk::Return == VK_RETURN, "VK_RETURN changed");
static_assert(vk::Pause == VK_PAUSE, "VK_PAUSE changed");
static_assert(vk::Capital == VK_CAPITAL, "VK_CAPITAL changed");
static_assert(vk::Kana == VK_KANA, "VK_KANA changed");
static_assert(vk::Hangul == VK_HANGUL, "VK_HANGUL changed");
static_assert(vk::Hanja == VK_HANJA, "VK_HANJA changed");
static_assert(vk::Escape == VK_ESCAPE, "VK_ESCAPE changed");
static_assert(vk::Convert == VK_CONVERT, "VK_CONVERT changed");
static_assert(vk::NonConvert == VK_NONCONVERT, "VK_NONCONVERT changed");
static_assert(vk::Space == VK_SPACE, "VK_SPACE changed");
static_assert(vk::Prior == VK_PRIOR, "VK_PRIOR changed");
static_assert(vk::Next == VK_NEXT, "VK_NEXT changed");
static_assert(vk::End == VK_END, "VK_END changed");
static_assert(vk::Home == VK_HOME, "VK_HOME changed");
static_assert(vk::Left == VK_LEFT, "VK_LEFT changed");
static_assert(vk::Up == VK_UP, "VK_UP changed");
static_assert(vk::Right == VK_RIGHT, "VK_RIGHT changed");
static_assert(vk::Down == VK_DOWN, "VK_DOWN changed");
static_assert(vk::Select == VK_SELECT, "VK_SELECT changed");
static_assert(vk::Execute == VK_EXECUTE, "VK_EXECUTE changed");
static_assert(vk::Snapshot == VK_SNAPSHOT, "VK_SNAPSHOT changed");
static_assert(vk::Insert == VK_INSERT, "VK_INSERT changed");
static_assert(vk::Delete == VK_DELETE, "VK_DELETE changed");
static_assert(vk::Help == VK_HELP, "VK_HELP changed");
static_assert(vk::LWin == VK_LWIN, "VK_LWIN changed");
static_assert(vk::RWin == VK_RWIN, "VK_RWIN changed");
static_assert(vk::Apps == VK_APPS, "VK_APPS changed");
static_assert(vk::Sleep == VK_SLEEP, "VK_SLEEP changed");
static_assert(vk::Multiply == VK_MULTIPLY, "VK_MULTIPLY changed");
static_assert(vk::Add == VK_ADD, "VK_ADD changed");
static_assert(vk::Separator == VK_SEPARATOR, "VK_SEPARATOR changed");
static_assert(vk::Subtract == VK_SUBTRACT, "VK_SUBTRACT changed");
static_assert(vk::Decimal == VK_DECIMAL, "VK_DECIMAL changed");
static_assert(vk::Divide == VK_DIVIDE, "VK_DIVIDE changed");
static_assert(vk::F1 == VK_F1, "VK_F1 changed");
static_assert(vk::F24 == VK_F24, "VK_F24 changed");
static_assert(vk::NumLock == VK_NUMLOCK, "VK_NUMLOCK changed");
static_assert(vk::Scroll == VK_SCROLL, "VK_SCROLL changed");
static_assert(vk::OemNecEqual == VK_OEM_NEC_EQUAL, "VK_OEM_NEC_EQUAL changed");
static_assert(vk::LShift == VK_LSHIFT, "VK_LSHIFT changed");
static_assert(vk::RShift == VK_RSHIFT, "VK_RSHIFT changed");
static_assert(vk::LControl == VK_LCONTROL, "VK_LCONTROL changed");
static_assert(vk::RControl == VK_RCONTROL, "VK_RCONTROL changed");
static_assert(vk::LMenu == VK_LMENU, "VK_LMENU changed");
static_assert(vk::RMenu == VK_RMENU, "VK_RMENU changed");
static_assert(vk::BrowserBack == VK_BROWSER_BACK, "VK_BROWSER_BACK changed");
static_assert(vk::BrowserForward == VK_BROWSER_FORWARD,
              "VK_BROWSER_FORWARD changed");
static_assert(vk::BrowserRefresh == VK_BROWSER_REFRESH,
              "VK_BROWSER_REFRESH changed");
static_assert(vk::BrowserStop == VK_BROWSER_STOP, "VK_BROWSER_STOP changed");
static_assert(vk::BrowserSearch == VK_BROWSER_SEARCH,
              "VK_BROWSER_SEARCH changed");
static_assert(vk::BrowserFavorites == VK_BROWSER_FAVORITES,
              "VK_BROWSER_FAVORITES changed");
static_assert(vk::BrowserHome == VK_BROWSER_HOME, "VK_BROWSER_HOME changed");
static_assert(vk::VolumeMute == VK_VOLUME_MUTE, "VK_VOLUME_MUTE changed");
static_assert(vk::VolumeDown == VK_VOLUME_DOWN, "VK_VOLUME_DOWN changed");
static_assert(vk::VolumeUp == VK_VOLUME_UP, "VK_VOLUME_UP changed");
static_assert(vk::MediaNextTrack == VK_MEDIA_NEXT_TRACK,
              "VK_MEDIA_NEXT_TRACK changed");
static_assert(vk::MediaPrevTrack == VK_MEDIA_PREV_TRACK,
              "VK_MEDIA_PREV_TRACK changed");
static_assert(vk::MediaStop == VK_MEDIA_STOP, "VK_MEDIA_STOP changed");
static_assert(vk::MediaPlayPause == VK_MEDIA_PLAY_PAUSE,
              "VK_MEDIA_PLAY_PAUSE changed");
static_assert(vk::LaunchMail == VK_LAUNCH_MAIL, "VK_LAUNCH_MAIL changed");
static_assert(vk::LaunchApp1 == VK_LAUNCH_APP1, "VK_LAUNCH_APP1 changed");
static_assert(vk::LaunchApp2 == VK_LAUNCH_APP2, "VK_LAUNCH_APP2 changed");
static_assert(vk::Oem1 == VK_OEM_1, "VK_OEM_1 changed");
static_assert(vk::OemPlus == VK_OEM_PLUS, "VK_OEM_PLUS changed");
static_assert(vk::OemComma == VK_OEM_COMMA, "VK_OEM_COMMA changed");
static_assert(vk::OemMinus == VK_OEM_MINUS, "VK_OEM_MINUS changed");
static_assert(vk::OemPeriod == VK_OEM_PERIOD, "VK_OEM_PERIOD changed");
static_assert(vk::Oem2 == VK_OEM_2, "VK_OEM_2 changed");
static_assert(vk::Oem3 == VK_OEM_3, "VK_OEM_3 changed");
static_assert(vk::Oem4 == VK_OEM_4, "VK_OEM_4 changed");
static_assert(vk::Oem5 == VK_OEM_5, "VK_OEM_5 changed");
static_assert(vk::Oem6 == VK_OEM_6, "VK_OEM_6 changed");
static_assert(vk::Oem7 == VK_OEM_7, "VK_OEM_7 changed");
static_assert(vk::Oem102 == VK_OEM_102, "VK_OEM_102 changed");
#endif

// Set-1 scan codes, indexed by DOM code. These are the positions printed on
// every PC keyboard, so they are the same on US, German and French hosts.
const unsigned int kScanLetters[26] = {
    /*A*/ 0x1E, /*B*/ 0x30, /*C*/ 0x2E, /*D*/ 0x20, /*E*/ 0x12, /*F*/ 0x21,
    /*G*/ 0x22, /*H*/ 0x23, /*I*/ 0x17, /*J*/ 0x24, /*K*/ 0x25, /*L*/ 0x26,
    /*M*/ 0x32, /*N*/ 0x31, /*O*/ 0x18, /*P*/ 0x19, /*Q*/ 0x10, /*R*/ 0x13,
    /*S*/ 0x1F, /*T*/ 0x14, /*U*/ 0x16, /*V*/ 0x2F, /*W*/ 0x11, /*X*/ 0x2D,
    /*Y*/ 0x15, /*Z*/ 0x2C};

const unsigned int kScanDigits[10] = {
    /*0*/ 0x0B, /*1*/ 0x02, /*2*/ 0x03, /*3*/ 0x04, /*4*/ 0x05,
    /*5*/ 0x06, /*6*/ 0x07, /*7*/ 0x08, /*8*/ 0x09, /*9*/ 0x0A};

const unsigned int kScanNumpad[10] = {
    /*0*/ 0x52, /*1*/ 0x4F, /*2*/ 0x50, /*3*/ 0x51, /*4*/ 0x4B,
    /*5*/ 0x4C, /*6*/ 0x4D, /*7*/ 0x47, /*8*/ 0x48, /*9*/ 0x49};

const unsigned int kScanFunction[24] = {
    /*F1*/ 0x3B,  /*F2*/ 0x3C,  /*F3*/ 0x3D,  /*F4*/ 0x3E,
    /*F5*/ 0x3F,  /*F6*/ 0x40,  /*F7*/ 0x41,  /*F8*/ 0x42,
    /*F9*/ 0x43,  /*F10*/ 0x44, /*F11*/ 0x57, /*F12*/ 0x58,
    /*F13*/ 0x64, /*F14*/ 0x65, /*F15*/ 0x66, /*F16*/ 0x67,
    /*F17*/ 0x68, /*F18*/ 0x69, /*F19*/ 0x6A, /*F20*/ 0x6B,
    /*F21*/ 0x6C, /*F22*/ 0x6D, /*F23*/ 0x6E, /*F24*/ 0x76};

// Linux evdev key codes. X key codes are these plus eight. The X server's
// evdev keymap assigns them by position, so no layout is involved.
const unsigned int kEvdevLetters[26] = {
    /*A*/ 30, /*B*/ 48, /*C*/ 46, /*D*/ 32, /*E*/ 18, /*F*/ 33,
    /*G*/ 34, /*H*/ 35, /*I*/ 23, /*J*/ 36, /*K*/ 37, /*L*/ 38,
    /*M*/ 50, /*N*/ 49, /*O*/ 24, /*P*/ 25, /*Q*/ 16, /*R*/ 19,
    /*S*/ 31, /*T*/ 20, /*U*/ 22, /*V*/ 47, /*W*/ 17, /*X*/ 45,
    /*Y*/ 21, /*Z*/ 44};

const unsigned int kEvdevDigits[10] = {
    /*0*/ 11, /*1*/ 2, /*2*/ 3, /*3*/ 4, /*4*/ 5,
    /*5*/ 6,  /*6*/ 7, /*7*/ 8, /*8*/ 9, /*9*/ 10};

const unsigned int kEvdevNumpad[10] = {
    /*0*/ 82, /*1*/ 79, /*2*/ 80, /*3*/ 81, /*4*/ 75,
    /*5*/ 76, /*6*/ 77, /*7*/ 71, /*8*/ 72, /*9*/ 73};

const unsigned int kEvdevFunction[24] = {
    /*F1*/ 59,  /*F2*/ 60,  /*F3*/ 61,  /*F4*/ 62,
    /*F5*/ 63,  /*F6*/ 64,  /*F7*/ 65,  /*F8*/ 66,
    /*F9*/ 67,  /*F10*/ 68, /*F11*/ 87, /*F12*/ 88,
    /*F13*/ 183, /*F14*/ 184, /*F15*/ 185, /*F16*/ 186,
    /*F17*/ 187, /*F18*/ 188, /*F19*/ 189, /*F20*/ 190,
    /*F21*/ 191, /*F22*/ 192, /*F23*/ 193, /*F24*/ 194};

constexpr unsigned int kX11KeycodeOffset = 8;

// macOS ANSI CGKeyCodes (kVK_ANSI_A .. kVK_ANSI_Z), indexed A..Z.
const unsigned int kMacLettersExplicit[26] = {
    /*A*/ 0,  /*B*/ 11, /*C*/ 8,  /*D*/ 2,  /*E*/ 14, /*F*/ 3,
    /*G*/ 5,  /*H*/ 4,  /*I*/ 34, /*J*/ 38, /*K*/ 40, /*L*/ 37,
    /*M*/ 46, /*N*/ 45, /*O*/ 31, /*P*/ 35, /*Q*/ 12, /*R*/ 15,
    /*S*/ 1,  /*T*/ 17, /*U*/ 32, /*V*/ 9,  /*W*/ 13, /*X*/ 7,
    /*Y*/ 16, /*Z*/ 6};

const unsigned int kMacDigits[10] = {
    /*0*/ 29, /*1*/ 18, /*2*/ 19, /*3*/ 20, /*4*/ 21,
    /*5*/ 23, /*6*/ 22, /*7*/ 26, /*8*/ 28, /*9*/ 25};

const unsigned int kMacNumpad[10] = {
    /*0*/ 82, /*1*/ 83, /*2*/ 84, /*3*/ 85, /*4*/ 86,
    /*5*/ 87, /*6*/ 88, /*7*/ 89, /*8*/ 91, /*9*/ 92};

// F1..F20 exist as Carbon key codes; F21..F24 do not (0 = unsupported).
const unsigned int kMacFunction[24] = {
    /*F1*/ 122,  /*F2*/ 120,  /*F3*/ 99,   /*F4*/ 118,
    /*F5*/ 96,   /*F6*/ 97,   /*F7*/ 98,   /*F8*/ 100,
    /*F9*/ 101,  /*F10*/ 109, /*F11*/ 103, /*F12*/ 111,
    /*F13*/ 105, /*F14*/ 107, /*F15*/ 113, /*F16*/ 106,
    /*F17*/ 64,  /*F18*/ 79,  /*F19*/ 80,  /*F20*/ 90,
    /*F21*/ 0,   /*F22*/ 0,   /*F23*/ 0,   /*F24*/ 0};

const key_entry kWindowsKeys[] = {
    win("Enter", vk::Return, 0x1C),
    win("NumpadEnter", vk::Return, 0x1C, true),
    win("Backspace", vk::Back, 0x0E), win("Tab", vk::Tab, 0x0F),
    win("Space", vk::Space, 0x39), win("Escape", vk::Escape, 0x01),
    win("Delete", vk::Delete, 0x53, true),
    win("Insert", vk::Insert, 0x52, true),
    win("Home", vk::Home, 0x47, true), win("End", vk::End, 0x4F, true),
    win("PageUp", vk::Prior, 0x49, true),
    win("PageDown", vk::Next, 0x51, true),
    win("ArrowLeft", vk::Left, 0x4B, true),
    win("ArrowRight", vk::Right, 0x4D, true),
    win("ArrowUp", vk::Up, 0x48, true),
    win("ArrowDown", vk::Down, 0x50, true),
    win("ShiftLeft", vk::LShift, 0x2A), win("ShiftRight", vk::RShift, 0x36),
    win("ControlLeft", vk::LControl, 0x1D),
    win("ControlRight", vk::RControl, 0x1D, true),
    win("AltLeft", vk::LMenu, 0x38), win("AltRight", vk::RMenu, 0x38, true),
    win("MetaLeft", vk::LWin, 0x5B, true),
    win("MetaRight", vk::RWin, 0x5C, true),
    // Windows reports AltGr as the right Alt key; there is no separate VK.
    win("AltGraph", vk::RMenu, 0x38, true),
    win("CapsLock", vk::Capital, 0x3A),
    win("NumLock", vk::NumLock, 0x45, true),
    win("ScrollLock", vk::Scroll, 0x46),
    win("PrintScreen", vk::Snapshot, 0x37, true),
    // Pause is the E1 1D 45 sequence, which KEYEVENTF_SCANCODE cannot spell;
    // the virtual key makes Windows emit the correct sequence.
    win_vk("Pause", vk::Pause),
    win("ContextMenu", vk::Apps, 0x5D, true),
    win("Minus", vk::OemMinus, 0x0C), win("Equal", vk::OemPlus, 0x0D),
    win("BracketLeft", vk::Oem4, 0x1A),
    win("BracketRight", vk::Oem6, 0x1B),
    win("Backslash", vk::Oem5, 0x2B), win("Semicolon", vk::Oem1, 0x27),
    win("Quote", vk::Oem7, 0x28), win("Backquote", vk::Oem3, 0x29),
    win("Comma", vk::OemComma, 0x33), win("Period", vk::OemPeriod, 0x34),
    win("Slash", vk::Oem2, 0x35),
    // The 102nd key and the JIS ro key share VK_OEM_102; the JIS yen key is in
    // the backslash position. Their scan codes differ by position.
    win("IntlBackslash", vk::Oem102, 0x56),
    win("IntlRo", vk::Oem102, 0x73),
    win("IntlYen", vk::Oem5, 0x7D),
    win("NumpadDecimal", vk::Decimal, 0x53),
    win("NumpadAdd", vk::Add, 0x4E),
    win("NumpadSubtract", vk::Subtract, 0x4A),
    win("NumpadMultiply", vk::Multiply, 0x37),
    win("NumpadDivide", vk::Divide, 0x35, true),
    win("NumpadEqual", vk::OemNecEqual, 0x59),
    // Both DOM codes land on the same Brazilian/JIS keypad position.
    win("NumpadComma", vk::Separator, 0x59),
    win("NumpadSeparator", vk::Separator, 0x59),
    // Media, browser, launch and Sleep are E0-prefixed keys.
    win("AudioVolumeMute", vk::VolumeMute, 0x20, true),
    win("AudioVolumeDown", vk::VolumeDown, 0x2E, true),
    win("AudioVolumeUp", vk::VolumeUp, 0x30, true),
    win("MediaTrackNext", vk::MediaNextTrack, 0x19, true),
    win("MediaTrackPrevious", vk::MediaPrevTrack, 0x10, true),
    win("MediaStop", vk::MediaStop, 0x24, true),
    win("MediaPlayPause", vk::MediaPlayPause, 0x22, true),
    win("BrowserBack", vk::BrowserBack, 0x6A, true),
    win("BrowserForward", vk::BrowserForward, 0x69, true),
    win("BrowserRefresh", vk::BrowserRefresh, 0x67, true),
    win("BrowserStop", vk::BrowserStop, 0x68, true),
    win("BrowserSearch", vk::BrowserSearch, 0x65, true),
    win("BrowserFavorites", vk::BrowserFavorites, 0x66, true),
    win("BrowserHome", vk::BrowserHome, 0x32, true),
    win("LaunchMail", vk::LaunchMail, 0x6C, true),
    win("LaunchApp1", vk::LaunchApp1, 0x6B, true),
    win("LaunchApp2", vk::LaunchApp2, 0x21, true),
    // These keys have no portable set-1 position on a PC keyboard; the
    // virtual key is the only way most hosts can receive them.
    win_vk("Help", vk::Help), win_vk("Select", vk::Select),
    win_vk("Execute", vk::Execute), win_vk("Clear", vk::Clear),
    // Windows has no virtual key for these editing/legacy keys: no keyboard
    // produces them, and SendInput cannot synthesise them either.
    unsupported("Again"), unsupported("Undo"), unsupported("Cut"),
    unsupported("Copy"), unsupported("Paste"), unsupported("Find"),
    win_vk("Cancel", vk::Cancel),
    win("Power", 0, 0x5E, true),
    win("Sleep", vk::Sleep, 0x5F, true),
    win("WakeUp", 0, 0x63, true),
    win_vk("Convert", vk::Convert), win_vk("NonConvert", vk::NonConvert),
    win_vk("KanaMode", vk::Kana), win_vk("HangulMode", vk::Hangul),
    win_vk("HanjaMode", vk::Hanja),
    // Lang1/Lang2 are the Korean Hangul/Hanja keys on Windows.
    win_vk("Lang1", vk::Kana), win_vk("Lang2", vk::Hanja),
};

const key_entry kX11Keys[] = {
    // X key code = evdev key code + 8. See the evdev keymap in
    // /usr/share/X11/xkb/keycodes/evdev; these positions do not depend on
    // the layout loaded on top of it.
    x11("Enter", "Return", 36), x11("NumpadEnter", "KP_Enter", 104),
    x11("Backspace", "BackSpace", 22), x11("Tab", "Tab", 23),
    x11("Space", "space", 65), x11("Escape", "Escape", 9),
    x11("Delete", "Delete", 119), x11("Insert", "Insert", 118),
    x11("Home", "Home", 110), x11("End", "End", 115),
    x11("PageUp", "Prior", 112), x11("PageDown", "Next", 117),
    x11("ArrowLeft", "Left", 113), x11("ArrowRight", "Right", 114),
    x11("ArrowUp", "Up", 111), x11("ArrowDown", "Down", 116),
    x11("ShiftLeft", "Shift_L", 50), x11("ShiftRight", "Shift_R", 62),
    x11("ControlLeft", "Control_L", 37),
    x11("ControlRight", "Control_R", 105),
    x11("AltLeft", "Alt_L", 64), x11("AltRight", "Alt_R", 108),
    x11("MetaLeft", "Super_L", 133), x11("MetaRight", "Super_R", 134),
    x11("AltGraph", "ISO_Level3_Shift", 108),
    x11("CapsLock", "Caps_Lock", 66), x11("NumLock", "Num_Lock", 77),
    x11("ScrollLock", "Scroll_Lock", 78),
    x11("PrintScreen", "Print", 107), x11("Pause", "Pause", 127),
    x11("ContextMenu", "Menu", 135),
    x11("Minus", "minus", 20), x11("Equal", "equal", 21),
    x11("BracketLeft", "bracketleft", 34),
    x11("BracketRight", "bracketright", 35),
    x11("Backslash", "backslash", 51), x11("Semicolon", "semicolon", 47),
    x11("Quote", "apostrophe", 48), x11("Backquote", "grave", 49),
    x11("Comma", "comma", 59), x11("Period", "period", 60),
    x11("Slash", "slash", 61),
    // The 102nd key is < > on ISO layouts; the JIS ro key is Romaji and the
    // yen key is yen.
    x11("IntlBackslash", "less", 94), x11("IntlRo", "Romaji", 97),
    x11("IntlYen", "yen", 132),
    x11("NumpadDecimal", "KP_Decimal", 91),
    x11("NumpadAdd", "KP_Add", 86),
    x11("NumpadSubtract", "KP_Subtract", 82),
    x11("NumpadMultiply", "KP_Multiply", 63),
    x11("NumpadDivide", "KP_Divide", 106),
    x11("NumpadEqual", "KP_Equal", 125),
    x11("NumpadComma", "KP_Separator", 129),
    x11("NumpadSeparator", "KP_Separator", 129),
    x11("AudioVolumeMute", "XF86AudioMute", 121),
    x11("AudioVolumeDown", "XF86AudioLowerVolume", 122),
    x11("AudioVolumeUp", "XF86AudioRaiseVolume", 123),
    x11("MediaTrackNext", "XF86AudioNext", 171),
    x11("MediaTrackPrevious", "XF86AudioPrev", 173),
    x11("MediaStop", "XF86AudioStop", 174),
    x11("MediaPlayPause", "XF86AudioPlay", 172),
    x11("BrowserBack", "XF86Back", 166),
    x11("BrowserForward", "XF86Forward", 167),
    x11("BrowserRefresh", "XF86Refresh", 181),
    x11("BrowserStop", "XF86Stop", 136),
    x11("BrowserSearch", "XF86Search", 225),
    x11("BrowserFavorites", "XF86Favorites", 164),
    x11("BrowserHome", "XF86HomePage", 180),
    x11("LaunchMail", "XF86Mail", 163),
    x11("LaunchApp1", "XF86Launch1", 165),
    x11("LaunchApp2", "XF86Launch2", 148),
    x11("Help", "Help", 146),
    // No physical Select/Execute/Clear key exists on evdev keyboards.
    x11("Select", "Select", 0), x11("Execute", "Execute", 0),
    x11("Clear", "Clear", 0),
    x11("Again", "Redo", 137), x11("Undo", "Undo", 139),
    x11("Cut", "Cut", 145), x11("Copy", "Copy", 141),
    x11("Paste", "Paste", 143), x11("Find", "Find", 144),
    x11("Cancel", "Cancel", 231),
    x11("Power", "XF86PowerOff", 124), x11("Sleep", "XF86Sleep", 150),
    x11("WakeUp", "XF86WakeUp", 151),
    x11("Convert", "Henkan", 100), x11("NonConvert", "Muhenkan", 102),
    x11("KanaMode", "Kana_Lock", 98), x11("HangulMode", "Hangul", 130),
    x11("HanjaMode", "Hangul_Hanja", 131),
    x11("Lang1", "Hangul", 130), x11("Lang2", "Hangul_Hanja", 131),
};

const key_entry kMacKeys[] = {
    mac("Enter", 36), mac("NumpadEnter", 76),
    mac("Backspace", 51), mac("Tab", 48), mac("Space", 49),
    mac("Escape", 53),
    // macOS has no Insert key; the Help key occupies that position on PC
    // keyboards attached to a Mac.
    mac("Delete", 117), mac("Insert", 114),
    mac("Home", 115), mac("End", 119),
    mac("PageUp", 116), mac("PageDown", 121),
    mac("ArrowLeft", 123), mac("ArrowRight", 124),
    mac("ArrowUp", 126), mac("ArrowDown", 125),
    mac("ShiftLeft", 56), mac("ShiftRight", 60),
    mac("ControlLeft", 59), mac("ControlRight", 62),
    mac("AltLeft", 58), mac("AltRight", 61),
    mac("MetaLeft", 55), mac("MetaRight", 54),
    unsupported("AltGraph"),  // no AltGr key on Apple keyboards
    mac("CapsLock", 57), mac("NumLock", 71), unsupported("ScrollLock"),
    unsupported("PrintScreen"), unsupported("Pause"),
    unsupported("ContextMenu"),
    mac("Minus", 27), mac("Equal", 24),
    mac("BracketLeft", 33), mac("BracketRight", 30),
    mac("Backslash", 42), mac("Semicolon", 41), mac("Quote", 39),
    mac("Backquote", 50), mac("Comma", 43), mac("Period", 47),
    mac("Slash", 44),
    mac("IntlBackslash", 10), mac("IntlRo", 94), mac("IntlYen", 93),
    mac("NumpadDecimal", 65), mac("NumpadAdd", 69),
    mac("NumpadSubtract", 78), mac("NumpadMultiply", 67),
    mac("NumpadDivide", 75), mac("NumpadEqual", 81),
    // The JIS keypad comma exists; there is no separate keypad separator.
    mac("NumpadComma", 95), unsupported("NumpadSeparator"),
    mac("AudioVolumeMute", 74), mac("AudioVolumeDown", 73),
    mac("AudioVolumeUp", 72),
    unsupported("MediaTrackNext"), unsupported("MediaTrackPrevious"),
    unsupported("MediaStop"), unsupported("MediaPlayPause"),
    unsupported("BrowserBack"), unsupported("BrowserForward"),
    unsupported("BrowserRefresh"), unsupported("BrowserStop"),
    unsupported("BrowserSearch"), unsupported("BrowserFavorites"),
    unsupported("BrowserHome"), unsupported("LaunchMail"),
    unsupported("LaunchApp1"), unsupported("LaunchApp2"),
    mac("Help", 114), unsupported("Select"), unsupported("Execute"),
    // Clear is the keypad Clear key, which browsers also report as NumLock.
    mac("Clear", 71),
    unsupported("Again"), unsupported("Undo"), unsupported("Cut"),
    unsupported("Copy"), unsupported("Paste"), unsupported("Find"),
    unsupported("Cancel"), unsupported("Power"), unsupported("Sleep"),
    unsupported("WakeUp"),
    unsupported("Convert"), unsupported("NonConvert"),
    // JIS Kana and Eisu are the closest keys the Mac keyboard has.
    mac("KanaMode", 104), unsupported("HangulMode"),
    unsupported("HanjaMode"), mac("Lang1", 104), mac("Lang2", 102),
};

bool parse_suffix_number(const std::string& code, const char* prefix,
                         std::size_t prefix_len, int& value) {
  if (code.size() <= prefix_len ||
      code.compare(0, prefix_len, prefix) != 0) {
    return false;
  }
  const std::string digits = code.substr(prefix_len);
  if (digits.empty()) return false;
  int parsed = 0;
  for (const char c : digits) {
    if (c < '0' || c > '9') return false;
    parsed = parsed * 10 + (c - '0');
    // Reject absurd input early instead of overflowing; no key index is
    // anywhere near this large.
    if (parsed > 10000) return false;
  }
  value = parsed;
  return true;
}

const key_entry* find_entry(const key_entry* table, std::size_t count,
                            const std::string& code) {
  for (std::size_t i = 0; i < count; ++i) {
    if (std::strcmp(table[i].code, code.c_str()) == 0) return &table[i];
  }
  return nullptr;
}

}  // namespace

key_lookup lookup_key(keymap_platform platform, const std::string& raw_code) {
  const std::string code = canonical_key_code(raw_code);
  key_lookup out;

  // Letters and the main digit row exist on every platform. Their position is
  // fixed: `KeyY` is scan 0x15 / X key code 29 on a US, German or French
  // desktop; only the character the host's layout produces differs.
  int number = 0;
  if (code.size() == 4 && code.compare(0, 3, "Key") == 0 && code[3] >= 'A' &&
      code[3] <= 'Z') {
    out.found = true;
    const int index = code[3] - 'A';
    switch (platform) {
      case keymap_platform::windows:
        out.value = static_cast<unsigned int>('A' + index);
        out.scan = kScanLetters[index];
        break;
      case keymap_platform::x11:
        out.keysym = std::string(1, static_cast<char>('a' + index));
        out.keycode = kEvdevLetters[index] + kX11KeycodeOffset;
        break;
      case keymap_platform::macos:
        out.value = kMacLettersExplicit[index];
        break;
    }
    return out;
  }
  if (parse_suffix_number(code, "Digit", 5, number) && number >= 0 &&
      number <= 9) {
    out.found = true;
    switch (platform) {
      case keymap_platform::windows:
        out.value = static_cast<unsigned int>('0' + number);
        out.scan = kScanDigits[number];
        break;
      case keymap_platform::x11:
        out.keysym = std::string(1, static_cast<char>('0' + number));
        out.keycode = kEvdevDigits[number] + kX11KeycodeOffset;
        break;
      case keymap_platform::macos:
        out.value = kMacDigits[number];
        break;
    }
    return out;
  }
  // Numpad digits: VK_NUMPAD0..9 are contiguous; X11 has KP_0..KP_9.
  if (parse_suffix_number(code, "Numpad", 6, number) && number >= 0 &&
      number <= 9) {
    out.found = true;
    switch (platform) {
      case keymap_platform::windows:
        out.value = 0x60u + static_cast<unsigned int>(number);
        out.scan = kScanNumpad[number];
        break;
      case keymap_platform::x11:
        out.keysym = "KP_" + std::string(1, static_cast<char>('0' + number));
        out.keycode = kEvdevNumpad[number] + kX11KeycodeOffset;
        break;
      case keymap_platform::macos:
        out.value = kMacNumpad[number];
        break;
    }
    return out;
  }
  if (parse_suffix_number(code, "F", 1, number) && number >= 1 &&
      number <= static_cast<int>(vk::F24 - vk::F1 + 1)) {
    out.found = true;
    switch (platform) {
      case keymap_platform::windows:
        out.value = vk::F1 + static_cast<unsigned int>(number - 1);
        out.scan = kScanFunction[number - 1];
        break;
      case keymap_platform::x11:
        out.keysym = code;  // F1..F24 are their own keysym names
        out.keycode = kEvdevFunction[number - 1] + kX11KeycodeOffset;
        break;
      case keymap_platform::macos:
        out.value = kMacFunction[number - 1];
        if (out.value == 0) out.unsupported = true;
        break;
    }
    return out;
  }

  const key_entry* table = nullptr;
  std::size_t count = 0;
  switch (platform) {
    case keymap_platform::windows:
      table = kWindowsKeys;
      count = sizeof(kWindowsKeys) / sizeof(kWindowsKeys[0]);
      break;
    case keymap_platform::x11:
      table = kX11Keys;
      count = sizeof(kX11Keys) / sizeof(kX11Keys[0]);
      break;
    case keymap_platform::macos:
      table = kMacKeys;
      count = sizeof(kMacKeys) / sizeof(kMacKeys[0]);
      break;
  }
  const key_entry* entry = find_entry(table, count, code);
  if (entry == nullptr) return out;  // unknown to this host

  out.found = true;
  out.unsupported = entry->unsupported;
  out.value = entry->value;
  out.scan = entry->scan;
  out.keycode = entry->keycode;
  if (entry->keysym != nullptr) out.keysym = entry->keysym;
  out.extended = entry->extended;
  return out;
}

bool key_depends_on_numlock(const std::string& raw_code) {
  const std::string code = canonical_key_code(raw_code);
  if (code == "NumpadDecimal") return true;
  int number = 0;
  return parse_suffix_number(code, "Numpad", 6, number) && number >= 0 &&
         number <= 9;
}

const char* const kCanonicalKeyCodes[] = {
    // Letters
    "KeyA", "KeyB", "KeyC", "KeyD", "KeyE", "KeyF", "KeyG", "KeyH", "KeyI",
    "KeyJ", "KeyK", "KeyL", "KeyM", "KeyN", "KeyO", "KeyP", "KeyQ", "KeyR",
    "KeyS", "KeyT", "KeyU", "KeyV", "KeyW", "KeyX", "KeyY", "KeyZ",
    // Digits
    "Digit0", "Digit1", "Digit2", "Digit3", "Digit4", "Digit5", "Digit6",
    "Digit7", "Digit8", "Digit9",
    // Function keys
    "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11",
    "F12", "F13", "F14", "F15", "F16", "F17", "F18", "F19", "F20", "F21",
    "F22", "F23", "F24",
    // Editing and navigation
    "Enter", "Backspace", "Tab", "Space", "Escape", "Delete", "Insert", "Home",
    "End", "PageUp", "PageDown", "ArrowUp", "ArrowDown", "ArrowLeft",
    "ArrowRight",
    // Modifiers
    "ShiftLeft", "ShiftRight", "ControlLeft", "ControlRight", "AltLeft",
    "AltRight", "MetaLeft", "MetaRight", "AltGraph",
    // Locks
    "CapsLock", "NumLock", "ScrollLock",
    // Punctuation
    "Minus", "Equal", "BracketLeft", "BracketRight", "Backslash", "Semicolon",
    "Quote", "Backquote", "Comma", "Period", "Slash", "IntlBackslash",
    "IntlRo", "IntlYen",
    // Numpad
    "Numpad0", "Numpad1", "Numpad2", "Numpad3", "Numpad4", "Numpad5",
    "Numpad6", "Numpad7", "Numpad8", "Numpad9", "NumpadAdd", "NumpadSubtract",
    "NumpadMultiply", "NumpadDivide", "NumpadDecimal", "NumpadEnter",
    "NumpadEqual", "NumpadComma", "NumpadSeparator",
    // Media
    "AudioVolumeMute", "AudioVolumeDown", "AudioVolumeUp", "MediaTrackNext",
    "MediaTrackPrevious", "MediaStop", "MediaPlayPause",
    // Browser and launch
    "BrowserBack", "BrowserForward", "BrowserRefresh", "BrowserStop",
    "BrowserSearch", "BrowserFavorites", "BrowserHome", "LaunchMail",
    "LaunchApp1", "LaunchApp2",
    // System
    "PrintScreen", "Pause", "ContextMenu", "Help", "Select", "Execute",
    "Clear", "Again", "Undo", "Cut", "Copy", "Paste", "Find", "Cancel",
    "Power", "Sleep", "WakeUp",
    // IME
    "Convert", "NonConvert", "KanaMode", "HangulMode", "HanjaMode", "Lang1",
    "Lang2",
};

const std::size_t kCanonicalKeyCodeCount =
    sizeof(kCanonicalKeyCodes) / sizeof(kCanonicalKeyCodes[0]);

}  // namespace molink
