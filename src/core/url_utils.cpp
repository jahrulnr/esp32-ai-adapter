#include "core/url_utils.h"

namespace ai::provider {

String joinUrl(const String& baseUrl, const char* path) {
  if (path == nullptr || path[0] == '\0') {
    return baseUrl;
  }

  if (baseUrl.length() == 0) {
    return String(path);
  }

  String normalizedBase = baseUrl;
  while (normalizedBase.length() > 0 && normalizedBase.endsWith("/")) {
    normalizedBase.remove(normalizedBase.length() - 1);
  }

  String normalizedPath(path);
  while (normalizedPath.length() > 0 && normalizedPath.startsWith("/")) {
    normalizedPath.remove(0, 1);
  }

  if (normalizedPath.length() == 0) {
    return normalizedBase;
  }

  return normalizedBase + "/" + normalizedPath;
}

}  // namespace ai::provider
