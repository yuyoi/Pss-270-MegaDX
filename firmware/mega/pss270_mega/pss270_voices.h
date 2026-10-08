// Yamaha PSS-270 voice list (service manual p.2). Index = displayed voice number 00-99.
// dbl = 1 for voices marked * in the manual (double-sounding: uses 2 OPLL channels).
#pragma once
#include <stdint.h>

struct Pss270Voice { const char *name; uint8_t dbl; };

static const Pss270Voice PSS270_VOICES[100] = {
  {"Piano 1",0},{"Piano 2",0},{"Honky-Tonk Piano",1},{"Electric Piano 1",0},{"Electric Piano 2",0},
  {"Harpsichord 1",0},{"Harpsichord 2",0},{"Harpsichord 3",0},{"Honky-Tonk Clavi",1},{"Glass Celesta",0},
  {"Reed Organ",0},{"Pipe Organ 1",1},{"Pipe Organ 2",0},{"Electronic Organ 1",0},{"Electronic Organ 2",1},
  {"Jazz Organ",0},{"Accordion",1},{"Vibraphone",0},{"Marimba 1",0},{"Marimba 2",0},
  {"Trumpet",0},{"Mute Trumpet",0},{"Trombone",0},{"Soft Trombone",0},{"Horn",0},
  {"Alpenhorn",0},{"Tuba",0},{"Brass Ensemble 1",1},{"Brass Ensemble 2",1},{"Brass Ensemble 3",1},
  {"Flute",0},{"Panflute",0},{"Piccolo",0},{"Clarinet",0},{"Bass Clarinet",0},
  {"Oboe",0},{"Bassoon",0},{"Saxophone",0},{"Bagpipe",0},{"Woodwinds",1},
  {"Violin 1",0},{"Violin 2",0},{"Cello",0},{"Strings",1},{"Electric Bass",0},
  {"Slap Bass",0},{"Wood Bass",0},{"Synth Bass",0},{"Banjo",0},{"Mandolin",1},
  {"Classic Guitar",0},{"Jazz Guitar",0},{"Folk Guitar",0},{"Hawaiian Guitar",0},{"Ukulele",0},
  {"Koto",0},{"Shamisen",0},{"Harp",0},{"Harmonica",0},{"Music Box",0},
  {"Brass & Marimba",0},{"Flute & Harpsichord",1},{"Oboe & Vibraphone",1},{"Clarinet & Harp",1},{"Violin & Steel Drum",1},
  {"Handsaw",0},{"Synth Brass",0},{"Metallic Synth",0},{"Sine Wave",0},{"Reverse",0},
  {"Human Voice 1",0},{"Human Voice 2",0},{"Human Voice 3",0},{"Whisper",0},{"Whistle",0},
  {"Gurgle",0},{"Bubble",0},{"Raindrop",0},{"Popcorn",0},{"Drip",0},
  {"Dog Pianist",1},{"Duck",0},{"Baby Doll",0},{"Telephone Bell",1},{"Emergency Alarm",1},
  {"Leaf Spring",0},{"Comet",0},{"Fireworks",0},{"Crystal",0},{"Ghost",0},
  {"Hand Bell",0},{"Chimes",0},{"Bell",0},{"Steel Drum",0},{"Cowbell",0},
  {"Synth Tom 1",0},{"Synth Tom 2",0},{"Snare Drum",0},{"Machine Gun",0},{"Wave",0},
};
