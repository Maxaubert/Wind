import { defineConfig } from 'vite';
import { svelte } from '@sveltejs/vite-plugin-svelte';
import { readFileSync } from 'node:fs';
// About shows the real version: read from src/version.h, the single source, at build time.
const versionH = readFileSync(new URL('../src/version.h', import.meta.url), 'utf8');
const version = (versionH.match(/WIND_VERSION_STR\s+"([^"]+)"/) || [])[1] || '';
export default defineConfig({
  plugins: [svelte()], base: './', build: { outDir: 'dist' },
  define: { __WIND_VERSION__: JSON.stringify(version) },
});
