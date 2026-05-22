#include <SoftwareSerial.h>

/*
SIT111HD WiFi Tool (Uno R3 + ESP32)
by Hirusha Adikari

The Arduino Uno R3 is the main controller.
An ESP32 is used for WiFi related work.
The user will be interacting with the Uno.
*/


// Uno D10 receives from ESP32 D17
// Uno D11 sends to ESP32 D16 (via the voltage divider)
SoftwareSerial espSerial(10, 11); // RX, TX

// output pins for visual status
const int RED_LED = 4;    // flashes when DEAUTH detected
const int GREEN_LED = 5;  // on if armed

// store the response lines
String pcLine = "";   // command recieved over serial, from computer
String espLine = "";  // response from esp32

unsigned long alertUntil = 0; // to turn LED off after short pulse

void showHelpCommand() {
  Serial.println("Commands:");
  Serial.println("  scan");
  Serial.println("  set channel 0-13");
  Serial.println("  arm");
  Serial.println("  disarm");
  Serial.println("  status");
  Serial.println("  help");
}

void setup() {
  pinMode(RED_LED, OUTPUT);
  pinMode(GREEN_LED, OUTPUT);

  digitalWrite(RED_LED, LOW);
  digitalWrite(GREEN_LED, LOW);

  // `Serial` is the USB serial link
  // between the computer and Uno
  Serial.begin(9600);

  // `espSerial` is the UART link
  // between Uno and ESP32
  // a different baud rate was selected to avoid potential errors
  espSerial.begin(4800);

  delay(1000);

  Serial.println("====================================");
  Serial.println("SIT111HD WiFi Tool (Uno R3 + ESP32)");
  Serial.println("------------------------------------");
  showHelpCommand();
  Serial.println("====================================");
}

void loop() {
  // continously check both sides
  readFromLinux();
  readFromEsp32();

  // if an alert has triggered the LED to turn on,
  // turn it off after a short alert window has passed
  if (alertUntil > 0 && millis() > alertUntil) {
    digitalWrite(RED_LED, LOW);
    alertUntil = 0;
  }
}

/*
Both `readFromLinux()` and `readFromEsp32()` functions follow a very similar structure
`readFromLinux` reads commands from the computer sent to the Uno over USB serial
`readFromEsp32` reads commanss from esp32 sent to the Uno over UART

For these functions,
- the only difference is the serial interface we read from (Serial vs espSerial)
- commands are line based
- so, we keep on concatenating characters to the current line until we get a \n or \r
- if we get this, that means the line has ended
  - then, we check if its an empty line or has any text in it
    - if it does, we process the command
    - otherwise, we just continue
  - then, we empty out the current command/line
- otherwise, just keep on concatenating the current character until the line ends  
*/

void readFromLinux() {
  while (Serial.available()) {
    char c = Serial.read();

    if (c == '\n' || c == '\r') {
      pcLine.trim();

      if (pcLine.length() > 0) {
        handleLinuxCommand(pcLine);
      }

      pcLine = "";
    } else {
      pcLine += c;
    }
  }
}

void readFromEsp32() {
  while (espSerial.available()) {
    char c = espSerial.read();

    if (c == '\n' || c == '\r') {
      espLine.trim();

      if (espLine.length() > 0) {
        handleEsp32Message(espLine);
      }

      espLine = "";
    } else {
      espLine += c;
    }
  }
}

void handleLinuxCommand(String cmd) {
  cmd.trim();
  cmd.toLowerCase();

  if (cmd == "scan") {
    Serial.println("[ARDUINO] Sending scan command to ESP32...");
    espSerial.println("SCAN");
  }

  else if (cmd.startsWith("set channel ")) {
    int channel = cmd.substring(12).toInt();

    if (channel >= 0 && channel <= 13) {
      if (channel == 0) {
        Serial.println("[ARDUINO] Enabling ESP32 channel hopping.");
      } else {
        Serial.print("[ARDUINO] Setting ESP32 channel to ");
        Serial.println(channel);
      }

      espSerial.print("CHAN:");
      espSerial.println(channel); // sends \n to end the command
    } else {
      Serial.println("[ERROR] Channel must be between 0 and 13.");
    }
  }

  else if (cmd == "arm") {
    digitalWrite(GREEN_LED, HIGH);

    Serial.println("[ARDUINO] System armed.");
    Serial.println("[ARDUINO] ESP32 is now monitoring the selected WiFi channel.");
    espSerial.println("ARM");
  }

  else if (cmd == "disarm") {
    digitalWrite(GREEN_LED, LOW);
    digitalWrite(RED_LED, LOW);

    Serial.println("[ARDUINO] System disarmed.");
    espSerial.println("DISARM");
  }

  else if (cmd == "status") {
    Serial.println("[ARDUINO] Requesting ESP32 status...");
    espSerial.println("STATUS");
  }

  else if (cmd == "help") {
    showHelpCommand();
  }

  else {
    Serial.println("[ERROR] Unknown command. Type help.");
  }
}

void handleEsp32Message(String msg) {
  Serial.print("[ESP32] ");
  Serial.println(msg);

  // flash red light if DEAUTH line
  if (msg.startsWith("DEAUTH")) {
    digitalWrite(RED_LED, HIGH);
    alertUntil = millis() + 300;
  }
}
