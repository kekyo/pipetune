import { afterEach, describe, expect, it } from 'vitest';
import { PNG } from 'pngjs';

import type {
  GtkElementOfKind,
  GtkWidgetElement,
  GtkWidgetKind,
} from 'gestament';
import { toPass, waitForResult } from 'gestament/testing';

import {
  launchPipeTuneGtk,
  type FakeControlRequest,
  type PipeTuneGtkTestSession,
} from '../support/session';

let session: PipeTuneGtkTestSession | undefined;

afterEach(async () => {
  if (session !== undefined) {
    await session.release();
    session = undefined;
  }
});

const elementOfKind = <Kind extends GtkWidgetKind>(
  element: GtkWidgetElement,
  kind: Kind
): GtkElementOfKind<Kind> => {
  if (element.kind !== kind) {
    throw new Error(`Expected ${kind}, received ${element.kind}`);
  }
  return element as GtkElementOfKind<Kind>;
};

const getElement = async <Kind extends GtkWidgetKind>(
  id: string,
  kind: Kind
): Promise<GtkElementOfKind<Kind>> => {
  if (session === undefined) {
    throw new Error('GTK session is unavailable');
  }
  return elementOfKind(await session.app.getById(id), kind);
};

const waitForLabel = async (id: string, expected: string): Promise<void> => {
  const label = await getElement(id, 'label');
  await toPass(
    async () => {
      expect(await label.text()).toBe(expected);
    },
    {
      timeoutMs: 10_000,
      message: `${id} did not become ${expected}`,
    }
  );
};

const waitForConnected = async (): Promise<void> => {
  await waitForLabel('status-system-connection', 'Connected');
  await waitForLabel('status-live-processing', 'Preset');
};

const waitForCommands = async (
  expected: readonly string[]
): Promise<readonly FakeControlRequest[]> => {
  if (session === undefined) {
    throw new Error('GTK session is unavailable');
  }
  return waitForResult(
    async () => {
      const requests = await session?.readRequests();
      const commands = requests?.map((request) => request.command) ?? [];
      for (const command of expected) {
        if (!commands.includes(command)) {
          throw new Error(
            `Missing ${command}; received ${commands.join(', ')}`
          );
        }
      }
      return requests ?? [];
    },
    {
      timeoutMs: 10_000,
      message: `Control commands did not arrive: ${expected.join(', ')}`,
    }
  );
};

const selectComboItem = async (id: string, index: number): Promise<void> => {
  const combo = await getElement(id, 'comboBox');
  await combo.selectChildAt(index);
};

const selectSettingsPage = async (index: number): Promise<void> => {
  const switcher = await getElement('settingsSwitcher', 'container');
  const child = await switcher.childAt(index);
  if (child === undefined) {
    throw new Error(`Settings page button ${String(index)} is unavailable`);
  }
  if (
    child.kind !== 'button' &&
    child.kind !== 'radio' &&
    child.kind !== 'toggleButton'
  ) {
    throw new Error(`Expected a settings page button, received ${child.kind}`);
  }
  if (
    (child.kind === 'radio' || child.kind === 'toggleButton') &&
    (await child.isChecked())
  ) {
    return;
  }
  await child.click();
};

const expectInsideWindow = async (
  element: GtkWidgetElement,
  window: GtkElementOfKind<'window'>
): Promise<void> => {
  const windowCapture = await window.capture();
  const elementCapture = await element.capture();
  expect(elementCapture.bounds.x).toBeGreaterThanOrEqual(
    windowCapture.bounds.x
  );
  expect(elementCapture.bounds.y).toBeGreaterThanOrEqual(
    windowCapture.bounds.y
  );
  expect(
    elementCapture.bounds.x + elementCapture.bounds.width
  ).toBeLessThanOrEqual(windowCapture.bounds.x + windowCapture.bounds.width);
  expect(
    elementCapture.bounds.y + elementCapture.bounds.height
  ).toBeLessThanOrEqual(windowCapture.bounds.y + windowCapture.bounds.height);
};

const resizeStatusPaneTo = async (targetWidth: number): Promise<void> => {
  if (session === undefined) {
    throw new Error('GTK session is unavailable');
  }
  const window = await getElement('mainWindow', 'window');
  const statusPane = await getElement('persistentStatusPane', 'container');
  const initialBounds = (await statusPane.capture()).bounds;
  const handleX = initialBounds.x + initialBounds.width + 2;
  const handleY = initialBounds.y + Math.floor(initialBounds.height / 2);
  await window.activate();
  await session.app.input.moveMouseTo(handleX, handleY);
  await session.app.input.setMouseButton('left', true);
  try {
    await session.app.input.moveMouseTo(initialBounds.x + targetWidth, handleY);
  } finally {
    await session.app.input.setMouseButton('left', false);
  }
  await toPass(
    async () => {
      expect(
        Math.abs((await statusPane.capture()).bounds.width - targetWidth)
      ).toBeLessThanOrEqual(5);
    },
    {
      timeoutMs: 10_000,
      message: 'Status pane did not reach the requested divider position.',
    }
  );
};

const scrollStatusPaneTo = async (fraction: number): Promise<void> => {
  const scrollbar = await getElement('statusScrollbar', 'scrollbar');
  const value = await scrollbar.valueInfo();
  await scrollbar.setValue(
    value.minimum + (value.maximum - value.minimum) * fraction
  );
};

interface PixelBounds {
  readonly x: number;
  readonly y: number;
  readonly width: number;
  readonly height: number;
}

const findOpaqueColorBounds = (
  image: Buffer,
  target: readonly [number, number, number]
): PixelBounds => {
  const png = PNG.sync.read(image);
  let minimumX = Number.POSITIVE_INFINITY;
  let minimumY = Number.POSITIVE_INFINITY;
  let maximumX = -1;
  let maximumY = -1;
  for (let y = 0; y < png.height; y += 1) {
    for (let x = 0; x < png.width; x += 1) {
      const offset = (y * png.width + x) * 4;
      if (
        png.data[offset] === target[0] &&
        png.data[offset + 1] === target[1] &&
        png.data[offset + 2] === target[2] &&
        png.data[offset + 3] === 255
      ) {
        minimumX = Math.min(minimumX, x);
        minimumY = Math.min(minimumY, y);
        maximumX = Math.max(maximumX, x);
        maximumY = Math.max(maximumY, y);
      }
    }
  }
  if (!Number.isFinite(minimumX)) {
    throw new Error(
      'The load meter HUE was not present in the window capture.'
    );
  }
  return {
    x: minimumX,
    y: minimumY,
    width: maximumX - minimumX + 1,
    height: maximumY - minimumY + 1,
  };
};

interface LoadMeterRasterMeasurement {
  readonly width: number;
  readonly rightInset: number;
}

const measureLoadMeterRaster = async (
  window: GtkElementOfKind<'window'>,
  statusPane: GtkElementOfKind<'container'>
): Promise<LoadMeterRasterMeasurement> => {
  const windowCapture = await window.capture();
  const statusCapture = await statusPane.capture();
  const filled = findOpaqueColorBounds(windowCapture.image, [134, 166, 78]);
  // Rounded ends exclude three boundary pixels from the exact solid HUE.
  const width = (filled.width + 3) / 0.6;
  const right = windowCapture.visibleBounds.x + filled.x + width;
  return {
    width,
    rightInset: statusCapture.bounds.x + statusCapture.bounds.width - right,
  };
};

const revealAndMeasureLoadMeter = async (
  window: GtkElementOfKind<'window'>,
  statusPane: GtkElementOfKind<'container'>
): Promise<LoadMeterRasterMeasurement> => {
  let lastError: unknown;
  for (const fraction of [
    0.5, 0.55, 0.6, 0.65, 0.7, 0.75, 0.8, 0.85, 0.9, 0.95,
  ]) {
    await scrollStatusPaneTo(fraction);
    try {
      return await measureLoadMeterRaster(window, statusPane);
    } catch (error) {
      lastError = error;
    }
  }
  if (lastError instanceof Error) {
    throw lastError;
  }
  throw new Error('The load meter could not be revealed in the status pane.');
};

const changeRateTo96Khz = async (): Promise<void> => {
  await selectSettingsPage(2);
  await selectComboItem('rateCombo', 3);
  await waitForCommands(['set-rate']);
  await waitForLabel('status-rates-fixed', '96 kHz');
};

const hideWithKeyboard = async (method: 'escape' | 'close'): Promise<void> => {
  if (session === undefined) {
    throw new Error('GTK session is unavailable');
  }
  const window = await getElement('mainWindow', 'window');
  await window.activate();
  if (method === 'escape') {
    await session.app.input.pressKey('Escape');
  } else {
    await (await getElement('closeButton', 'button')).click();
  }
};

describe('PipeTune GTK dialog', () => {
  it('keeps structured status visible beside every settings page at minimum size', async () => {
    session = await launchPipeTuneGtk();
    await waitForConnected();
    const window = await getElement('mainWindow', 'window');
    const hints = await window.resizeHints();
    expect(hints.minWidth).toBe(900);
    expect(hints.minHeight).toBe(560);

    const resized = await window.resizeTo(900, 560);
    expect(resized.width).toBe(900);
    expect(resized.height).toBe(560);
    const statusPane = await getElement('persistentStatusPane', 'container');
    const settingsPane = await getElement('settingsPane', 'container');
    expect((await statusPane.capture()).clipped).toBe(false);
    expect((await settingsPane.capture()).clipped).toBe(false);
    await expectInsideWindow(statusPane, window);
    await expectInsideWindow(settingsPane, window);
    expect((await statusPane.capture()).bounds.width).toBeGreaterThanOrEqual(
      340
    );

    const switcher = await getElement('settingsSwitcher', 'container');
    for (let page = 0; page < 5; page += 1) {
      const pageButton = await switcher.childAt(page);
      expect(pageButton).toBeDefined();
      await expectInsideWindow(pageButton as GtkWidgetElement, window);
      await selectSettingsPage(page);
      expect(
        await (await getElement('status-live-processing', 'label')).text()
      ).toBe('Preset');
    }
    await expectInsideWindow(
      await getElement('logToggleButton', 'toggleButton'),
      window
    );
    await expectInsideWindow(
      await getElement('dialogFooter', 'container'),
      window
    );
    await expectInsideWindow(
      await getElement('cancelButton', 'button'),
      window
    );
    await expectInsideWindow(await getElement('applyButton', 'button'), window);
    expect(
      (await (await getElement('logRevealer', 'container')).info()).states
    ).not.toContain('showing');

    await selectSettingsPage(1);
    const outputButton = await getElement('outputMenuButton', 'toggleButton');
    const outputInfo = await outputButton.info();
    expect(outputInfo.name).toMatch(/^Studio DAC/);
    expect(outputInfo.name).not.toContain('alsa_output.usb-long');
    expect(outputInfo.description).toContain(
      'alsa_output.usb-long-studio-dac.analog-stereo'
    );
    await outputButton.click();
    const outputPopover = await getElement('outputPopover', 'container');
    const outputList = await getElement('outputList', 'list');
    await toPass(
      async () => {
        const capture = await outputPopover.capture();
        expect(capture.bounds.width).toBeGreaterThanOrEqual(440);
        expect(capture.bounds.height).toBeGreaterThanOrEqual(140);
        expect((await outputList.info()).states).toContain('showing');
      },
      {
        timeoutMs: 10_000,
        message: 'Output choices did not become visibly available.',
      }
    );
  });

  it('uses expanded status-pane space for long values', async () => {
    session = await launchPipeTuneGtk();
    await waitForConnected();
    const window = await getElement('mainWindow', 'window');
    await window.resizeTo(1200, 740);
    const savedOutput = await getElement('status-saved-output', 'label');
    expect((await savedOutput.info()).states).toContain('showing');
    expect(await savedOutput.text()).toBe(
      'alsa_output.usb-long-studio-dac.analog-stereo'
    );
    const initialWidth = (await savedOutput.capture()).bounds.width;

    await resizeStatusPaneTo(680);

    const expandedWidth = (await savedOutput.capture()).bounds.width;
    expect(expandedWidth).toBeGreaterThanOrEqual(480);
    expect(expandedWidth).toBeGreaterThan(initialWidth + 200);
  });

  it('renders DSP load as a right-aligned level meter with a capped responsive width', async () => {
    session = await launchPipeTuneGtk();
    await waitForConnected();
    const window = await getElement('mainWindow', 'window');
    await window.resizeTo(900, 560);
    await resizeStatusPaneTo(340);
    await changeRateTo96Khz();

    const meter = await getElement('status-dsp-load-meter', 'progressBar');
    await toPass(
      async () => {
        const value = await meter.valueInfo();
        expect(value.value).toBeCloseTo(60, 5);
        expect(value.minimum).toBe(0);
        expect(value.maximum).toBe(100);
        expect((await meter.info()).name).toBe('Load 60.0%');
      },
      {
        timeoutMs: 10_000,
        message: 'DSP load meter did not expose its current bounded value.',
      }
    );
    const statusPane = await getElement('persistentStatusPane', 'container');
    const narrowMeter = await waitForResult(
      async () => revealAndMeasureLoadMeter(window, statusPane),
      {
        timeoutMs: 10_000,
        message: 'DSP load meter was not measurable at the narrow pane width.',
      }
    );
    expect(narrowMeter.width).toBeGreaterThanOrEqual(150);
    expect(narrowMeter.width).toBeLessThan(280);
    expect(narrowMeter.rightInset).toBeGreaterThanOrEqual(15);
    expect(narrowMeter.rightInset).toBeLessThanOrEqual(30);

    await window.resizeTo(1200, 740);
    await resizeStatusPaneTo(680);
    await toPass(
      async () => {
        const expandedMeter = await revealAndMeasureLoadMeter(
          window,
          statusPane
        );
        expect(expandedMeter.width).toBeGreaterThan(narrowMeter.width);
        expect(expandedMeter.width).toBeGreaterThanOrEqual(278);
        expect(expandedMeter.width).toBeLessThanOrEqual(282);
        expect(expandedMeter.rightInset).toBeGreaterThanOrEqual(15);
        expect(expandedMeter.rightInset).toBeLessThanOrEqual(30);
      },
      {
        timeoutMs: 10_000,
        message: 'DSP load meter did not grow to its capped width.',
      }
    );
  });

  it('applies every setting live, persists once, and remains open', async () => {
    session = await launchPipeTuneGtk();
    await waitForConnected();
    await session.clearRequests();
    const initial = await session.inspectConfig();

    await selectSettingsPage(1);
    const outputButton = await getElement('outputMenuButton', 'toggleButton');
    await outputButton.click();
    const secondStudioDac = await getElement('outputChoice2', 'button');
    await secondStudioDac.click();
    let requests = await waitForCommands(['set-output']);
    expect(
      requests.find((request) => request.command === 'set-output')
    ).toMatchObject({
      target: 'alsa_output.pci-0000_0b_00.4.hdmi-stereo-extra-long',
    });

    await changeRateTo96Khz();
    await selectSettingsPage(3);
    await selectComboItem('dspBackendCombo', 0);
    await waitForCommands(['set-dsp-backend']);
    await waitForLabel('status-dsp-backend', 'Scalar');
    await selectComboItem('dspIdlePolicyCombo', 0);
    await waitForCommands(['set-dsp-idle-policy']);
    await waitForLabel('status-dsp-idle-policy', 'Conservative');
    await selectSettingsPage(0);
    const processing = await getElement('processingEnabledSwitch', 'switch');
    await processing.toggle();
    await waitForCommands(['bypass']);
    await waitForLabel('status-live-processing', 'Bypass');

    expect(await session.inspectConfig()).toEqual(initial);
    const apply = await getElement('applyButton', 'button');
    await apply.click();
    await waitForLabel('status-saved-processing', 'Bypass');
    expect(await session.inspectConfig()).toEqual({
      preset: null,
      preferredOutput: 'alsa_output.pci-0000_0b_00.4.hdmi-stereo-extra-long',
      rateMode: 'fixed',
      fixedRate: 96000,
      rateEnforcement: 'force',
      dspBackend: 'scalar',
      dspSimdVariant: 'auto',
      dspIdlePolicy: 'conservative',
    });
    expect(await session.app.getWindowCount()).toBeGreaterThan(0);
    requests = await session.readRequests();
    expect(requests.map((request) => request.command)).toEqual(
      expect.arrayContaining([
        'set-output',
        'set-rate',
        'set-dsp-backend',
        'set-dsp-idle-policy',
        'bypass',
      ])
    );
  });

  it.each(['escape', 'close'] as const)(
    'rolls live changes back before hiding on %s',
    async (method) => {
      session = await launchPipeTuneGtk();
      await waitForConnected();
      await session.clearRequests();
      const initial = await session.inspectConfig();
      await changeRateTo96Khz();
      await session.clearRequests();

      await hideWithKeyboard(method);
      const requests = await waitForCommands(['set-rate']);
      expect(
        requests.find((request) => request.command === 'set-rate')
      ).toMatchObject({ sampleRate: 192000, enforcement: 'force' });
      await waitForResult(
        async () => {
          const count = await session?.app.getWindowCount();
          if (count !== 0) {
            throw new Error(`dialog still has ${String(count)} windows`);
          }
          return true;
        },
        { timeoutMs: 10_000, message: 'Dialog did not hide after rollback.' }
      );
      expect(await session.inspectConfig()).toEqual(initial);
    }
  );

  it('restores defaults live but waits for Apply before persisting', async () => {
    session = await launchPipeTuneGtk();
    await waitForConnected();
    await session.clearRequests();
    const initial = await session.inspectConfig();
    await selectSettingsPage(4);
    const restore = await getElement('restoreDefaultsButton', 'button');
    await restore.click();
    const requests = await waitForCommands([
      'clear-output',
      'set-rate',
      'set-dsp-backend',
      'set-dsp-idle-policy',
      'bypass',
    ]);
    expect(requests.map((request) => request.command)).toEqual([
      'clear-output',
      'set-rate',
      'set-dsp-backend',
      'set-dsp-idle-policy',
      'bypass',
    ]);
    expect(await session.inspectConfig()).toEqual(initial);

    const apply = await getElement('applyButton', 'button');
    await toPass(
      async () => {
        expect((await apply.info()).states).toContain('sensitive');
      },
      {
        timeoutMs: 10_000,
        message: 'Apply did not become available after restoring defaults.',
      }
    );
    await apply.click();
    await waitForLabel('status-saved-processing', 'Bypass');
    expect(await session.inspectConfig()).toEqual({
      preset: null,
      preferredOutput: null,
      rateMode: 'max',
      fixedRate: 0,
      rateEnforcement: 'suggest',
      dspBackend: 'scalar',
      dspSimdVariant: 'auto',
      dspIdlePolicy: 'conservative',
    });
  });

  it('retains, filters, clears, and dismisses rejected-action logs', async () => {
    session = await launchPipeTuneGtk({ rejectedCommand: 'setRate' });
    await waitForConnected();
    await session.clearRequests();
    await selectSettingsPage(2);
    await selectComboItem('rateCombo', 3);
    await waitForCommands(['set-rate']);
    const drawerToggle = await getElement('logToggleButton', 'toggleButton');
    await toPass(
      async () => {
        expect(await drawerToggle.isChecked()).toBe(true);
      },
      { timeoutMs: 10_000, message: 'Error did not open the log drawer.' }
    );
    const logList = await getElement('logList', 'list');
    expect(await logList.getChildCount()).toBeGreaterThan(1);
    await selectComboItem('logFilterCombo', 2);
    expect(await logList.getChildCount()).toBe(1);
    await (await getElement('logClearButton', 'button')).click();
    expect(await logList.getChildCount()).toBe(0);
    await drawerToggle.toggle();
    expect(await drawerToggle.isChecked()).toBe(false);
    expect(await session.app.getWindowCount()).toBeGreaterThan(0);
  });

  it('keeps the dialog open and the saved snapshot intact after persistence failure', async () => {
    session = await launchPipeTuneGtk();
    await waitForConnected();
    const initial = await session.inspectConfig();
    await session.clearRequests();
    await changeRateTo96Khz();
    await session.setConfigDirectoryWritable(false);
    await (await getElement('applyButton', 'button')).click();
    const drawerToggle = await getElement('logToggleButton', 'toggleButton');
    await toPass(
      async () => {
        expect(await drawerToggle.isChecked()).toBe(true);
      },
      {
        timeoutMs: 10_000,
        message: 'Persistence error did not open the log drawer.',
      }
    );
    expect(await session.inspectConfig()).toEqual(initial);
    expect(await session.app.getWindowCount()).toBeGreaterThan(0);
    await session.setConfigDirectoryWritable(true);
  });

  it('becomes read-only on disconnect and reapplies pending live state after reconnect', async () => {
    session = await launchPipeTuneGtk();
    await waitForConnected();
    await session.clearRequests();
    await changeRateTo96Khz();
    await session.clearRequests();

    await session.disconnectDaemon();
    await waitForLabel('status-system-connection', 'Disconnected');
    const rateCombo = await getElement('rateCombo', 'comboBox');
    expect((await rateCombo.info()).states).not.toContain('sensitive');

    await session.reconnectDaemon();
    await waitForConnected();
    const requests = await waitForCommands(['subscribe', 'set-rate']);
    expect(
      requests.find((request) => request.command === 'set-rate')
    ).toMatchObject({ sampleRate: 96000 });
    await waitForLabel('status-rates-fixed', '96 kHz');
  });
});
