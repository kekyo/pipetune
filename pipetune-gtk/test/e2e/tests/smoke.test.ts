import { afterEach, describe, expect, it } from 'vitest';

import { toPass } from 'gestament/testing';

import {
  launchPipeTuneGtk,
  type PipeTuneGtkTestSession,
} from '../support/session';

let session: PipeTuneGtkTestSession | undefined;

afterEach(async () => {
  if (session !== undefined) {
    await session.release();
    session = undefined;
  }
});

describe('PipeTune GTK dialog', () => {
  it('connects to the daemon and exposes stable widget ids', async () => {
    session = await launchPipeTuneGtk();
    const window = await session.app.getById('mainWindow');
    expect(window.kind).toBe('window');

    const processingMode = await session.app.getById('processingModeLabel');
    if (processingMode.kind !== 'label') {
      throw new Error(`Unexpected widget kind: ${processingMode.kind}`);
    }
    await toPass(
      async () => {
        expect(await processingMode.text()).toBe('Preset');
      },
      {
        timeoutMs: 10_000,
        message: 'PipeTune GTK did not display fake daemon status.',
      }
    );
  });
});
