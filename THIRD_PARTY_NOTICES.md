# Third-party notices

The executables built from this repository include the following third-party software.

## BTstack (personal, non-commercial use only)

Bluetooth stack used to drive the USB dongle. Included as the `btstack/` git submodule
(<https://github.com/bluekitchen/btstack>).

**Important:** clause 4 of the BTstack license means the bridge executables may only be used,
modified and redistributed for personal benefit and not for any commercial purpose or monetary gain.
Commercial licenses are available from BlueKitchen GmbH.

```
Copyright (C) 2009 BlueKitchen GmbH
All rights reserved

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

3. Neither the name of the copyright holders nor the names of
   contributors may be used to endorse or promote products derived
   from this software without specific prior written permission.

4. Any redistribution, use, or modification is done solely for
   personal benefit and not for any commercial purpose or for
   monetary gain.

THIS SOFTWARE IS PROVIDED BY BLUEKITCHEN GMBH AND CONTRIBUTORS
``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL BLUEKITCHEN
GMBH OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
SUCH DAMAGE.
```

BTstack itself bundles the following, which are compiled into the bridge:

| Component | Used for | License |
|---|---|---|
| Bluedroid SBC encoder/decoder (Broadcom) | mSBC wideband speech | Apache License 2.0 |
| micro-ecc (Kenneth MacKay) | LE Secure Connections | BSD 2-Clause |
| yxml (Yoran Heling) | XML parsing | MIT |
| rijndael | AES | Public domain |
| md5 | hashing | Public domain |

The full texts are in the corresponding folders under `btstack/3rd-party/`.

## PortAudio

Audio I/O library, linked statically (<https://www.portaudio.com>).

```
PortAudio Portable Real-Time Audio Library
Copyright (c) 1999-2011 Ross Bencina and Phil Burk

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files
(the "Software"), to deal in the Software without restriction,
including without limitation the rights to use, copy, modify, merge,
publish, distribute, sublicense, and/or sell copies of the Software,
and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR
ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

The text above constitutes the entire PortAudio license; however,
the PortAudio community also makes the following non-binding requests:

Any person wishing to distribute modifications to the Software is
requested to send the modifications to the original developer so that
they can be incorporated into the canonical version. It is also
requested that these non-binding requests be included along with the
license above.
```

## MinGW-w64 runtime

The executables are built with MinGW-w64 GCC and link its runtime statically. The MinGW-w64 runtime
is provided under permissive licenses (ZPL / public domain), and libgcc under the GCC Runtime Library
Exception, which allows this without imposing license terms on the program.
