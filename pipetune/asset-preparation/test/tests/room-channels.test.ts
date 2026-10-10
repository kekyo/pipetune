import { spawnSync } from 'node:child_process';
import { mkdir, mkdtemp, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { pathToFileURL } from 'node:url';
import { expect, test } from 'vitest';

const cases = [
  {
    name: 'selected pair uses relative slots',
    channel: '34',
    channels: 6,
    input: 2,
    parameters: { ms: 'left', ms0: 'right', ms1: 'left', ms2: 'missing' },
    source: 'right',
  },
  {
    name: 'pair second member uses second slot',
    channel: '34',
    channels: 6,
    input: 3,
    parameters: { ms0: 'right', ms1: 'left', ms3: 'missing' },
    source: 'left',
  },
  {
    name: 'shared fallback in selected last pair',
    channel: '1516',
    channels: 16,
    input: 15,
    parameters: { ms: 'left', ms0: 'right' },
    source: 'left',
  },
  {
    name: 'array assignments and explicit slot precedence',
    channel: '34',
    channels: 6,
    input: 3,
    parameters: { ms: ['right', 'right'], ms1: 'left' },
    source: 'left',
  },
  {
    name: 'single channel ignores all per-channel slots',
    channel: '16',
    channels: 16,
    input: 15,
    parameters: { ms: 'left', ms0: 'missing', ms15: 'missing' },
    source: 'left',
  },
  {
    name: 'intentional unassigned channel remains aligned',
    channel: '34',
    channels: 6,
    input: 3,
    parameters: { ms0: 'left' },
    source: '',
  },
  {
    name: 'All uses final slot',
    channel: 'A',
    channels: 16,
    input: 15,
    parameters: { ms: 'left', ms15: 'right' },
    source: 'right',
  },
  {
    name: 'unselected channel retains delayed dry input',
    channel: '34',
    channels: 6,
    input: 0,
    parameters: { ms0: 'left', ms1: 'right' },
    source: '',
    selected: false,
  },
  {
    name: 'wide All clamps requested taps',
    channel: 'A',
    channels: 16,
    input: 15,
    parameters: { ms: 'left', ms15: 'right' },
    source: 'right',
    taps: 131072,
  },
].map((entry) => ({ taps: 8192, selected: true, ...entry }));

test.each(cases)(
  '$name matches the selected output response',
  async (scenario) => {
    const directory = await mkdtemp(join(tmpdir(), 'pipetune-room-channel-'));
    try {
      const library = join(directory, 'effetune', 'measurement-backups');
      await mkdir(library, { recursive: true });
      const measurements = Object.fromEntries(
        ['left', 'right'].map((id, side) => [
          id,
          {
            id,
            averageFrequencyResponse: Array.from({ length: 60 }, (_, index) => {
              const frequency = 20 * 1000 ** (index / 59);
              return [
                frequency,
                (side ? -9 : 9) * Math.exp(-(Math.log2(frequency / 800) ** 2)),
              ];
            }),
          },
        ])
      );
      for (const [id, measurement] of Object.entries(measurements))
        await writeFile(
          join(library, id + '.json'),
          JSON.stringify(measurement)
        );
      const preset = join(directory, 'channel.effetune_preset');
      await writeFile(
        preset,
        JSON.stringify({
          pipeline: [
            {
              name: 'Room EQ',
              enabled: true,
              channel: scenario.channel,
              parameters: {
                ...scenario.parameters,
                pm: 'lin',
                tp: scenario.taps,
                fl: 20,
              },
            },
          ],
        })
      );
      const effectiveTaps =
        scenario.channel === 'A' && scenario.channels > 8
          ? Math.min(scenario.taps, 65536)
          : scenario.taps;
      const upstream = await import(
        /* @vite-ignore */ pathToFileURL(
          process.env.PIPETUNE_EFFETUNE_SOURCE + '/js/room-eq/design-core.js'
        ).href
      );
      const reference = upstream.designRoomEq({
        config: { sampleRate: 48000, taps: effectiveTaps, phase: 'lin' },
        sources: [
          scenario.source
            ? { measurement: measurements[scenario.source] }
            : null,
        ],
      });
      const frames = effectiveTaps + 256;
      for (const backend of ['scalar', 'simd']) {
        const result = spawnSync(
          process.env.PIPETUNE_ASSET_DRIVER!,
          [
            preset,
            '48000',
            String(scenario.channels),
            String(frames),
            String(scenario.input),
            backend,
          ],
          {
            encoding: 'utf8',
            maxBuffer: 48 * 1024 * 1024,
            env: {
              ...process.env,
              PATH: '/nonexistent',
              XDG_CONFIG_HOME: directory,
              XDG_CACHE_HOME: join(directory, 'cache'),
            },
          }
        );
        expect(result.status, result.stderr).toBe(0);
        const [active, latency, ...audio] = result.stdout
          .trim()
          .split(/\s+/)
          .map(Number);
        expect(active).toBe(1);
        expect(latency).toBe(128 + effectiveTaps / 2);
        let error = 0;
        for (let channel = 0; channel < scenario.channels; channel++)
          for (let frame = 0; frame < frames; frame++) {
            const expected =
              channel !== scenario.input
                ? 0
                : !scenario.selected
                  ? Number(frame === latency)
                  : frame >= 128 && frame < effectiveTaps + 128
                    ? reference.channels[0][frame - 128]
                    : 0;
            error = Math.max(
              error,
              Math.abs(audio[channel * frames + frame] - expected)
            );
          }
        expect(error).toBeLessThan(0.0002);
      }
    } finally {
      await rm(directory, { recursive: true, force: true });
    }
  },
  120000
);

test.each([44100, 48000, 96000, 192000, 384000])(
  'saved milliseconds regenerate delay at %s Hz',
  async (rate) => {
    const directory = await mkdtemp(join(tmpdir(), 'pipetune-room-delay-'));
    try {
      const preset = join(directory, 'delay.effetune_preset');
      await writeFile(
        preset,
        JSON.stringify({
          pipeline: [
            {
              name: 'Room EQ',
              enabled: true,
              parameters: { dl: 5, dy: 240, fd: 123, gn: -6 },
            },
          ],
        })
      );
      const frames = 8192,
        frequency = 1234;
      const result = spawnSync(
        process.env.PIPETUNE_ASSET_DRIVER!,
        [
          preset,
          String(rate),
          '2',
          String(frames),
          '0',
          'scalar',
          String(frequency),
        ],
        {
          encoding: 'utf8',
          env: {
            ...process.env,
            XDG_CONFIG_HOME: directory,
            PATH: '/nonexistent',
          },
        }
      );
      expect(result.status, result.stderr).toBe(0);
      const [active, latency, ...audio] = result.stdout
        .trim()
        .split(/\s+/)
        .map(Number);
      const delay = Math.round(rate * 0.005);
      // User Delay is an intentional effect, excluded from automatic latency compensation.
      expect(active).toBe(1);
      expect(latency).toBe(0);
      // Observe steady state after the kernel's initial manual-delay crossfade.
      for (let frame = 4096; frame < frames; frame++) {
        const expected =
          0.3 *
          10 ** (-6 / 20) *
          Math.sin((2 * Math.PI * frequency * (frame - delay)) / rate);
        expect(Math.abs(audio[frame] - expected)).toBeLessThan(1e-7);
      }
    } finally {
      await rm(directory, { recursive: true, force: true });
    }
  }
);
