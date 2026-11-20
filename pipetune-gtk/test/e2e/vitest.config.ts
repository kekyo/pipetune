import { defineConfig } from 'vitest/config';

export default defineConfig({
  test: {
    environment: 'node',
    include: process.env.PIPETUNE_GTK_REMOTE_PANEL_DAEMON
      ? ['integration/remote-panel.test.ts']
      : ['tests/**/*.test.ts'],
    fileParallelism: false,
    maxWorkers: 1,
    // A case can inspect every settings page at multiple window sizes via AT-SPI.
    testTimeout: 120_000,
    hookTimeout: 30_000,
    coverage: {
      enabled: false,
    },
  },
});
