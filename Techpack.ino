// Location tracking reported to a Phant server over GSM
// RFID assets reported to a Phant server over GSM
// LED and button controls
//
// Author: Braden Kallin
//
// Inspired by the code from Marco Schwartz and Tony DiCola

// Libraries
//#include <Adafruit_SleepyDog.h>
#include <SoftwareSerial.h>
#include "Adafruit_FONA.h"
#include <Wire.h>
#include <SPI.h>
#include <Adafruit_PN532.h>

// Define I2C Pins for PN532
#define PN532_IRQ   (2)
#define PN532_RESET (3)  // Not connected by default on the NFC Shield
Adafruit_PN532 nfc(PN532_IRQ, PN532_RESET);

// FONA pins configuration
#define FONA_RX              8   // FONA serial RX pin (pin 2 for shield).
#define FONA_TX              9   // FONA serial TX pin (pin 3 for shield).
#define FONA_RST             10  // FONA reset pin (pin 4 for shield)

// FONA instance & configuration
SoftwareSerial fonaSS = SoftwareSerial(FONA_TX, FONA_RX);     // FONA software serial connection.
Adafruit_FONA fona = Adafruit_FONA(FONA_RST);                 // FONA library connection.

// Latitude & longitude
float latitude, longitude, speed_kph, heading, altitude;

// GPS fix status
int fix;

void setup() {
  
  // Initialize serial output.
  Serial.begin(115200);
  Serial.println(F("Geotracking with Adafruit IO & FONA808"));

  setupFONA();
  setupRFID();

  // Use the watchdog to simplify retry logic and make things more robust.
  // Enable this after FONA is intialized because FONA init takes forever.
  // Watchdog.enable(30000);
  // Watchdog.reset(); 
}

// !! Currently checks for cards and posts location as quickly as possible
// !! Need to add timing code
void loop() {
  postLocation();
  // !! Currently only reads cards, and does not record them anywhere.
  checkRFID();
  // !! Need code to handle onboard buttons.
  // !! Need code to handle SMS activated lights
}

// !! Need to implement postRFID code to post RFID card data. 
// !! Need to store RFID tag IDs in EEPROM for robustness.
// Mifare Card handling
void checkRFID(void){
  uint8_t success;
  uint8_t uid[] = { 0, 0, 0, 0, 0, 0, 0 };  // Buffer to store the returned UID
  uint8_t uidLength;                        // Length of the UID (4 or 7 bytes depending on ISO14443A card type)
  uint8_t timeoutTime = 10;           // Timeout after 10ms
  
  // Wait for an ISO14443A type cards (Mifare, etc.).  When one is found
  // 'uid' will be populated with the UID, and uidLength will indicate
  // if the uid is 4 bytes (Mifare Classic) or 7 bytes (Mifare Ultralight)
  success = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, timeoutTime);
  
  if (success) {
  // Display some basic information about the card
  Serial.println(F("Found an ISO14443A card"));
  Serial.print(F("  UID Length: "));
  Serial.print(uidLength, DEC);
  Serial.println(" bytes");
  Serial.print(F("  UID Value: "));
  nfc.PrintHex(uid, uidLength);
  Serial.println(F(""));
  
  if (uidLength == 4)
  {
    // We probably have a Mifare Classic card ... 
    Serial.println(F("Seems to be a Mifare Classic card (4 byte UID)"));
    
    // Now we need to try to authenticate it for read/write access
    // Try with the factory default KeyA: 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF
    Serial.println(F("Trying to authenticate block 4 with default KEYA value"));
    uint8_t keya[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    
    // Start with block 4 (the first block of sector 1) since sector 0
    // contains the manufacturer data and it's probably better just
    // to leave it alone unless you know what you're doing
    success = nfc.mifareclassic_AuthenticateBlock(uid, uidLength, 4, 0, keya);
    
    if (success)
    {
      Serial.println(F("Sector 1 (Blocks 4..7) has been authenticated"));
      uint8_t data[16];
      
      // If you want to write something to block 4 to test with, uncomment
      // the following line and this text should be read back in a minute
      //memcpy(data, (const uint8_t[]){ 'a', 'd', 'a', 'f', 'r', 'u', 'i', 't', '.', 'c', 'o', 'm', 0, 0, 0, 0 }, sizeof data);
      // success = nfc.mifareclassic_WriteDataBlock (4, data);
  
      // Try to read the contents of block 4
      success = nfc.mifareclassic_ReadDataBlock(4, data);
      
      if (success)
      {
        // Data seems to have been read ... spit it out
        Serial.println(F("Reading Block 4:"));
        nfc.PrintHexChar(data, 16);
        Serial.println(F(""));
        
        // Wait a bit before reading the card again
        delay(1000);
      }
      else
      {
        Serial.println(F("Ooops ... unable to read the requested block.  Try another key?"));
      }
    }
    else
    {
      Serial.println(F("Ooops ... authentication failed: Try another key?"));
    }
  }
  
    if (uidLength == 7)
    {
      // We probably have a Mifare Ultralight card ...
      Serial.println(F("Seems to be a Mifare Ultralight tag (7 byte UID)"));
      
      // Try to read the first general-purpose user page (#4)
      Serial.println(F("Reading page 4"));
      uint8_t data[32];
      success = nfc.mifareultralight_ReadPage (4, data);
      if (success)
      {
        // Data seems to have been read ... spit it out
        nfc.PrintHexChar(data, 4);
        Serial.println(F(""));
        
        // Wait a bit before reading the card again
        delay(1000);
      }
      else
      {
        Serial.println(F("Ooops ... unable to read the requested page!?"));
      }
    }
  }
}

// Location handling and reporting
void postLocation(void){
  //Sparkfun URL Building
  const String publicKey = "5JDdvbVgx6urREAVgKOM"; //Public Key for data stream
  const String privateKey = "7BEe4kl6xRC7jo2neKrx"; //Private Key for data stream
  const byte NUM_FIELDS = 2; //number of fields in data stream
  const String fieldNames[NUM_FIELDS] = {"lat","long"}; //actual data fields
  float fieldData[NUM_FIELDS]; //holder for the data values
  
  // Watchdog reset at start of loop--make sure every step takes less than 30ish seconds in normal operation!
  //Watchdog.reset();

  // Grab a GPS reading.
  float latitude, longitude, speed_kph, heading, altitude;
  bool gpsFix = fona.getGPS(&fix, &latitude, &longitude, &speed_kph, &heading, &altitude);

  if(fix == 1) {

    fieldData[0] = latitude;
    fieldData[1] = longitude;

    Serial.print(F("Latitude: "));
    printFloat(latitude, 5);
    Serial.println(F(""));

    Serial.print(F("Longitude: "));
    printFloat(longitude, 5);
    Serial.println(F(""));

    //Watchdog.reset();

    uint16_t statuscode;
    int16_t length;
    String url = "http://data.sparkfun.com/input/";
    url += publicKey;
    url += "?private_key=";
    url += privateKey;
    for (uint8_t i_url=0; i_url<NUM_FIELDS; i_url++) {
      url += "&";
      url += fieldNames[i_url];
      url += "=";
      url += String(fieldData[i_url],6);
    }
    char buf[255];
    url.toCharArray(buf, url.length());

    Serial.println(buf);

    if (!fona.HTTP_GET_start(buf, &statuscode, (uint16_t *)&length)) {  
      Serial.println(F("Failed!"));
    }

    //Watchdog.reset();

    while (length > 0) {
      while (fona.available()) {
        char c = fona.read();
        Serial.write(c);
        length--;
      }
    }
    fona.HTTP_GET_end();

    //Watchdog.reset();

    Serial.println(F("Got fix and attempted to post. Waiting 5sec."));
    delay(5000);

  } 
  else {
    Serial.println(F("No Fix."));
  }
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
    d/= 10.0;    
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
    Serial.print('-');

  if (tenscount == 0)
    Serial.print(0, DEC);

  for (i=0; i< tenscount; i++) {
    digit = (int) (tempfloat/tens);
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
    Serial.print(digit,DEC);  
    // once written, subtract off that digit
    tempfloat = tempfloat - (float) digit; 
  }
}

// =================== SETUP FUNCTIONS ===================
void setupFONA(void){
  // Initialize the FONA module
  Serial.println(F("Initializing FONA....(may take 10 seconds)"));
  fonaSS.begin(4800);

  if (!fona.begin(fonaSS)) {
    halt(F("Couldn't find FONA"));
  }

  fonaSS.println("AT+CMEE=2");
  Serial.println(F("FONA is OK"));

  // Enable GPS.
  while(!fona.enableGPS(true)){delay(500);}

  // Enable GPRS
  while(!fona.enableGPRS(true)){delay(500);}

  // Wait a little bit to stabilize the connection.
  delay(1000);
  Serial.println(F("FONA setup OK!"));
}

void setupRFID(void){
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
  Serial.print(F("Found chip PN5")); Serial.println((versiondata>>24) & 0xFF, HEX); 
  Serial.print(F("Firmware ver. ")); Serial.print((versiondata>>16) & 0xFF, DEC); 
  Serial.print('.'); Serial.println((versiondata>>8) & 0xFF, DEC);
  
  // configure board to read RFID tags
  nfc.SAMConfig();
  
  Serial.println(F("RFID setup OK!"));
}


