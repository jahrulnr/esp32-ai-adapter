#pragma once

#include <ArduinoJson.h>

#include "core/ai_provider_types.h"

namespace ai::provider {

template <typename TJsonDocument>
bool applyJsonBodyToHttpRequest(const TJsonDocument& jsonDoc,
                                const AiRequestBodySpoolOptions& spoolOptions,
                                AiHttpRequest& outHttpRequest,
                                String& outErrorMessage) {
  outErrorMessage = String();

  outHttpRequest.body = "";
  outHttpRequest.bodyFs = nullptr;
  outHttpRequest.bodyFilePath = "";
  outHttpRequest.removeBodyFileAfterSend = false;
  outHttpRequest.bodyStreamChunkBytes = 1024;

  if (spoolOptions.forceSpool &&
      (spoolOptions.filesystem == nullptr || spoolOptions.filePath.length() == 0)) {
    outErrorMessage = "request_body_spool_force_requires_fs_and_path";
    return false;
  }

  const size_t estimatedBytes = measureJson(jsonDoc);
  const bool canSpool = spoolOptions.filesystem != nullptr && spoolOptions.filePath.length() > 0;
  const bool shouldSpool = canSpool &&
                           (spoolOptions.forceSpool ||
                            (spoolOptions.thresholdBytes > 0 &&
                             estimatedBytes >= spoolOptions.thresholdBytes));

  if (!shouldSpool) {
    const size_t written = serializeJson(jsonDoc, outHttpRequest.body);
    if (estimatedBytes > 0 && written == 0) {
      outErrorMessage = "request_body_json_serialize_failed";
      return false;
    }
    return true;
  }

  File spoolFile = spoolOptions.filesystem->open(spoolOptions.filePath, "w");
  if (!spoolFile) {
    outErrorMessage = "request_body_spool_open_failed";
    return false;
  }

  const size_t written = serializeJson(jsonDoc, spoolFile);
  spoolFile.flush();
  spoolFile.close();

  if (written == 0 && estimatedBytes > 0) {
    outErrorMessage = "request_body_spool_write_failed";
    return false;
  }

  if (estimatedBytes > 0 && written != estimatedBytes) {
    outErrorMessage = "request_body_spool_incomplete_write";
    return false;
  }

  outHttpRequest.bodyFs = spoolOptions.filesystem;
  outHttpRequest.bodyFilePath = spoolOptions.filePath;
  outHttpRequest.removeBodyFileAfterSend = spoolOptions.removeAfterSend;
  outHttpRequest.bodyStreamChunkBytes = spoolOptions.streamChunkBytes;
  return true;
}

}  // namespace ai::provider
