#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <MFRC522.h>
#include <LiquidCrystal_I2C.h>
#include <EEPROM.h>

// =========================================================
//  PINS
//  RFID : SS=10, RST=9, SPI (MOSI 11, MISO 12, SCK 13)
//  LCD  : I2C -> SDA = A4, SCL = A5 (Uno/Nano)
//  RED LED (already recorded) : pin 5  -> resistor -> GND
//  OVERRIDE BUTTON            : pin 8  -> other leg to GND
// =========================================================
#define SS_PIN      10
#define RST_PIN      9
#define LED_PIN      6
#define BUZZER_PIN   7
#define RED_LED_PIN  5      // glows red when the card is already recorded
#define OVERRIDE_BTN_PIN 8  // push button to GND, toggles testing override

// =========================================================
//  LCD SETTINGS  (try 0x3F if 0x27 shows nothing)
// =========================================================
#define LCD_ADDR    0x27
#define LCD_COLS    16
#define LCD_ROWS    2

#define SCAN_SHOW_MS  4000UL   // how long the name screen stays up
#define SCROLL_MS      350UL   // scroll speed for text wider than LCD

// Block 4 = data block in sector 1 (never use 3, 7, 11 ...)
#define VALUE_BLOCK  4

#define LOGOUT_WAIT_SEC  3600UL  // logout opens 1 hour after login
#define RED_LED_MS       1500UL  // how long the red LED glows
#define OVERRIDE_LED_MS  3000UL  // how long BOTH LEDs glow when override is toggled ON or OFF
#define BTN_DEBOUNCE_MS    50UL

LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, LCD_ROWS);
MFRC522 rfid(SS_PIN, RST_PIN);
MFRC522::MIFARE_Key key;

// =========================================================
//  NAME TABLE
// =========================================================
struct Entry {
    byte        value;
    const char *name;
};

static const Entry NAMES[] = {
    {  1, "Apilado, Nathaniel Dustin L." },
    {  2, "Balante, Jonas T." },
    {  3, "Bautista, Vergel A." },
    {  4, "Del Rosario, Shean Vherl" },
    {  5, "Gaurano, Trisha Ann D." },
    {  6, "Lapid, Jaycee P." },
    {  7, "Llaban, Jose Ynnos E." },
    {  8, "Padali, Jamaica Christine M." },
    {  9, "Quinto, Lara Aaliyah L." },
    { 10, "Rivella, Jon Cristian R." },
    { 11, "Rovillos, Ashley P." },
    { 12, "Villaret, Jeremiah A." },
};
static const byte NAME_COUNT = sizeof(NAMES) / sizeof(NAMES[0]);

// =========================================================
//  STATE
// =========================================================
bool writeMode    = false;
byte valueToWrite = 0;

// --- clock + calendar (software; resets on power loss) ---
// Time starts at 12:00:00. Date starts at the day the firmware was compiled.
unsigned long clockBaseSec = 12UL * 3600UL;   // seconds since midnight
unsigned long clockRefMs   = 0;
int           dateYear     = 2026;
byte          dateMonth    = 1;
byte          dateDay      = 1;
long          lastShownSec = -1;

static const char DAYS[]   = "SunMonTueWedThuFriSat";
static const char MONTHS[] = "JanFebMarAprMayJunJulAugSepOctNovDec";

// --- attendance state per user (RAM, resets on power loss) ---
enum { ST_NONE = 0, ST_IN = 1, ST_OUT = 2 };
byte          userState[NAME_COUNT];   // none / logged in / logged out
long          userDay[NAME_COUNT];     // YYYYMMDD the state belongs to
unsigned long userInUp[NAME_COUNT];    // uptime seconds at login (unaffected by clock changes)
bool          userActive[NAME_COUNT];  // false = card is tagged INACTIVE (saved in EEPROM)
bool          overrideMode = false;    // testing override: logout without the 1 hour wait
unsigned long redUntil     = 0;        // when to switch the red LED off
unsigned long ovrLedUntil  = 0;        // when to switch BOTH LEDs off after an override toggle

// --- scan screen ---
bool          showingScan = false;
unsigned long scanUntil   = 0;
unsigned long lastScroll  = 0;
byte          scrollPos   = 0;
byte          scrollPos1  = 0;
char          scanLine1[32];
char          scanLine2[32];

// =========================================================
//  CLOCK + CALENDAR
// =========================================================
byte daysInMonth(int y, byte m) {
    static const byte d[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    return d[m - 1];
}

void advanceDays(unsigned long n) {
    while (n--) {
        if (++dateDay > daysInMonth(dateYear, dateMonth)) {
            dateDay = 1;
            if (++dateMonth > 12) { dateMonth = 1; dateYear++; }
        }
    }
}

byte weekdayOf(int y, byte m, byte d) {          // 0 = Sunday
    static const byte t[] = {0,3,2,5,0,3,5,1,4,6,2,4};
    if (m < 3) y -= 1;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

// Advance the clock; rolls the date over at midnight
void tickClock() {
    unsigned long e = (millis() - clockRefMs) / 1000UL;
    if (e == 0) return;
    clockRefMs += e * 1000UL;
    unsigned long total = clockBaseSec + e;
    clockBaseSec = total % 86400UL;
    if (total >= 86400UL) advanceDays(total / 86400UL);
}

void formatTime(char *out) {                     // 9 chars: "HH:MM:SS"
    tickClock();
    byte h = clockBaseSec / 3600UL;
    byte m = (clockBaseSec % 3600UL) / 60UL;
    byte s = clockBaseSec % 60UL;
    sprintf(out, "%02d:%02d:%02d", h, m, s);
}

void formatDate(char *out) {                     // 16 chars: "Mon 23 Nov 2026"
    byte wd = weekdayOf(dateYear, dateMonth, dateDay);
    char w[4], mo[4];
    memcpy(w,  DAYS   + wd * 3,           3); w[3]  = '\0';
    memcpy(mo, MONTHS + (dateMonth - 1) * 3, 3); mo[3] = '\0';
    sprintf(out, "%s %02d %s %d", w, dateDay, mo, dateYear);
}

void setClock(byte h, byte m, byte s) {
    clockBaseSec = h * 3600UL + m * 60UL + s;
    clockRefMs   = millis();
    lastShownSec = -1;
}

void setDate(int y, byte m, byte d) {
    dateYear = y; dateMonth = m; dateDay = d;
    lastShownSec = -1;
}

// Use the compile date (e.g. "Nov 23 2026") as the starting date
void setDateFromCompile() {
    char mm[4];
    memcpy(mm, __DATE__, 3); mm[3] = '\0';
    const char *p = strstr(MONTHS, mm);
    byte m = p ? (p - MONTHS) / 3 + 1 : 1;
    setDate(atoi(__DATE__ + 7), m, atoi(__DATE__ + 4));
}

// =========================================================
//  LCD HELPERS
// =========================================================

// Print text centred on a row (clipped to LCD width)
void lcdCenter(byte row, const char *text) {
    byte len    = strlen(text);
    if (len > LCD_COLS) len = LCD_COLS;
    byte offset = (LCD_COLS - len) / 2;
    lcd.setCursor(0, row);
    for (byte i = 0; i < LCD_COLS; i++) {
        if (i >= offset && i < offset + len) lcd.print(text[i - offset]);
        else                                 lcd.print(' ');
    }
}

// Print a scrolling window of text (used when text is wider than LCD)
void lcdScrollWindow(byte row, const char *text, byte pos) {
    byte len   = strlen(text);
    byte total = len + 3;                // 3-space gap before it wraps
    lcd.setCursor(0, row);
    for (byte i = 0; i < LCD_COLS; i++) {
        byte idx = (pos + i) % total;
        lcd.print(idx < len ? text[idx] : ' ');
    }
}

// Show a result screen: name on row 1, message on row 2
void showScreen(const char *line1, const char *line2) {
    lcd.clear();
    strncpy(scanLine1, line1, sizeof(scanLine1) - 1);
    scanLine1[sizeof(scanLine1) - 1] = '\0';
    strncpy(scanLine2, line2, sizeof(scanLine2) - 1);
    scanLine2[sizeof(scanLine2) - 1] = '\0';

    scrollPos1 = 0;
    if (strlen(scanLine1) > LCD_COLS) lcdScrollWindow(0, scanLine1, scrollPos1);
    else                              lcdCenter(0, scanLine1);

    if (strlen(scanLine2) > LCD_COLS) {
        scrollPos = 0;
        lcdScrollWindow(1, scanLine2, scrollPos);
    } else {
        lcdCenter(1, scanLine2);
    }

    showingScan = true;
    scanUntil   = millis() + SCAN_SHOW_MS;
    lastScroll  = millis();
}

void showClockScreen() {
    lcd.clear();
    lastShownSec = -1;                   // force redraw of date + time
}

// Called every loop: handles idle clock + scan screen timeout/scroll
void updateLcd() {
    unsigned long now = millis();
    tickClock();

    if (showingScan) {
        if ((long)(now - scanUntil) >= 0) {
            showingScan = false;
            showClockScreen();
        } else if ((strlen(scanLine1) > LCD_COLS || strlen(scanLine2) > LCD_COLS) &&
                   now - lastScroll >= SCROLL_MS) {
            lastScroll = now;
            if (strlen(scanLine1) > LCD_COLS) {            // long name on the top row
                scrollPos1 = (scrollPos1 + 1) % (strlen(scanLine1) + 3);
                lcdScrollWindow(0, scanLine1, scrollPos1);
            }
            if (strlen(scanLine2) > LCD_COLS) {
                scrollPos  = (scrollPos + 1) % (strlen(scanLine2) + 3);
                lcdScrollWindow(1, scanLine2, scrollPos);
            }
        }
        return;
    }

    if ((long)clockBaseSec != lastShownSec) {
        lastShownSec = clockBaseSec;
        char buf[17];
        formatDate(buf);
        lcdCenter(0, buf);               // top row: date
        formatTime(buf);
        lcdCenter(1, buf);               // bottom row: time
        if (overrideMode) {              // marker while override is on
            lcd.setCursor(13, 1);
            lcd.print(F("OVR"));
        }
    }
}

// =========================================================
//  GENERAL HELPERS
// =========================================================
void beep(int duration = 150, int freq = 2000) {
    tone(BUZZER_PIN, freq);
    delay(duration);
    noTone(BUZZER_PIN);
}

void printBinary8(byte v) {
    Serial.print(F("0b"));
    for (int i = 7; i >= 0; i--) Serial.print((v >> i) & 1);
}

void printValue(byte v) {
    Serial.print(v);
    Serial.print(F("  ("));
    printBinary8(v);
    Serial.print(F(", 0x"));
    if (v < 0x10) Serial.print('0');
    Serial.print(v, HEX);
    Serial.println(')');
}

// Returns nullptr when the value has no name
const char *lookupName(byte v) {
    for (byte i = 0; i < NAME_COUNT; i++) {
        if (NAMES[i].value == v) return NAMES[i].name;
    }
    return nullptr;
}

void printUid() {
    for (byte i = 0; i < rfid.uid.size; i++) {
        if (rfid.uid.uidByte[i] < 0x10) Serial.print('0');
        Serial.print(rfid.uid.uidByte[i], HEX);
        if (i < rfid.uid.size - 1) Serial.print(' ');
    }
}

bool isMifareClassic(MFRC522::PICC_Type t) {
    return t == MFRC522::PICC_TYPE_MIFARE_MINI ||
           t == MFRC522::PICC_TYPE_MIFARE_1K   ||
           t == MFRC522::PICC_TYPE_MIFARE_4K;
}

bool authenticate() {
    MFRC522::StatusCode status = rfid.PCD_Authenticate(
        MFRC522::PICC_CMD_MF_AUTH_KEY_A, VALUE_BLOCK, &key, &rfid.uid);
    if (status != MFRC522::STATUS_OK) {
        Serial.print(F("[ERROR] Auth failed: "));
        Serial.println(rfid.GetStatusCodeName(status));
        return false;
    }
    return true;
}

// Block layout: byte 0 = value, byte 1 = ~value (check byte), rest 0
bool writeValue(byte v) {
    byte buf[16] = {0};
    buf[0] = v;
    buf[1] = (byte)~v;
    MFRC522::StatusCode status = rfid.MIFARE_Write(VALUE_BLOCK, buf, 16);
    if (status != MFRC522::STATUS_OK) {
        Serial.print(F("[ERROR] Write failed: "));
        Serial.println(rfid.GetStatusCodeName(status));
        return false;
    }
    return true;
}

bool readValue(byte &v) {
    byte buf[18];
    byte size = sizeof(buf);
    MFRC522::StatusCode status = rfid.MIFARE_Read(VALUE_BLOCK, buf, &size);
    if (status != MFRC522::STATUS_OK) {
        Serial.print(F("[ERROR] Read failed: "));
        Serial.println(rfid.GetStatusCodeName(status));
        return false;
    }
    if (buf[1] != (byte)~buf[0]) return false;
    v = buf[0];
    return true;
}

// =========================================================
//  ATTENDANCE LOGIC
//  1st tap of the day  = LOGIN
//  tap 1 hour or later = LOGOUT
//  taps in between, or after logout, are ignored and the
//  red LED glows ("already recorded").
//  Override mode (push button or serial  o ) is for demos:
//  every tap is recorded, as many times as you like
//  (login, logout, login, ...) with no waiting.
// =========================================================
long dateKey() {
    return dateYear * 10000L + dateMonth * 100L + dateDay;
}

int indexOfValue(byte v) {
    for (byte i = 0; i < NAME_COUNT; i++) {
        if (NAMES[i].value == v) return i;
    }
    return -1;
}

void formatSec(unsigned long s, char *out) {     // "HH:MM:SS"
    s %= 86400UL;
    sprintf(out, "%02d:%02d:%02d",
            (int)(s / 3600UL), (int)((s % 3600UL) / 60UL), (int)(s % 60UL));
}

void redGlow() {
    digitalWrite(RED_LED_PIN, HIGH);
    redUntil = millis() + RED_LED_MS;
}

void updateRedLed() {
    unsigned long now = millis();
    if (redUntil != 0 && (long)(now - redUntil) >= 0) {
        digitalWrite(RED_LED_PIN, LOW);
        redUntil = 0;
    }
    // override toggle: both LEDs go off together when the timer ends
    if (ovrLedUntil != 0 && (long)(now - ovrLedUntil) >= 0) {
        digitalWrite(LED_PIN, LOW);
        digitalWrite(RED_LED_PIN, LOW);
        ovrLedUntil = 0;
        redUntil    = 0;
    }
}

void setOverride(bool on) {
    overrideMode = on;
    Serial.print(F("\n[OVERRIDE MODE] "));
    Serial.println(on ? F("ON  (every tap is recorded, no waiting)") : F("OFF"));
    Serial.print(F("OVERRIDE|"));
    Serial.println(on ? 1 : 0);

    showScreen("Override Mode:", on ? "ON" : "OFF");

    // BOTH LEDs glow for OVERRIDE_LED_MS (non-blocking, so the button,
    // serial and LCD keep working while they are lit)
    digitalWrite(LED_PIN, HIGH);
    digitalWrite(RED_LED_PIN, HIGH);
    ovrLedUntil = millis() + OVERRIDE_LED_MS;
    redUntil    = 0;
    beep(150, on ? 2500 : 1200);
}

// Tag a user active / inactive (kept in EEPROM, so it survives power loss)
void setUserActive(byte v, bool on) {
    int i = indexOfValue(v);
    if (i < 0) {
        Serial.println(F("[ERROR] That ID is not in the name table."));
        return;
    }
    userActive[i] = on;
    EEPROM.update(i, on ? 0xFF : 0x00);
    Serial.print(F("[USER] ")); Serial.print(NAMES[i].name);
    Serial.println(on ? F(" is ACTIVE") : F(" is INACTIVE"));
}

// Restore a user's attendance state after the Arduino restarts (sent by the web page).
// st = 1 logged in, 2 logged out; elapsedSec = seconds since the ORIGINAL login tap,
// so the 1 hour logout countdown keeps counting from the first tap.
void restoreUserState(byte v, byte st, unsigned long elapsedSec) {
    int i = indexOfValue(v);
    if (i < 0) {
        Serial.println(F("[ERROR] That ID is not in the name table."));
        return;
    }
    userState[i] = st;
    userDay[i]   = dateKey();
    userInUp[i]  = millis() / 1000UL - elapsedSec;     // wraps correctly: elapsed stays exact
    Serial.print(F("[RESTORE] ")); Serial.print(NAMES[i].name);
    Serial.print(st == ST_IN ? F(" logged in ") : F(" logged out "));
    Serial.print(elapsedSec); Serial.println(F(" s ago"));
}

// Forget attendance so the next tap is a fresh first login. id < 0 = everyone.
void clearAttendance(int id) {
    for (byte i = 0; i < NAME_COUNT; i++) {
        if (id < 0 || NAMES[i].value == id) {
            userState[i] = ST_NONE;
            userDay[i]   = 0;
            userInUp[i]  = 0;
        }
    }
    if (id < 0) {
        Serial.println(F("[CLEAR] All attendance cleared. Next tap is a fresh login."));
        showScreen("Records Cleared", "Tap to log in");
    } else {
        Serial.print(F("[CLEAR] Attendance cleared for ID ")); Serial.println(id);
    }
}

// Push button (to GND, internal pull-up): each press toggles override mode
void checkButton() {
    static bool lastRead = HIGH;
    static bool stable   = HIGH;
    static unsigned long changedAt = 0;

    bool r = digitalRead(OVERRIDE_BTN_PIN);
    if (r != lastRead) {
        lastRead  = r;
        changedAt = millis();
    }
    if (millis() - changedAt >= BTN_DEBOUNCE_MS && r != stable) {
        stable = r;
        if (stable == LOW) {
            Serial.println(F("[BUTTON] pressed"));
            setOverride(!overrideMode);
        }
    }
}

// =========================================================
//  SERIAL MENU
// =========================================================
void printMenu() {
    Serial.println(F("\n=============================="));
    Serial.println(F("   RFID ATTENDANCE SYSTEM"));
    Serial.println(F("=============================="));
    Serial.println(F("  w <0-255>   write value to next card"));
    Serial.println(F("  r           attendance mode (default)"));
    Serial.println(F("  t HH:MM:SS  set the time (24h)"));
    Serial.println(F("  d YYYY-MM-DD set the date"));
    Serial.println(F("  o [1|0]     override mode, unlimited taps (or push button)"));
    Serial.println(F("  a <ID> <1|0> tag a user active (1) or inactive (0)"));
    Serial.println(F("  s <ID> <1|2> <sec>  restore state: 1=in, 2=out, seconds since first tap"));
    Serial.println(F("  c [ID]      clear attendance (everyone, or one ID) for a fresh first tap"));
    Serial.println(F("  l           list name table"));
    Serial.println(F("  m           show this menu"));
    Serial.println(F("------------------------------"));
}

void printNameTable() {
    Serial.println(F("\nName table:"));
    for (byte i = 0; i < NAME_COUNT; i++) {
        Serial.print(F("  "));
        printBinary8(NAMES[i].value);
        Serial.print(F("  ("));
        Serial.print(NAMES[i].value);
        Serial.print(F(")  "));
        Serial.print(NAMES[i].name);
        Serial.println(userActive[i] ? F("") : F("  [INACTIVE]"));
    }
}

void handleCommand(const String &line) {
    if (line.length() == 0) return;
    char cmd = line.charAt(0);

    if (cmd == 'w' || cmd == 'W') {
        String arg = line.substring(1);
        arg.trim();
        bool numeric = (arg.length() > 0);
        for (unsigned int i = 0; i < arg.length(); i++) {
            if (!isDigit(arg.charAt(i))) { numeric = false; break; }
        }
        long n = numeric ? arg.toInt() : -1;
        if (n < 0 || n > 255) {
            Serial.println(F("[ERROR] Use a whole number 0-255, e.g.  w 5"));
            return;
        }
        valueToWrite = (byte)n;
        writeMode    = true;
        Serial.print(F("\n[WRITE MODE] Will write "));
        printValue(valueToWrite);
        const char *nm = lookupName(valueToWrite);
        Serial.print(F("Name: "));
        Serial.println(nm ? nm : "(no name assigned)");
        Serial.println(F("Tap the card now..."));

        lcd.clear();
        lcdCenter(0, "WRITE MODE");
        char buf[17];
        sprintf(buf, "Value: %d", valueToWrite);
        lcdCenter(1, buf);
        showingScan = true;
        scanUntil   = millis() + SCAN_SHOW_MS;
        scanLine2[0] = '\0';

    } else if (cmd == 'r' || cmd == 'R') {
        writeMode = false;
        Serial.println(F("\n[ATTENDANCE MODE]"));

    } else if (cmd == 't' || cmd == 'T') {
        int h, m, s;
        if (sscanf(line.c_str(), "%*c %d:%d:%d", &h, &m, &s) == 3 &&
            h >= 0 && h < 24 && m >= 0 && m < 60 && s >= 0 && s < 60) {
            setClock(h, m, s);
            Serial.println(F("[OK] Clock set."));
        } else {
            Serial.println(F("[ERROR] Use  t HH:MM:SS  e.g.  t 08:30:00"));
        }

    } else if (cmd == 'd' || cmd == 'D') {
        int y, mo, d;
        if (sscanf(line.c_str(), "%*c %d-%d-%d", &y, &mo, &d) == 3 &&
            y >= 2000 && y <= 2099 && mo >= 1 && mo <= 12 &&
            d >= 1 && d <= daysInMonth(y, mo)) {
            setDate(y, mo, d);
            Serial.println(F("[OK] Date set."));
        } else {
            Serial.println(F("[ERROR] Use  d YYYY-MM-DD  e.g.  d 2026-11-23"));
        }

    } else if (cmd == 'o' || cmd == 'O') {
        String arg = line.substring(1);
        arg.trim();
        if (arg == "1")      setOverride(true);
        else if (arg == "0") setOverride(false);
        else                 setOverride(!overrideMode);

    } else if (cmd == 'a' || cmd == 'A') {
        int id, on;
        if (sscanf(line.c_str(), "%*c %d %d", &id, &on) == 2 &&
            id >= 0 && id <= 255 && (on == 0 || on == 1)) {
            setUserActive((byte)id, on == 1);
        } else {
            Serial.println(F("[ERROR] Use  a <ID> <1|0>  e.g.  a 5 0"));
        }

    } else if (cmd == 'c' || cmd == 'C') {
        String arg = line.substring(1);
        arg.trim();
        if (arg.length() == 0) {
            clearAttendance(-1);
        } else {
            bool numeric = true;
            for (unsigned int i = 0; i < arg.length(); i++)
                if (!isDigit(arg.charAt(i))) { numeric = false; break; }
            if (numeric && arg.toInt() <= 255) clearAttendance(arg.toInt());
            else Serial.println(F("[ERROR] Use  c  or  c <ID>"));
        }

    } else if (cmd == 's' || cmd == 'S') {
        int id, st; long el;
        if (sscanf(line.c_str(), "%*c %d %d %ld", &id, &st, &el) == 3 &&
            id >= 0 && id <= 255 && (st == 1 || st == 2) && el >= 0) {
            restoreUserState((byte)id, (byte)st, (unsigned long)el);
        } else {
            Serial.println(F("[ERROR] Use  s <ID> <1|2> <seconds>  e.g.  s 5 1 600"));
        }

    } else if (cmd == 'l' || cmd == 'L') {
        printNameTable();

    } else if (cmd == 'm' || cmd == 'M') {
        printMenu();

    } else {
        Serial.println(F("Unknown command. Type  m  for the menu."));
    }
}

void checkSerial() {
    if (Serial.available()) {
        String line = Serial.readStringUntil('\n');
        line.trim();
        handleCommand(line);
    }
}

// =========================================================
//  ATTENDANCE RECORD
//  Serial lines for the web monitor:
//    SCAN|<value>|<name>|IN        login recorded
//    SCAN|<value>|<name>|OUT       logout recorded
//    DENY|<value>|<name>|EARLY|<HH:MM:SS>   too early, logout opens at that time
//    DENY|<value>|<name>|DONE      already logged out
//    DENY|<value>|<n>|INACTIVE   card is tagged inactive
//    INVALID|<value or BLANK or TYPE>   card is not in the records
//    OVERRIDE|<0 or 1>             override mode changed
// =========================================================
// Returns true when the tap was recorded, false when it was ignored
bool recordAttendance(byte v, const char *name) {
    char t[9], dt[17], opens[9];
    formatTime(t);                      // also ticks the clock
    formatDate(dt);

    int i = indexOfValue(v);
    if (i < 0) return false;

    if (!userActive[i]) {               // tagged inactive from the web page
        Serial.println(F("\n[CARD INACTIVE]"));
        Serial.print(F("  Name  : ")); Serial.println(name);
        Serial.print(F("DENY|")); Serial.print(v);
        Serial.print('|');        Serial.print(name);
        Serial.println(F("|INACTIVE"));
        showScreen("Card Inactive", name);
        return false;
    }

    long today = dateKey();
    if (userDay[i] != today) {          // new day: everyone starts fresh
        userState[i] = ST_NONE;
        userDay[i]   = today;
    }

    unsigned long nowUp = millis() / 1000UL;
    bool isOut = false;

    if (userState[i] == ST_NONE) {
        userState[i] = ST_IN;
        userInUp[i]  = nowUp;

    } else if (userState[i] == ST_IN) {
        unsigned long elapsed = nowUp - userInUp[i];
        if (overrideMode || elapsed >= LOGOUT_WAIT_SEC) {
            userState[i] = ST_OUT;
            isOut = true;
        } else {
            unsigned long left = LOGOUT_WAIT_SEC - elapsed;
            formatSec(clockBaseSec + left, opens);

            Serial.println(F("\n[ALREADY RECORDED]"));
            Serial.print(F("  Name  : ")); Serial.println(name);
            Serial.print(F("  Logout opens at ")); Serial.print(opens);
            Serial.print(F(" (")); Serial.print((left + 59UL) / 60UL);
            Serial.println(F(" min left)"));
            Serial.print(F("DENY|")); Serial.print(v);
            Serial.print('|');        Serial.print(name);
            Serial.print(F("|EARLY|")); Serial.println(opens);

            char msg[24];
            sprintf(msg, "In already, out %.5s", opens);
            showScreen(name, msg);
            return false;
        }

    } else {                            // already logged out today
        if (overrideMode) {             // demo: start a new login
            userState[i] = ST_IN;
            userInUp[i]  = nowUp;
        } else {
            Serial.println(F("\n[ALREADY RECORDED]"));
            Serial.print(F("  Name  : ")); Serial.println(name);
            Serial.println(F("  Already logged out today."));
            Serial.print(F("DENY|")); Serial.print(v);
            Serial.print('|');        Serial.print(name);
            Serial.println(F("|DONE"));
            showScreen(name, "Already Recorded");
            return false;
        }
    }

    Serial.println(F("\n[ATTENDANCE]"));
    Serial.print(F("  Name  : ")); Serial.println(name);
    Serial.print(F("  Type  : ")); Serial.println(isOut ? F("LOGOUT") : F("LOGIN"));
    Serial.print(F("  Date  : ")); Serial.println(dt);
    Serial.print(F("  Time  : ")); Serial.println(t);
    Serial.print(F("  Value : ")); printValue(v);

    Serial.print(F("SCAN|"));
    Serial.print(v);
    Serial.print('|');
    Serial.print(name);
    Serial.print('|');
    Serial.println(isOut ? F("OUT") : F("IN"));

    // LCD: name on row 1, message on row 2
    showScreen(name, isOut ? "Logout Recorded" : "Login Recorded");
    return true;
}

// Like delay(), but keeps the button, LEDs and LCD alive while waiting
void waitMs(unsigned long ms) {
    unsigned long start = millis();
    while (millis() - start < ms) {
        checkButton();
        updateRedLed();
        updateLcd();
    }
}

// =========================================================
//  SETUP
// =========================================================
void setup() {
    Serial.begin(9600);
    while (!Serial);
    Serial.setTimeout(200);

    pinMode(LED_PIN,    OUTPUT);
    pinMode(BUZZER_PIN, OUTPUT);
    pinMode(RED_LED_PIN, OUTPUT);
    pinMode(OVERRIDE_BTN_PIN, INPUT_PULLUP);
    digitalWrite(LED_PIN, LOW);
    digitalWrite(RED_LED_PIN, LOW);
    noTone(BUZZER_PIN);

    for (byte i = 0; i < NAME_COUNT; i++)           // erased EEPROM (0xFF) = active
        userActive[i] = (EEPROM.read(i) != 0x00);

    lcd.init();
    lcd.backlight();
    lcdCenter(0, "Attendance");
    lcdCenter(1, "Starting...");

    for (byte i = 0; i < 6; i++) key.keyByte[i] = 0xFF;

    SPI.begin();
    rfid.PCD_Init();
    delay(100);

    byte ver = rfid.PCD_ReadRegister(MFRC522::VersionReg);
    Serial.print(F("Firmware Version: 0x"));
    Serial.println(ver, HEX);

    if (ver == 0x00 || ver == 0xFF) {
        Serial.println(F("[ERROR] RFID module not detected! Check wiring."));
        lcd.clear();
        lcdCenter(0, "RFID ERROR");
        lcdCenter(1, "Check wiring");
        delay(2000);
    } else {
        Serial.println(F("[OK] RFID module detected."));
    }

    printMenu();
    delay(1000);

    setDateFromCompile();        // date = day the firmware was built
    setClock(12, 0, 0);          // time always starts at 12:00:00
    showClockScreen();
    Serial.println(F("OVERRIDE|0"));   // tells the web page override is off
    Serial.println(F("[INFO] Override button on pin 8 (other leg to GND)."));
    Serial.println(F("READY"));        // tells the web page it can send the clock and user status now
}

// =========================================================
//  LOOP
// =========================================================
void loop() {
    checkSerial();
    checkButton();
    updateRedLed();
    updateLcd();

    if (!rfid.PICC_IsNewCardPresent()) return;
    if (!rfid.PICC_ReadCardSerial())   return;

    Serial.println(F("\n----------------------------"));
    Serial.print(F("UID: "));
    printUid();
    Serial.println();

    MFRC522::PICC_Type cardType = rfid.PICC_GetType(rfid.uid.sak);
    Serial.print(F("Type: "));
    Serial.println(rfid.PICC_GetTypeName(cardType));

    if (!isMifareClassic(cardType)) {
        Serial.println(F("[ERROR] MIFARE Classic card required."));
        Serial.println(F("INVALID|TYPE"));
        showScreen("Card Invalid", "Not in records");
        beep(400, 500);
        rfid.PICC_HaltA();
        rfid.PCD_StopCrypto1();
        waitMs(500);
        return;
    }

    if (authenticate()) {
        if (writeMode) {
            // ----- WRITE -----
            if (writeValue(valueToWrite)) {
                byte check;
                if (readValue(check) && check == valueToWrite) {
                    const char *nm = lookupName(check);
                    Serial.print(F("[OK] Written & verified: "));
                    printValue(check);
                    Serial.print(F("Name: "));
                    Serial.println(nm ? nm : "(no name assigned)");
                    showScreen(nm ? nm : "Card Written", "Card Programmed");
                    digitalWrite(LED_PIN, HIGH);
                    beep(200, 2000);
                } else {
                    Serial.println(F("[ERROR] Verify failed. Try again."));
                    showScreen("Write Failed", "Try Again");
                    beep(400, 500);
                }
            } else {
                showScreen("Write Failed", "Try Again");
                beep(400, 500);
            }
            writeMode = false;
            Serial.println(F("[ATTENDANCE MODE] Back to attendance mode."));

        } else {
            // ----- ATTENDANCE -----
            byte v;
            if (readValue(v)) {
                const char *name = lookupName(v);
                if (name) {
                    if (recordAttendance(v, name)) {
                        digitalWrite(LED_PIN, HIGH);     // recorded
                        beep(150, 2000);
                    } else {
                        redGlow();                       // already recorded
                        beep(300, 600);
                    }
                } else {
                    Serial.print(F("[!] Unknown value: "));
                    printValue(v);
                    Serial.print(F("INVALID|")); Serial.println(v);
                    showScreen("Card Invalid", "Not in records");
                    beep(300, 800);
                }
            } else {
                Serial.println(F("[!] No value on this card."));
                Serial.println(F("    Use  w <0-255>  to program it first."));
                Serial.println(F("INVALID|BLANK"));
                showScreen("Card Invalid", "Not in records");
                beep(300, 800);
            }
        }
    } else {
        showScreen("Card Error", "Not Recorded");
        beep(400, 500);
    }

    Serial.println(F("----------------------------"));

    waitMs(600);
    if (ovrLedUntil == 0) digitalWrite(LED_PIN, LOW);   // don't cut the override glow short

    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
    waitMs(400);
}