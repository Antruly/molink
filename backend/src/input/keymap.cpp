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

struct key_entry {
  const char* code;
  unsigned int value;   // Windows VK / scan code, or macOS CGKeyCode
  const char* keysym;   // X11 keysym name
  bool extended;        // Windows E0 prefix
  bool unsupported;     // known DOM code, no key on this platform
  bool scan_code;       // Windows: value is a scan code, not a VK
};

constexpr key_entry win(const char* code, unsigned int vk,
                        bool extended = false) {
  return key_entry{code, vk, nullptr, extended, false, false};
}

constexpr key_entry win_scan(const char* code, unsigned int scan,
                             bool extended) {
  return key_entry{code, scan, nullptr, extended, false, true};
}

constexpr key_entry x11(const char* code, const char* keysym) {
  return key_entry{code, 0, keysym, false, false, false};
}

constexpr key_entry mac(const char* code, unsigned int keycode) {
  return key_entry{code, keycode, nullptr, false, false, false};
}

constexpr key_entry unsupported(const char* code) {
  return key_entry{code, 0, nullptr, false, true, false};
}

// Windows virtual-key values. Keeping them here (instead of including
// <windows.h> in the table) lets the Linux CI build exercise the Windows
// table too. On Windows every value is checked against the real VK_ constant
// below, so a typo cannot survive the windows-core job.
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
// No VK_ constant exists for the ACPI Power and Wake Up keys; they are sent
// as their E0 scan codes instead.
constexpr unsigned int ScanPower = 0x5E;
constexpr unsigned int ScanWakeUp = 0x63;
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
    win("Enter", vk::Return),
    win("NumpadEnter", vk::Return, true),
    win("Backspace", vk::Back), win("Tab", vk::Tab),
    win("Space", vk::Space), win("Escape", vk::Escape),
    win("Delete", vk::Delete, true), win("Insert", vk::Insert, true),
    win("Home", vk::Home, true), win("End", vk::End, true),
    win("PageUp", vk::Prior, true), win("PageDown", vk::Next, true),
    win("ArrowLeft", vk::Left, true), win("ArrowRight", vk::Right, true),
    win("ArrowUp", vk::Up, true), win("ArrowDown", vk::Down, true),
    win("ShiftLeft", vk::LShift), win("ShiftRight", vk::RShift),
    win("ControlLeft", vk::LControl), win("ControlRight", vk::RControl, true),
    win("AltLeft", vk::LMenu), win("AltRight", vk::RMenu, true),
    win("MetaLeft", vk::LWin, true), win("MetaRight", vk::RWin, true),
    // Windows reports AltGr as the right Alt key; there is no separate VK.
    win("AltGraph", vk::RMenu, true),
    win("CapsLock", vk::Capital), win("NumLock", vk::NumLock, true),
    win("ScrollLock", vk::Scroll), win("PrintScreen", vk::Snapshot, true),
    win("Pause", vk::Pause), win("ContextMenu", vk::Apps, true),
    win("Minus", vk::OemMinus), win("Equal", vk::OemPlus),
    win("BracketLeft", vk::Oem4), win("BracketRight", vk::Oem6),
    win("Backslash", vk::Oem5), win("Semicolon", vk::Oem1),
    win("Quote", vk::Oem7), win("Backquote", vk::Oem3),
    win("Comma", vk::OemComma), win("Period", vk::OemPeriod),
    win("Slash", vk::Oem2),
    // The 102nd key and the JIS ro key share VK_OEM_102; the JIS yen key is in
    // the backslash position.
    win("IntlBackslash", vk::Oem102), win("IntlRo", vk::Oem102),
    win("IntlYen", vk::Oem5),
    win("NumpadDecimal", vk::Decimal), win("NumpadAdd", vk::Add),
    win("NumpadSubtract", vk::Subtract), win("NumpadMultiply", vk::Multiply),
    win("NumpadDivide", vk::Divide, true),
    win("NumpadEqual", vk::OemNecEqual),
    // Both DOM codes land on VK_SEPARATOR; Windows has only one separator VK.
    win("NumpadComma", vk::Separator),
    win("NumpadSeparator", vk::Separator),
    // Media, browser, launch and Sleep are E0-prefixed keys.
    win("AudioVolumeMute", vk::VolumeMute, true),
    win("AudioVolumeDown", vk::VolumeDown, true),
    win("AudioVolumeUp", vk::VolumeUp, true),
    win("MediaTrackNext", vk::MediaNextTrack, true),
    win("MediaTrackPrevious", vk::MediaPrevTrack, true),
    win("MediaStop", vk::MediaStop, true),
    win("MediaPlayPause", vk::MediaPlayPause, true),
    win("BrowserBack", vk::BrowserBack, true),
    win("BrowserForward", vk::BrowserForward, true),
    win("BrowserRefresh", vk::BrowserRefresh, true),
    win("BrowserStop", vk::BrowserStop, true),
    win("BrowserSearch", vk::BrowserSearch, true),
    win("BrowserFavorites", vk::BrowserFavorites, true),
    win("BrowserHome", vk::BrowserHome, true),
    win("LaunchMail", vk::LaunchMail, true),
    win("LaunchApp1", vk::LaunchApp1, true),
    win("LaunchApp2", vk::LaunchApp2, true),
    win("Help", vk::Help), win("Select", vk::Select),
    win("Execute", vk::Execute), win("Clear", vk::Clear),
    // Windows has no virtual key for these editing/legacy keys: no keyboard
    // produces them, and SendInput cannot synthesise them either.
    unsupported("Again"), unsupported("Undo"), unsupported("Cut"),
    unsupported("Copy"), unsupported("Paste"), unsupported("Find"),
    win("Cancel", vk::Cancel),
    win_scan("Power", vk::ScanPower, true),
    win("Sleep", vk::Sleep, true),
    win_scan("WakeUp", vk::ScanWakeUp, true),
    win("Convert", vk::Convert), win("NonConvert", vk::NonConvert),
    win("KanaMode", vk::Kana), win("HangulMode", vk::Hangul),
    win("HanjaMode", vk::Hanja),
    // Lang1/Lang2 are the Korean Hangul/Hanja keys on Windows.
    win("Lang1", vk::Kana), win("Lang2", vk::Hanja),
};

const key_entry kX11Keys[] = {
    x11("Enter", "Return"), x11("NumpadEnter", "KP_Enter"),
    x11("Backspace", "BackSpace"), x11("Tab", "Tab"),
    x11("Space", "space"), x11("Escape", "Escape"),
    x11("Delete", "Delete"), x11("Insert", "Insert"),
    x11("Home", "Home"), x11("End", "End"),
    x11("PageUp", "Prior"), x11("PageDown", "Next"),
    x11("ArrowLeft", "Left"), x11("ArrowRight", "Right"),
    x11("ArrowUp", "Up"), x11("ArrowDown", "Down"),
    x11("ShiftLeft", "Shift_L"), x11("ShiftRight", "Shift_R"),
    x11("ControlLeft", "Control_L"), x11("ControlRight", "Control_R"),
    x11("AltLeft", "Alt_L"), x11("AltRight", "Alt_R"),
    x11("MetaLeft", "Super_L"), x11("MetaRight", "Super_R"),
    x11("AltGraph", "ISO_Level3_Shift"),
    x11("CapsLock", "Caps_Lock"), x11("NumLock", "Num_Lock"),
    x11("ScrollLock", "Scroll_Lock"), x11("PrintScreen", "Print"),
    x11("Pause", "Pause"), x11("ContextMenu", "Menu"),
    x11("Minus", "minus"), x11("Equal", "equal"),
    x11("BracketLeft", "bracketleft"), x11("BracketRight", "bracketright"),
    x11("Backslash", "backslash"), x11("Semicolon", "semicolon"),
    x11("Quote", "apostrophe"), x11("Backquote", "grave"),
    x11("Comma", "comma"), x11("Period", "period"),
    x11("Slash", "slash"),
    // The 102nd key is < > on ISO layouts; the JIS ro key is Romaji and the
    // yen key is yen.
    x11("IntlBackslash", "less"), x11("IntlRo", "Romaji"),
    x11("IntlYen", "yen"),
    x11("NumpadDecimal", "KP_Decimal"), x11("NumpadAdd", "KP_Add"),
    x11("NumpadSubtract", "KP_Subtract"),
    x11("NumpadMultiply", "KP_Multiply"),
    x11("NumpadDivide", "KP_Divide"), x11("NumpadEqual", "KP_Equal"),
    x11("NumpadComma", "KP_Separator"),
    x11("NumpadSeparator", "KP_Separator"),
    x11("AudioVolumeMute", "XF86AudioMute"),
    x11("AudioVolumeDown", "XF86AudioLowerVolume"),
    x11("AudioVolumeUp", "XF86AudioRaiseVolume"),
    x11("MediaTrackNext", "XF86AudioNext"),
    x11("MediaTrackPrevious", "XF86AudioPrev"),
    x11("MediaStop", "XF86AudioStop"),
    x11("MediaPlayPause", "XF86AudioPlay"),
    x11("BrowserBack", "XF86Back"), x11("BrowserForward", "XF86Forward"),
    x11("BrowserRefresh", "XF86Refresh"), x11("BrowserStop", "XF86Stop"),
    x11("BrowserSearch", "XF86Search"),
    x11("BrowserFavorites", "XF86Favorites"),
    x11("BrowserHome", "XF86HomePage"),
    x11("LaunchMail", "XF86Mail"), x11("LaunchApp1", "XF86Launch1"),
    x11("LaunchApp2", "XF86Launch2"),
    x11("Help", "Help"), x11("Select", "Select"),
    x11("Execute", "Execute"), x11("Clear", "Clear"),
    x11("Again", "Redo"), x11("Undo", "Undo"),
    x11("Cut", "Cut"), x11("Copy", "Copy"), x11("Paste", "Paste"),
    x11("Find", "Find"), x11("Cancel", "Cancel"),
    x11("Power", "XF86PowerOff"), x11("Sleep", "XF86Sleep"),
    x11("WakeUp", "XF86WakeUp"),
    x11("Convert", "Henkan"), x11("NonConvert", "Muhenkan"),
    x11("KanaMode", "Kana_Lock"), x11("HangulMode", "Hangul"),
    x11("HanjaMode", "Hangul_Hanja"),
    x11("Lang1", "Hangul"), x11("Lang2", "Hangul_Hanja"),
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

  // Letters and the main digit row exist on every platform. Windows uses the
  // ASCII VK value, X11 the keysym name, macOS the ANSI key code.
  int number = 0;
  if (code.size() == 4 && code.compare(0, 3, "Key") == 0 && code[3] >= 'A' &&
      code[3] <= 'Z') {
    out.found = true;
    const int index = code[3] - 'A';
    switch (platform) {
      case keymap_platform::windows:
        out.value = static_cast<unsigned int>('A' + index);
        break;
      case keymap_platform::x11:
        out.keysym = std::string(1, static_cast<char>('a' + index));
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
        break;
      case keymap_platform::x11:
        out.keysym = std::string(1, static_cast<char>('0' + number));
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
        break;
      case keymap_platform::x11:
        out.keysym = "KP_" + std::string(1, static_cast<char>('0' + number));
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
        break;
      case keymap_platform::x11:
        out.keysym = code;  // F1..F24 are their own keysym names
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
  if (entry->keysym != nullptr) out.keysym = entry->keysym;
  out.extended = entry->extended;
  out.scan_code = entry->scan_code;
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
