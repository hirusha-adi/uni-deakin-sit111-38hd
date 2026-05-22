#!/usr/bin/env python3

import json
import sys
import time
from urllib.parse import urlencode
from urllib.request import Request, urlopen

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    print("pyserial is not installed.")
    print("\tpython -m pip install pyserial")
    print("\tyay -S python-pyserial")
    sys.exit(1)


# ---------- constants ----------

BAUD = 9600
WIGLE_API_URL = "https://api.wigle.net/api/v2/network/search"
WIGLE_AUTHORIZATION = "Basic QUlEMDcxNmFmZWM4OTkyMjllODkyMWY3YTVmZDFhODI3YzM6N2IwNzk5ODNhNTE2ODFiNDk5MzAxMjliNzFjZTlkMGM="


# ---------- helper functions ----------

def find_arduino_port():
    ports = []

    for port in list_ports.comports():
        if "ttyACM" in port.device or "ttyUSB" in port.device:
            ports.append(port.device)

    if not ports:
        print("No Arduino serial port found.")
        sys.exit(1)

    if len(ports) == 1:
        return ports[0]

    print("Multiple serial ports found:")
    for index, port in enumerate(ports, start=1):
        print(f"{index}. {port}")

    choice = int(input("Select port number: "))
    return ports[choice - 1]


# ---------- main body ----------

def main():
    # ----- execute the `scan` command -----

    device = find_arduino_port()
    scan_lines = []

    print(f"Opening {device} at {BAUD} baud")

    with serial.Serial(device, BAUD, timeout=0.5) as ser:
        print("Waiting for Arduino reset...")
        time.sleep(2)
        ser.reset_input_buffer()
        time.sleep(0.5)

        print("Sending scan command...")
        ser.write(b"scan\n")
        ser.flush()
        time.sleep(0.5)

        scan_retry_sent = False

        while True:
            line = ser.readline().decode("utf-8", errors="replace").strip()

            if not line:
                continue

            print(line)

            # --- workaround for potential failiure ---

            # UDPATE: this isn't technically required anymore
            # this was caused because the ESP32 stays running when the Uno resets after this script opens the USB serial port
            # if the ESP32 had noisy UART stuff buffered, the next "SCAN" command would have other junk in it
            # thats why it kept on returning "ERROR,UNKNOWN_COMMAND"
            # to fix this, I updated the readCommandFromArduino() function.
            #   now, if the partial UART text is old, it will be cleared before reading the next burst
            #   and if the line gets too long, it will be treated as bad input as well
            #   and newline will reset the discard state after a command line ends
            if "UNKNOWN_COMMAND" in line:
                if scan_retry_sent:
                    print("Scan command failed after retry.")
                    return

                print("ESP32 rejected scan command. Retrying once...")
                ser.write(b"scan\n")
                ser.flush()
                scan_retry_sent = True
                continue

            scan_lines.append(line)
            
            # read all lines until the line ends with `SCAN_END`
            if line == "SCAN_END" or line.endswith("SCAN_END"):
                break

    # ----- convert results to dict -----

    networks = []

    for line in scan_lines:
        if "] AP," in line:
            line = "AP," + line.split("] AP,", 1)[1]

        if not line.startswith("AP,"):
            continue

        fields = {}

        for part in line.split(","):
            if "=" in part:
                key, value = part.split("=", 1)
                fields[key.strip()] = value.strip()

        try:
            rssi = int(fields.get("rssi", ""))
        except ValueError:
            continue

        if fields.get("ssid") and fields.get("bssid"):
            networks.append({
                "ssid": fields["ssid"],
                "bssid": fields["bssid"],
                "rssi": rssi,
            })

    # ----- get closest wifi networks -----

    networks = sorted(networks, key=lambda item: item["rssi"], reverse=True)[:3]

    print()
    print(f"Using {len(networks)} strongest network(s) for location lookup.")

    # ----- get location from APs via WiGle -----

    locations = []

    for index, network in enumerate(networks, start=1):
        if index > 1:
            time.sleep(3)

        print(f"Looking up {network['ssid']} ({network['bssid']})")

        params = urlencode({
            "netid": network["bssid"],
            "ssid": network["ssid"],
            "resultsPerPage": 1,
        })
        request = Request(
            f"{WIGLE_API_URL}?{params}",
            headers={
                "Authorization": WIGLE_AUTHORIZATION,
                "Accept": "application/json",
                "User-Agent": "Mozilla/5.0 (Windows NT 10.0; rv:151.0) Gecko/20100101 Firefox/151.0", # https://useragents.io/
            },
        )

        try:
            with urlopen(request, timeout=20) as response:
                payload = json.loads(response.read().decode("utf-8"))
                print(payload)
        except Exception as error:
            print(f"Lookup failed: {error}")
            continue

        results = payload.get("results", [])

        if not results:
            print("No location found.")
            continue

        latitude = results[0].get("trilat")
        longitude = results[0].get("trilong")

        if not isinstance(latitude, (int, float)) or not isinstance(longitude, (int, float)):
            print("No coordinates found.")
            continue

        latitude = float(latitude)
        longitude = float(longitude)
        maps_url = f"https://www.google.com/maps/search/?api=1&query={latitude:.8f},{longitude:.8f}"

        locations.append((latitude, longitude))
        print(f"AP coordinates: {latitude:.8f}, {longitude:.8f}")
        print(f"AP Google Maps: {maps_url}")
        print("====================================")

    if len(locations) < 3:
        print()
        print(f"Need 3 locations for estimate, found {len(locations)}.")
        return

    # ----- calculate centre of triangle to estimate location -----

    latitude = sum(item[0] for item in locations) / len(locations)
    longitude = sum(item[1] for item in locations) / len(locations)

    print()
    print(f"Estimated location: {latitude:.8f}, {longitude:.8f}")
    print(f"Google Maps: https://www.google.com/maps/search/?api=1&query={latitude:.8f},{longitude:.8f}")


if __name__ == "__main__":
    main()
