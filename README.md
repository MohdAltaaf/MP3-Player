# MP3 Player

# Pocket MP3 Player (ESP32)

A tiny, battery-powered MP3 player you can build on a breadboard in an evening.
Drop songs on a micro SD card, turn the knob to pick a track, press it to play.
The little screen shows what's playing, and a real DAC chip keeps the sound clean.



## What it does

- Plays MP3 files from a micro SD card
- Shows the track name and status on a 0.96" OLED
- One rotary knob: **turn** to scroll or change volume, **press** to play/pause
- Runs on a rechargeable battery, charges over USB-C
- Clean audio out through a PCM5102A I2S DAC (no scratchy ESP32 built-in DAC)

- <img width="872" height="501" alt="Screenshot 2026-11-04 094244" src="https://github.com/user-attachments/assets/94d387c6-77e8-467f-89c3-0e418cdb7ddb" />

- <img width="1581" height="771" alt="Screenshot 2026-11-04 094307" src="https://github.com/user-attachments/assets/6298e94d-8cd2-4eeb-ac9e-e086dd6d6549" />




## Getting it running

1. Format your micro SD card as **FAT32** and copy some `.mp3` files to it.
2. Wire everything up using the tables above.
3. Install the Arduino IDE (or PlatformIO) with ESP32 board support.
4. Open the project, pick your ESP32 board, and upload.
5. Switch it on, plug in headphones, turn the knob.
