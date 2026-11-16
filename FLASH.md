# Flashing the Synth to Device

## From GitHub Actions Build

After GitHub Actions builds the project:

1. **Download the artifacts** from the GitHub Actions run:
   - Go to: `https://github.com/YOUR_USERNAME/wavesharesynth/actions`
   - Click the latest successful build
   - Download `wavesharesynth-build` artifact
   - Extract it

2. **Flash to device**:
   ```bash
   # Make sure device is connected to /dev/cu.usbmodem2101
   
   # Option A: Using idf.py (requires ESP-IDF installed)
   idf.py -p /dev/cu.usbmodem2101 write_flash @build/flash_args
   
   # Option B: Using esptool directly
   esptool.py -p /dev/cu.usbmodem2101 \
     write_flash \
     0x0 build/bootloader/bootloader.bin \
     0x8000 build/partition_table/partition-table.bin \
     0x20000 build/wavesharesynth.bin
   
   # Option C: Using this project's build directory
   cd /path/to/extracted/build
   idf.py -p /dev/cu.usbmodem2101 write_flash @flash_args
   ```

3. **Monitor output**:
   ```bash
   idf.py -p /dev/cu.usbmodem2101 monitor
   # Or press Ctrl+] to quit
   ```

## Expected Output

```
I (xxx) WAVESHARESYNTH: === Waveshare Synth Boot ===
I (xxx) WAVESHARESYNTH: I2C master initialized on SDA=7, SCL=8
I (xxx) WAVESHARESYNTH: Speaker amp enabled on GPIO 53
I (xxx) WAVESHARESYNTH: ES8311 codec initialized: 32000 Hz, volume -20 dB
I (xxx) WAVESHARESYNTH: I2S initialized: MCLK=13, BCK=12, WS=10, DO=11, DI=9
I (xxx) WAVESHARESYNTH: Audio system ready at 32000 Hz
I (xxx) WAVESHARESYNTH: === Boot complete ===
```

## Troubleshooting

**"Could not open port /dev/cu.usbmodem2101"**
- Check device is connected: `ls /dev/cu.*`
- Replace `/dev/cu.usbmodem2101` with the correct port
- Try unplugging/replugging the USB cable

**Device not detected by esptool**
- Make sure you have `esptool` installed: `pip install esptool`
- Check device in System Report → USB
- Try different USB cable or port on your Mac

**Binary too large**
- The bootloader reserves 0x20000 (128KB) for the app
- Check build output for size

## GitHub Actions Setup

To use GitHub Actions:

1. Push this repo to GitHub
2. GitHub Actions automatically builds on every push
3. Download artifacts from the Actions tab
4. Flash locally as shown above

That's it! No local ESP-IDF needed on macOS.
