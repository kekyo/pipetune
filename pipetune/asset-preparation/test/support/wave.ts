/**
 * Encodes equal-length planar float channels as an IEEE float WAV fixture.
 * @param channels Channels in their original order.
 * @param sampleRate Original sample rate in hertz.
 * @returns A RIFF file containing interleaved float32 PCM.
 */
export const encodeWave = (
  channels: readonly Float32Array[],
  sampleRate: number
): Buffer => {
  const frames = channels[0].length;
  const bytes = frames * channels.length * 4;
  const output = Buffer.alloc(44 + bytes);
  output.write('RIFF', 0);
  output.writeUInt32LE(36 + bytes, 4);
  output.write('WAVEfmt ', 8);
  output.writeUInt32LE(16, 16);
  output.writeUInt16LE(3, 20);
  output.writeUInt16LE(channels.length, 22);
  output.writeUInt32LE(sampleRate, 24);
  output.writeUInt32LE(sampleRate * channels.length * 4, 28);
  output.writeUInt16LE(channels.length * 4, 32);
  output.writeUInt16LE(32, 34);
  output.write('data', 36);
  output.writeUInt32LE(bytes, 40);
  for (let frame = 0; frame < frames; frame++) {
    for (let channel = 0; channel < channels.length; channel++) {
      output.writeFloatLE(
        channels[channel][frame],
        44 + (frame * channels.length + channel) * 4
      );
    }
  }
  return output;
};
