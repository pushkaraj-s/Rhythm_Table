# Rhythm Table

Arduino code for **Rhythm Table**, an interactive project for the **Soft Interaction Lab** at the College of Performance, Visualization & Fine Arts, Texas A&M University.

At the Soft Interaction Lab, we explore soft robotics by combining electronics, programming and interactive ideas to build experiences people of all ages can explore, from kids to adults.

## Contents

| Sketch | What it does |
| --- | --- |
| [`Arduino_Codes/Initial_Test`](Arduino_Codes/Initial_Test/Initial_Test.ino) | First hardware test. Plays audio files from a microSD card through the Adafruit Music Maker MP3 shield, with playback and volume controlled from the Serial Monitor. |

## Hardware

- Arduino Uno R3
- Adafruit "Music Maker" MP3 Shield (#1790, VS1053 codec)
- microSD card (FAT16/FAT32) with audio files in the root folder
- Headphones or speakers on the 3.5 mm jack

## Getting started

1. In the Arduino IDE, install the **Adafruit VS1053** library (and its dependency **Adafruit BusIO**) from the Library Manager.
2. Copy a few `.mp3` files to the root of the microSD card and insert it into the shield.
3. Open and upload `Arduino_Codes/Initial_Test/Initial_Test.ino`.
4. Open the Serial Monitor at **115200 baud**.

| Key | Action |
| --- | --- |
| `n` | Next track |
| `p` | Pause / resume |
| `s` | Stop |
| `+` / `-` | Volume up / down |
| `v` | Show volume |
| `l` | List playlist |
