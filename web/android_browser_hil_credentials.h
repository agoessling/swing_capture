#ifndef SWING_CAPTURE_WEB_ANDROID_BROWSER_HIL_CREDENTIALS_H_
#define SWING_CAPTURE_WEB_ANDROID_BROWSER_HIL_CREDENTIALS_H_

#include <string>
#include <string_view>

namespace swing_capture::web {

[[nodiscard]] std::string ExtractAndroidControlToken(std::string_view preferences_xml);
void ValidateDirectLanNodeOrigin(std::string_view origin);

}  // namespace swing_capture::web

#endif  // SWING_CAPTURE_WEB_ANDROID_BROWSER_HIL_CREDENTIALS_H_
