import { defineConfig } from '@playwright/test';
// Own dev server on its own port, never a reused one: several worktrees of this repo run tests on
// this machine, and reusing another checkout's server on 5173 tested the wrong code (13 false
// failures, 2026-10-02). strictPort makes a busy port fail loudly instead of drifting.
export default defineConfig({
  testDir: './tests',
  webServer: { command: 'npm run dev -- --port 5199 --strictPort', port: 5199, reuseExistingServer: false },
  use: { baseURL: 'http://localhost:5199' },
});
