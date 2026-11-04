// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
import { defineConfig } from 'vite';
import { svelte } from '@sveltejs/vite-plugin-svelte';
import { readFileSync } from 'node:fs';
// About shows the real version: read from ../VERSION (Wind for Mac's single source) at build time.
const version = readFileSync(new URL('../VERSION', import.meta.url), 'utf8').trim();
export default defineConfig({
  plugins: [svelte()], base: './', build: { outDir: 'dist' },
  define: { __WIND_VERSION__: JSON.stringify(version) },
});
