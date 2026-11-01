// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
import { defineConfig, devices } from '@playwright/test';
// Own dev server on its own port, never a reused one: several worktrees of this repo run tests on
// this machine, and reusing another checkout's server on 5173 tested the wrong code (13 false
// failures, 2026-10-02). strictPort makes a busy port fail loudly instead of drifting.
// Wind for Mac hosts the page in a WKWebView, so WebKit is the project that counts; Chromium runs too
// so a Blink-only or WebKit-only behaviour shows up as a difference between the two.
export default defineConfig({
  testDir: './tests',
  testMatch: /.*\.spec\.js$/,
  webServer: { command: 'npm run dev -- --port 5199 --strictPort', port: 5199, reuseExistingServer: false },
  use: { baseURL: 'http://localhost:5199', viewport: { width: 1040, height: 720 } },
  projects: [
    { name: 'webkit', use: { ...devices['Desktop Safari'], viewport: { width: 1040, height: 720 } } },
    { name: 'chromium', use: { ...devices['Desktop Chrome'], viewport: { width: 1040, height: 720 } } },
  ],
});
