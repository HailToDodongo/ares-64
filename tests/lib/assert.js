// Shared assertion helpers for ares-test scripts.
// Usage: import { assert, assertEq } from "./lib/assert.js";

export function assert(condition, message) {
  if (!condition) throw new Error(message || "assertion failed");
}

export function assertEq(actual, expected, message) {
  if (actual !== expected) {
    throw new Error((message || "assertEq failed") + ": " + actual + " !== " + expected);
  }
}

export function assertThrows(fn, message) {
  try { fn(); } catch (e) { return e; }
  throw new Error(message || "expected an exception");
}

// Snapshot helper: compare `media` (image or audio object) against a golden file.
// If the golden does not exist yet, it is created and the test passes with a note.
export function assertSnapshot(media, goldenPath, tolerance) {
  let golden;
  try {
    golden = goldenPath.endsWith(".wav") ? ares.loadAudio(goldenPath) : ares.loadImage(goldenPath);
  } catch (e) {
    media.save(goldenPath);
    console.log("snapshot: created new golden", goldenPath);
    return;
  }
  const cmp = media.compare(golden, tolerance || 0);
  if (!cmp.match) {
    throw new Error("snapshot mismatch vs " + goldenPath + ": " + JSON.stringify(cmp));
  }
}
