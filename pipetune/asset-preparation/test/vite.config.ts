import prettierMax from 'prettier-max';
import { defineConfig } from 'vite';

export default defineConfig({
  plugins: [prettierMax({ typescript: 'tsconfig.json' })],
  build: {
    lib: { entry: 'support/wave.ts', fileName: 'wave', formats: ['es'] },
    target: 'node20',
    minify: false,
  },
});
