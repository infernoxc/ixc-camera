# Third-party components

IXC Camera bundles **one** third-party component: **libfacedetection**, used for optional face tracking.

It also links against Windows system components that ship with Windows and against Windows SDK import libraries (kernel32, user32, gdi32, shell32, ole32, Media Foundation, Direct3D 11). The C++ runtime is linked statically from the Microsoft Visual C++ toolset, under the Visual Studio license terms, which allow redistribution of compiled applications.

Every new dependency is added to this table **before** it's merged:

| Component | Version | License | Why needed | Size impact | Security notes |
|---|---|---|---|---|---|
| [libfacedetection](https://github.com/ShiqiYu/libfacedetection) | commit `acf7b254121927e7dced30e233a2e03119f28ea2` (vendored, unmodified, in `third_party/libfacedetection`) | BSD 3-Clause | Face detection with 5 landmarks for face tracking (Phase 7). It was the only candidate that found the face in every test frame; see `docs/face-tracking-design.md` | +0.59 MB for each of `IXCCameraSource.dll` and `IXCCamera.exe` (two CPU variants, weights compiled in). Runtime: ~1–4 MB peak while tracking, nothing when off | Pure computation on an in-memory image: no file, network or model loading. Built as C++ with IXC's CRT and Control Flow Guard settings. Runs only when face tracking is on. Removable with `-DIXC_WITH_FACE_TRACKING=OFF` |

## libfacedetection license

```
By downloading, copying, installing or using the software you agree to this license.
If you do not agree to this license, do not download, install,
copy or use the software.


                  License Agreement For libfacedetection
                     (3-clause BSD License)

Copyright (c) 2015-2019, Shiqi Yu, all rights reserved.
shiqi.yu@gmail.com

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

  * Redistributions of source code must retain the above copyright notice,
    this list of conditions and the following disclaimer.

  * Redistributions in binary form must reproduce the above copyright notice,
    this list of conditions and the following disclaimer in the documentation
    and/or other materials provided with the distribution.

  * Neither the names of the copyright holders nor the names of the contributors
    may be used to endorse or promote products derived from this software
    without specific prior written permission.

This software is provided by the copyright holders and contributors "as is" and
any express or implied warranties, including, but not limited to, the implied
warranties of merchantability and fitness for a particular purpose are disclaimed.
In no event shall copyright holders or contributors be liable for any direct,
indirect, incidental, special, exemplary, or consequential damages
(including, but not limited to, procurement of substitute goods or services;
loss of use, data, or profits; or business interruption) however caused
and on any theory of liability, whether in contract, strict liability,
or tort (including negligence or otherwise) arising in any way out of
the use of this software, even if advised of the possibility of such damage.
```
