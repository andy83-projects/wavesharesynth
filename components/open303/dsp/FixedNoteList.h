#pragma once

#include "rosic_MidiNoteEvent.h"

namespace rosic {
class FixedNoteList {
public:
  bool empty() const { return count == 0; }
  void clear() { count = 0; }
  MidiNoteEvent &front() { return notes[0]; }
  void remove(const MidiNoteEvent &note) {
    unsigned dst = 0;
    for(unsigned i = 0; i < count; ++i)
      if(!(notes[i] == note)) notes[dst++] = notes[i];
    count = dst;
  }
  void push_front(const MidiNoteEvent &note) {
    remove(note);
    if(count == 16) --count;
    for(unsigned i = count; i > 0; --i) notes[i] = notes[i - 1];
    notes[0] = note;
    ++count;
  }
private:
  MidiNoteEvent notes[16];
  unsigned count = 0;
};
}
