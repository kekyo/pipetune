import { mkdtempSync, writeFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { spawnSync } from 'node:child_process';

const [runner, upstream] = process.argv.slice(2);
const { designCrosstalkCancellation } = await import(pathToFileURL(resolve(upstream, 'js/crosstalk-cancellation/design-core.js')));
const directory = mkdtempSync(join(tmpdir(), 'pipetune-crosstalk-parity-'));
const encode = samples => {
  const bytes = Buffer.alloc(samples.length * 4);
  samples.forEach((value, index) => bytes.writeFloatLE(value, index * 4));
  return bytes;
};
try {
  for (const rate of [44100, 48000, 96000]) {
    const sources = {};
    for (const [ear, keys] of [['left-ear', ['ll', 'rl']], ['right-ear', ['lr', 'rr']]]) {
      const records = keys.map((slot, index) => {
        const direct = slot === 'll' || slot === 'rr';
        const samples = new Float32Array(700);
        const onset = direct ? 48 : 62;
        samples[onset] = direct ? 1 : (slot === 'lr' ? 0.3 : 0.45);
        samples[onset + 9] = direct ? -0.07 : 0.025;
        samples[onset + 140] = 0.04;
        // Exercise calibrated and uncalibrated measurements in one matrix.
        const refScale = ear === 'left-ear' ? 20000 : 1;
        for (let i = 0; i < samples.length; ++i) samples[i] *= refScale;
        const record = { measurementId: ear, pointId: index, channel: index === 0 ? 'left' : 'right',
          sampleRate: 48000, trimStartSamples: ear === 'left-ear' ? 11 : 19,
          onsetIndex: onset, refScale, outputTimeReference: 'audio-context', data: samples };
        sources[slot] = { id: `${ear}::ch=${record.channel}`, measurement: { points: [{ pointId: 0 }] }, impulses: [record] };
        return record;
      });
      const measurement = { id: ear, outputChannels: ['left', 'right'],
        points: [{ pointId: 0, channels: records.map(record => ({ channel: record.channel, irId: record.pointId, ir: { stored: true } })) }],
        impulseResponses: records.map(record => ({ ...record, data: encode(record.data).toString('base64') })) };
      writeFileSync(join(directory, `${ear}.json`), JSON.stringify(measurement));
    }
    for (const taps of [1024, 4096]) {
      const config = { sampleRate: rate, taps, regularization: taps === 1024 ? 25 : 75,
        maxGainDb: taps === 1024 ? 6 : 12, lowFrequency: 200, highFrequency: 6000, directWindowMs: 8 };
      const designed = designCrosstalkCancellation({ config, sources });
      const expected = join(directory, 'coefficients.f32');
      writeFileSync(expected, Buffer.concat(designed.channels.map(encode)));
      for (const [latency, strength, gain] of [[0, 0, 0], [128, 70, -3], [256, 100, 0], [512, 100, 6], [1024, 100, 0]]) {
        const parameters = { ll: sources.ll.id, lr: sources.lr.id, rl: sources.rl.id, rr: sources.rr.id,
          tp: taps, rg: config.regularization, mg: config.maxGainDb, fl: 200, fh: 6000, wl: 8,
          lt: String(latency), st: strength, og: gain, fd: 123 }; // fd must be derived from taps.
        const preset = join(directory, 'test.effetune_preset');
        writeFileSync(preset, JSON.stringify({ pipeline: [{ name: 'Crosstalk Cancellation', enabled: true, parameters }] }));
        const result = spawnSync(runner, [preset, directory, expected, String(rate), String(taps), String(latency), String(strength), String(gain)], { encoding: 'utf8' });
        if (result.status !== 0) throw new Error(`Crosstalk ${rate}/${taps}/${latency}/${strength}:\n${result.stdout}\n${result.stderr}`);
      }
      const valid = { ll: sources.ll.id, lr: sources.lr.id, rl: sources.rl.id, rr: sources.rr.id, tp: taps };
      for (const [parameters, enabled, channel, section, mode] of [
        [{...valid, ll: 'absent::ch=left', rl: 'absent::ch=right'}, true, 'A', false, 'skip'],
        [{...valid, rl: valid.ll}, true, 'A', false, 'skip'],
        [valid, true, 'L', false, 'skip'],
        [{}, false, 'A', false, 'disabled'],
        [{}, true, 'A', true, 'disabled']
      ]) {
        const pipeline = [{name: 'Crosstalk Cancellation', enabled, channel, parameters}];
        if (section) pipeline.unshift({name: 'Section', enabled: false, parameters: {}});
        const preset = join(directory, 'omitted.effetune_preset');
        writeFileSync(preset, JSON.stringify({pipeline}));
        const tested = spawnSync(runner, [preset, directory, expected, String(rate), String(taps), '128', '70', '0', mode], {encoding: 'utf8'});
        if (tested.status !== 0) throw new Error(tested.stderr);
      }
    }
  }
} finally {
  rmSync(directory, { recursive: true, force: true });
}
