// Smoke test: exercises the scripting host without needing a ROM.
// Run: ares-test tests/smoke.test.js
import { assert, assertThrows } from "./lib/assert.js";

console.log("smoke: args =", ares.args);

assert(ares.isPaused(), "emulator should start paused");

// wait() without a ROM must throw
ares.resume();
const err = assertThrows(() => ares.wait(1), "wait() without a ROM should throw");
console.log("smoke: expected error:", err.message);
ares.pause();

// loadRom with a bad path must throw, not crash
assertThrows(() => ares.loadRom("/nonexistent/rom.z64"), "loadRom of missing file should throw");

// renderer validation
assertThrows(() => ares.setRenderer("gpu-magic"), "setRenderer should reject unknown names");
ares.setRenderer("angrylion");

// loading missing media must throw
assertThrows(() => ares.loadImage("/nonexistent/golden.png"), "loadImage of missing file should throw");
assertThrows(() => ares.loadAudio("/nonexistent/golden.wav"), "loadAudio of missing file should throw");

// capture without a ROM must throw
assertThrows(() => ares.screenshot(), "screenshot() without a ROM should throw");
assertThrows(() => ares.startAudio(), "startAudio() without a ROM should throw");

// controller object shape
const p1 = ares.controller(1);
p1.hold("A").release("A").stick(0.5, -0.5).clear();
assertThrows(() => p1.hold("NotAButton"), "hold() should reject unknown buttons");

console.log("smoke: OK");
