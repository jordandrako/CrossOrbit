#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <cstdint>
#include <string>

// Document matching method for BookOrbit sync (kept independent of the upstream KOReader
// store so this module never has to be reconciled on an upstream pull).
enum class BookOrbitMatchMethod : uint8_t {
  FILENAME = 0,  // Match by filename (works across differently-sourced copies)
  BINARY = 1,    // Match by partial MD5 of file content (exact, but files must be identical)
};

/**
 * Single BookOrbit configuration: the BookOrbit base URL, a dedicated login, the document
 * match method, and a per-feature enable toggle. Every endpoint is derived from the one URL
 * (getKoreaderBaseUrl() -> {url}/api/v1/koreader), so the user only ever enters the BookOrbit
 * address. Persisted to /.crosspoint/bookorbit.json.
 *
 * Self-loading: callers use ensureLoaded() instead of a boot-time hook, so main.cpp needs no
 * BookOrbit-specific edits.
 */
class BookOrbitConfig : public PersistableStore<BookOrbitConfig> {
 private:
  std::string serverUrl;
  std::string username;
  std::string password;
  BookOrbitMatchMethod matchMethod = BookOrbitMatchMethod::FILENAME;
  bool syncProgress = true;
  bool syncSessions = true;
  bool syncHighlights = true;
  bool syncBookmarks = true;
  bool loaded = false;

  BookOrbitConfig() = default;
  ~BookOrbitConfig() = default;

  friend class PersistableStore<BookOrbitConfig>;

 public:
  static const char* getFilePath() { return "/.crosspoint/bookorbit.json"; }

  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  // Loads the config from disk once, on first access. Lets this module avoid a boot hook.
  void ensureLoaded();

  // --- Credentials ---
  void setCredentials(const std::string& user, const std::string& pass);
  const std::string& getUsername() const { return username; }
  const std::string& getPassword() const { return password; }
  std::string getMd5Password() const;
  bool hasCredentials() const;
  void clearCredentials();

  // --- URL ---
  void setServerUrl(const std::string& url);
  const std::string& getServerUrl() const { return serverUrl; }
  // Normalized BookOrbit base URL (adds https:// when no scheme, strips trailing slashes).
  // Empty when no URL is configured.
  std::string getBaseUrl() const;
  // KOReader plugin base for all endpoints: {base}/api/v1/koreader
  std::string getKoreaderBaseUrl() const;

  bool isConfigured() const { return !getBaseUrl().empty() && hasCredentials(); }

  // --- Document match method ---
  void setMatchMethod(BookOrbitMatchMethod method);
  BookOrbitMatchMethod getMatchMethod() const { return matchMethod; }

  // --- Feature toggles ---
  bool syncProgressEnabled() const { return syncProgress; }
  bool syncSessionsEnabled() const { return syncSessions; }
  bool syncHighlightsEnabled() const { return syncHighlights; }
  bool syncBookmarksEnabled() const { return syncBookmarks; }
  void setSyncProgress(bool enabled);
  void setSyncSessions(bool enabled);
  void setSyncHighlights(bool enabled);
  void setSyncBookmarks(bool enabled);
};

// Helper macro to access the BookOrbit config
#define BOOKORBIT BookOrbitConfig::getInstance()
