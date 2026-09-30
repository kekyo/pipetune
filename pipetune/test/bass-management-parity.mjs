import { mkdtempSync, writeFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { spawnSync } from 'node:child_process';

const [runner, upstream] = process.argv.slice(2);
const { designBassManagement } = await import(pathToFileURL(resolve(upstream, 'js/bass-management/design-core.js')));
const { estimateIrKernelCommitFootprint } = await import(pathToFileURL(resolve(upstream, 'js/ir-library/ir-plugin-contract.js')));
const directory = mkdtempSync(join(tmpdir(), 'pipetune-bass-management-'));
const encode = samples => {
  const bytes = Buffer.alloc(samples.length * 4);
  samples.forEach((value, index) => bytes.writeFloatLE(value, index * 4));
  return bytes;
};
try {
  let index = 0;
  for (const taps of [8192, 16384, 32768]) {
    for (const slope of [24, 48, 96]) {
      for (const cutoff of [20, 300]) {
        const width = [1, 2, 4, 6, 16, 16][index % 6];
        const rate = [44100, 48000, 96000, 192000, 384000, 48000][index % 6];
        const maximum = index % 6 === 4;
        const roles = Array(width).fill(maximum || width === 1 ? 2 : 1);
        const routes = roles.map((_, ch) => maximum || width === 1 ? 1 << ch : 1 << (width - 1));
        if (!maximum && width > 1) {
          roles[width - 1] = 3; routes[width - 1] = 0;
          if (width >= 4) { roles[1] = 2; roles[2] = 0; routes[2] = 0; }
          if (width >= 6) { roles[3] = 3; routes[3] = 0; }
        }
        const lowpass = index % 6 !== 3;
        const subs = maximum ? 65535 : 1 << (width - 1);
        const frequencies = roles.map((_, ch) => ch % 2 === 0 ? cutoff : 80);
        const slopes = roles.map((_, ch) => ch % 2 === 0 ? slope : 48);
        const inversions = routes.map((route, ch) => ch % 3 === 0 ? route : 0);
        const config = { sampleRate: rate, channelCount: width, taps, roles, frequencies, slopes,
          lfeLowpass: lowpass, lfeFrequency: cutoff, lfeSlope: slope };
        const designed = designBassManagement(config);
        const parameters = { ph: 'Linear', tp: String(taps), ro: roles, fc: frequencies, sl: slopes,
          rt: routes, su: subs, ri: inversions, lo: lowpass, lf: cutoff, ls: slope, hg: -3, bg: -6, lg: 3 };
        // Arrays take precedence over direct keys; without an array the direct
        // values must also reach both the native kernel and the FIR designer.
        if (index % 2 === 0) parameters.fc0 = 151;
        else {
          delete parameters.fc;
          frequencies.forEach((value, ch) => { parameters[`fc${ch}`] = value; });
        }
        const preset = join(directory, 'test.effetune_preset');
        writeFileSync(preset, JSON.stringify({ pipeline: [{ name: 'Bass Management', channel: 'All', parameters }] }));
        const coefficients = join(directory, 'coefficients.f32');
        writeFileSync(coefficients, Buffer.concat(designed.channels.map(encode)));
        const frames = taps + 256;
        const pcm = new Float32Array(width * frames);
        const latency = taps / 2 + 128;
        const headroom = 10 ** (-3 / 20), bass = 10 ** (-6 / 20), lfe = 10 ** (3 / 20);
        for (let ch = 0; ch < width; ++ch) {
          const amplitude = (ch + 1) * 0.05, onset = 17 + ch * 3;
          const irIndex = designed.inputChannels.indexOf(ch);
          const ir = irIndex < 0 ? null : designed.channels[irIndex];
          for (let frame = 0; frame < frames; ++frame) {
            const dry = frame === onset + latency ? amplitude : 0;
            const sample = frame - onset - 128;
            const low = ir ? (sample >= 0 && sample < taps ? ir[sample] * amplitude : 0) : dry;
            if (roles[ch] === 0 || roles[ch] === 1) pcm[ch * frames + frame] += (roles[ch] === 1 ? dry - low : dry) * headroom;
            if (roles[ch] === 1 || roles[ch] === 2) {
              const destinations = Array.from({ length: width }, (_, out) => out).filter(out => routes[ch] & (1 << out));
              for (const out of destinations) pcm[out * frames + frame] += low * (roles[ch] === 1 ? bass : lfe) * headroom *
                (inversions[ch] & (1 << out) ? -1 : 1) / destinations.length;
            }
          }
        }
        const output = join(directory, 'pcm.f32'); writeFileSync(output, encode(pcm));
        const footprint = estimateIrKernelCommitFootprint({ frames: taps, assetChannels: designed.channels.length,
          topology: 4, processingChannels: width, headBlock: 128, rateDivider: 1,
          pathCount: designed.channels.length, inputCount: width });
        const result = spawnSync(runner, [preset, coefficients, output, String(rate), String(width), String(frames), String(footprint)], { encoding: 'utf8' });
        if (result.status !== 0) throw new Error(`Bass Management ${rate}/${width}/${taps}/${slope}/${cutoff}:\n${result.stdout}\n${result.stderr}`);
        process.stdout.write(`${rate} Hz, ${width} ch, ${taps} taps, ${slope} dB/oct, ${cutoff} Hz: ${result.stdout}`);
        ++index;
      }
    }
  }
} finally {
  rmSync(directory, { recursive: true, force: true });
}
