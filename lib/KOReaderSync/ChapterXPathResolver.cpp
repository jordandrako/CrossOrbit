#include "ChapterXPathResolver.h"

#include <Logging.h>
#include <Print.h>
#include <Utf8.h>
#include <XmlParserUtils.h>
#include <expat.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {
std::string stripPrefix(const XML_Char* name) {
  if (!name) {
    return "";
  }

  const char* local = std::strrchr(name, ':');
  return local ? std::string(local + 1) : std::string(name);
}

struct NameCounter {
  std::string name;
  int count;
};

struct ParentState {
  std::vector<NameCounter> children;

  int nextIndex(const std::string& name) {
    for (auto& child : children) {
      if (child.name == name) {
        child.count++;
        return child.count;
      }
    }

    children.push_back({name, 1});
    return 1;
  }
};

struct PathSegment {
  std::string name;
  int index;
};

std::string buildParagraphXPath(const int spineIndex, const std::vector<PathSegment>& path, const int textNodeIndex,
                                const size_t charOffset) {
  std::string xpath = "/body/DocFragment[" + std::to_string(spineIndex + 1) + "]/body";
  for (const auto& segment : path) {
    xpath += "/" + segment.name + "[" + std::to_string(segment.index) + "]";
  }
  if (textNodeIndex > 0 && charOffset > 0) {
    xpath += "/text()[" + std::to_string(textNodeIndex) + "]." + std::to_string(charOffset);
  }
  return xpath;
}

size_t countUtf8Codepoints(const XML_Char* data, const int len) {
  if (!data || len <= 0) {
    return 0;
  }

  size_t count = 0;
  const unsigned char* ptr = reinterpret_cast<const unsigned char*>(data);
  const unsigned char* end = ptr + len;
  while (ptr < end) {
    utf8NextCodepoint(&ptr);
    count++;
  }

  return count;
}

bool isNonVisibleTextTag(const std::string& name) {
  return name == "head" || name == "style" || name == "script" || name == "title" || name == "rp" || name == "rt";
}

// Inline (non-word-breaking) elements. Text runs across these without a word break, so
// mid-word markup like <i> inside a word stays one token; any other element is treated as a
// block boundary that ends the current word.
bool isInlineTag(const std::string& name) {
  return name == "a" || name == "span" || name == "i" || name == "b" || name == "em" || name == "strong" ||
         name == "sup" || name == "sub" || name == "small" || name == "u" || name == "s" || name == "strike" ||
         name == "code" || name == "mark" || name == "abbr" || name == "cite" || name == "q" || name == "var" ||
         name == "kbd" || name == "samp" || name == "big" || name == "tt" || name == "font" || name == "bdi" ||
         name == "bdo" || name == "wbr" || name == "time" || name == "label" || name == "ins" || name == "del";
}

class ParagraphTextCounter final : public Print {
 public:
  ParagraphTextCounter() {
    parser = XML_ParserCreate(nullptr);
    if (!parser) {
      LOG_ERR("KOX", "Failed to create XML parser");
      return;
    }

    XML_SetUserData(parser, this);
    XML_SetElementHandler(parser, &ParagraphTextCounter::startElement, &ParagraphTextCounter::endElement);
    XML_SetCharacterDataHandler(parser, &ParagraphTextCounter::characterData);
  }

  ~ParagraphTextCounter() override { destroyXmlParser(parser); }

  bool ok() const { return parser != nullptr && parseOk; }

  bool finish() {
    if (!parser || !parseOk || stopped) {
      return parseOk;
    }

    if (XML_Parse(parser, "", 0, XML_TRUE) == XML_STATUS_ERROR) {
      LOG_ERR("KOX", "Final XML parse error: %s", XML_ErrorString(XML_GetErrorCode(parser)));
      parseOk = false;
    }
    return parseOk;
  }

  size_t write(uint8_t c) override { return write(&c, 1); }

  size_t write(const uint8_t* buffer, size_t size) override {
    if (!parser || !parseOk || stopped) {
      return size;
    }

    if (XML_Parse(parser, reinterpret_cast<const char*>(buffer), static_cast<int>(size), XML_FALSE) != XML_STATUS_OK) {
      const enum XML_Error error = XML_GetErrorCode(parser);
      if (error != XML_ERROR_ABORTED) {
        LOG_ERR("KOX", "XML parse error: %s", XML_ErrorString(error));
        parseOk = false;
      }
    }

    return size;
  }

  size_t totalVisibleChars() const { return visibleChars; }

 private:
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char**) {
    auto* self = static_cast<ParagraphTextCounter*>(userData);
    self->onStartElement(name);
  }

  static void XMLCALL endElement(void* userData, const XML_Char* name) {
    auto* self = static_cast<ParagraphTextCounter*>(userData);
    self->onEndElement(name);
  }

  static void XMLCALL characterData(void* userData, const XML_Char* data, const int len) {
    auto* self = static_cast<ParagraphTextCounter*>(userData);
    self->onCharacterData(data, len);
  }

  void onStartElement(const XML_Char* rawName) {
    const std::string name = stripPrefix(rawName);

    if (!insideBody) {
      if (name == "body") {
        insideBody = true;
        bodyDepth = depth;
      }
      depth++;
      return;
    }

    if (isNonVisibleTextTag(name)) nonVisibleDepth++;
    depth++;
  }

  void onEndElement(const XML_Char* rawName) {
    const std::string name = stripPrefix(rawName);

    depth--;
    if (!insideBody) {
      return;
    }

    if (depth == bodyDepth && name == "body") {
      insideBody = false;
      return;
    }

    if (isNonVisibleTextTag(name) && nonVisibleDepth > 0) nonVisibleDepth--;
  }

  void onCharacterData(const XML_Char* data, const int len) {
    if (!insideBody || nonVisibleDepth > 0 || len <= 0) {
      return;
    }

    visibleChars += countUtf8Codepoints(data, len);
  }

 private:
  XML_Parser parser = nullptr;
  bool parseOk = true;
  bool insideBody = false;
  bool stopped = false;
  int depth = 0;
  int bodyDepth = -1;
  int nonVisibleDepth = 0;
  size_t visibleChars = 0;
};

class XPathElementResolver final : public Print {
 public:
  XPathElementResolver(const int targetElement, const char* targetTag)
      : targetElement(targetElement), targetTag(targetTag) {
    parser = XML_ParserCreate(nullptr);
    if (!parser) {
      LOG_ERR("KOX", "Failed to create XML parser");
      return;
    }

    XML_SetUserData(parser, this);
    XML_SetElementHandler(parser, &XPathElementResolver::startElement, &XPathElementResolver::endElement);
  }

  ~XPathElementResolver() override { destroyXmlParser(parser); }

  bool ok() const { return parser != nullptr && parseOk; }

  bool finish() {
    if (!parser || !parseOk || stopped) {
      return parseOk;
    }

    if (XML_Parse(parser, "", 0, XML_TRUE) == XML_STATUS_ERROR) {
      LOG_ERR("KOX", "Final XML parse error: %s", XML_ErrorString(XML_GetErrorCode(parser)));
      parseOk = false;
    }
    return parseOk;
  }

  bool hasMatch() const { return !xpath.empty(); }
  const std::string& getXPath() const { return xpath; }

  size_t write(uint8_t c) override { return write(&c, 1); }

  size_t write(const uint8_t* buffer, size_t size) override {
    if (!parser || !parseOk || stopped) {
      return size;
    }

    if (XML_Parse(parser, reinterpret_cast<const char*>(buffer), static_cast<int>(size), XML_FALSE) != XML_STATUS_OK) {
      const enum XML_Error error = XML_GetErrorCode(parser);
      if (error != XML_ERROR_ABORTED) {
        LOG_ERR("KOX", "XML parse error: %s", XML_ErrorString(error));
        parseOk = false;
      }
    }

    return size;
  }

  int spineIndex = 0;

 private:
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char**) {
    auto* self = static_cast<XPathElementResolver*>(userData);
    self->onStartElement(name);
  }

  static void XMLCALL endElement(void* userData, const XML_Char* name) {
    auto* self = static_cast<XPathElementResolver*>(userData);
    self->onEndElement(name);
  }

  void onStartElement(const XML_Char* rawName) {
    const std::string name = stripPrefix(rawName);

    if (!insideBody) {
      if (name == "body") {
        insideBody = true;
        bodyDepth = depth;
        parentStates.emplace_back();
      }
      depth++;
      return;
    }

    const int siblingIndex = parentStates.back().nextIndex(name);
    path.push_back({name, siblingIndex});
    parentStates.emplace_back();

    if (name == targetTag) {
      elementCount++;
      if (elementCount == targetElement) {
        xpath = buildParagraphXPath(spineIndex, path, 0, 0);
        stopped = true;
        XML_StopParser(parser, XML_FALSE);
      }
    }

    depth++;
  }

  void onEndElement(const XML_Char* rawName) {
    const std::string name = stripPrefix(rawName);

    depth--;
    if (!insideBody) {
      return;
    }

    if (depth == bodyDepth && name == "body") {
      insideBody = false;
      parentStates.clear();
      path.clear();
      return;
    }

    if (!path.empty()) {
      path.pop_back();
    }
    if (!parentStates.empty()) {
      parentStates.pop_back();
    }
  }

  XML_Parser parser = nullptr;
  const int targetElement;
  const char* targetTag;
  bool parseOk = true;
  bool insideBody = false;
  bool stopped = false;
  int depth = 0;
  int bodyDepth = -1;
  int elementCount = 0;
  std::vector<ParentState> parentStates;
  std::vector<PathSegment> path;
  std::string xpath;
};

class XPathProgressResolver final : public Print {
 public:
  explicit XPathProgressResolver(const size_t targetVisibleChar) : targetVisibleChar(targetVisibleChar) {
    parser = XML_ParserCreate(nullptr);
    if (!parser) {
      LOG_ERR("KOX", "Failed to create XML parser");
      return;
    }

    XML_SetUserData(parser, this);
    XML_SetElementHandler(parser, &XPathProgressResolver::startElement, &XPathProgressResolver::endElement);
    XML_SetCharacterDataHandler(parser, &XPathProgressResolver::characterData);
  }

  ~XPathProgressResolver() override { destroyXmlParser(parser); }

  bool ok() const { return parser != nullptr && parseOk; }

  bool finish() {
    if (!parser || !parseOk || stopped) {
      return parseOk;
    }

    if (XML_Parse(parser, "", 0, XML_TRUE) == XML_STATUS_ERROR) {
      LOG_ERR("KOX", "Final XML parse error: %s", XML_ErrorString(XML_GetErrorCode(parser)));
      parseOk = false;
    }
    return parseOk;
  }

  bool hasMatch() const { return !xpath.empty(); }
  const std::string& getXPath() const { return xpath; }

  size_t write(uint8_t c) override { return write(&c, 1); }

  size_t write(const uint8_t* buffer, size_t size) override {
    if (!parser || !parseOk || stopped) {
      return size;
    }

    if (XML_Parse(parser, reinterpret_cast<const char*>(buffer), static_cast<int>(size), XML_FALSE) != XML_STATUS_OK) {
      const enum XML_Error error = XML_GetErrorCode(parser);
      if (error != XML_ERROR_ABORTED) {
        LOG_ERR("KOX", "XML parse error: %s", XML_ErrorString(error));
        parseOk = false;
      }
    }

    return size;
  }

  int spineIndex = 0;

 private:
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char**) {
    auto* self = static_cast<XPathProgressResolver*>(userData);
    self->onStartElement(name);
  }

  static void XMLCALL endElement(void* userData, const XML_Char* name) {
    auto* self = static_cast<XPathProgressResolver*>(userData);
    self->onEndElement(name);
  }

  static void XMLCALL characterData(void* userData, const XML_Char* data, const int len) {
    auto* self = static_cast<XPathProgressResolver*>(userData);
    self->onCharacterData(data, len);
  }

  void onStartElement(const XML_Char* rawName) {
    const std::string name = stripPrefix(rawName);

    if (!insideBody) {
      if (name == "body") {
        insideBody = true;
        bodyDepth = depth;
        parentStates.emplace_back();
      }
      depth++;
      return;
    }

    const int siblingIndex = parentStates.back().nextIndex(name);
    path.push_back({name, siblingIndex});
    parentStates.emplace_back();
    textNodeIndexStack.push_back(0);
    pendingTextNode = true;

    if (name == "p") {
      paragraphDepth++;
    }
    if (name == "li") {
      liDepth++;
    }
    if (isNonVisibleTextTag(name)) nonVisibleDepth++;

    depth++;
  }

  void onEndElement(const XML_Char* rawName) {
    const std::string name = stripPrefix(rawName);

    depth--;
    if (!insideBody) {
      return;
    }

    if (depth == bodyDepth && name == "body") {
      insideBody = false;
      parentStates.clear();
      path.clear();
      textNodeIndexStack.clear();
      return;
    }

    if (name == "p" && paragraphDepth > 0) {
      paragraphDepth--;
    }
    if (name == "li" && liDepth > 0) {
      liDepth--;
    }
    if (isNonVisibleTextTag(name) && nonVisibleDepth > 0) nonVisibleDepth--;

    if (!textNodeIndexStack.empty()) {
      textNodeIndexStack.pop_back();
    }
    if (paragraphDepth > 0 || liDepth > 0) {
      pendingTextNode = true;
    }
    if (!path.empty()) {
      path.pop_back();
    }
    if (!parentStates.empty()) {
      parentStates.pop_back();
    }
  }

  void onCharacterData(const XML_Char* data, const int len) {
    if (!insideBody || nonVisibleDepth > 0 || len <= 0 || stopped) {
      return;
    }

    const size_t codepointCount = countUtf8Codepoints(data, len);
    if (codepointCount == 0) {
      return;
    }

    // Start a new text node on first non-empty content after any element boundary.
    // Only counting non-empty nodes matches KOReader's text()[N] indexing behavior,
    // which skips empty text nodes created by bare <a id="anchor"/> anchors.
    if (pendingTextNode) {
      if (!textNodeIndexStack.empty()) {
        textNodeIndexStack.back()++;
      }
      textNodeStartChars = visibleChars;
      pendingTextNode = false;
    }

    const size_t nextVisibleChars = visibleChars + codepointCount;
    if (targetVisibleChar <= nextVisibleChars) {
      const size_t delta = targetVisibleChar - visibleChars;
      const int texNode = textNodeIndexStack.empty() ? 0 : textNodeIndexStack.back();
      const size_t charOff = visibleChars - textNodeStartChars + delta;
      xpath = buildParagraphXPath(spineIndex, path, texNode, charOff);
      stopped = true;
      XML_StopParser(parser, XML_FALSE);
      return;
    }

    visibleChars = nextVisibleChars;
  }

  XML_Parser parser = nullptr;
  const size_t targetVisibleChar;
  bool parseOk = true;
  bool insideBody = false;
  bool stopped = false;
  bool pendingTextNode = true;
  int depth = 0;
  int bodyDepth = -1;
  int paragraphDepth = 0;
  int liDepth = 0;
  int nonVisibleDepth = 0;
  size_t visibleChars = 0;
  size_t textNodeStartChars = 0;
  std::vector<int> textNodeIndexStack;
  std::vector<ParentState> parentStates;
  std::vector<PathSegment> path;
  std::string xpath;
};

std::string findXPathForElement(const std::shared_ptr<Epub>& epub, const int spineIndex, const uint16_t elementIndex,
                                const char* tagName) {
  if (!epub || elementIndex == 0 || spineIndex < 0 || spineIndex >= epub->getSpineItemsCount()) {
    return "";
  }

  const auto href = epub->getSpineItem(spineIndex).href;
  if (href.empty()) {
    return "";
  }

  XPathElementResolver resolver(elementIndex, tagName);
  if (!resolver.ok()) {
    return "";
  }

  resolver.spineIndex = spineIndex;
  if (!epub->readItemContentsToStream(href, resolver, 1024) || !resolver.finish()) {
    return "";
  }

  if (resolver.hasMatch()) {
    LOG_DBG("KOX", "Resolved %s %u in spine %d -> %s", tagName, elementIndex, spineIndex, resolver.getXPath().c_str());
    return resolver.getXPath();
  }

  LOG_DBG("KOX", "%s %u not found in spine %d", tagName, elementIndex, spineIndex);
  return "";
}

std::vector<std::string> splitIntoWords(const std::string& text) {
  std::vector<std::string> words;
  std::string current;
  for (const char c : text) {
    if (static_cast<unsigned char>(c) <= 0x20) {
      if (!current.empty()) {
        words.push_back(current);
        current.clear();
      }
    } else {
      current.push_back(c);
    }
  }
  if (!current.empty()) words.push_back(current);
  return words;
}

// Reduce a word to a form that compares equal across the two sources of the same text: the
// rendered clip (from the laid-out page) and the source XHTML (via HighlightSequenceFinder).
// These differ only by typography in practice, so we lowercase ASCII, fold curly quotes and
// en/em dashes to ASCII, drop soft hyphens and non-breaking spaces, and trim edge punctuation.
// An exact byte compare would otherwise drop the whole clip over one smart quote.
std::string normalizeWord(const std::string& word) {
  std::string out;
  out.reserve(word.size());
  const unsigned char* p = reinterpret_cast<const unsigned char*>(word.data());
  const unsigned char* end = p + word.size();
  while (p < end) {
    const unsigned char lead = *p;
    if (lead < 0x80) {
      unsigned char c = lead;
      if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c - 'A' + 'a');
      out.push_back(static_cast<char>(c));
      p++;
      continue;
    }
    // Decode one multi-byte UTF-8 codepoint to fold known typographic characters.
    const unsigned char* cpStart = p;
    int extra = (lead & 0xE0) == 0xC0 ? 1 : (lead & 0xF0) == 0xE0 ? 2 : (lead & 0xF8) == 0xF0 ? 3 : -1;
    if (extra < 0) {
      p++;  // invalid lead byte; skip it
      continue;
    }
    uint32_t cp = lead & (0x7F >> (extra + 1));
    bool valid = true;
    for (int k = 1; k <= extra; k++) {
      if (cpStart + k >= end || (cpStart[k] & 0xC0) != 0x80) {
        valid = false;
        break;
      }
      cp = (cp << 6) | (cpStart[k] & 0x3F);
    }
    p = cpStart + extra + 1;
    if (!valid) continue;
    switch (cp) {
      case 0x2018:
      case 0x2019:
        out.push_back('\'');
        break;  // curly single quotes / apostrophe
      case 0x201C:
      case 0x201D:
        out.push_back('"');
        break;  // curly double quotes
      case 0x2013:
      case 0x2014:
        out.push_back('-');
        break;      // en / em dash
      case 0x00AD:  // soft hyphen
      case 0x00A0:  // non-breaking space
        break;      // drop
      default:
        out.append(reinterpret_cast<const char*>(cpStart), static_cast<size_t>(extra + 1));
        break;
    }
  }
  // Trim leading/trailing non-alphanumeric ASCII (attached punctuation differs between sources).
  auto isCore = [](const unsigned char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch >= 0x80;
  };
  size_t b = 0;
  size_t e = out.size();
  while (b < e && !isCore(static_cast<unsigned char>(out[b]))) b++;
  while (e > b && !isCore(static_cast<unsigned char>(out[e - 1]))) e--;
  return out.substr(b, e - b);
}

// Finds a normalized word sequence anywhere in a spine document's <p>/<li> text, in the same
// cumulative visible-char space XPathProgressResolver uses, while holding only a sliding window
// of the target's length in memory. Unlike a single-paragraph match this tolerates the reader's
// paragraph numbering disagreeing with the source XHTML's <p> numbering (e.g. stage-direction
// blocks that one side counts and the other does not), and a missing paragraph anchor.
class HighlightSequenceFinder final : public Print {
 public:
  explicit HighlightSequenceFinder(const std::vector<std::string>& targetNorm) : target(targetNorm) {
    parser = XML_ParserCreate(nullptr);
    if (!parser) {
      LOG_ERR("KOX", "Failed to create XML parser");
      return;
    }
    XML_SetUserData(parser, this);
    XML_SetElementHandler(parser, &HighlightSequenceFinder::startElement, &HighlightSequenceFinder::endElement);
    XML_SetCharacterDataHandler(parser, &HighlightSequenceFinder::characterData);
  }

  ~HighlightSequenceFinder() override { destroyXmlParser(parser); }

  bool ok() const { return parser != nullptr && parseOk; }

  bool finish() {
    if (!parser || !parseOk || stopped) return parseOk;
    if (XML_Parse(parser, "", 0, XML_TRUE) == XML_STATUS_ERROR) {
      LOG_ERR("KOX", "Final XML parse error: %s", XML_ErrorString(XML_GetErrorCode(parser)));
      parseOk = false;
    }
    return parseOk;
  }

  size_t write(uint8_t c) override { return write(&c, 1); }

  size_t write(const uint8_t* buffer, size_t size) override {
    if (!parser || !parseOk || stopped) return size;
    if (XML_Parse(parser, reinterpret_cast<const char*>(buffer), static_cast<int>(size), XML_FALSE) != XML_STATUS_OK) {
      const enum XML_Error error = XML_GetErrorCode(parser);
      if (error != XML_ERROR_ABORTED) {
        LOG_ERR("KOX", "XML parse error: %s", XML_ErrorString(error));
        parseOk = false;
      }
    }
    return size;
  }

  bool matched() const { return found; }
  size_t startChar() const { return matchStartVisible; }
  size_t endChar() const { return matchEndVisible; }

 private:
  struct WindowWord {
    std::string norm;
    size_t start;
    size_t end;
  };

  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char**) {
    static_cast<HighlightSequenceFinder*>(userData)->onStartElement(name);
  }
  static void XMLCALL endElement(void* userData, const XML_Char* name) {
    static_cast<HighlightSequenceFinder*>(userData)->onEndElement(name);
  }
  static void XMLCALL characterData(void* userData, const XML_Char* data, const int len) {
    static_cast<HighlightSequenceFinder*>(userData)->onCharacterData(data, len);
  }

  void onStartElement(const XML_Char* rawName) {
    const std::string name = stripPrefix(rawName);
    if (!insideBody) {
      if (name == "body") {
        insideBody = true;
        bodyDepth = depth;
      }
      depth++;
      return;
    }
    if (isNonVisibleTextTag(name)) nonVisibleDepth++;
    if (!isInlineTag(name)) flushWord();  // block boundary ends the current word
    depth++;
  }

  void onEndElement(const XML_Char* rawName) {
    const std::string name = stripPrefix(rawName);
    depth--;
    if (!insideBody) return;
    if (depth == bodyDepth && name == "body") {
      flushWord();
      insideBody = false;
      return;
    }
    if (isNonVisibleTextTag(name) && nonVisibleDepth > 0) nonVisibleDepth--;
    if (!isInlineTag(name)) flushWord();  // block boundary ends the current word
  }

  void onCharacterData(const XML_Char* data, const int len) {
    // Count exactly the codepoints XPathProgressResolver counts (all visible body text, including
    // inter-element whitespace) so the offsets fed back to it resolve to the right position.
    if (!insideBody || nonVisibleDepth > 0 || len <= 0 || stopped) return;
    const unsigned char* ptr = reinterpret_cast<const unsigned char*>(data);
    const unsigned char* end = ptr + len;
    while (ptr < end) {
      const unsigned char* cpStart = ptr;
      utf8NextCodepoint(&ptr);
      const bool isWhitespace = (ptr - cpStart == 1) && (*cpStart <= 0x20);
      if (isWhitespace) {
        flushWord();
      } else {
        if (!inWord) {
          inWord = true;
          wordStart = visibleChars;
          wordBuf.clear();
        }
        wordBuf.append(reinterpret_cast<const char*>(cpStart), static_cast<size_t>(ptr - cpStart));
      }
      visibleChars++;
    }
  }

  void flushWord() {
    if (!inWord) return;
    const size_t s = wordStart;
    const size_t e = visibleChars;
    const std::string norm = normalizeWord(wordBuf);
    inWord = false;
    wordBuf.clear();
    // Skip pure-punctuation tokens so a stray dash on one side does not break alignment.
    if (found || target.empty() || norm.empty()) return;

    window.push_back({norm, s, e});
    if (window.size() > target.size()) window.erase(window.begin());
    if (window.size() < target.size()) return;
    for (size_t k = 0; k < target.size(); k++) {
      if (window[k].norm != target[k]) return;
    }
    found = true;
    matchStartVisible = window.front().start;
    matchEndVisible = window.back().end;
    stopped = true;
    XML_StopParser(parser, XML_FALSE);
  }

  XML_Parser parser = nullptr;
  const std::vector<std::string>& target;
  bool parseOk = true;
  bool insideBody = false;
  bool stopped = false;
  bool found = false;
  int depth = 0;
  int bodyDepth = -1;
  int nonVisibleDepth = 0;
  size_t visibleChars = 0;
  bool inWord = false;
  size_t wordStart = 0;
  std::string wordBuf;
  std::vector<WindowWord> window;
  size_t matchStartVisible = 0;
  size_t matchEndVisible = 0;
};

// Runs XPathProgressResolver at an absolute visible-char target (no [1,total] clamp) so a
// highlight boundary at char 0 or at end-of-paragraph resolves correctly.
std::string resolveXPathAtVisibleChar(const std::shared_ptr<Epub>& epub, const std::string& href, const int spineIndex,
                                      const size_t targetChar) {
  XPathProgressResolver resolver(targetChar);
  if (!resolver.ok()) return "";
  resolver.spineIndex = spineIndex;
  if (!epub->readItemContentsToStream(href, resolver, 1024) || !resolver.finish()) return "";
  return resolver.hasMatch() ? resolver.getXPath() : "";
}
}  // namespace

std::string ChapterXPathResolver::findXPathForParagraph(const std::shared_ptr<Epub>& epub, const int spineIndex,
                                                        const uint16_t paragraphIndex) {
  return findXPathForElement(epub, spineIndex, paragraphIndex, "p");
}

std::string ChapterXPathResolver::findXPathForListItem(const std::shared_ptr<Epub>& epub, const int spineIndex,
                                                       const uint16_t listItemIndex) {
  return findXPathForElement(epub, spineIndex, listItemIndex, "li");
}

std::string ChapterXPathResolver::findXPathForProgress(const std::shared_ptr<Epub>& epub, const int spineIndex,
                                                       const float intraSpineProgress) {
  if (!epub || spineIndex < 0 || spineIndex >= epub->getSpineItemsCount()) {
    return "";
  }

  const auto href = epub->getSpineItem(spineIndex).href;
  if (href.empty()) {
    return "";
  }

  if (!(intraSpineProgress > 0.0f)) {
    return "/body/DocFragment[" + std::to_string(spineIndex + 1) + "]/body";
  }

  ParagraphTextCounter counter;
  if (!counter.ok() || !epub->readItemContentsToStream(href, counter, 1024) || !counter.finish()) {
    return "";
  }

  const size_t totalVisibleChars = counter.totalVisibleChars();
  if (totalVisibleChars == 0) {
    return "";
  }

  const float clamped = std::max(0.0f, std::min(1.0f, intraSpineProgress));
  const size_t targetVisibleChar =
      std::max<size_t>(1, std::min(totalVisibleChars, static_cast<size_t>(std::ceil(clamped * totalVisibleChars))));

  XPathProgressResolver resolver(targetVisibleChar);
  if (!resolver.ok()) {
    return "";
  }

  resolver.spineIndex = spineIndex;
  if (!epub->readItemContentsToStream(href, resolver, 1024) || !resolver.finish()) {
    return "";
  }

  if (resolver.hasMatch()) {
    LOG_DBG("KOX", "Resolved progress %.3f in spine %d -> %s", intraSpineProgress, spineIndex,
            resolver.getXPath().c_str());
    return resolver.getXPath();
  }

  LOG_DBG("KOX", "Could not resolve progress %.3f in spine %d", intraSpineProgress, spineIndex);
  return "";
}

std::string ChapterXPathResolver::findXPathForVisibleTextOffset(const std::shared_ptr<Epub>& epub, const int spineIndex,
                                                                const uint32_t visibleTextOffset) {
  if (!epub || spineIndex < 0 || spineIndex >= epub->getSpineItemsCount()) {
    return "";
  }

  const auto href = epub->getSpineItem(spineIndex).href;
  if (href.empty()) {
    return "";
  }

  // XPath text offsets are one-based, while the cache records the first
  // visible codepoint on a page with a zero-based offset.
  const size_t targetVisibleChar = static_cast<size_t>(visibleTextOffset) + 1;
  XPathProgressResolver resolver(targetVisibleChar);
  if (!resolver.ok()) {
    return "";
  }
  resolver.spineIndex = spineIndex;
  if (!epub->readItemContentsToStream(href, resolver, 1024) || !resolver.finish()) {
    return "";
  }
  if (!resolver.hasMatch()) {
    LOG_DBG("KOX", "Could not resolve visible offset %lu in spine %d", static_cast<unsigned long>(visibleTextOffset),
            spineIndex);
    return "";
  }
  return resolver.getXPath();
}

bool ChapterXPathResolver::findHighlightXPathRange(const std::shared_ptr<Epub>& epub, const int spineIndex,
                                                   const uint16_t paragraphIndex, const std::string& text,
                                                   std::string& outPos0, std::string& outPos1) {
  outPos0.clear();
  outPos1.clear();
  if (!epub || spineIndex < 0 || spineIndex >= epub->getSpineItemsCount()) {
    return false;
  }

  const auto href = epub->getSpineItem(spineIndex).href;
  if (href.empty()) {
    return false;
  }

  // Normalize the clip words and drop pure-punctuation tokens, so the search tolerates typography
  // differences (smart quotes, dashes, case, attached punctuation) between the rendered clip and
  // the source XHTML. The paragraph index is only a hint for logging: the reader's paragraph
  // numbering can disagree with the source <p> numbering, so we search the whole document.
  std::vector<std::string> clipNorm;
  for (const auto& w : splitIntoWords(text)) {
    std::string n = normalizeWord(w);
    if (!n.empty()) clipNorm.push_back(std::move(n));
  }
  if (clipNorm.empty()) {
    return false;
  }

  HighlightSequenceFinder finder(clipNorm);
  if (!finder.ok() || !epub->readItemContentsToStream(href, finder, 1024) || !finder.finish()) {
    return false;
  }
  if (!finder.matched()) {
    LOG_INF("KOX", "Highlight text not found in spine %d (para hint %u, clip=%u words: '%s'..)", spineIndex,
            paragraphIndex, (unsigned)clipNorm.size(), clipNorm.front().c_str());
    return false;
  }

  outPos0 = resolveXPathAtVisibleChar(epub, href, spineIndex, finder.startChar());
  outPos1 = resolveXPathAtVisibleChar(epub, href, spineIndex, finder.endChar());
  if (outPos0.empty() || outPos1.empty()) {
    outPos0.clear();
    outPos1.clear();
    return false;
  }

  LOG_DBG("KOX", "Resolved highlight in spine %d: %s .. %s", spineIndex, outPos0.c_str(), outPos1.c_str());
  return true;
}
