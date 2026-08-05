#pragma once
#include <Epub.h>

#include <cstdint>
#include <memory>
#include <string>

/**
 * Reader-facing capture helpers. These are the only BookOrbit calls that live inside the
 * upstream reader, and each is a single line at an existing event site. All the real work
 * (config load, hash, xpath resolution, buffering) happens here so the upstream files stay
 * almost untouched.
 *
 * Every function is a no-op unless BookOrbit is configured (URL + login) and the relevant
 * feature toggle is on.
 */
namespace BookOrbitCapture {

// Load the BookOrbit config and the pending buffers from the SD card once. Called internally
// by the capture functions; the sync activity also calls it before flushing (after a reboot).
void ensureLoaded();

// Buffer a completed reading session (called from the reader's onExit stats commit).
// startProgressPercent/endProgressPercent are book progress 0-100.
void captureSession(const std::string& epubPath, uint32_t durationSeconds, float startProgressPercent,
                    float endProgressPercent);

// Buffer a clipping as a highlight. Resolves pos0/pos1 while the epub is loaded.
void captureClipping(const std::shared_ptr<Epub>& epub, int spineIndex, uint16_t paragraphIndex,
                     const std::string& text, const std::string& chapter);

// Buffer a bookmark. Resolves the xpath position while the epub is loaded.
void captureBookmark(const std::shared_ptr<Epub>& epub, int spineIndex, uint16_t paragraphIndex, float intraSpineProgress,
                     const std::string& chapter, const std::string& snippet);

}  // namespace BookOrbitCapture
