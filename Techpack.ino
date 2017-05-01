// Location tracking reported to a Phant server over GSM
// RFID assets reported to a Phant server over GSM
// LED and button controls
// Emergency SMS sent on command
//
// Author: Braden Kallin
//
// Inspired by the code from Marco Schwartz and Tony DiCola

// Libraries
#include "Adafruit_FONA.h"
#include <Adafruit_NeoPixel.h>
#include <Adafruit_PN532.h>
#include <avr/wdt.h>
#include <avr/power.h>
#include <EEPROM.h>
#include <SoftwareSerial.h>
#include <SPI.h>
#include <Wire.h>

// Lights, buzzer and buttons 
#define BUZZER_GND 4
#define BUZZER     5
#define PIXEL_PIN  6
#define BUTTON1    A0 // Labled 3
#define BUTTON2    A1 // Labled 1
#define BUTTON3    A2 // Labled 2
#define BUTTON4    A3 // Labled 4

// Define I2C Pins for PN532
#define PN532_IRQ   (2)
#define PN532_RESET (3)  // Not connected by default on the NFC Shield

// FONA pins
#define FONA_VREF   7   // A pin tied high for IO reference
#define FONA_RST    8   // FONA serial RX pin (pin 2 for shield).
#define FONA_RX     9   // FONA serial TX pin (pin 3 for shield).
#define FONA_TX     10  // FONA reset pin (pin 4 for shield)
#define FONA_KEY    11  // FONA power button
#define FONA_RI     12  // FONA ringer
#define FONA_PS     13  // FONA power status

// NeoPixel instance
Adafruit_NeoPixel strip = Adafruit_NeoPixel(7, PIXEL_PIN, NEO_GRB + NEO_KHZ800);

// PN532 instance
Adafruit_PN532 nfc(PN532_IRQ, PN532_RESET);

// FONA instance & configuration
SoftwareSerial fonaSS = SoftwareSerial(FONA_TX, FONA_RX); 
Adafruit_FONA fona = Adafruit_FONA(FONA_RST);

// GPS variables
float latitude, longitude, 
      speed_kph, heading, altitude = 0;
bool  gpsSuccess = false;
int   fix = 0;

// RFID variables
const uint8_t numTags = 5;
const uint8_t UIDs[numTags][7] = {{0x5B, 0x1B, 0x4C, 0x21, 0, 0, 0}, 
                                  {0x8B, 0x31, 0x4C, 0x21, 0, 0, 0},
                                  {0xFB, 0x27, 0x4C, 0x21, 0, 0, 0}, 
                                  {0xEB, 0x17, 0x48, 0x21, 0, 0, 0},
                                  {0xAB, 0x19, 0x4C, 0x21, 0, 0, 0}};
uint8_t UIDstatus[numTags] = {0, 0, 0, 0, 0};
bool tagFlag = false;

// Timing vars (in ms)
const unsigned long statusTime    = 5000;
const unsigned long SMSTime       = 5000;
const unsigned long updateLocTime = 60000;
const unsigned long tagTime       = 10000;

// Misc vars
bool lightsOn = false;
bool emergencyFlag = false;
bool bagPower = true;

void setup() {
  // WDT because there's no reset button
  wdt_enable(WDTO_8S);
  wdt_reset();

  // Initialize serial output.
  Serial.begin(115200);
  Serial.println(F("Geotracking with FONA808"));

  // Setup buttons and pins.
  setupIO();

  // Start light strip.
  strip.begin();

  // Lights to keep track of startup state.
  statusLights(1);
  
  setupRFID();
  wdt_reset();
  
  statusLights(2);

  setupFONA();
  wdt_reset();

  statusLights(3);

  // Try 4 times to post location on startup.
  for (uint8_t i = 0; i < 4; i++) {
    if (getLocation()) {
      if (postLocation()) {
        break;
      }
    }
    wdt_reset();
  }

  statusLights(3);

  // Try 4 times to post assets on startup.
  for(uint8_t i = 0; i < 4; i++){
      if(postRFID()) break;
      wdt_reset();
  }

  clearStrip();
}

void loop() {
  // Timing variables.
  unsigned long        currentStatusMillis  = 0;
  static unsigned long previousStatusMillis = millis();
  unsigned long        currentLocMillis     = 0;
  static unsigned long previousLocMillis    = millis();
  static unsigned long tagStartMillis       = millis();
  unsigned long        currentSMSMillis     = 0;
  static unsigned long previousSMSMillis    = millis();
  static bool          tagTimerFlag = false;

  wdt_reset();

  checkButtons();

  if(bagPower){
    checkRFID();

    if(lightsOn){
      setStripColor(100,100,100);
    }
    else{
      clearStrip();
    }

    // Let the terminal know we're still running every few seconds
    currentStatusMillis = millis();
    if (currentStatusMillis - previousStatusMillis >= statusTime) {
      Serial.println(F("Still running!"));
      previousStatusMillis = currentStatusMillis;
    }

    // When a tag is read, wait a few seconds then update the database
    if(tagFlag){
      tagStartMillis = millis();
      tagFlag = false;
      tagTimerFlag = true;
    }
    if(tagTimerFlag && (millis() - tagStartMillis >= tagTime)){
      for(uint8_t i = 0; i < 2; i++){ // try to update twice
        if(postRFID()) break;
      }
      tagTimerFlag = false;
    }

    // Update the location every few minutes
    // Also reset the emergency flag if set
    currentLocMillis = millis();
    if (currentLocMillis - previousLocMillis >= updateLocTime) {
      // Try 2 times to post location.
      for (uint8_t i = 0; i < 2; i++) {
        if (getLocation()) {
          if (postLocation()) {
            break;
          }
        }
        wdt_reset();
        delay(1000);
      }
      previousLocMillis = currentLocMillis;
      emergencyFlag = false;
    }
  }

  // Check for lost bag mode every few seconds
  currentSMSMillis = millis();
  if(currentSMSMillis - previousSMSMillis >= SMSTime){
    checkLostBag();
    previousSMSMillis = currentSMSMillis;
  }
}

// ============================================================================
// ============================== RFID FUNCTIONS ==============================
// ============================================================================

// Read RFID cards. If they match a registered tag, update check-in/out status
void checkRFID(void) {
  uint8_t success;
  uint8_t uid[] = { 0, 0, 0, 0, 0, 0, 0 };  // Buffer to store the returned UID
  uint8_t uidLength;                        // Length of the UID 
  uint8_t timeoutTime = 10;                 // Timeout after 10ms

  // Wait for an ISO14443A type cards (Mifare, etc.).  When one is found
  // 'uid' will be populated with the UID, and uidLength will indicate
  // if the uid is 4 bytes (Mifare Classic) or 7 bytes (Mifare Ultralight)
  success = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, 
                                    &uidLength, timeoutTime);

  if (success) {
    // Display some basic information about the card
    Serial.println(F("Found a card"));
    Serial.print(F("  UID Length: "));
    Serial.print(uidLength, DEC);
    Serial.println(F(" bytes"));
    Serial.print(F("  UID Value: "));
    nfc.PrintHex(uid, uidLength);
    Serial.println(F(""));

    if (uidLength == 4)
    {
      // Toggle the tag in or out if it's one of the registered tags
      uint8_t i = 0;
      uint8_t j = 0;
      boolean match;
      for (i = 0; i < numTags; i++) {
        match = true;
        for (j = 0; j < 7; j++) {
          if (UIDs[i][j] != uid[j]) {
            match = false;
          }
        }
        if (match) {
          UIDstatus[i] = UIDstatus[i] ^ 0xFF; 
          EEPROM.update(i, UIDstatus[i]); // store tag status in EEPROM
          Serial.println(F("")); //indicate check-in/out to the terminal
          Serial.print(F("Tag "));
          Serial.print(i);
          Serial.print(F(" status: "));
          Serial.println(EEPROM.read(i));
          tagFlag = true;

          if(UIDstatus[i] == 0){
            blinkStrip(20,0,0); //indicate check-in/out to the user
          }
          else{
            blinkStrip(0,20,0);
          }
        }
      }

      // wait a little bit before reading again.
      delay(1000);

    }
  }
}

// Post tag statuses to data stream
bool postRFID(void) {
  //Sparkfun URL Building
  const String publicKey = F("7vz3gGRdAnFlGJYdbpZ8"); //Public Key for stream
  const String privateKey = F("mqrWmkvRVxTRGq1a8bjd"); //Private Key for stream
  const byte NUM_FIELDS = 5; //number of fields in data stream
  const String fieldNames[NUM_FIELDS] = {F("tag0"), F("tag1"), F("tag2"), 
                                         F("tag3"), F("tag4")}; // data fields
  bool success = true;
  uint16_t statuscode;
  int16_t length;

  wdt_reset();

  // let the user know the fona is trying to update the data stream
  strip.setPixelColor(3,strip.Color(20,0,20));
  strip.show();

  // url building
  String url = "http://data.sparkfun.com/input/";
  url += publicKey;
  url += F("?private_key=");
  url += privateKey;
  for (uint8_t i_url = 0; i_url < NUM_FIELDS; i_url++) {
    url += F("&");
    url += fieldNames[i_url];
    url += F("=");
    url += EEPROM.read(i_url);
  }
  url += F(" ");
  char buf[255];
  url.toCharArray(buf, url.length());

  Serial.println(buf);

  wdt_disable(); // next step takes a while

  // Attempt to post the update
  if (!fona.HTTP_GET_start(buf, &statuscode, (uint16_t *)&length)) {
    Serial.println(F("Failed to post tags!"));
    success = false;
    wdt_enable(WDTO_8S); // stuff gets caught up here sometimes tho
  }
  while (length > 0) {
    while (fona.available()) {
      char c = fona.read();
      Serial.write(c);
      length--;
    }
  }
  fona.HTTP_GET_end();

  wdt_reset();

  if(success){
    colorWipe(0,30,0);
    Serial.println(F("Posted tags!"));
  }
  else colorWipe(30,0,0);
  
  return success;
}

// ============================================================================
// =============================== GPS FUNCTIONS ==============================
// ============================================================================

// Location handling
bool getLocation(void) {
  strip.setPixelColor(3,strip.Color(0,20,20));
  strip.show();
  
  // Grab a GPS reading.
  wdt_reset();
  bool gpsSuccess = fona.getGPS(&fix, &latitude, &longitude, 
                                &speed_kph, &heading, &altitude);
  wdt_reset();

  // Return true if a GPS lock is acquired
  if (gpsSuccess && fix == 1) {
    return true;
  }
  else {
    Serial.println(F("No Fix."));
    colorWipe(30,0,0);
    return false;
  }
}

// Location reporting
bool postLocation(void) {
  //Sparkfun URL Building
  const String publicKey = F("5JDdvbVgx6urREAVgKOM"); //Public Key for stream
  const String privateKey = F("7BEe4kl6xRC7jo2neKrx"); //Private Key for stream
  const byte NUM_FIELDS = 2; //number of fields in data stream
  const String fieldNames[NUM_FIELDS] = {"lat", "long"}; // data fields
  float fieldData[NUM_FIELDS]; //holder for the data values
  bool success = true;

  wdt_reset();

  strip.setPixelColor(3, strip.Color(0,40,40));
  strip.show();

  fieldData[0] = latitude;
  fieldData[1] = longitude;

  Serial.print(F("Lat: "));
  printFloat(latitude, 5);
  Serial.println(F(""));

  Serial.print(F("Long: "));
  printFloat(longitude, 5);
  Serial.println(F(""));

  uint16_t statuscode;
  int16_t length;
  String url = "http://data.sparkfun.com/input/";
  url += publicKey;
  url += F("?private_key=");
  url += privateKey;
  for (uint8_t i_url = 0; i_url < NUM_FIELDS; i_url++) {
    url += F("&");
    url += fieldNames[i_url];
    url += F("=");
    url += String(fieldData[i_url], 6);
  }
  url += F(" ");
  char buf[255];
  url.toCharArray(buf, url.length());

  Serial.println(buf);

  wdt_disable();

  if (!fona.HTTP_GET_start(buf, &statuscode, (uint16_t *)&length)) {
    Serial.println(F("Failed to post location!"));
    success = false;
    colorWipe(30,0,0);
    wdt_enable(WDTO_8S); // stuff gets caught up here sometimes
  }
  while (length > 0) {
    while (fona.available()) {
      char c = fona.read();
      Serial.write(c);
      length--;
    }
  }
  
  fona.HTTP_GET_end();

  wdt_reset();

  colorWipe(0,30,0);
  Serial.println(F("Posted location."));
  return success;
}

void checkLostBag(void){
  int8_t numSMS = fona.getNumSMS();
  if(numSMS != 0){
    if(!emergencyFlag){ // Don't flash these lights in emergency mode
      lostBagFlash();
    }

    clearStrip();
    
    fona.deleteAllSMS();
  }
}

// ============================================================================
// ============================== LIGHT FUNCTIONS =============================
// ============================================================================

// Clear the LED strip
void clearStrip(void){
  for(uint8_t i = 0; i < 7; i++){
    strip.setPixelColor(i, strip.Color(0,0,0));
  }
  strip.show();
}

// Power lights
void pwrLights(bool pwr){
  if(pwr){
    for(uint8_t i = 0; i < 4; i++){
      strip.setPixelColor(3-i, strip.Color(0,0,40));
      strip.setPixelColor(3+i, strip.Color(0,0,40));
      strip.show();
      delay(500);
    }
    clearStrip();
  }
  else{
    setStripColor(40,0,0);
    for(uint8_t i = 0; i < 4; i++){
      strip.setPixelColor(6-i, strip.Color(0,0,0));
      strip.setPixelColor(i, strip.Color(0,0,0));
      strip.show();
      delay(500);
    }
  }
}

// Blink the LED strip
void blinkStrip(uint8_t r, uint8_t g, uint8_t b){
  for(uint8_t i = 0; i < 6; i++){
    for(uint8_t j = 0; j < 7; j++){
      if(i%2==0){
        strip.setPixelColor(j, strip.Color(r,g,b));
        strip.show();
      }
      else {
        strip.setPixelColor(j, strip.Color(0,0,0));
        strip.show();
      }
    }
    delay(100);
  }
}

// Solid strip color
void setStripColor(uint8_t r, uint8_t g, uint8_t b){
  for(uint8_t i = 0; i < 7; i++){
    strip.setPixelColor(i, strip.Color(r,g,b));
    strip.show();
  }
}

// Single LEDs emanating from the center.
void colorWipe(uint8_t r, uint8_t g, uint8_t b){
  for(uint8_t i = 0; i < 3; i++){
    strip.setPixelColor(2-i, strip.Color(r,g,b));
    strip.setPixelColor(4+i, strip.Color(r,g,b));
    strip.show();
    delay(100);
  }
  for(uint8_t i = 0; i < 3; i++){
    strip.setPixelColor(2-i, strip.Color(0,0,0));
    strip.setPixelColor(4+i, strip.Color(0,0,0));
    strip.show();
    delay(100);
  }
}

// Emanate colors from the center
// Also beep a buzzer
void lostBagFlash(void){
  uint8_t pwrButtonState;
  static uint8_t lastPwrButtonState = LOW;
  
  for(uint8_t i = 0; i < 25; i++){
    for(uint8_t j = 4; j > 0; j--){
      uint8_t a, b, c;
      a = i % 3;
      b = (i+1) % 3;
      c = (i+2) % 3;
      
      strip.setPixelColor(j-1, strip.Color(a*50,b*50,c*50));
      strip.setPixelColor(7-j, strip.Color(a*50,b*50,c*50));
      strip.show();
      delay(100);
      wdt_reset();
    }
    if(i % 2 == 1){
      digitalWrite(BUZZER, HIGH);
    }
    else{
      digitalWrite(BUZZER, LOW);
    }
    pwrButtonState = digitalRead(BUTTON2); // Stop if the power button is pressed
    
    if(pwrButtonState == LOW && lastPwrButtonState == HIGH)
      break;
      
    lastPwrButtonState = pwrButtonState;
  }
  digitalWrite(BUZZER, LOW);
}

// ============================================================================
// ============================== SETUP FUNCTIONS =============================
// ============================================================================
void setupFONA(void) {
  pinMode(FONA_RI, INPUT);
  pinMode(FONA_PS, INPUT);
  pinMode(FONA_KEY, OUTPUT);
  pinMode(FONA_VREF, OUTPUT);

  digitalWrite(FONA_VREF, HIGH);

  if (FONA_PS == LOW) {
    digitalWrite(FONA_KEY, LOW);
    wdt_reset();
    delay(2100);
    digitalWrite(FONA_KEY, HIGH);
  }

  wdt_disable();
  // Initialize the FONA module
  Serial.println(F("Initializing FONA....(may take 10 seconds)"));
  fonaSS.begin(4800);

  if (!fona.begin(fonaSS)) {
    halt(F("Couldn't find FONA"));
  }

  fonaSS.println(F("AT+CMEE=2"));
  Serial.println(F("FONA is OK"));

  wdt_enable(WDTO_8S);
  // Enable GPS.
  while (!fona.enableGPS(true)) {delay(1000); wdt_reset();}

  wdt_reset();
  // Enable GPRS
  while (!fona.enableGPRS(true)) {delay(1000); wdt_reset();}

  wdt_reset();
  // Wait a little bit to stabilize the connection.
  delay(500);
  Serial.println(F("FONA setup OK!"));
}

void setupRFID(void) {
  // Start the rfid driver
  nfc.begin();

  // Check for the PN532 chip
  uint32_t versiondata = nfc.getFirmwareVersion();
  if (! versiondata) {
    halt(F("Couldn't find FONA")); // halt
  }
  // Got ok data, print it out!
  Serial.print(F("Found chip PN5")); 
  Serial.println((versiondata >> 24) & 0xFF, HEX);

  // configure board to read RFID tags
  nfc.SAMConfig();

  // Read asset status from EEPROM
  for (uint8_t i = 0; i < numTags; i++) {
    UIDstatus[i] = EEPROM.read(i);
    Serial.print(F("Tag "));
    Serial.print(i);
    Serial.print(F(" status: "));
    Serial.println(UIDstatus[i]);
  }

  Serial.println(F("RFID setup OK!"));
}

void setupIO(void){
  // button i/o setup
  pinMode(BUTTON1, INPUT);
  pinMode(BUTTON2, INPUT);
  pinMode(BUTTON3, INPUT);
  pinMode(BUTTON4, INPUT);
  pinMode(BUZZER, OUTPUT);
  pinMode(BUZZER_GND, OUTPUT);
  digitalWrite(BUZZER, LOW);
  digitalWrite(BUZZER_GND, LOW);
  digitalWrite(BUTTON1, INPUT_PULLUP);
  digitalWrite(BUTTON2, INPUT_PULLUP);
  digitalWrite(BUTTON3, INPUT_PULLUP);
  digitalWrite(BUTTON4, INPUT_PULLUP);
}

void statusLights(uint8_t status){
  switch(status){
    case 1:
      strip.setPixelColor(0, strip.Color(75,40,0));
      strip.setPixelColor(6, strip.Color(75,40,0));
      strip.show();
      break;
    case 2:
      strip.setPixelColor(0, strip.Color(75,40,0));
      strip.setPixelColor(6, strip.Color(75,40,0));
      strip.setPixelColor(1, strip.Color(75,40,0));
      strip.setPixelColor(5, strip.Color(75,40,0));
      strip.show();
      break;
    case 3:
      strip.setPixelColor(0, strip.Color(75,40,0));
      strip.setPixelColor(6, strip.Color(75,40,0));
      strip.setPixelColor(1, strip.Color(75,40,0));
      strip.setPixelColor(5, strip.Color(75,40,0));    
      strip.setPixelColor(2, strip.Color(75,40,0));
      strip.setPixelColor(4, strip.Color(75,40,0));
      strip.show();
      break;
    default:
      clearStrip();
      break;
  }
}

// ============================================================================
// =============================== MISC FUNCTIONS =============================
// ============================================================================

// Halt function called when an error occurs.  
// Will print an error and stop execution
void halt(const __FlashStringHelper *error) {
  wdt_enable(WDTO_1S);
  strip.setPixelColor(0, strip.Color(50,0,0));
  strip.show();
  Serial.println(error);
}

void printFloat(float value, int places) {
  // this is used to cast digits
  int digit;
  float tens = 0.1;
  int tenscount = 0;
  int i;
  float tempfloat = value;

  // make sure we round properly. this could use pow from <math.h>, 
  // but doesn't seem worth the import
  // if this rounding step isn't here, the value  54.321 prints as 54.3209

  // calculate rounding term d:   0.5/pow(10,places)
  float d = 0.5;
  if (value < 0)
    d *= -1.0;
  // divide by ten for each decimal place
  for (i = 0; i < places; i++)
    d /= 10.0;
  // this small addition, combined with truncation will round our values properly
  tempfloat +=  d;

  // first get value tens to be the large power of ten less than value
  // tenscount isn't necessary but it would be useful if you 
  // wanted to know after this how many chars the number will take

  if (value < 0)
    tempfloat *= -1.0;
  while ((tens * 10.0) <= tempfloat) {
    tens *= 10.0;
    tenscount += 1;
  }

  // write out the negative if needed
  if (value < 0)
    Serial.print(F("-"));

  if (tenscount == 0)
    Serial.print(0, DEC);

  for (i = 0; i < tenscount; i++) {
    digit = (int) (tempfloat / tens);
    Serial.print(digit, DEC);
    tempfloat = tempfloat - ((float)digit * tens);
    tens /= 10.0;
  }

  // if no places after decimal, stop now and return
  if (places <= 0)
    return;

  // otherwise, write the point and continue on
  Serial.print('.');

  // now write out each decimal place by shifting digits 
  // one by one into the ones place and writing the truncated value
  for (i = 0; i < places; i++) {
    tempfloat *= 10.0;
    digit = (int) tempfloat;
    Serial.print(digit, DEC);
    // once written, subtract off that digit
    tempfloat = tempfloat - (float) digit;
  }
}

void emergencySMS(void){
  //String message = F("EMERGENCY CONTACT FROM JAMES TECHPACK");
  //String address = F("5397771317");
  emergencyFlag=true;
  fona.sendSMS("5397771317","TECHPACK CONTACT");
}

void checkButtons(void){ 
  uint8_t lightsButtonState = digitalRead(BUTTON3);
  static uint8_t lastLightsButtonState = HIGH;
  uint8_t emergencyButtonState = digitalRead(BUTTON4);
  static uint8_t lastEmergencyButtonState = HIGH;
  uint8_t stealthButtonState = digitalRead(BUTTON1);
  static uint8_t lastStealthButtonState = HIGH;
  static unsigned long emergencyStartTime = 0;
  const unsigned long emergencyTime = 5000;
  uint8_t powerButtonState = digitalRead(BUTTON2);
  static uint8_t lastPowerButtonState = HIGH;
  static unsigned long powerStartTime = 0;
  const unsigned long powerTime = 5000;

  // Bright lights button
  if (lightsButtonState != lastLightsButtonState) {
    if (lightsButtonState == HIGH) {
      lightsOn = !lightsOn;
    }
    // Delay a little bit to avoid bouncing
    delay(50);
  }
  lastLightsButtonState = lightsButtonState;

  // Activate emergency mode if the button is held for a few seconds
  if(emergencyButtonState == LOW && lastEmergencyButtonState == HIGH){
    emergencyStartTime = millis();
    Serial.println(F("em button"));
  } 
  else if(emergencyButtonState == LOW && lastEmergencyButtonState == LOW){
    if(millis() - emergencyStartTime >= emergencyTime){
      emergencyStartTime = millis();
      setStripColor(255,0,0); // Strip is red while we try to send the SMS
      emergencySMS();
      for(uint8_t i = 0; i < 10; i++){
        colorWipe(30,0,0);    // Flash lights conspicuously
        colorWipe(0,0,30);
        clearStrip();
        wdt_reset();
      }
    }
  }
  lastEmergencyButtonState = emergencyButtonState;

  // Emergency mode with no lights
  if(stealthButtonState == LOW && lastStealthButtonState == HIGH){
    emergencyStartTime = millis();
    Serial.println(F("sm button"));
  } 
  else if(stealthButtonState == LOW && lastStealthButtonState == LOW){
    if(millis() - emergencyStartTime >= emergencyTime){
      emergencyStartTime = millis();
      emergencySMS();
    }
  }
  lastStealthButtonState = stealthButtonState;

  // A pseudo power button
  if(powerButtonState == LOW && lastPowerButtonState == HIGH){
    powerStartTime = millis();
    Serial.println(F("pw button"));
  } 
  else if(powerButtonState == LOW && lastPowerButtonState == LOW){
    if(millis() - powerStartTime >= powerTime){
      powerStartTime = millis();
      Serial.println(F("Bag pwr."));
      bagPower = !bagPower;
      pwrLights(bagPower);
    }
  }
  lastPowerButtonState = powerButtonState;
}