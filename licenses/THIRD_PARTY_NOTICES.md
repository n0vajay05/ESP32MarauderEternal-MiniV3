# Third-party notices

## nyanBOX

The BLE detector fingerprints, feature selection, Device Scout behavior,
wireless activity-tool concepts, and Remote ID receive/transmit handling were
adapted from [nyanBOX](https://github.com/jbohack/nyanBOX),
commit `b15e6bdcb784e12ebf9a9b292c6a63e4ce0af5af`.

MIT License

Copyright (c) 2025 jbohack  
Original work Copyright (c) 2022 CiferTech

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## opendroneid-core-c

The compact Open Drone ID decoder in `esp32_marauder/DroneRemoteID.cpp` and
message encoder in `esp32_marauder/DroneRemoteIDSpoofer.cpp` are derived from
[opendroneid-core-c](https://github.com/opendroneid/opendroneid-core-c) and
modifications made by nyanBOX. The upstream library is licensed under the
Apache License, Version 2.0. A copy of the license is available at
<https://www.apache.org/licenses/LICENSE-2.0>.

The Marauder adaptation adds fixed-memory storage, strict transport bounds,
NimBLE-Arduino 2.x integration, ESP32-C5 Wi-Fi/BLE coexistence, channel
hopping, and a 128x128 joystick-driven display.
