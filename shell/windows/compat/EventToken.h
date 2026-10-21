#pragma once
// Minimal stand-in for the Windows SDK header. MinGW-w64 does not ship it and
// WebView2.h only needs the struct.
typedef struct EventRegistrationToken {
  __int64 value;
} EventRegistrationToken;
