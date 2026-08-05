#include "BookOrbitConfig.h"

#include <Logging.h>
#include <MD5Builder.h>
#include <ObfuscationUtils.h>

void BookOrbitConfig::toJson(JsonDocument& doc) const {
  doc["v"] = 1;
  doc["serverUrl"] = serverUrl;
  doc["username"] = username;
  doc["password_obf"] = obfuscation::obfuscateToBase64(password);
  doc["matchMethod"] = static_cast<uint8_t>(matchMethod);
  doc["syncProgress"] = syncProgress;
  doc["syncSessions"] = syncSessions;
  doc["syncHighlights"] = syncHighlights;
  doc["syncBookmarks"] = syncBookmarks;
}

bool BookOrbitConfig::fromJson(JsonVariantConst doc) {
  serverUrl = doc["serverUrl"] | "";
  username = doc["username"] | "";

  obfuscation::DecodeStatus status = obfuscation::DecodeStatus::INVALID;
  password = obfuscation::deobfuscateFromBase64(doc["password_obf"] | "", &status);
  if (status == obfuscation::DecodeStatus::INVALID) {
    password.clear();
  }

  const uint8_t method = doc["matchMethod"] | static_cast<uint8_t>(0);
  matchMethod = method <= static_cast<uint8_t>(BookOrbitMatchMethod::BINARY) ? static_cast<BookOrbitMatchMethod>(method)
                                                                            : BookOrbitMatchMethod::FILENAME;

  syncProgress = doc["syncProgress"] | true;
  syncSessions = doc["syncSessions"] | true;
  syncHighlights = doc["syncHighlights"] | true;
  syncBookmarks = doc["syncBookmarks"] | true;
  return true;
}

void BookOrbitConfig::ensureLoaded() {
  if (loaded) return;
  loaded = true;
  loadFromFile();  // silently no-ops when the file does not exist yet
}

void BookOrbitConfig::setCredentials(const std::string& user, const std::string& pass) {
  username = user;
  password = pass;
}

std::string BookOrbitConfig::getMd5Password() const {
  if (password.empty()) return "";
  MD5Builder md5;
  md5.begin();
  md5.add(password.c_str());
  md5.calculate();
  return md5.toString().c_str();
}

bool BookOrbitConfig::hasCredentials() const { return !username.empty() && !password.empty(); }

void BookOrbitConfig::clearCredentials() {
  username.clear();
  password.clear();
  saveToFile();
}

void BookOrbitConfig::setServerUrl(const std::string& url) { serverUrl = url; }

std::string BookOrbitConfig::getBaseUrl() const {
  if (serverUrl.empty()) return "";

  std::string url = serverUrl.find("://") == std::string::npos ? "https://" + serverUrl : serverUrl;
  while (!url.empty() && url.back() == '/') {
    url.pop_back();
  }
  return url;
}

std::string BookOrbitConfig::getKoreaderBaseUrl() const {
  const std::string base = getBaseUrl();
  return base.empty() ? "" : base + "/api/v1/koreader";
}

void BookOrbitConfig::setMatchMethod(BookOrbitMatchMethod method) { matchMethod = method; }

void BookOrbitConfig::setSyncProgress(bool enabled) { syncProgress = enabled; }
void BookOrbitConfig::setSyncSessions(bool enabled) { syncSessions = enabled; }
void BookOrbitConfig::setSyncHighlights(bool enabled) { syncHighlights = enabled; }
void BookOrbitConfig::setSyncBookmarks(bool enabled) { syncBookmarks = enabled; }
