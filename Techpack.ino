// Location tracking reported to a Phant server over GSM
// RFID assets reported to a Phant server over GSM
// LED and button controls
//
// Author: Braden Kallin
//
// Inspired by the code from Marco Schwartz and Tony DiCola

// Libraries
#include <SoftwareSerial.h>
#include "Adafruit_FONA.h"
#include <Wire.h>
#include <SPI.h>
#include <Adafruit_PN532.h>
#include <EEPROM.h>

// Define I2C Pins for PN532
#define PN532_IRQ   (2)
#define PN532_RESET (3)  // Not connected by default on the NFC Shield
Adafruit_PN532 nfc(PN532_IRQ, PN532_RESET);

// FONA pins configuration
#define FONA_RX     8   // FONA serial RX pin (pin 2 for shield).
#define FONA_TX     9   // FONA serial TX pin (pin 3 for shield).
#define FONA_RST    10  // FONA reset pin (pin 4 for shield)
#define FONA_KEY    11  // FONA power button
#define FONA_RI     12  // FONA ringer
#define FONA_PS     13  // FONA power status

// FONA instance & configuration
SoftwareSerial fonaSS = SoftwareSerial(FONA_TX, FONA_RX);     // FONA software serial connection.
Adafruit_FONA fona = Adafruit_FONA(FONA_RST);                 // FONA library connection.

// GPS variables
const unsigned long updateLocTime = 300000;
float latitude, longitude, speed_kph, heading, altitude = 0;
bool gpsSuccess = false;
int fix = 0;

// RFID variables
const uint8_t numTags = 5;
const uint8_t UIDs[numTags][7] = {{0x5B, 0x1B, 0x4C, 0x21, 0, 0, 0}, {0x8B, 0x31, 0x4C, 0x21, 0, 0, 0},
  {0xFB, 0x27, 0x4C, 0x21, 0, 0, 0}, {0xEB, 0x17, 0x48, 0x21, 0, 0, 0},
  {0xAB, 0x19, 0x4C, 0x21, 0, 0, 0}};
uint8_t UIDstatus[5] = {0, 0, 0, 0, 0};
bool tagFlag = false;
const unsigned long tagTime = 5000;

// Misc vars
const unsigned long statusTime = 5000;

void setup() {

  // Initialize serial output.
  Serial.begin(115200);
  Serial.println(F("Geotracking with Adafruit IO & FONA808"));

  setupFONA();
  setupRFID();

  // Try 4 times to post location on startup.
  for (uint8_t i = 0; i < 4; i++) {
    if (getLocation()) {
      if (postLocation()) {
        break;
      }
    }
    delay(1000);
  }
}

void loop() {
  // Timing variables.
  unsigned long currentStatusMillis = 0;
  static unsigned long previousStatusMillis = 0;
  unsigned long currentLocMillis = 0;
  static unsigned long previousLocMillis = 0;
  static unsigned long tagStartMillis = 0;
  static bool timeFlag = false;

  // Check for tags
  checkRFID();

  // !! Need code to handle onboard buttons.
  // !! Need code to handle SMS activated lights

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
    timeFlag = true;
  }
  if(timeFlag && (millis()-tagStartMillis>=tagTime)){
    postRFID();
    timeFlag = false;
  }

  // Update the location every few minutes
  currentLocMillis = millis();
  if (currentLocMillis - previousLocMillis >= updateLocTime) {
    // Try 4 times to post location.
    for (uint8_t i = 0; i < 4; i++) {
      if (getLocation()) {
        if (postLocation()) {
          break;
        }
      }
      delay(1000);
    }
    previousLocMillis = currentLocMillis;
  }
}

// Mifare Card handling
void checkRFID(void) {
  uint8_t success;
  uint8_t uid[] = { 0, 0, 0, 0, 0, 0, 0 };  // Buffer to store the returned UID
  uint8_t uidLength;                        // Length of the UID (4 or 7 bytes depending on ISO14443A card type)
  uint8_t timeoutTime = 10;                 // Timeout after 10ms

  // Wait for an ISO14443A type cards (Mifare, etc.).  When one is found
  // 'uid' will be populated with the UID, and uidLength will indicate
  // if the uid is 4 bytes (Mifare Classic) or 7 bytes (Mifare Ultralight)
  success = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, timeoutTime);

  if (success) {
    // Display some basic information about the card
    Serial.println(F("Found an ISO14443A card"));
    Serial.print(F("  UID Length: "));
    Serial.print(uidLength, DEC);
    Serial.println(F(" bytes"));
    Serial.print(F("  UID Value: "));
    nfc.PrintHex(uid, uidLength);
    Serial.println(F(""));

    if (uidLength == 4)
    {
      // We probably have a Mifare Classic card ...
      Serial.println(F("Seems to be a Mifare Classic card (4 byte UID)"));

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
          EEPROM.update(i, UIDstatus[i]);
          Serial.println(F(""));
          Serial.print(F("Tag "));
          Serial.print(i);
          Serial.print(F(" status: "));
          Serial.println(EEPROM.read(i));
          tagFlag = true;
        }
      }

      // wait a little bit before reading again.
      delay(1000);

    }

    if (uidLength == 7)
    {
      // We probably have a Mifare Ultralight card ...
      Serial.println(F("Seems to be a Mifare Ultralight tag (7 byte UID)"));

      // wait a little bit before reading again.
      delay(1000);
    }
  }
}

// Asset reporting
bool postRFID(void) {
  //Sparkfun URL Building
  const String publicKey = F("7vz3gGRdAnFlGJYdbpZ8"); //Public Key for data stream
  const String privateKey = F("mqrWmkvRVxTRGq1a8bjd"); //Private Key for data stream
  const byte NUM_FIELDS = 5; //number of fields in data stream
  const String fieldNames[NUM_FIELDS] = {"tag0", "tag1", "tag2", "tag3", "tag4"}; //actual data fields

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
    url += EEPROM.read(i_url);
  }
  url += F(" ");
  char buf[255];
  url.toCharArray(buf, url.length());

  Serial.println(buf);

  if (!fona.HTTP_GET_start(buf, &statuscode, (uint16_t *)&length)) {
    Serial.println(F("Failed to post assets!"));
  }

  while (length > 0) {
    while (fona.available()) {
      char c = fona.read();
      Serial.write(c);
      length--;
    }
  }
  fona.HTTP_GET_end();

  Serial.println(F("Attempted to post assets. Waiting 1sec."));
  delay(1000);
  return true;


  Serial.println(F("No Fix."));
  return false;

}

// Location handling
bool getLocation(void) {
  // Grab a GPS reading.
  float latitude, longitude, speed_kph, heading, altitude;
  bool gpsSuccess = fona.getGPS(&fix, &latitude, &longitude, &speed_kph, &heading, &altitude);

  // Return true if a GPS lock is acquired
  if (gpsSuccess && fix == 1) {
    return true;
  }
  else {
    Serial.println(F("No Fix."));
    return false;
  }
}

// Location reporting
bool postLocation(void) {
  //Sparkfun URL Building
  const String publicKey = F("5JDdvbVgx6urREAVgKOM"); //Public Key for data stream
  const String privateKey = F("7BEe4kl6xRC7jo2neKrx"); //Private Key for data stream
  const byte NUM_FIELDS = 2; //number of fields in data stream
  const String fieldNames[NUM_FIELDS] = {"lat", "long"}; //actual data fields
  float fieldData[NUM_FIELDS]; //holder for the data values
  bool success = true;

  fieldData[0] = latitude;
  fieldData[1] = longitude;

  Serial.print(F("Latitude: "));
  printFloat(latitude, 5);
  Serial.println(F(""));

  Serial.print(F("Longitude: "));
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

  if (!fona.HTTP_GET_start(buf, &statuscode, (uint16_t *)&length)) {
    Serial.println(F("Failed to post locatoin!"));
    success = false;
  }

  while (length > 0) {
    while (fona.available()) {
      char c = fona.read();
      Serial.write(c);
      length--;
    }
  }
  
  fona.HTTP_GET_end();

  Serial.println(F("Got fix and attempted to post. Waiting 1 sec."));
  delay(1000);
  return success;
}

// =================== MISC FUNCTIONS ===================

// Halt function called when an error occurs.  Will print an error and stop execution while
// doing a fast blink of the LED.  If the watchdog is enabled it will reset after 8 seconds.
void halt(const __FlashStringHelper *error) {
  Serial.println(error);
  /*while (1) {
    digitalWrite(ledPin, LOW);
    delay(100);
    digitalWrite(ledPin, HIGH);
    delay(100);
  }*/
}

void printFloat(float value, int places) {
  // this is used to cast digits
  int digit;
  float tens = 0.1;
  int tenscount = 0;
  int i;
  float tempfloat = value;

  // make sure we round properly. this could use pow from <math.h>, but doesn't seem worth the import
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
  // tenscount isn't necessary but it would be useful if you wanted to know after this how many chars the number will take

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

  // now write out each decimal place by shifting digits one by one into the ones place and writing the truncated value
  for (i = 0; i < places; i++) {
    tempfloat *= 10.0;
    digit = (int) tempfloat;
    Serial.print(digit, DEC);
    // once written, subtract off that digit
    tempfloat = tempfloat - (float) digit;
  }
}

// =================== SETUP FUNCTIONS ===================
void setupFONA(void) {
  pinMode(FONA_RI, INPUT);
  pinMode(FONA_PS, INPUT);
  pinMode(FONA_KEY, OUTPUT);

  if (FONA_PS == LOW) {
    digitalWrite(FONA_KEY, LOW);
    delay(2100);
    digitalWrite(FONA_KEY, HIGH);
  }

  // Initialize the FONA module
  Serial.println(F("Initializing FONA....(may take 10 seconds)"));
  fonaSS.begin(4800);

  if (!fona.begin(fonaSS)) {
    halt(F("Couldn't find FONA"));
  }

  fonaSS.println("AT+CMEE=2");
  Serial.println(F("FONA is OK"));

  // Enable GPS.
  while (!fona.enableGPS(true)) {delay(1000);}

  // Enable GPRS
  while (!fona.enableGPRS(true)) {delay(1000);}

  // Wait a little bit to stabilize the connection.
  delay(1000);
  Serial.println(F("FONA setup OK!"));
}

void setupRFID(void) {
  // Start the rfid driver
  nfc.begin();

  // Check for the PN532 chip
  uint32_t versiondata = nfc.getFirmwareVersion();
  if (! versiondata) {
    Serial.print(F("Didn't find PN53x board"));
    while (1); // halt
    // NOTE maybe blink an LED at this point
  }
  // Got ok data, print it out!
  Serial.print(F("Found chip PN5")); Serial.println((versiondata >> 24) & 0xFF, HEX);
  Serial.print(F("Firmware ver. ")); Serial.print((versiondata >> 16) & 0xFF, DEC);
  Serial.print('.'); Serial.println((versiondata >> 8) & 0xFF, DEC);

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
