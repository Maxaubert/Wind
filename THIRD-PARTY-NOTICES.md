# Third-party notices

Wind is proprietary (see `LICENSE`), but it builds on the components below, each under its own
licence. Those licences are unaffected by Wind's licence and their notices must be preserved.

## Shipped in the product

### Microsoft Edge WebView2 SDK 1.0.2792.45
`third_party/webview2/`. `WebView2LoaderStatic.lib` is linked into `WindConfig.exe`.
Distributed under the Microsoft Software License Terms for the WebView2 SDK, which permit
redistribution of the loader as part of an application.
https://developer.microsoft.com/microsoft-edge/webview2/

### Microsoft Edge WebView2 Runtime bootstrapper
`installer/MicrosoftEdgeWebview2Setup.exe`, redistributed unmodified as published by Microsoft.
Setup runs it only when the WebView2 runtime is absent. Microsoft Software License Terms apply.

### Svelte 5 (MIT)
Copyright (c) 2016-2026 Svelte contributors. The Svelte runtime is compiled into `ui/dist`,
which ships inside the installer. https://github.com/sveltejs/svelte

## Build and test only, not shipped

doctest (MIT, `third_party/doctest.h`, compiled only into `wind_tests.exe`), Vite, @sveltejs/vite-plugin-svelte and @playwright/test (MIT and Apache-2.0) are build and test tooling, not linked into any shipped binary.

---

MIT licence text, as it applies to the MIT components above:

> Permission is hereby granted, free of charge, to any person obtaining a copy of this software
> and associated documentation files (the "Software"), to deal in the Software without
> restriction, including without limitation the rights to use, copy, modify, merge, publish,
> distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the
> Software is furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in all copies or
> substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
> BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
> NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
> DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
> OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
