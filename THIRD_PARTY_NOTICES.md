# Third-Party Notices

This file lists third-party components used by or linked into this repository and their licences.

## Components

### qoa

- File: `src/libs/qoa.h`
- Licence: MIT (SPDX-License-Identifier: MIT)
- Copyright: Copyright (c) 2023, Dominic Szablewski
- Licence text: the file header contains only the copyright line and the SPDX identifier below, not the full MIT text. See the upstream repository https://github.com/phoboslab/qoa (LICENSE).

```
Copyright (c) 2023, Dominic Szablewski - https://phoboslab.org
SPDX-License-Identifier: MIT
```

### pl_mpeg

- File: `src/libs/pl_mpeg.h`
- Licence: MIT
- Copyright: Copyright (c) 2019 Dominic Szablewski

```
-- LICENSE: The MIT License(MIT)

Copyright(c) 2019 Dominic Szablewski

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files(the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and / or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions :
The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### stb_image_write

- File: `src/libs/stb_image_write.h`
- Licence: dual MIT / Public Domain (Unlicense), choose either
- Copyright: Copyright (c) 2017 Sean Barrett (MIT alternative)

```
------------------------------------------------------------------------------
This software is available under 2 licenses -- choose whichever you prefer.
------------------------------------------------------------------------------
ALTERNATIVE A - MIT License
Copyright (c) 2017 Sean Barrett
Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:
The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
------------------------------------------------------------------------------
ALTERNATIVE B - Public Domain (www.unlicense.org)
This is free and unencumbered software released into the public domain.
Anyone is free to copy, modify, publish, use, compile, sell, or distribute this
software, either in source code form or as a compiled binary, for any purpose,
commercial or non-commercial, and by any means.
In jurisdictions that recognize copyright laws, the author or authors of this
software dedicate any and all copyright interest in the software to the public
domain. We make this dedication for the benefit of the public at large and to
the detriment of our heirs and successors. We intend this dedication to be an
overt act of relinquishment in perpetuity of all present and future rights to
this software under copyright law.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
------------------------------------------------------------------------------
```

### cJSON

- File: `src/wipeout/cJSON.h`
- Licence: MIT
- Copyright: Copyright (c) 2009-2017 Dave Gamble and cJSON contributors

```

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
  THE SOFTWARE.
```

### libogc (devkitPro)

- Source: https://github.com/devkitPro/libogc
- Use: linked into `boot.dol` for the Wii build (not vendored in this repository)
- Licence: zlib-style licence. See the LICENSE file in the libogc repository.
- Copyright: see the libogc repository LICENSE file.

### devkitPPC toolchain (devkitPro)

- Source: https://devkitpro.org
- Use: build tool only. Not distributed with this repository.
- Licence: the GCC runtime libraries (libgcc, newlib) linked into the build fall under the GCC Runtime Library Exception and the newlib licences. See https://devkitpro.org for the toolchain licence terms.

## Upstream licence status

The game code derives from phoboslab/wipeout-rewrite, which has NO licence. README.md (lines 325-331) states:

> # License
> 
> There is none. This code may or may not be based on the source code of the PC (ATI-Rage) version that was leaked in 2022. If it were, it would probably violate copyright law, but it may also fall under fair use ¯\\\_(ツ)\_/¯
> 
> Working with this source code is probably fine, considering that this game was originally released 28 years ago (in 1995), that the current copyright holders historically didn't care about any wipEout related files or code being available on the net and that the game is currently not purchasable in any shape or form.
> 
> In any case, you may NOT use this source code in a commercial release. A commercial release includes hosting it on a website that shows any forms of advertising.

Commercial release is not allowed.

Decision pending (Matt): licence status must be resolved before this repository is made public.
