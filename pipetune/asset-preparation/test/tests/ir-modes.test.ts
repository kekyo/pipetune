import { spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { mkdir, mkdtemp, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { pathToFileURL } from 'node:url';
import { expect, test } from 'vitest';
import { encodeWave } from '../support/wave';

const cases = [
  { name: 'independent stereo', channels: 2, cm: 'indep', input: 1 },
  { name: 'true stereo left', channels: 4, cm: 'true' },
  { name: 'true stereo right', channels: 4, input: 1 },
  { name: 'diagonal matrix', channels: 3, width: 6, input: 2 },
  { name: 'mono uses the first response', channels: 4, cm: 'mono', input: 1 },
  { name: 'zero latency', channels: 1, lt: '0' },
  { name: '256 latency', channels: 1, lt: '256' },
  { name: 'numeric latency', channels: 1, lt: 256 },
  { name: '512 latency', channels: 1, lt: '512' },
  { name: '1024 latency and predelay', channels: 1, lt: '1024', pd: 7.5 },
  { name: 'direct cut', channels: 2, dc: true },
  { name: 'early cut', channels: 1, dc: true, co: -0.25 },
  {
    name: 'late cut with decay and trim',
    channels: 4,
    dc: true,
    co: 0.5,
    dt: 50,
    tr: 25,
  },
  { name: 'long decay and trim', channels: 2, dt: 400, tr: 50 },
  { name: 'dry and wet gain', channels: 1, de: true, dl: -6, dw: -12 },
  {
    name: 'capacity reduction warning',
    channels: 16,
    width: 16,
    sourceFrames: 110000,
    limited: true,
  },
  {
    name: 'last selected pair',
    channels: 2,
    width: 16,
    input: 15,
    channel: '1516',
    first: 14,
  },
  {
    name: 'last selected mono',
    channels: 1,
    width: 16,
    input: 15,
    channel: '16',
    first: 15,
  },
].map((entry) => ({
  width: 2,
  channel: 'A',
  first: 0,
  cm: 'auto',
  input: 0,
  lt: '128',
  pd: 0,
  dc: false,
  co: 0,
  dt: 100,
  tr: 100,
  de: false,
  dl: 0,
  dw: 0,
  sourceFrames: 4096,
  limited: false,
  ...entry,
}));

test.each(
  cases.flatMap((scenario) =>
    ['scalar', 'simd'].map((backend) => ({ ...scenario, backend }))
  )
)(
  '$backend $name: registered originals produce the application wet response',
  async (scenario) => {
    const directory = await mkdtemp(join(tmpdir(), 'pipetune-ir-'));
    try {
      const sourceRoot = process.env.PIPETUNE_EFFETUNE_SOURCE;
      const preparationUrl = pathToFileURL(
        sourceRoot + '/js/ir-library/ir-preparation.js'
      ).href;
      const contractUrl = pathToFileURL(
        sourceRoot + '/js/ir-library/ir-plugin-contract.js'
      ).href;
      const preparation = await import(/* @vite-ignore */ preparationUrl);
      const contract = await import(/* @vite-ignore */ contractUrl);
      const library = join(directory, 'effetune', 'ir-library');
      await mkdir(library, { recursive: true });
      const channels = Array.from(
        { length: scenario.channels },
        (_, channel) => {
          const impulse = new Float32Array(scenario.sourceFrames);
          impulse[0] = 1 - channel * 0.1;
          impulse[64 + channel * 16] = 0.5 + channel * 0.1;
          for (let frame = 128; frame < impulse.length; frame++)
            impulse[frame] =
              0.03 *
              Math.exp(-(frame - 128) / 400) *
              Math.sin(frame * (0.1 + channel * 0.013));
          return impulse;
        }
      );
      const original = encodeWave(channels, 48000);
      const sha256 = createHash('sha256').update(original).digest('hex');
      const irId = sha256.slice(0, 24);
      await writeFile(join(library, irId + '.wav'), original);
      await writeFile(
        join(library, 'index.json'),
        JSON.stringify({
          version: 1,
          entries: {
            [irId]: {
              irId,
              displayName: 'Native impulse',
              composition: 'single',
              originals: [
                {
                  role: 'single',
                  fileName: 'impulse.wav',
                  storageName: irId + '.wav',
                  sha256,
                  byteLength: original.length,
                },
              ],
              bytes: original.length,
              channels: channels.length,
              frames: scenario.sourceFrames,
              sampleRate: 48000,
              topology: 'original',
              pathSummary: [],
              importedAt: '2026-10-09T00:00:00.000Z',
              analysis: {
                storageName: irId + '.analysis',
                onsetFrame: 0,
                rt60: null,
                peakDb: 0,
              },
            },
          },
        })
      );
      const preset = join(directory, 'wet.effetune_preset');
      const parameters = {
        ir: irId,
        cm: scenario.cm,
        lt: scenario.lt,
        cr: 'full',
        dw: scenario.dw,
        de: scenario.de,
        dl: scenario.dl,
        pd: scenario.pd,
        dc: scenario.dc,
        co: scenario.co,
        dt: scenario.dt,
        tr: scenario.tr,
      };
      await writeFile(
        preset,
        JSON.stringify({
          pipeline: [
            {
              name: 'IR Reverb',
              enabled: true,
              channel: scenario.channel,
              parameters,
            },
          ],
        })
      );
      const config = contract.resolveIrProcessingConfig({
        sampleRate: 48000,
        channelCount: channels.length,
        engineChannels: scenario.width,
        selectedChannels: contract.selectedIrChannelCount(
          scenario.channel,
          scenario.width
        ),
        channelMode: scenario.cm,
        latency: scenario.lt,
        convolutionRate: 'full',
      });
      expect(config.valid).toBe(true);
      const prepared = preparation.prepareIr({
        channels,
        sampleRate: 48000,
        options: {
          topology: config.topology,
          paths: config.paths,
          directCut: scenario.dc,
          cutOffsetMs: scenario.co,
          decayPercent: scenario.dt,
          trimPercent: scenario.tr,
        },
      });
      const reference = preparation.emitPreparedIr({
        ...prepared,
        options: {
          topology: config.topology,
          paths: config.paths,
          assetChannels: config.assetChannels,
          maxFrames: contract.maximumIrFramesForKernel({
            ...config,
            sourceFrames: prepared.frames,
          }),
        },
      });
      const driver = process.env.PIPETUNE_ASSET_DRIVER;
      expect(
        driver,
        'the product driver must be provided by CTest'
      ).toBeTruthy();
      const frames = 8192;
      const result = spawnSync(
        driver!,
        [
          preset,
          '48000',
          String(scenario.width),
          String(frames),
          String(scenario.input),
          scenario.backend,
        ],
        {
          encoding: 'utf8',
          env: {
            ...process.env,
            XDG_CONFIG_HOME: directory,
            XDG_CACHE_HOME: join(directory, 'cache'),
          },
          maxBuffer: 16 * 1024 * 1024,
        }
      );
      expect(result.status, result.stderr).toBe(0);
      const [active, latency, ...audio] = result.stdout
        .trim()
        .split(/\s+/)
        .map(Number);
      expect(active, result.stderr).toBe(1);
      if (scenario.limited) expect(result.stderr).toContain('reduced to fit');
      expect(latency).toBe(Number(scenario.lt));
      expect(audio).toHaveLength(frames * scenario.width);
      const expected = new Float64Array(audio.length);
      const input = scenario.input - scenario.first;
      if (scenario.de)
        expected[scenario.input * frames + latency] = 10 ** (scenario.dl / 20);
      const routes =
        config.topology === 1
          ? [
              {
                inputSlot: input,
                outputSlot: input,
                irChannel: 0,
              },
            ]
          : config.topology === 2
            ? [
                {
                  inputSlot: input,
                  outputSlot: input,
                  irChannel: input,
                },
              ]
            : config.topology === 3
              ? [
                  {
                    inputSlot: input,
                    outputSlot: 0,
                    irChannel: input * 2,
                  },
                  {
                    inputSlot: input,
                    outputSlot: 1,
                    irChannel: input * 2 + 1,
                  },
                ]
              : config.paths;
      for (const route of routes) {
        if (route.inputSlot !== input) continue;
        const response: Float32Array = reference.channels[route.irChannel];
        for (
          let frame = 0;
          frame <
          Math.min(
            response.length,
            frames - latency - Math.round(scenario.pd * 48)
          );
          frame++
        )
          expected[
            (route.outputSlot + scenario.first) * frames +
              latency +
              Math.round(scenario.pd * 48) +
              frame
          ] += response[frame] * 10 ** (scenario.dw / 20);
      }
      let maximumError = 0;
      for (let frame = 0; frame < audio.length; frame++)
        maximumError = Math.max(
          maximumError,
          Math.abs(audio[frame] - expected[frame])
        );
      expect(maximumError, result.stderr).toBeLessThan(0.0002);
    } finally {
      await rm(directory, { recursive: true, force: true });
    }
  }
);
