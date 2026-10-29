#pragma once

#include <bitset>
#include <cstdint>

namespace stasis {

// Held / latched MIDI notes, shared by the FOCUS engines.
// LATCH keeps the last chord after release; on legato chord changes it follows
// what is still held, so overlapping chords never pile up.
class NoteSet {
public:
    void setLatch(bool latch) { latch_ = latch; }

    void noteOn(int note) {
        if (note < 0 || note > 127) return;
        if (held_.none()) latched_.reset();
        held_.set(size_t(note));
        latched_.set(size_t(note));
    }
    void noteOff(int note) {
        if (note < 0 || note > 127) return;
        held_.reset(size_t(note));
        if (held_.any()) latched_ = held_;
    }

    const std::bitset<128>& active() const { return latch_ ? latched_ : held_; }
    bool empty() const { return active().none(); }

    // Bit n = pitch class n (0 = C).
    uint16_t pitchClasses() const {
        uint16_t mask = 0;
        for (int note = 0; note < 128; ++note)
            if (active().test(size_t(note))) mask |= uint16_t(1u << (note % 12));
        return mask;
    }

private:
    std::bitset<128> held_, latched_;
    bool latch_ = true;
};

}  // namespace stasis
