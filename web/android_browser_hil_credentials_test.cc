#include "web/android_browser_hil_credentials.h"

#include <stdexcept>
#include <string>

namespace {

void Check(bool condition) {
  if (!condition) {
    throw std::runtime_error("Android browser HIL credential contract check failed");
  }
}

template <typename Exception, typename Action>
void CheckRejected(Action action) {
  bool rejected = false;
  try {
    action();
  } catch (const Exception &) {
    rejected = true;
  }
  Check(rejected);
}

}  // namespace

int main() {
  using swing_capture::web::ExtractAndroidControlToken;
  using swing_capture::web::ValidateDirectLanNodeOrigin;

  Check(ExtractAndroidControlToken(
            "<map><string name=\"control_token\">A2345678901234567890123456789012"
            "</string></map>") == "A2345678901234567890123456789012");
  CheckRejected<std::runtime_error>(
      [&] { static_cast<void>(ExtractAndroidControlToken("<map/>")); });
  CheckRejected<std::runtime_error>([&] {
    static_cast<void>(
        ExtractAndroidControlToken("<string name=\"control_token\">too-short</string>"));
  });

  ValidateDirectLanNodeOrigin("http://10.168.168.241:8088");
  for (const std::string invalid : {"http://127.0.0.1:8088", "https://10.0.0.2:8088",
                                    "http://10.0.0.2:8088/", "http://10.0.0.2:9000"}) {
    CheckRejected<std::invalid_argument>([&] { ValidateDirectLanNodeOrigin(invalid); });
  }
  return 0;
}
