/*
  Basic SD card music player for the Adafruit "Music Maker" MP3 Shield (#1790)
  on an Arduino Uno R3, listening through the 3.5 mm headphone jack.

  What it does
    1. Finds the VS1053 codec and plays a short test beep (proves the audio
       path to your headphones works even before the SD card is involved).
    2. Mounts the microSD card and lists every file on it.
    3. Builds a playlist of the audio files in the card's root folder and
       plays them one after another in the background (DREQ interrupt).
    4. Lets you control playback from the Serial Monitor (115200 baud):
         n = next track     p = pause / resume     s = stop
         + = louder         - = quieter            l = list playlist
         v = show current volume
       Every volume change prints the new level in dB, e.g.
         Volume: -15.0 dB  [#################...]

  Shield pins (fixed by the shield, nothing to wire by hand):
    D7 = VS1053 command chip select (MCS)
    D6 = VS1053 data chip select (DCS)
    D4 = microSD chip select (CCS)
    D3 = VS1053 DREQ (must be an interrupt pin; D3 is INT1 on the Uno)
    SPI = ICSP header (new boards) or D11/D12/D13 (older boards)

  Library: "Adafruit VS1053" (install from Library Manager, accept the
  dependency "Adafruit BusIO"). SD and SPI come with the Arduino IDE.
*/

#include <SPI.h>
#include <SD.h>
#include <Adafruit_VS1053.h>

// ---------- Shield pin map ----------
#define SHIELD_RESET  -1   // VS1053 reset is tied to the Arduino reset, unused
#define SHIELD_CS      7   // VS1053 command (SCI) chip select
#define SHIELD_DCS     6   // VS1053 data (SDI) chip select
#define CARDCS         4   // microSD chip select
#define DREQ           3   // VS1053 data request, interrupt pin

Adafruit_VS1053_FilePlayer musicPlayer =
  Adafruit_VS1053_FilePlayer(SHIELD_RESET, SHIELD_CS, SHIELD_DCS, DREQ, CARDCS);

// ---------- Playlist ----------
// The Uno has only 2 KB of RAM, so keep the playlist small.
#define MAX_TRACKS 10
char playlist[MAX_TRACKS][13];   // 8.3 names: 8 + '.' + 3 + '\0' = 13
uint8_t trackCount = 0;
int8_t  currentIndex = -1;

// ---------- Volume ----------
// VS1053 volume is ATTENUATION in 0.5 dB steps: 0 = loudest, bigger = quieter.
// So the level in dB is -(volume / 2): 0 -> 0.0 dB (max), 30 -> -15.0 dB.
// STARTING VOLUME: change this number to set how loud it is at power up.
//   20 = -10 dB (loud)   30 = -15 dB   50 = -25 dB (comfortable)   60 = -30 dB
uint8_t volume = 50;
const uint8_t VOLUME_STEP = 5;   // 5 steps = 2.5 dB per '+' or '-'
const uint8_t QUIETEST    = 200; // -100 dB, practically silent

bool userStopped = false;        // true after 's', so we don't auto advance
uint8_t failedInARow = 0;        // stops endless skipping if no file will play

// ---------- Helpers ----------
bool endsWithIgnoreCase(const char *name, const char *ext) {
  size_t n = strlen(name), e = strlen(ext);
  if (n < e) return false;
  return strcasecmp(name + n - e, ext) == 0;
}

bool isAudioFile(const char *name) {
  // Skip hidden/system files, e.g. the "._xxx" files macOS writes,
  // which the SD library shows as names starting with '_' or '~'.
  if (name[0] == '_' || name[0] == '.' || name[0] == '~') return false;
  return endsWithIgnoreCase(name, ".MP3") || endsWithIgnoreCase(name, ".OGG") ||
         endsWithIgnoreCase(name, ".WAV") || endsWithIgnoreCase(name, ".M4A") ||
         endsWithIgnoreCase(name, ".AAC") || endsWithIgnoreCase(name, ".WMA") ||
         endsWithIgnoreCase(name, ".MID");
}

void listFiles(File dir, uint8_t depth) {
  while (true) {
    File entry = dir.openNextFile();
    if (!entry) break;
    for (uint8_t i = 0; i < depth; i++) Serial.print(F("  "));
    Serial.print(entry.name());
    if (entry.isDirectory()) {
      Serial.println('/');
      listFiles(entry, depth + 1);
    } else {
      Serial.print(F("  ("));
      Serial.print(entry.size());
      Serial.println(F(" bytes)"));
    }
    entry.close();
  }
}

void buildPlaylist() {
  trackCount = 0;
  File root = SD.open("/");
  while (trackCount < MAX_TRACKS) {
    File entry = root.openNextFile();
    if (!entry) break;
    if (!entry.isDirectory() && isAudioFile(entry.name())) {
      strncpy(playlist[trackCount], entry.name(), 12);
      playlist[trackCount][12] = '\0';
      trackCount++;
    }
    entry.close();
  }
  root.close();
}

void printPlaylist() {
  Serial.print(F("Playlist ("));
  Serial.print(trackCount);
  Serial.println(F(" tracks):"));
  for (uint8_t i = 0; i < trackCount; i++) {
    Serial.print(i == currentIndex ? F(" > ") : F("   "));
    Serial.println(playlist[i]);
  }
}

void playTrack(uint8_t index) {
  if (trackCount == 0) return;
  currentIndex = index % trackCount;

  if (!musicPlayer.stopped()) musicPlayer.stopPlaying();

  Serial.print(F("Now playing: "));
  Serial.println(playlist[currentIndex]);

  userStopped = false;
  if (musicPlayer.startPlayingFile(playlist[currentIndex])) {
    failedInARow = 0;
  } else {
    Serial.println(F("  Could not open/play that file, skipping."));
    if (++failedInARow >= trackCount) {
      Serial.println(F("  No file could be played. Stopping."));
      userStopped = true;
      failedInARow = 0;
    }
  }
}

// Prints the current level in dB plus a 20 character level bar, e.g.
//   Volume: -15.0 dB  [#################...]
// 0.0 dB is the loudest the chip can go; more '#' = louder.
void printVolume() {
  Serial.print(F("Volume: "));
  if (volume > 0) Serial.print('-');
  Serial.print(volume / 2);                       // whole dB
  Serial.print((volume % 2) ? F(".5") : F(".0")); // half dB step
  Serial.print(F(" dB  ["));
  uint8_t bars = (QUIETEST - volume) / 10;        // 0..20
  for (uint8_t i = 0; i < 20; i++) Serial.print(i < bars ? '#' : '.');
  Serial.print(']');
  if (volume == 0)              Serial.print(F("  (max)"));
  else if (volume >= QUIETEST)  Serial.print(F("  (min)"));
  Serial.println();
}

void applyVolume() {
  musicPlayer.setVolume(volume, volume);
  printVolume();
}

void halt(const __FlashStringHelper *msg) {
  Serial.println(msg);
  while (true) delay(10);
}

// ---------- Arduino entry points ----------
void setup() {
  Serial.begin(115200);
  while (!Serial) { ; }          // harmless on the Uno, needed on Leonardo
  Serial.println(F("\nAdafruit Music Maker demo"));

  // 1. Talk to the VS1053 over SPI
  if (!musicPlayer.begin()) {
    halt(F("VS1053 not found. Check header solder joints and the ICSP socket."));
  }
  Serial.println(F("VS1053 found."));

  // 2. Short sine beep straight from the chip (no SD card needed).
  //    Note: sineTest() resets the chip, so set the volume AFTER it.
  musicPlayer.sineTest(0x44, 500);

  // 3. Mount the SD card
  if (!SD.begin(CARDCS)) {
    halt(F("SD card failed or not present. Use a FAT16/FAT32 card and reseat it."));
  }
  Serial.println(F("SD card OK. Files on card:"));
  File root = SD.open("/");
  listFiles(root, 1);
  root.close();

  // 4. Volume and background playback
  applyVolume();
  if (!musicPlayer.useInterrupt(VS1053_FILEPLAYER_PIN_INT)) {
    halt(F("DREQ pin is not an interrupt pin."));
  }

  // 5. Playlist
  buildPlaylist();
  if (trackCount == 0) {
    halt(F("No audio files found in the card's root folder."));
  }
  printPlaylist();
  Serial.println(F("Commands: n=next p=pause s=stop +=louder -=quieter v=volume l=list"));

  playTrack(0);
}

void loop() {
  // Auto advance when a track finishes (but not after the user pressed 's')
  if (musicPlayer.stopped() && !userStopped) {
    playTrack(currentIndex + 1);
  }

  if (Serial.available()) {
    char c = Serial.read();
    switch (c) {
      case 'n':
        playTrack(currentIndex + 1);
        break;

      case 'p':
        if (musicPlayer.stopped()) {
          playTrack(currentIndex < 0 ? 0 : currentIndex);
        } else if (musicPlayer.paused()) {
          Serial.println(F("Resumed"));
          musicPlayer.pausePlaying(false);
        } else {
          Serial.println(F("Paused"));
          musicPlayer.pausePlaying(true);
        }
        break;

      case 's':
        musicPlayer.stopPlaying();
        userStopped = true;
        Serial.println(F("Stopped. Press p to play again."));
        break;

      case '+':
        volume = (volume > VOLUME_STEP) ? volume - VOLUME_STEP : 0;
        applyVolume();
        break;

      case '-':
        volume = (volume + VOLUME_STEP <= QUIETEST) ? volume + VOLUME_STEP : QUIETEST;
        applyVolume();
        break;

      case 'v':
        printVolume();
        break;

      case 'l':
        printPlaylist();
        printVolume();
        break;
    }
  }

  delay(50);
}