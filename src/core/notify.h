#pragma once
#include <string>

// nerdfont codepoints, hex without prefix (wispctl's icon-cp argument)
#define ICON_MAIL "f0e0"
#define ICON_LOCK "f023"

// runs the configured hook, argv order is wispctl's:
//   <hook> <urgency 0|1|2> <summary> <body> <icon-cp>
// empty `notify` config silences everything; failures are never fatal
void notify(int urgency, const std::string &summary, const std::string &body, const char *icon);
