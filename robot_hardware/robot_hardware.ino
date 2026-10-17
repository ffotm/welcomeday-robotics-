/*
  Robot face sketch: Arduino Uno + 2.42" SSD1309 OLED + touch button + speaker.
  The OLED shows the face, and the response text under a smaller face while it speaks.
  The PC tells the board the robot's state; the board draws a face and plays a short sound.
  (The AI's spoken voice still comes from the PC. The speaker here plays beeps/tones.)

  Library: "U8g2" by oliver (Library Manager)

  Pin map (each device needs its OWN pin; the speaker and OLED RES must not share one):
    OLED (SPI):   SCK -> 13, SDA (MOSI) -> 11, CS -> 10, DC -> 9, RES -> 8, plus GND and VCC
    Touch button: OUT -> 2 (TTP223-style module)
    Speaker:      pin 5 -> 100-220 ohm resistor -> speaker -> GND (a piezo buzzer can go direct)
    Mic module:   pin 7 (not used by this sketch yet, the PC mic does the listening)
  If your speaker is on another pin, change SPEAKER_PIN below.
  Do not connect a bare 8-ohm speaker straight to a pin; always use the resistor.

  For I2C OLED wiring instead, comment out "#define USE_SPI" (SDA -> A4, SCL -> A5).

  Serial protocol (115200 baud):
    PC -> board : STATE:idle | listening | thinking | speaking | error
                  FACE:neutral | happy | sad | surprised   (used while speaking)
                  SAY:line1|line2      (response text, max 21 chars per line)
                  anything else (ACTION:...) is ignored for now
    board -> PC : BTN:down / BTN:up   (touch pressed / released)
*/
#include <U8g2lib.h>

#define USE_SPI

const int TOUCH_PIN = 2;
const int SPEAKER_PIN = 5;
const int OLED_RES_PIN = 8;
const bool TOUCH_ACTIVE_HIGH = false;   // TTP223 modules output HIGH when touched
const bool SOUND_ON = true;            // set false to mute

#ifdef USE_SPI
U8G2_SSD1309_128X64_NONAME0_1_4W_HW_SPI u8g2(U8G2_R0, /*cs=*/10, /*dc=*/9, /*reset=*/OLED_RES_PIN);
#else
U8G2_SSD1309_128X64_NONAME0_1_HW_I2C u8g2(U8G2_R0, /*reset=*/OLED_RES_PIN);
#endif

String state = "idle";
String mood = "neutral";
String line = "";
String textA = "";             // response text, line 1
String textB = "";             // response text, line 2
unsigned long textExpire = 0;  // when to remove the text after speaking
bool lastTouch = false;
unsigned long lastFrame = 0;

const int LX = 36;   // left eye center x
const int RX = 92;   // right eye center x
const int EY = 22;   // eye center y

// ---------- sounds (played without blocking the animation) ----------
// Each sound is a list of pairs: frequency in Hz, duration in ms
const int SND_LISTEN[]    = {660, 70, 880, 90};
const int SND_HAPPY[]     = {523, 80, 659, 80, 784, 140};
const int SND_SAD[]       = {392, 140, 330, 140, 262, 260};
const int SND_SURPRISED[] = {500, 60, 900, 120};
const int SND_NEUTRAL[]   = {600, 80};
const int SND_ERROR[]     = {300, 120, 220, 220};

const int* seq = nullptr;
int seqLen = 0;   // number of notes
int seqIdx = 0;
unsigned long seqNext = 0;

#define PLAY(s) startSound(s, sizeof(s) / sizeof(s[0]) / 2)

void startSound(const int* s, int notes) {
  if (!SOUND_ON) return;
  seq = s;
  seqLen = notes;
  seqIdx = 0;
  seqNext = 0;
}

void updateSound() {
  if (seq == nullptr || millis() < seqNext) return;
  if (seqIdx >= seqLen) {
    seq = nullptr;
    noTone(SPEAKER_PIN);
    return;
  }
  int freq = seq[seqIdx * 2];
  int ms = seq[seqIdx * 2 + 1];
  tone(SPEAKER_PIN, freq, ms);
  seqNext = millis() + ms + 20;
  seqIdx++;
}

bool hasText() {
  return textA.length() > 0 || textB.length() > 0;
}

void clearText() {
  textA = "";
  textB = "";
  textExpire = 0;
}

void setState(const String &s) {
  state = s;
  if (s == "listening") {
    clearText();
    mood = "neutral";
    PLAY(SND_LISTEN);
  } else if (s == "thinking") {
    clearText();
    mood = "neutral";
  } else if (s == "error") {
    PLAY(SND_ERROR);
  } else if (s == "speaking") {
    clearText();
    if (mood == "happy") PLAY(SND_HAPPY);
    else if (mood == "sad") PLAY(SND_SAD);
    else if (mood == "surprised") PLAY(SND_SURPRISED);
    else PLAY(SND_NEUTRAL);
  } else if (s == "idle" && hasText()) {
    textExpire = millis() + 4000;   // keep the answer on screen for 4 more seconds
  }
}

void handleSay(const String &p) {
  int sep = p.indexOf('|');
  if (sep >= 0) {
    textA = p.substring(0, sep).substring(0, 21);
    textB = p.substring(sep + 1).substring(0, 21);
  } else {
    textA = p.substring(0, 21);
    textB = "";
  }
}

// ---------- setup / loop ----------
void setup() {
  Serial.begin(115200);
  pinMode(TOUCH_PIN, TOUCH_ACTIVE_HIGH ? INPUT : INPUT_PULLUP);
  pinMode(SPEAKER_PIN, OUTPUT);
  u8g2.begin();
  // u8g2.setI2CAddress(0x3D * 2);   // I2C only: uncomment if your module uses address 0x3D
  u8g2.setPowerSave(0);
  u8g2.setContrast(255);

  // Self-test: if you see this and hear a beep, the screen and speaker work
  PLAY(SND_LISTEN);
  u8g2.setFont(u8g2_font_6x10_tr);
  unsigned long start = millis();
  do {
    u8g2.firstPage();
    do {
      u8g2.drawFrame(0, 0, 128, 64);
      u8g2.drawStr(34, 36, "OLED OK");
    } while (u8g2.nextPage());
    updateSound();
  } while (millis() - start < 1000);

  Serial.println("READY");   // tells the backend the board is up
}

void loop() {
  checkTouch();
  checkSerial();
  updateSound();
  delay(200);
  if (textExpire != 0 && millis() > textExpire) clearText();
  if (millis() - lastFrame > 60) {
    lastFrame = millis();
    drawFace();
  }
}

// ---------- serial / touch ----------
void checkSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      line.trim();
      if (line.startsWith("STATE:")) setState(line.substring(6));
      else if (line.startsWith("FACE:")) mood = line.substring(5);
      else if (line.startsWith("SAY:")) handleSay(line.substring(4));
      line = "";
    } else if (c != '\r') {
      line += c;
    }
  }
}

bool readTouched() {
  return digitalRead(TOUCH_PIN) == (TOUCH_ACTIVE_HIGH ? HIGH : LOW);
}

void checkTouch() {
  bool touched = readTouched();
  if (touched != lastTouch) {
    delay(30);  // debounce
    touched = readTouched();
    if (touched != lastTouch) {
      lastTouch = touched;
      Serial.println(touched ? "BTN:down" : "BTN:up");
    }
  }
}

// ---------- drawing helpers ----------
void thickLine(int x0, int y0, int x1, int y1) {
  u8g2.drawLine(x0, y0, x1, y1);
  u8g2.drawLine(x0, y0 + 1, x1, y1 + 1);
  u8g2.drawLine(x0, y0 + 2, x1, y1 + 2);
}

void eyesRound(int w, int h, int dx, int dy) {
  int r = h / 2 < 5 ? h / 2 : 5;
  u8g2.drawRBox(LX - w / 2 + dx, EY - h / 2 + dy, w, h, r);
  u8g2.drawRBox(RX - w / 2 + dx, EY - h / 2 + dy, w, h, r);
}

void eyesHappy() {  // "^ ^"
  thickLine(LX - 14, EY + 8, LX, EY - 6);
  thickLine(LX, EY - 6, LX + 14, EY + 8);
  thickLine(RX - 14, EY + 8, RX, EY - 6);
  thickLine(RX, EY - 6, RX + 14, EY + 8);
}

void browsSad() {  // inner ends raised
  thickLine(LX - 16, 9, LX + 12, 2);
  thickLine(RX - 12, 2, RX + 16, 9);
}

void mouthSmile(int cy, int r) {
  u8g2.drawCircle(64, cy, r, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);
  u8g2.drawCircle(64, cy, r - 1, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);
}

void mouthFrown() {
  u8g2.drawCircle(64, 60, 10, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
  u8g2.drawCircle(64, 60, 9, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
}

// ---------- faces ----------
void drawMood(const String &m, bool blink, bool talking, unsigned long t) {
  int ph = (t / 90) % 12;
  int open = 2 + (ph < 6 ? ph : 12 - ph);  // mouth opening, 2..8

  if (m == "happy") {
    if (blink) eyesRound(26, 4, 0, 0); else eyesHappy();
    if (talking) u8g2.drawFilledEllipse(64, 50, 12, open);
    else mouthSmile(44, 14);
  } else if (m == "sad") {
    eyesRound(24, blink ? 4 : 24, 0, 2);
    if (!blink) browsSad();
    if (talking) u8g2.drawFilledEllipse(64, 52, 8, open);
    else mouthFrown();
  } else if (m == "surprised") {
    eyesRound(30, blink ? 4 : 34, 0, -2);
    if (talking) u8g2.drawFilledEllipse(64, 52, 7, open + 2);
    else u8g2.drawCircle(64, 52, 7);
  } else {  // neutral
    eyesRound(26, blink ? 4 : 30, 0, 0);
    if (talking) u8g2.drawFilledEllipse(64, 52, 10, open);
    else mouthSmile(46, 8);
  }
}

// ---------- small face + response text ----------
void miniEyes(int w, int h, int dy) {
  int r = h / 2 < 4 ? h / 2 : 4;
  u8g2.drawRBox(LX - w / 2, 12 - h / 2 + dy, w, h, r);
  u8g2.drawRBox(RX - w / 2, 12 - h / 2 + dy, w, h, r);
}

void miniFace(const String &m, bool blink, bool talking, unsigned long t) {
  int ph = (t / 90) % 12;
  int open = 1 + (ph < 6 ? ph : 12 - ph) / 2;   // mouth opening, 1..4

  if (m == "happy") {
    if (blink) miniEyes(22, 3, 0);
    else {
      thickLine(LX - 10, 17, LX, 7);
      thickLine(LX, 7, LX + 10, 17);
      thickLine(RX - 10, 17, RX, 7);
      thickLine(RX, 7, RX + 10, 17);
    }
    if (talking) u8g2.drawFilledEllipse(64, 26, 9, open);
    else u8g2.drawCircle(64, 21, 7, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);
  } else if (m == "sad") {
    miniEyes(20, blink ? 3 : 12, 3);
    if (talking) u8g2.drawFilledEllipse(64, 27, 7, open);
    else u8g2.drawCircle(64, 32, 6, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
  } else if (m == "surprised") {
    miniEyes(24, blink ? 3 : 22, 0);
    if (talking) u8g2.drawFilledEllipse(64, 26, 6, open + 1);
    else u8g2.drawCircle(64, 26, 4);
  } else {  // neutral
    miniEyes(22, blink ? 3 : 16, 0);
    if (talking) u8g2.drawFilledEllipse(64, 26, 8, open);
    else u8g2.drawCircle(64, 23, 5, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);
  }
}

void drawCentered(const String &s, int y) {
  int w = u8g2.getStrWidth(s.c_str());
  u8g2.drawStr((128 - w) / 2, y, s.c_str());
}

void drawTextLayout(bool blink, unsigned long t) {
  miniFace(mood, blink, state == "speaking", t);
  u8g2.setFont(u8g2_font_6x10_tr);
  drawCentered(textA, 47);
  drawCentered(textB, 60);
}

void drawFace() {
  unsigned long t = millis();
  bool blink = (t % 3600) < 130;

  u8g2.firstPage();
  do {  // page mode: this block runs once per page, so it must only draw
    if (state == "listening") {
      eyesRound(30, blink ? 4 : 36, 0, -2);   // wide open eyes
      u8g2.drawCircle(64, 52, 4);             // small "o" mouth
    } else if (state == "thinking") {
      eyesRound(24, blink ? 4 : 26, 8, -6);   // looking up and to the side
      thickLine(54, 52, 74, 52);              // flat mouth
      int dots = (t / 300) % 4;
      for (int i = 0; i < dots; i++) u8g2.drawDisc(98 + i * 8, 58, 2);
    } else if (state == "error") {
      drawMood("sad", false, false, t);
    } else if (state == "speaking" || hasText()) {
      drawTextLayout(blink, t);               // speaking, or the answer still on screen
    } else {  // idle
      drawMood("neutral", blink, false, t);
    }
  } while (u8g2.nextPage());
}
