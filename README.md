
DrmDmp64_mass is a mass storage device firmware for the DreamDumper64 project.
The DreamDumper64 uses a WeAct pico to read N64 caridges.
The drmdmp64_mass firmware is able to read up to 64MB ROMs and also does its best to detect the actual rom size.
Aside from the game data, drmdmp64_mass also reads the save game chips, such as the SRAM (battery backed), SI EEPROM (most common) and FlashRAM (largests size).
All of these chips are exposed to the host PC as a file on a USB drive.
![alt text](https://github.com/nopjne/drmdmp64_mass/blob/master/game.jpg?raw=true)

The DrmDmp64_mass also does cartridge verification, it will detect whether subsystems of the cartridge are present and/or working.
![alt text](https://github.com/nopjne/drmdmp64_mass/blob/master/carttester.jpg?raw=true)

```
Cart tester report:
    EEPROM     - Not present
    SRAM       - OK!
    FlashRam   - OK!
    CIC        - PAL 6103
    Romsize    - 16MB
    RomName    - POKEMON SNAP
    RomID      - 504E PF
    CartType   - N
    RomRegion  - F
    RomVersion - 00
```

The following files are exposed from the virtual disk: 

```
D:\>dir /s
 Volume in drive D is DreamDump64
 Volume Serial Number is 0022-DC8F

 Directory of D:\

09/05/2008  04:20 PM               512 ROM.EEP
09/05/2008  04:20 PM           131,072 ROM.FLA
09/05/2008  04:20 PM        12,582,912 ROM.N64
09/05/2008  04:20 PM        12,582,912 ROMF.Z64
09/05/2008  04:20 PM           131,072 ROMF.RAM
09/05/2008  04:20 PM               512 ROMF.EEP
09/05/2008  04:20 PM             2,048 CARTTEST.TXT
               7 File(s)     25,431,040 bytes
               
ROM.EEP      - Is either 512Byte or 2048Byte depending on 4K or 16K eeprom.
ROM.FLA      - Is either the SRAM or FlashRAM, which is between 32KB or 128KB, the file is always exposed as 128KB for compatibility with the DaisyDrive64.
ROM.N64      - Is the N64 Native format of the ROM, this format is directly compatible with the DaisyDrive64.
ROMF.Z64     - This is the same data as ROM.N64 however 16bit byte flipped, for compatibility with PC emulators.
ROMF.RAM     - The SRAM or FlashRAM data in byteflipped mode, for compatibility with PC emulators. (Ares)
ROMF.EEP     - The EEPROM data in byteflipped mode, for compatibility with PC emulators.
CARTTEST.TXT - The output of the cart tester during initialization.
```
How to build (this project depends on tinyusb):
```
git submodule update --init
mkdir build
cd build
cmake ..
cmake --build .
(optional) put pico in bootloader mode and copy the drmdmp64_mass.uf to the attached drive.
```

How to use:

```
1. Insert cartridge into the cartridge slot.
2. Connect USB to PC.
3. Navigate to the drive, use normally.
```

NOTE: When swapping cartridges make sure you disconnect and eject the drive, otherwise the operating system may cache the files from the previous cartridge.

Please look for PCBs here: 
https://dreamcraftindustries.com/products/dreamdump64-pcb

Gerber files here:
https://github.com/khill25/Dreamdumper
