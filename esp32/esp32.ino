#include <WiFi.h>
#include "esp_wifi.h"

/*
SIT111HD WiFi Tool (Uno R3 + ESP32)
by Hirusha Adikari

Related docs:
- Arduino-ESP32 WiFi library: https://docs.espressif.com/projects/arduino-esp32/en/latest/api/wifi.html
- Arduino-ESP32 UART / HardwareSerial: https://docs.espressif.com/projects/arduino-esp32/en/latest/api/serial.html
- ESP-IDF (Espressif IoT Development Framework) WiFi driver API: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/network/esp_wifi.html
- IEEE 802.11 standard: https://github.com/WeitaoZhu/wi-fi_books/blob/master/IEEE_802.11_Specification/802.11-2020.pdf
- Wireshark 802.11 field reference: https://www.wireshark.org/docs/dfref/w/wlan.html

Related Definitions / Networking Concepts:
  SSID
    - the wifi network name
    - eg: GL-SFT1200-d52
  BSSID
    - the MAC address of a WAP
    - note that one SSID can have multiple BSSIDs if there are multiple APs
  
  RSSI
    - Received Signal Strength Indicator
    - represents the strength of the recieved signal
    - measurews in a negative dBm value
    - value closer to 0 is stronger/closer.
    - eg: -47 is closer/stronger than -90

  Channel:
    - A 2.4 GHz WiFi channel
    - This project monitors channels 1-13.
  
  Management frame:
    - a wifi frame type used for control operations
    - like: 
      - beacon: 
        - sent by an AP regularly
        - advertises that WiFI network exists
        - contains information like SSID, BSSID, security capabilities and other details
      - probe: 
        - used by clients to discover nearby WiFi networks
        - a client sends a probe request asking "is this network nearby?"
        - an access point can reply with a probe response containing network details
      - authentication: 
        - used when a client starts the process of joining an access point
        - this is an early 802.11 step before association
        - for WPA/WPA2, this is not the same as typing the WiFi password. 
        - it's part of the lower-level WiFi connection process
      - association: 
        - used after authentication
        - this is where the client formally joins the access point
        - after association, the client becomes part of that WiFi network 
        - and can continue with higher level security steps like WPA/WPA2 key exchange
      - deauthentication: 
        - used to terminate association between a client and an AP
        - after this, the client is no longer authenticated with the AP
        - attackers abuse this by sending fake deauth frames to kick clients off WiFi
      - disassociation:
        - used to terminate association between a client and an access point
        - the client may still be authenticated, but it is no longer associated with the AP
        - this can happen normally when a device roams, disconnects, or sleeps, but it can also be abused
  
  Promiscuous mode
    - an ESP32 WiFi mode where the device can receive raw WiFi frames from the air
      instead of only traffic addressed to itself


*/

// ESP32 D16 receives from Uno D11 (via the voltage divider)
// ESP32 D17 sends to Uno D10
const int ESP_RX2 = 16;
const int ESP_TX2 = 17;
// this uses ESP32 UART2
// the Uno uses 5V logic, ESP32 uses 3.3V logic
// that is why Arduino TX --> ESP32 RX needs a voltage divider


// UART baud rate between Arduino and ESP32.
// this matches with SoftwareSerial baud rate
const int LINK_BAUD = 4800;

// 2.4 GHz Band:  
//  - https://en.wikipedia.org/wiki/List_of_WLAN_channels
//  - https://portal.powertec.com.au/industry-resources/countries-territories/asia-pacific/australia/spectrum/au-2400-24835-mhz
// details:
//  frequences: 2400Mhz to 2483.5Mhz
//  channel range: 1-3
//  spaced 5MHz apart from each other except for a 12MHz space before channel 14
// however, we will also allow channel 0 (channel hopping mode)
const int MIN_WIFI_CHANNEL = 1;
const int MAX_WIFI_CHANNEL = 13;

// if the current channel is set to 0, hopping mode will be enabled
// this will make the ESP32 move to the next channel every 350ms
// lower value --> faster hopping (less time in each channel, but all channels covered per unit time)
// higher value --> slower hopping (more time in each channel, but less channels covered per unit time)
const unsigned long CHANNEL_HOP_INTERVAL_MS = 350;

// UART2 is used for Uno <--> ESP32 communication
HardwareSerial linkSerial(2);

// stores the current command line (to execute)
String inputLine = "";
unsigned long lastLinkByteMs = 0;   // when we last recieved a UART byte from Uno
bool discardLinkLine = false;       // ignore bad line until it ends

// UART commands from Uno are short and sent together
// if a line becomes old or too long, it is probably a bad partial command
const unsigned long LINK_LINE_GAP_MS = 50;
const int MAX_LINK_COMMAND_LENGTH = 32;

volatile bool sniffEnabled = false;     // process (true) or ignore (false) captured WiFi frames
volatile uint32_t managementCount = 0;  // total management frames seen while armed
volatile uint32_t deauthCount = 0;      // number of deauthentication frames seen
volatile uint32_t disassocCount = 0;    // number of disassociation frames seen
volatile int lastRssi = 0;              // RSSI of the latest detected deauth/disassociation frame

// MAC address metadata from the latest mgmnt frame
// but NOTE that in deauth attacks, the src MAC can be spoofed
// in a normal 802.11 mgmnt frame,
//  Address 1 --> dst
//  Address 2 --> src (lastSourceAp) 
//  Address 3 --> BSSID (lastBssidAp)
// these are packet metadata and cannot be gauranteed
volatile uint8_t lastSourceAp[6] = {0, 0, 0, 0, 0, 0};
volatile uint8_t lastBssidAp[6] = {0, 0, 0, 0, 0, 0};
volatile bool lastApAddressSeen = false;

int currentChannel = 1; // 0 (hopping), 1-13 (fixed)
int activeChannel = 1;  // currently listening to this channel

uint32_t lastReportedCount = 0;     // deauth+disassoc count
unsigned long lastReportMs = 0;     // to rate limit deauth reports
unsigned long lastChannelHopMs = 0; // to decide when to move to the next channel

// forward declerations
void hopChannelIfNeeded();

void setup() {
  // USB serial for debugging directly from the ESP32
  // visible when you open the ESP32 Serial Monitor at 115200 baud
  // this won't read any commands, only show extra logging
  Serial.begin(115200);

  // UART2 serial link to Arduino
  // SERIAL_8N1 means 8 data bits, no parity with 1 stop bit
  // https://docs.espressif.com/projects/arduino-esp32/en/latest/api/serial.html#arduino-esp32-serial-api
  linkSerial.begin(LINK_BAUD, SERIAL_8N1, ESP_RX2, ESP_TX2);

  // put into WiFi station mode
  // making it act like a wifi client
  // we won't connect to an AP, we will just be sniffing
  WiFi.mode(WIFI_STA);

  // disconnect from any existing connections
  // to ensure it's free to scan
  // it won't be connected, but this is just to be safe
  WiFi.disconnect(true);
  delay(300);

  // setup promiscuous mode and mgmnt frame filtering
  setupSniffer();

  Serial.println("ESP32 WiFi detector ready.");
  delay(300);
  linkSerial.println("READY,ESP32");
}

void loop() {
  readCommandFromArduino();
  hopChannelIfNeeded();
  reportDeauthIfNeeded();
}

void readCommandFromArduino() {
  // identical to `readFromLinux()` / `readFromEsp32()`
  // commands are line based
  while (linkSerial.available()) {
    char c = linkSerial.read();
    unsigned long now = millis();

    // if there was a long gap
    //  do not join old UART text with the new command
    if ((inputLine.length() > 0 || discardLinkLine) && now - lastLinkByteMs > LINK_LINE_GAP_MS) {
      inputLine = "";
      discardLinkLine = false;
    }

    lastLinkByteMs = now;

    if (c == '\n' || c == '\r') {
      inputLine.trim();

      if (inputLine.length() > 0) {
        handleCommand(inputLine);
      }

      inputLine = "";
      discardLinkLine = false;
    } else if (discardLinkLine) {
      continue;

    } else if (inputLine.length() < MAX_LINK_COMMAND_LENGTH) {
      inputLine += c;
      
    } else {
      // line is too long to be a valid Uno command
      // wait for its end before reading another command
      inputLine = "";
      discardLinkLine = true;
    }
  }
}

void handleCommand(String cmd) {
  cmd.trim();
  cmd.toUpperCase();

  // gives Arduino SoftwareSerial time to switch back to reading
  // it can miss bytes if both devices talk too quickly and show jibberish
  // this small delay gives the Arduino time to switch back to reading mode
  delay(200);

  if (cmd == "SCAN") {
    scanAccessPoints();
  }

  else if (cmd.startsWith("CHAN:")) {
    // set the monitor channel from CHAN:X
    int ch = cmd.substring(5).toInt();

    if (ch >= 0 && ch <= MAX_WIFI_CHANNEL) {
      currentChannel = ch;
      if (currentChannel == 0) {
        // start hopping from 0
        activeChannel = MIN_WIFI_CHANNEL;
      } else {
        // fixed to set channel
        activeChannel = currentChannel;
      }

      setSnifferChannel(activeChannel);

      // reset counters as targets changed
      managementCount = 0;
      deauthCount = 0;
      disassocCount = 0;
      lastReportedCount = 0; 
      lastChannelHopMs = millis();

      delay(100);

      linkSerial.print("OK,CHANNEL,");
      linkSerial.println(currentChannel);

      if (currentChannel == 0) {
        // CHAN:0
        Serial.println("Channel hopping enabled.");
      } else {
        // CHAN:1 to CHAN:13
        Serial.print("Channel set to ");
        Serial.println(currentChannel);
      }
    } else {
      linkSerial.println("ERROR,CHANNEL_RANGE");
    }
  }

  else if (cmd == "ARM") {
    sniffEnabled = true;

    // reset counters
    managementCount = 0;
    deauthCount = 0;
    disassocCount = 0;
    lastReportedCount = 0;

    if (currentChannel == 0) {
      // start hopping from 0
      activeChannel = MIN_WIFI_CHANNEL;
    } else {
      // fixed to set channel
      activeChannel = currentChannel;
    }

    lastChannelHopMs = millis();
    setSnifferChannel(activeChannel);

    delay(100);
    linkSerial.println("OK,ARMED");
    Serial.println("Detector armed.");
  }

  else if (cmd == "DISARM") {
    // sniffer remains configured
    // we just ignore the frames 
    sniffEnabled = false;

    delay(100);
    linkSerial.println("OK,DISARMED");
    Serial.println("Detector disarmed.");
  }

  else if (cmd == "STATUS") {
    delay(200);

    // added small delays between fields because of using SoftwareSerial in the Uno side
    // sendng the line in smaller chunks is more reliable than one very fast burst

    linkSerial.print("STATUS,");
    delay(20);

    linkSerial.print("armed=");
    linkSerial.print(sniffEnabled ? "YES" : "NO");
    delay(20);

    linkSerial.print(",channel=");
    linkSerial.print(currentChannel);
    delay(20);

    linkSerial.print(",active_channel=");
    linkSerial.print(activeChannel);
    delay(20);

    linkSerial.print(",mgmt_count=");
    linkSerial.print(managementCount);
    delay(20);

    linkSerial.print(",deauth_count=");
    linkSerial.print(deauthCount);
    delay(20);

    linkSerial.print(",disassoc_count=");
    linkSerial.println(disassocCount);
  }

  else {
    delay(100);
    linkSerial.println("ERROR,UNKNOWN_COMMAND");
  }
}

void scanAccessPoints() {
  // wifi scanning and promiscuous sniffing should not run simultaneously
  // this function disables detection/sniffing
  //  --> scans nearby access points
  //  --> sends AP details back to Arduino
  //  --> restores sniffer configuration
  sniffEnabled = false;
  esp_wifi_set_promiscuous(false);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(300);

  // https://docs.espressif.com/projects/arduino-esp32/en/latest/api/wifi.html#wifiscan
  // int16_t scanNetworks(async, show_hidden, passive, max_ms_per_chan, channel) with default values
  // scanNetworks(false,  true)
  //    - run an async scan. function will wait until the scan is complete
  //    - include hidden networks in the scan
  int networks = WiFi.scanNetworks(false, true);

  linkSerial.print("SCAN_BEGIN,count=");
  linkSerial.println(networks);

  Serial.print("SCAN_BEGIN,count=");
  Serial.println(networks);

  for (int i = 0; i < networks; i++) {
    // --- SSID ---
    String ssid = WiFi.SSID(i);

    // if `ssid` is hidden, it will re turn an empty string
    if (ssid.length() == 0) {
      ssid = "[hidden]";
    }
    ssid.replace(",", " "); // to not break other stuff

    String line = "";
    line += "AP,ssid=";
    line += ssid;

    // --- BSSID ---
    // note: one SSID can have multiple BSSIDs if it has multiple APs
    line += ",bssid=";
    line += WiFi.BSSIDstr(i);

    // --- Channel ---
    // used by the 2.4GHz network
    line += ",channel=";
    line += WiFi.channel(i);

    // --- Received Signal Strength Indicator ---
    line += ",rssi=";
    line += WiFi.RSSI(i);

    linkSerial.println(line);
    Serial.println(line);
    delay(20);
  }

  linkSerial.println("SCAN_END");
  Serial.println("SCAN_END");

  // free scan result memory
  WiFi.scanDelete();

  // restore stuff
  setupSniffer();
  if (currentChannel == 0) {
    // start hopping from 0
    activeChannel = MIN_WIFI_CHANNEL;
  } else {
    // fixed to set channel
    activeChannel = currentChannel;
  }
  setSnifferChannel(activeChannel);
}

void setupSniffer() {
  // set up the ESP32 promiscuous mode
  // it lets us see raw WiFi frames from the air
  esp_wifi_set_promiscuous(false);
  delay(50);

  // filter only mgmnt frames
  wifi_promiscuous_filter_t filter;
  filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
  esp_wifi_set_promiscuous_filter(&filter);
  esp_wifi_set_promiscuous_rx_cb(&snifferCallback);

  // set the channel before enabling promiscuous mode
  esp_wifi_set_channel(activeChannel, WIFI_SECOND_CHAN_NONE);
  delay(50);
  esp_wifi_set_promiscuous(true);
  delay(50);
}

void setSnifferChannel(int channel) {
  // change the active wifi channel used by promiscuous mode
  // esp32 can only listen on one 2.4GHz channel at a time
  // so, we will be switching the activeChannel repeatedly
  if (channel < MIN_WIFI_CHANNEL || channel > MAX_WIFI_CHANNEL) {
    return;
  }

  activeChannel = channel;

  // disable promiscuous mode before changing channel for stability
  esp_wifi_set_promiscuous(false);
  delay(50);
  esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
  delay(50);
  esp_wifi_set_promiscuous(true);
  delay(50);
}

void hopChannelIfNeeded() {
  // system needs to be armed
  if (!sniffEnabled) {
    return;
  }

  // currentChannel should be 0
  if (currentChannel != 0) {
    return;
  }

  // enough time has passed since the last hop
  if (millis() - lastChannelHopMs < CHANNEL_HOP_INTERVAL_MS) {
    return;
  }

  lastChannelHopMs = millis();
  activeChannel++;

  // wrap back to channel 1 after channel 13
  if (activeChannel > MAX_WIFI_CHANNEL) {
    activeChannel = MIN_WIFI_CHANNEL;
  }

  setSnifferChannel(activeChannel);

  // NOTE: Use only when debugging, creates a lot of noise
  // ----
  // linkSerial.print("HOP,channel=");
  // linkSerial.println(activeChannel);
  // Serial.print("HOP,channel=");
  // Serial.println(activeChannel);
}

void copyMacAddress(volatile uint8_t *dest, const uint8_t *src) {
  // copy a 6 byte (48 bit) MAC address (L2) into a global volatile buffer
  for (int i = 0; i < 6; i++) {
    dest[i] = src[i];
  }
}

void printMacAddress(Print &out, const volatile uint8_t *mac) {
  // convert a 6 byte MAC address into readable text
  // size 18:
  //  - 6 groups * 2 hex characters = 12
  //  - 5 colons = 5
  //  - null terminator (\0) = 1
  char text[18];

  // https://stackoverflow.com/a/11070196
  snprintf(
    text, sizeof(text),
    "%02X:%02X:%02X:%02X:%02X:%02X",
    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]
  );

  out.print(text);
}

void snifferCallback(void *buf, wifi_promiscuous_pkt_type_t type) {
  // only if system is armed
  if (!sniffEnabled) {
    return;
  }

  // filter mgmnt frames only
  if (type != WIFI_PKT_MGMT) {
    return;
  }

  managementCount++;

  // callback gives the packet as a generic void pointer
  // so, we caste it to wifi_promiscuous_pkt_t to access
  wifi_promiscuous_pkt_t *packet = (wifi_promiscuous_pkt_t *)buf;

  // https://www.wireshark.org/docs/dfref/w/wlan.html
  // 802.11 mgmnt frames have a MAC header
  // basic 802.11 MAC header layout:
  //  - Frame Control = 2 bytes
  //  - Duration = 2 bytes
  //  - Address 1 = 6 bytes
  //  - Address 2 = 6 bytes
  //  - Address 3 = 6 bytes
  //  - Sequence Ctrl = 2 bytes
  // so, total minimum header size should be 2+2+6+6+6+2=18
  // otherwise, we cannot read it safely
  if (packet->rx_ctrl.sig_len < 24) {
    return;
  }

  // raw frame bytes
  // here, the payload[0] and payload[1] contain the Frame Control field
  // it contains the protocol version, frame type, subtype and flags
  const uint8_t *payload = packet->payload;

  // combile the first 2 bytes into a 16 bit value
  uint16_t frameControl = payload[0] | (payload[1] << 8);

  // bits 2-3 store the frame type
  // binary mask for bits 2 and 3 is 0x000C
  // and perform a logical right shift (move type bits to the start of the number)
  // frameType: 0=management, 1=control, 2=data
  uint8_t frameType = (frameControl & 0x000C) >> 2;

  // bits 4-7 store the frame subtype
  // binary mask for bits 4, 5, 6 and 7 is 0x00F0
  // and perform a logical right shift (move subtype bits to the start of the number)
  // frameSubtype: 10=disassociation,12=deauthentication
  uint8_t frameSubtype = (frameControl & 0x00F0) >> 4;

  // Address layout in normal mgmnt frames are as follows:
  //  - payload + 4  --> Address 1 / receiver / destination
  //  - payload + 10 --> Address 2 / transmitter / source
  //  - payload + 16 --> Address 3 / BSSID

  if (frameType == 0 && frameSubtype == 12) {
    deauthCount++;
    lastRssi = packet->rx_ctrl.rssi;
    copyMacAddress(lastSourceAp, payload + 10);
    copyMacAddress(lastBssidAp, payload + 16);
    lastApAddressSeen = true;
  }

  else if (frameType == 0 && frameSubtype == 10) {
    disassocCount++;
    lastRssi = packet->rx_ctrl.rssi;
    copyMacAddress(lastSourceAp, payload + 10);
    copyMacAddress(lastBssidAp, payload + 16);
    lastApAddressSeen = true;
  }
}

void reportDeauthIfNeeded() {
  // only if system is armed
  if (!sniffEnabled) {
    return;
  }

  uint32_t currentBadFrameCount = deauthCount + disassocCount;

  if (currentBadFrameCount > lastReportedCount && millis() - lastReportMs > 500) {
    lastReportedCount = currentBadFrameCount;
    lastReportMs = millis();
    
    // send to arduino (UART2)
    linkSerial.print("DEAUTH,");
    linkSerial.print("total=");
    linkSerial.print(currentBadFrameCount);

    linkSerial.print(",deauth=");
    linkSerial.print(deauthCount);

    linkSerial.print(",disassoc=");
    linkSerial.print(disassocCount);

    linkSerial.print(",channel=");
    linkSerial.print(activeChannel);

    linkSerial.print(",source_ap=");

    if (lastApAddressSeen) {
      printMacAddress(linkSerial, lastSourceAp);
    } else {
      linkSerial.print("unknown");
    }

    linkSerial.print(",bssid_ap=");

    if (lastApAddressSeen) {
      printMacAddress(linkSerial, lastBssidAp);
    } else {
      linkSerial.print("unknown");
    }

    linkSerial.print(",rssi=");
    linkSerial.println(lastRssi);

    // send to computer (USB)
    Serial.print("DEAUTH,total=");
    Serial.print(currentBadFrameCount);

    Serial.print(",deauth=");
    Serial.print(deauthCount);

    Serial.print(",disassoc=");
    Serial.print(disassocCount);

    Serial.print(",channel=");
    Serial.print(activeChannel);

    Serial.print(",source_ap=");

    if (lastApAddressSeen) {
      printMacAddress(Serial, lastSourceAp);
    } else {
      Serial.print("unknown");
    }

    Serial.print(",bssid_ap=");

    if (lastApAddressSeen) {
      printMacAddress(Serial, lastBssidAp);
    } else {
      Serial.print("unknown");
    }

    Serial.print(",rssi=");
    Serial.println(lastRssi);
  }
}
