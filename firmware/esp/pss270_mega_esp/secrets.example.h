// Copy this file to secrets.h and put your own WiFi networks in it. secrets.h is git-ignored.
// Optional: you can also add networks later on the /wifi page, so this file is only a fallback.
// (struct Net is declared in pss270_mega_esp.ino before this file is included.)
const Net NETS[] = {
  {"YourHomeWiFi",  "your-password"},
  {"YourOtherWiFi", "its-password"},
};
