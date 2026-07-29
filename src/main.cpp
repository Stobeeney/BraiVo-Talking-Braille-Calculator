#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Keypad.h>
#include <Talkie.h>
#include <Vocab_US_Large.h>
#include <Vocab_Special.h>
#include <WiFi.h>
#include <vector>
#include <cmath>

// ============================================================================
// BRAIVO AUDIO-ASSISTED BRAILLE CALCULATOR FIRMWARE
// Spec: BraiVo Voice Response Mechanism + Error Specification (all sections)
// ============================================================================

LiquidCrystal_I2C lcd(0x27, 16, 2);
Talkie voice;

const byte ROWS = 4;
const byte COLS = 6;

// Matrix Layout:
// Row 1: [---] [ 7 ] [ 8 ] [ 9 ] [DEL] [ AC ]
// Row 2: [ SQ] [ 4 ] [ 5 ] [ 6 ] [ / ] [  - ]
// Row 3: [ SD] [ 1 ] [ 2 ] [ 3 ] [ * ] [  + ]   (SD = ANS / S-D toggle)
// Row 4: [ ( ] [ ) ] [ 0 ] [ . ] [ % ] [  = ]
char keys[ROWS][COLS] = {
  {' ', '7', '8', '9', 'D', 'C'},
  {'Q', '4', '5', '6', '/', '-'},
  {'S', '1', '2', '3', '*', '+'},
  {'(', ')', '0', '.', '%', '='}
};

const uint8_t sp_WELCOME[] PROGMEM = {
  0x0A, 0x58, 0x71, 0x4B, 0x95, 0xDD, 0x55, 0x55, 0x93, 0xD7, 0x0A, 0x68, 0x61, 0x55, 0x77, 0x8D,
  0xAD, 0x4A, 0x48, 0x3D, 0x2A, 0xA1, 0x32, 0x4A, 0x35, 0x1A, 0x96, 0xE9, 0x76, 0x8C, 0xAD, 0xBA,
  0xCD, 0x24, 0x33, 0x25, 0x22, 0x3B, 0x6E, 0x9B, 0x13, 0xAE, 0xD4, 0x27, 0xAE, 0x27, 0xA8, 0x48,
  0xA8, 0x9A, 0xDB, 0xCA, 0x73, 0x45, 0x11, 0x05, 0x79, 0x58, 0xE4, 0x2D, 0x6B, 0x29, 0xCD, 0x5E,
  0x1A, 0xAE, 0x24, 0x4E, 0x23, 0xC6, 0xC6, 0x88, 0xA9, 0xC5, 0x39, 0xC9, 0x4B, 0xED, 0xAC, 0xED,
  0x24, 0x53, 0x2B, 0x0C, 0xAB, 0x26, 0xCA, 0xD5, 0x2A, 0xD5, 0x64, 0x28, 0x91, 0xB6, 0x0D, 0x8E,
  0x49, 0x79, 0x78, 0x24, 0xC4, 0x4F, 0x00, 0x10, 0x00, 0x40, 0x66, 0x20, 0x07, 0x0F
};


byte rowPins[ROWS] = {13, 12, 14, 27};
byte colPins[COLS] = {26, 16, 33, 32, 19, 18};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// State
String  currentExpr    = "";
double  lastResult     = 0.0;
bool    hasLastResult  = false;
bool    justCalculated = false;   // true right after '=' was pressed
bool    isSdDecimal    = false;   // SD toggle state
int     lastNum        = 0;       // numerator of last division (for fraction display)
int     lastDen        = 1;       // denominator of last division

unsigned long sqPressTime = 0;
bool          sqPressed   = false;
const int     BATT_PIN    = 34;

// ---- Fraction analysis helper -----------------------------------------------
struct FracInfo { int startPos; int cycleLen; };

int calcGcd(int a, int b) {
  a = abs(a); b = abs(b);
  while (b) { int t = b; b = a % b; a = t; }
  return a ? a : 1;
}

// Returns: startPos  = number of non-repeating decimal digits
//          cycleLen  = 0 if terminating, >0 if repeating
FracInfo getFracInfo(int num, int den) {
  FracInfo fi = {0, 0};
  if (den == 0 || den == 1) return fi;
  int g = calcGcd(abs(num), abs(den));
  den = abs(den) / g;
  if (den <= 1) return fi;
  int sp = 0, td = den;
  while (td % 2 == 0) { td /= 2; sp++; }
  while (td % 5 == 0) { td /= 5; sp++; }
  fi.startPos = sp;
  if (td == 1) { fi.cycleLen = 0; return fi; }
  // Multiplicative order of 10 mod td
  long r = 10 % td;
  int cl = 1;
  while (r != 1 && cl < 300) { r = (r * 10) % td; cl++; }
  fi.cycleLen = (r == 1) ? cl : 0;
  return fi;
}

// ---- Forward declarations ---------------------------------------------------
void speakText(const String& msg);
void speakKey(char key);
void speakNumber(long num);
void speakDecimalVoice(double absResult, bool repeating);
void speakResultVoice(double result, int num, int den);
String getSpokenExpr(const String& expr);
void speakExprVoice(const String& expr);
void updateDisplay(const String& l1, const String& l2);
bool isArithOp(char c);
bool isOperator(char c);
bool validateExpression(const String& expr, String& err);
double evaluateExpression(const String& expr, bool& divByZero, int& num, int& den);
void handleAnsKey(bool wasJustCalc);
void handleSqKey();
void handleKeyPress(char key);
void muteAudio();
void checkBattery();

// ============================================================================
void setup() {
  WiFi.mode(WIFI_OFF);
  btStop();
  Serial.begin(115200);
  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();
  updateDisplay("     BraiVo", "   Calculator");
  // 1.1.1 Power On
  speakText("Calculator Ready");
  voice.say(sp2_READY);
  muteAudio();
  keypad.setDebounceTime(50);
  pinMode(BATT_PIN, INPUT);
}

void loop() {
  checkBattery();
  if (!keypad.getKeys()) return;

  int   active = 0;
  char  pressed = 0;

  for (int i = 0; i < LIST_MAX; i++) {
    if (keypad.key[i].kstate == PRESSED || keypad.key[i].kstate == HOLD) {
      if (keypad.key[i].kchar != ' ') active++;
    }
    if (keypad.key[i].stateChanged && keypad.key[i].kstate == PRESSED)
      pressed = keypad.key[i].kchar;
  }

  // 1.2.5 Multiple Keys Simultaneously
  if (active > 1) {
    speakText("Press one key at a time");
    voice.say(sp2_PRESS); voice.say(sp2_ONE); voice.say(sp2_TIME);
    muteAudio();
    return;
  }

  // SQ long-press detection
  for (int i = 0; i < LIST_MAX; i++) {
    if (keypad.key[i].kchar != 'Q') continue;
    if (keypad.key[i].stateChanged && keypad.key[i].kstate == PRESSED) {
      sqPressTime = millis(); sqPressed = true;
    } else if (keypad.key[i].stateChanged && keypad.key[i].kstate == RELEASED && sqPressed) {
      unsigned long dur = millis() - sqPressTime;
      sqPressed = false;
      if (dur >= 400) {
        handleSqKey();
      } else {
        // 1.2.4 Long press not detected
        speakText("Long press required");
        voice.say(sp3_ERROR);
        muteAudio();
      }
      return;
    }
  }

  if (pressed != 0 && pressed != ' ' && pressed != 'Q')
    handleKeyPress(pressed);
}

// ============================================================================
void handleKeyPress(char key) {
  bool wasJustCalc = justCalculated;
  justCalculated = false;

  // Handle continuing a calculation or starting fresh right after '='
  if (wasJustCalc) {
    if (isOperator(key)) {
      // Auto-insert previous answer
      String ansStr = (lastResult == (double)(long)lastResult)
                        ? String((long)lastResult)
                        : String(lastResult, 6);
      currentExpr = ansStr;
    } else if ((key >= '0' && key <= '9') || key == '(' || key == '.') {
      // Start fresh
      currentExpr = "";
    }
  }

  switch (key) {

    // 1.2.1 AC → "Cancel"
    case 'C':
      currentExpr = "";
      updateDisplay("", "");
      speakText("Canceled");
      voice.say(sp2_CANCEL);
      muteAudio();
      break;

    // 1.2.2 DEL → "Clear"
    case 'D':
      if (currentExpr.length() > 0) {
        currentExpr.remove(currentExpr.length() - 1);
        updateDisplay(currentExpr.length() > 16
                        ? currentExpr.substring(currentExpr.length() - 16)
                        : currentExpr, "");
        speakText("Clear");
        voice.say(sp3_CLEAR);
      } else {
        speakText("Cleared");
        voice.say(sp3_CLEAR);
      }
      muteAudio();
      break;

    // 1.2.3 SD / ANS key
    case 'S':
      handleAnsKey(wasJustCalc);
      if (wasJustCalc) {
        justCalculated = true; // Preserve state so next operator auto-inserts ANS
      }
      break;

    // Equals: validate → evaluate → announce result
    case '=': {
      justCalculated = true;

      if (currentExpr.length() == 0) {
        // 2.1.5 No expression
        updateDisplay("", "No expr entered");
        speakText("No expression entered.");
        voice.say(sp4_NO);
        voice.say(sp2_NUMBER);
        voice.say(sp2_ENTER);
        muteAudio();
        break;
      }

      String errMsg;
      if (!validateExpression(currentExpr, errMsg)) {
        String dLine1 = currentExpr.length() > 16
                          ? currentExpr.substring(currentExpr.length() - 16)
                          : currentExpr;
        String dLine2 = errMsg.length() > 16 ? errMsg.substring(0, 16) : errMsg;
        updateDisplay(dLine1, dLine2);
        speakText(errMsg);
        if (errMsg == "No operation entered.") {
          voice.say(sp4_NO);
          voice.say(sp2_OPERATOR);
          voice.say(sp2_ENTER);
        } else if (errMsg == "Incomplete Expression.") {
          voice.say(sp3_IN);
          voice.say(sp2_COMPLETE);
        } else if (errMsg == "Expression cannot begin with an operator.") {
          voice.say(sp4_NO);
          voice.say(sp2_START);
          voice.say(sp2_OPERATOR);
        } else if (errMsg == "Empty parentheses are not allowed.") {
          voice.say(sp4_NO);
          voice.say(sp2_NUMBER);
          voice.say(sp3_IN);
          voice.say(sp2_OPEN);
          voice.say(sp4_CLOSE);
        } else if (errMsg == "Missing closing parenthesis.") {
          voice.say(sp4_NO);
          voice.say(sp4_CLOSE);
        } else if (errMsg == "Missing opening parenthesis.") {
          voice.say(sp4_NO);
          voice.say(sp2_OPEN);
        } else if (errMsg == "Percentage requires a preceding number.") {
          voice.say(sp2_PERCENT);
          voice.say(sp4_NO);
          voice.say(sp2_NUMBER);
        } else if (errMsg == "Incomplete decimal number.") {
          voice.say(sp3_IN);
          voice.say(sp2_COMPLETE);
        } else if (errMsg == "Only one decimal point is allowed per number.") {
          voice.say(sp4_NO);
          voice.say(sp2_POINT);
          voice.say(sp2_POINT);
        } else if (errMsg == "Invalid operation. Two operators entered consecutively.") {
          voice.say(sp4_NO);
          voice.say(sp2_OPERATOR);
          voice.say(sp2_OPERATOR);
        } else {
          voice.say(sp3_ERROR);
        }
        muteAudio();
        break;
      }

      bool divByZero = false;
      int  num = 0, den = 1;
      double result = evaluateExpression(currentExpr, divByZero, num, den);

      if (divByZero) {
        // 2.6
        updateDisplay(currentExpr, "Undef: Div/0");
        speakText("Current Result: Undefined, cannot divide by zero.");
        voice.say(sp4_NO);
        voice.say(sp3_DIVIDED);
        voice.say(sp3_BY);
        voice.say(sp2_ZERO);
        muteAudio();
        break;
      }

      lastResult    = result;
      hasLastResult = true;
      lastNum       = num;
      lastDen       = den;
      isSdDecimal   = false;

      speakResultVoice(result, num, den);
      muteAudio();
      break;
    }

    // Numbers, operators, parentheses, decimal, percent
    default: {
      // Prevent operator (except minus) if expression is empty (Rule 2.1.5 and 2.7.4)
      if (currentExpr.length() == 0) {
        if (key == '+' || key == '*' || key == '/') {
          speakText("No expression entered.");
          voice.say(sp4_NO);
          voice.say(sp2_NUMBER);
          voice.say(sp2_ENTER);
          muteAudio();
          break;
        } else if (key == '%') {
          speakText("Percentage requires a preceding number.");
          voice.say(sp2_PERCENT);
          voice.say(sp4_NO);
          voice.say(sp2_NUMBER);
          muteAudio();
          break;
        }
      }

      currentExpr += key;
      String disp = currentExpr.length() > 16
                      ? currentExpr.substring(currentExpr.length() - 16)
                      : currentExpr;
      updateDisplay(disp, "");
      speakKey(key);
      muteAudio();
      break;
    }
  }
}

// ============================================================================
// Speak and display result (used after '=' and during SD toggle)
void speakResultVoice(double result, int num, int den) {
  // 2.7.1 Scientific notation
  bool isSci = (fabs(result) >= 1e9) || (result != 0.0 && fabs(result) < 1e-4);
  if (isSci) {
    int    expVal   = (int)floor(log10(fabs(result)));
    double mantissa = result / pow(10.0, expVal);
    String sciDisp  = String(mantissa, 2) + "e" + String(expVal);
    updateDisplay(currentExpr, "= " + sciDisp);
    String expWord  = (expVal < 0)
                        ? "negative " + String(-expVal)
                        : String(expVal);
    speakText("Current Result: " + String(mantissa, 1)
              + " multiplied to ten raised to the " + expWord + " power.");
    speakNumber((long)round(mantissa));
    voice.say(sp3_TIMES);
    voice.say(sp2_TEN);
    voice.say(sp5_RAISE);
    voice.say(sp4_TO);
    if (expVal < 0) {
      voice.say(sp3_NEGATIVE);
      speakNumber(-expVal);
    } else {
      speakNumber(expVal);
    }
    voice.say(sp2_POWER);
    return;
  }

  // Integer result
  bool isInt = (result == (double)(long)result);
  if (isInt) {
    String rs = String((long)result);
    updateDisplay(currentExpr, "= " + rs);
    speakText("Current Result: " + rs + ".");
    speakNumber((long)result);
    return;
  }

  // Decimal result — check if repeating
  FracInfo fi      = getFracInfo(abs(num), abs(den));
  bool isRepeating = (fi.cycleLen > 0);
  bool withinFour  = isRepeating && ((fi.startPos + fi.cycleLen) <= 4);
  bool repeating   = isRepeating && withinFour;
  bool approx      = isRepeating && !withinFour;

  if (isRepeating) {
    // Show as simplified fraction on LCD first (spec 2.7.2)
    int g    = calcGcd(abs(num), abs(den));
    int sNum = num / g;
    int sDen = abs(den) / g;
    if (result < 0 && sNum > 0) sNum = -sNum;
    updateDisplay(currentExpr, "= " + String(sNum) + "/" + String(sDen));
  } else {
    String ds = String(fabs(result), 6);
    if (result < 0) ds = "-" + ds;
    updateDisplay(currentExpr, "= " + ds);
  }

  // Build serial log: "zero point three three three three, repeating."
  double absRes = fabs(result);
  long   intPart = (long)absRes;
  double frac   = absRes - (double)intPart;
  String digs   = "";
  double tf     = frac;
  for (int i = 0; i < 4; i++) {
    tf *= 10; int d = (int)tf; if (d > 9) d = 9; tf -= d;
    digs += String(d); if (i < 3) digs += " ";
  }
  String intWord = (intPart == 0) ? "zero" : String(intPart);
  String prompt  = "Current Result: ";
  if (result < 0) prompt += "negative ";
  prompt += intWord + " point " + digs;
  if (repeating) prompt += ", repeating.";
  else if (approx) prompt += ", approximately.";
  else prompt += ".";
  speakText(prompt);

  // Voice output
  speakDecimalVoice(absRes, repeating);
}

// ============================================================================
// 1.2.3 ANS key / 2.7.2 S-D toggle
void handleAnsKey(bool wasJustCalc) {
  if (!hasLastResult) {
    // 2.5 No previous answer
    speakText("No previous answer available.");
    voice.say(sp4_NO);
    voice.say(sp3_ANSWER);
    muteAudio();
    return;
  }

  if (wasJustCalc) {
    // S-D toggle: switch between fraction view and decimal view
    isSdDecimal = !isSdDecimal;
    if (isSdDecimal) {
      // Switch to decimal — re-run announcement using decimal display
      FracInfo fi     = getFracInfo(abs(lastNum), abs(lastDen));
      bool isRepeating = (fi.cycleLen > 0);
      bool withinFour  = isRepeating && ((fi.startPos + fi.cycleLen) <= 4);
      bool repeating   = isRepeating && withinFour;
      bool approx      = isRepeating && !withinFour;

      double absRes  = fabs(lastResult);
      long   intPart = (long)absRes;
      double frac    = absRes - (double)intPart;
      String digs    = "";
      double tf = frac;
      for (int i = 0; i < 4; i++) {
        tf *= 10; int d = (int)tf; if (d > 9) d = 9; tf -= d;
        digs += String(d); if (i < 3) digs += " ";
      }
      String decStr = String(absRes, 4);
      if (lastResult < 0) decStr = "-" + decStr;
      updateDisplay("", "= " + decStr);

      String intWord = (intPart == 0) ? "zero" : String(intPart);
      String prompt  = "Current Result: ";
      if (lastResult < 0) prompt += "negative ";
      prompt += intWord + " point " + digs;
      if (repeating)  prompt += ", repeating.";
      else if (approx) prompt += ", approximately.";
      else            prompt += ".";
      speakText(prompt);
      speakDecimalVoice(absRes, repeating);

    } else {
      // Switch back to fraction / integer
      if (lastDen != 1 && lastDen != 0) {
        int g    = calcGcd(abs(lastNum), abs(lastDen));
        int sNum = lastNum / g;
        int sDen = abs(lastDen) / g;
        if (lastResult < 0 && sNum > 0) sNum = -sNum;
        String fs = String(sNum) + "/" + String(sDen);
        updateDisplay("", "= " + fs);
        speakText("Current Result: " + fs + ".");
        speakNumber(sNum);
        voice.say(sp3_DIVIDED);
        speakNumber(sDen);
      } else {
        String rs = String((long)lastResult);
        updateDisplay("", "= " + rs);
        speakText("Current Result: " + rs + ".");
        speakNumber((long)lastResult);
      }
    }
  } else {
    // ANS: insert previous result into expression
    String ansStr = (lastResult == (double)(long)lastResult)
                      ? String((long)lastResult)
                      : String(lastResult, 6);
    currentExpr += ansStr;
    String disp = currentExpr.length() > 16
                    ? currentExpr.substring(currentExpr.length() - 16)
                    : currentExpr;
    updateDisplay(disp, "Ans=" + ansStr);
    speakText("Previous Answer Inserted: " + ansStr + ".");
    voice.say(sp3_ANSWER);
    speakNumber((long)lastResult);
  }
  muteAudio();
}

// ============================================================================
// 1.3 Status Query (SQ long press)
void handleSqKey() {
  if (currentExpr.length() > 0) {
    String errMsg;
    bool   isValid  = validateExpression(currentExpr, errMsg);
    String spoken   = getSpokenExpr(currentExpr);
    // 1.3.1
    speakText("Current Expression: " + spoken + ".");
    speakExprVoice(currentExpr);
    // 1.3.2
    if (!isValid) {
      speakText("The working expression has an error: " + errMsg);
      if (errMsg == "No operation entered.") {
        voice.say(sp4_NO);
        voice.say(sp2_OPERATOR);
        voice.say(sp2_ENTER);
      } else if (errMsg == "Incomplete Expression.") {
        voice.say(sp3_IN);
        voice.say(sp2_COMPLETE);
      } else if (errMsg == "Expression cannot begin with an operator.") {
        voice.say(sp4_NO);
        voice.say(sp2_START);
        voice.say(sp2_OPERATOR);
      } else if (errMsg == "Empty parentheses are not allowed.") {
        voice.say(sp4_NO);
        voice.say(sp2_NUMBER);
        voice.say(sp3_IN);
        voice.say(sp2_OPEN);
        voice.say(sp4_CLOSE);
      } else if (errMsg == "Missing closing parenthesis.") {
        voice.say(sp4_NO);
        voice.say(sp4_CLOSE);
      } else if (errMsg == "Missing opening parenthesis.") {
        voice.say(sp4_NO);
        voice.say(sp2_OPEN);
      } else if (errMsg == "Percentage requires a preceding number.") {
        voice.say(sp2_PERCENT);
        voice.say(sp4_NO);
        voice.say(sp2_NUMBER);
      } else if (errMsg == "Incomplete decimal number.") {
        voice.say(sp3_IN);
        voice.say(sp2_COMPLETE);
      } else if (errMsg == "Only one decimal point is allowed per number.") {
        voice.say(sp4_NO);
        voice.say(sp2_POINT);
        voice.say(sp2_POINT);
      } else if (errMsg == "Invalid operation. Two operators entered consecutively.") {
        voice.say(sp4_NO);
        voice.say(sp2_OPERATOR);
        voice.say(sp2_OPERATOR);
      } else {
        voice.say(sp3_ERROR);
      }
    }
  } else if (hasLastResult) {
    // 1.3.3
    String rs = (lastResult == (double)(long)lastResult)
                  ? String((long)lastResult)
                  : String(lastResult, 4);
    speakText("Current Result: " + rs + ".");
    speakNumber((long)lastResult);
  } else {
    speakText("No expression entered.");
    voice.say(sp4_NO);
    voice.say(sp2_NUMBER);
    voice.say(sp2_ENTER);
  }
  muteAudio();
}

// ============================================================================
// 1.1.3 Battery Low
void checkBattery() {
  // Battery feature removed as requested
}

// ============================================================================
// DAC mute (prevents idle hum/vibration on speaker)
void muteAudio() {
  dacWrite(25, 0);
  dacDisable(25);
}



// ============================================================================
// Voice helpers

void speakText(const String& msg) {
  Serial.print("[VOICE]: \""); Serial.print(msg); Serial.println("\"");
}

// Speak 4 decimal digits one-by-one from absResult (e.g. 0.3333 → "three three three three")
void speakDecimalVoice(double absResult, bool repeating) {
  long   intPart = (long)absResult;
  double frac    = absResult - (double)intPart;

  // Integer part
  if (intPart == 0) voice.say(sp2_ZERO);
  else              speakNumber(intPart);

  voice.say(sp2_POINT);

  // 4 decimal digits individually
  for (int i = 0; i < 4; i++) {
    frac *= 10;
    int d = (int)frac;
    if (d > 9) d = 9;
    frac -= d;
    speakKey('0' + d);
  }

  // Suffix: "repeating" if applicable
  // sp2_REPEAT says "repeat" — closest available in Talkie
  if (repeating) voice.say(sp2_REPEAT);
  // "approximately" has no Talkie match — omit audio, text printed to Serial
}

String getSpokenExpr(const String& expr) {
  String out = "";
  for (size_t i = 0; i < expr.length(); i++) {
    switch (expr[i]) {
      case '0': out += "zero "; break;
      case '1': out += "one "; break;
      case '2': out += "two "; break;
      case '3': out += "three "; break;
      case '4': out += "four "; break;
      case '5': out += "five "; break;
      case '6': out += "six "; break;
      case '7': out += "seven "; break;
      case '8': out += "eight "; break;
      case '9': out += "nine "; break;
      case '+': out += "plus "; break;
      case '-': out += "minus "; break;
      case '*': out += "times "; break;
      case '/': out += "divided by "; break;
      case '.': out += "point "; break;
      case '(': out += "open parenthesis "; break;
      case ')': out += "close parenthesis "; break;
      case '%': out += "percent "; break;
    }
  }
  return out;
}

void speakExprVoice(const String& expr) {
  for (size_t i = 0; i < expr.length(); i++) {
    switch (expr[i]) {
      case '0': voice.say(sp2_ZERO); break;
      case '1': voice.say(sp2_ONE); break;
      case '2': voice.say(sp2_TWO); break;
      case '3': voice.say(sp2_THREE); break;
      case '4': voice.say(sp2_FOUR); break;
      case '5': voice.say(sp2_FIVE); break;
      case '6': voice.say(sp2_SIX); break;
      case '7': voice.say(sp2_SEVEN); break;
      case '8': voice.say(sp2_EIGHT); break;
      case '9': voice.say(sp2_NINE); break;
      case '+': voice.say(sp2_PLUS); break;
      case '-': voice.say(sp2_MINUS); break;
      case '*': voice.say(sp3_TIMES); break;
      case '/': voice.say(sp3_DIVIDED); break;
      case '.': voice.say(sp2_POINT); break;
      case '(': voice.say(sp2_OPEN); break;
      case ')': voice.say(sp4_CLOSE); break;
      case '%': voice.say(sp2_PERCENT); break;
    }
  }
}

void speakKey(char key) {
  switch (key) {
    case '0': voice.say(sp2_ZERO); break;
    case '1': voice.say(sp2_ONE); break;
    case '2': voice.say(sp2_TWO); break;
    case '3': voice.say(sp2_THREE); break;
    case '4': voice.say(sp2_FOUR); break;
    case '5': voice.say(sp2_FIVE); break;
    case '6': voice.say(sp2_SIX); break;
    case '7': voice.say(sp2_SEVEN); break;
    case '8': voice.say(sp2_EIGHT); break;
    case '9': voice.say(sp2_NINE); break;
    case '+': voice.say(sp2_PLUS); break;
    case '-': voice.say(sp2_MINUS); break;
    case '*': voice.say(sp3_TIMES); break;
    case '/': voice.say(sp3_DIVIDED); break;
    case '.': voice.say(sp2_POINT); break;
    case '(': speakText("Open parenthesis"); voice.say(sp2_OPEN); break;
    case ')': speakText("Close parenthesis"); voice.say(sp4_CLOSE); break;
    case '%': speakText("Percent"); voice.say(sp2_PERCENT); break;
  }
}

void speakNumber(long num) {
  if (num < 0) { voice.say(sp2_MINUS); num = -num; }
  if (num == 0) { voice.say(sp2_ZERO); return; }
  if (num >= 1000000) { speakNumber(num / 1000000); voice.say(sp3_MILLION); num %= 1000000; }
  if (num >= 1000)    { speakNumber(num / 1000);    voice.say(sp2_THOUSAND); num %= 1000; }
  if (num >= 100)     { speakNumber(num / 100);     voice.say(sp2_HUNDRED);  num %= 100; }
  if (num >= 20) {
    switch ((num / 10) * 10) {
      case 20: voice.say(sp2_TWENTY); break;
      case 30: voice.say(sp3_THIRTY); break;
      case 40: voice.say(sp3_FOURTY); break;
      case 50: voice.say(sp3_FIFTY);  break;
      case 60: voice.say(sp3_SIXTY);  break;
      case 70: voice.say(sp3_SEVENTY);break;
      case 80: voice.say(sp3_EIGHTY); break;
      case 90: voice.say(sp3_NINETY); break;
    }
    num %= 10;
  } else if (num >= 10) {
    switch (num) {
      case 10: voice.say(sp2_TEN);       break;
      case 11: voice.say(sp2_ELEVEN);    break;
      case 12: voice.say(sp2_TWELVE);    break;
      case 13: voice.say(sp3_THIRTEEN);  break;
      case 14: voice.say(sp3_FOURTEEN);  break;
      case 15: voice.say(sp3_FIFTEEN);   break;
      case 16: voice.say(sp3_SIXTEEN);   break;
      case 17: voice.say(sp3_SEVENTEEN); break;
      case 18: voice.say(sp3_EIGHTEEN);  break;
      case 19: voice.say(sp3_NINETEEN);  break;
    }
    return;
  }
  if (num > 0) speakKey('0' + (char)num);
}

void updateDisplay(const String& l1, const String& l2) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(l1.length() > 16 ? l1.substring(l1.length() - 16) : l1);
  lcd.setCursor(0, 1);
  lcd.print(l2.length() > 16 ? l2.substring(0, 16) : l2);
}

// ============================================================================
// Expression helpers
bool isArithOp(char c) { return c == '+' || c == '-' || c == '*' || c == '/'; }
bool isOperator(char c) { return isArithOp(c) || c == '%'; }

// ============================================================================
// Validation — all 12 spec error rules
bool validateExpression(const String& expr, String& err) {
  int len = expr.length();
  if (len == 0) { err = "No expression entered."; return false; }

  // 2.1.3 Starts with forbidden operator
  if (expr[0] == '+' || expr[0] == '*' || expr[0] == '/' || expr[0] == '%') {
    err = "Expression cannot begin with an operator."; return false;
  }
  // 2.1.4 Ends with operator or 2.2.2 ends with decimal
  if (isArithOp(expr[len - 1])) { err = "Incomplete Expression."; return false; }
  if (expr[len - 1] == '.')      { err = "Incomplete decimal number."; return false; }

  int  parenDepth = 0;
  bool hasOp      = false;
  bool hasDecimal = false;
  int  consOps    = 0;

  for (int i = 0; i < len; i++) {
    char c = expr[i];

    if (isArithOp(c)) {
      hasOp = true;
      hasDecimal = false;   // reset decimal tracking for next number
      consOps++;
      // 2.1.1 Multiple consecutive operators
      if (consOps >= 2) {
        String w = (consOps == 2) ? "Two" : (consOps == 3) ? "Three" : "Multiple";
        err = "Invalid operation. " + w + " operators entered consecutively.";
        return false;
      }
      // 2.2.2 Operator right after decimal point
      if (i > 0 && expr[i - 1] == '.') { err = "Incomplete decimal number."; return false; }
    } else if (c == '%') {
      hasOp = true;
      consOps   = 0;
      hasDecimal = false;
      // 2.4 % needs a preceding number
      if (i == 0 || isOperator(expr[i - 1]) || expr[i - 1] == '(') {
        err = "Percentage requires a preceding number."; return false;
      }
    } else {
      consOps = 0;
    }

    if (c == '.') {
      // 2.2.1 Two decimals in same number
      if (hasDecimal) { err = "Only one decimal point is allowed per number."; return false; }
      hasDecimal = true;
    } else if (c == '(') {
      parenDepth++;
      hasDecimal = false;
      // 2.3.1 Empty parentheses
      if (i < len - 1 && expr[i + 1] == ')') {
        err = "Empty parentheses are not allowed."; return false;
      }
    } else if (c == ')') {
      parenDepth--;
      // 2.3.2 Extra closing paren
      if (parenDepth < 0) { err = "Missing opening parenthesis."; return false; }
    }
  }

  // 2.3.2 Unclosed parenthesis
  if (parenDepth > 0) { err = "Missing closing parenthesis."; return false; }
  // 2.1.2 No operator at all
  if (!hasOp) { err = "No operation entered."; return false; }

  return true;
}

// ============================================================================
// Shunting-Yard evaluator — handles PEMDAS and parentheses correctly
// % as postfix is pre-processed to /100
double evaluateExpression(const String& expr, bool& divByZero, int& num, int& den) {
  divByZero = false;

  // Pre-process: postfix % → /100
  String processed = "";
  for (int i = 0; i < (int)expr.length(); i++) {
    if (expr[i] == '%') {
      bool postfix = (i == (int)expr.length() - 1)
                     || isArithOp(expr[i + 1])
                     || expr[i + 1] == ')';
      if (postfix) processed += "/100";
      else         processed += '%';
    } else {
      processed += expr[i];
    }
  }

  std::vector<double> output;
  std::vector<char>   opStack;

  auto prec = [](char op) -> int {
    return (op == '*' || op == '/' || op == '%') ? 2 : 1;
  };

  auto applyOp = [&](char op) {
    if (output.size() < 2) return;
    double b = output.back(); output.pop_back();
    double a = output.back(); output.pop_back();
    if      (op == '+') output.push_back(a + b);
    else if (op == '-') output.push_back(a - b);
    else if (op == '*') output.push_back(a * b);
    else if (op == '/') {
      if (fabs(b) < 1e-12) { divByZero = true; output.push_back(0); return; }
      num = (int)round(a); den = (int)round(b);
      output.push_back(a / b);
    }
    else if (op == '%') output.push_back(fmod(a, b));
  };

  int pLen = processed.length();
  int i    = 0;

  while (i < pLen) {
    char c = processed[i];

    if (isdigit(c) || c == '.') {
      String ns = "";
      while (i < pLen && (isdigit(processed[i]) || processed[i] == '.'))
        ns += processed[i++];
      output.push_back(ns.toDouble());
      continue;
    }

    if (c == '(') {
      opStack.push_back(c);
    } else if (c == ')') {
      while (!opStack.empty() && opStack.back() != '(') {
        applyOp(opStack.back()); opStack.pop_back();
      }
      if (!opStack.empty()) opStack.pop_back();   // discard '('
    } else if (isArithOp(c)) {
      // Handle unary minus (start of expression or right after '(')
      if (c == '-' && (output.empty() ||
          (!opStack.empty() && opStack.back() == '('))) {
        output.push_back(0.0);   // 0 − x trick
      }
      while (!opStack.empty() && opStack.back() != '(' &&
             prec(opStack.back()) >= prec(c)) {
        applyOp(opStack.back()); opStack.pop_back();
      }
      opStack.push_back(c);
    }

    i++;
  }

  while (!opStack.empty()) {
    applyOp(opStack.back()); opStack.pop_back();
  }

  if (divByZero) return 0;

  double result = output.empty() ? 0 : output[0];

  // If no division happened, reset den=1 so fraction display is skipped
  if (den == 0) den = 1;

  return result;
}
