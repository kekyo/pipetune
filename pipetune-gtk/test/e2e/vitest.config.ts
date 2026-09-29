import { defineConfig } from 'vitest/config';

export default defineConfig({
  test: {
    environment: 'node',
    include: ['tests/**/*.test.ts'],
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
