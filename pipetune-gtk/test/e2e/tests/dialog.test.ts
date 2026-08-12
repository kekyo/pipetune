import { afterEach, describe, expect, it } from 'vitest';
import { readFile } from 'node:fs/promises';
import { PNG } from 'pngjs';

import type {
  GtkCaptureBounds,
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

const getWidget = async (id: string): Promise<GtkWidgetElement> => {
  if (session === undefined) {
    throw new Error('GTK session is unavailable');
  }
  return session.app.getById(id);
};

const getElement = async <Kind extends GtkWidgetKind>(
  id: string,
  kind: Kind
): Promise<GtkElementOfKind<Kind>> => {
  return elementOfKind(await getWidget(id), kind);
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
  await toPass(
    async () => {
      const combo = await getElement(id, 'comboBox');
      if (!(await combo.isChildSelected(index))) {
        await combo.selectChildAt(index);
      }
      expect(
        await (await getElement(id, 'comboBox')).isChildSelected(index)
      ).toBe(true);
    },
    {
      timeoutMs: 10_000,
      message: `${id} did not select item ${String(index)}`,
    }
  );
};

const findComboItem = async (id: string, name: string): Promise<number> =>
  waitForResult(
    async () => {
      const combo = await getElement(id, 'comboBox');
      const count = await combo.getChildCount();
      for (let index = 0; index < count; index += 1) {
        const child = await combo.childAt(index);
        if (child !== undefined && (await child.info()).name === name) {
          return index;
        }
      }
      throw new Error(`${name} is unavailable in ${id}`);
    },
    {
      timeoutMs: 10_000,
      message: `${id} did not expose ${name}`,
    }
  );

const expectComboItemSelected = async (
  id: string,
  index: number
): Promise<void> => {
  await toPass(
    async () => {
      expect(
        await (await getElement(id, 'comboBox')).isChildSelected(index)
      ).toBe(true);
    },
    {
      timeoutMs: 10_000,
      message: `${id} did not display item ${String(index)}`,
    }
  );
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

const expectInsideBounds = async (
  element: GtkWidgetElement,
  containerBounds: GtkCaptureBounds
): Promise<void> => {
  const elementCapture = await element.capture();
  expect(elementCapture.bounds.x).toBeGreaterThanOrEqual(containerBounds.x);
  expect(elementCapture.bounds.y).toBeGreaterThanOrEqual(containerBounds.y);
  expect(
    elementCapture.bounds.x + elementCapture.bounds.width
  ).toBeLessThanOrEqual(containerBounds.x + containerBounds.width);
  expect(
    elementCapture.bounds.y + elementCapture.bounds.height
  ).toBeLessThanOrEqual(containerBounds.y + containerBounds.height);
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

const expectLoadMeterBelowConnectionStatus = async (
  meter: GtkElementOfKind<'progressBar'>
): Promise<void> => {
  const heading = await getElement('statusHeadingLabel', 'label');
  const summary = await getElement('connectionSummaryLabel', 'label');
  const icon = await getElement('statusImage', 'image');
  const [meterCapture, headingCapture, summaryCapture, iconCapture] =
    await Promise.all([
      meter.capture(),
      heading.capture(),
      summary.capture(),
      icon.capture(),
    ]);
  const summaryBottom = summaryCapture.bounds.y + summaryCapture.bounds.height;
  const iconRight = iconCapture.bounds.x + iconCapture.bounds.width;
  expect(
    Math.abs(meterCapture.bounds.x - headingCapture.bounds.x)
  ).toBeLessThanOrEqual(2);
  expect(
    Math.abs(summaryCapture.bounds.x - headingCapture.bounds.x)
  ).toBeLessThanOrEqual(2);
  expect(meterCapture.bounds.y).toBeGreaterThan(summaryBottom);
  expect(meterCapture.bounds.y - summaryBottom).toBeLessThanOrEqual(8);
  expect(meterCapture.bounds.x).toBeGreaterThan(iconRight);
};

const changeRateTo96Khz = async (): Promise<void> => {
  await selectSettingsPage(1);
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
  it('reports setup failure and still connects to an available daemon', async () => {
    session = await launchPipeTuneGtk({
      rejectedCommand: undefined,
      setupFails: true,
    });
    await waitForConnected();
    const drawerToggle = await getElement('logToggleButton', 'toggleButton');
    await toPass(
      async () => {
        expect(await drawerToggle.isChecked()).toBe(true);
      },
      {
        timeoutMs: 10_000,
        message: 'Setup failure did not open the action log.',
      }
    );
    await selectComboItem('logFilterCombo', 2);
    expect(await (await getElement('logList', 'list')).getChildCount()).toBe(1);
  });

  it('keeps structured status and controls visible at compact window sizes', async () => {
    session = await launchPipeTuneGtk();
    await waitForConnected();
    const longPresetName =
      'Measurement 1 · 37db4eddbb8479aa · listening-room correction with an intentionally long saved preset name';
    await session.replaceEffeTuneSavedPresets(
      JSON.stringify({
        [longPresetName]: {
          plugins: [{ nm: 'Volume', en: true, vl: 0, ch: 'A' }],
        },
      })
    );
    await findComboItem('presetCombo', `Saved in EffeTune · ${longPresetName}`);

    const window = await getElement('mainWindow', 'window');
    const hints = await window.resizeHints();
    expect(hints.minWidth).toBe(900);
    expect(hints.minHeight).toBe(560);
    const statusPane = await getElement('persistentStatusPane', 'container');
    const settingsPane = await getElement('settingsPane', 'container');
    const switcher = await getElement('settingsSwitcher', 'container');
    const controlsByPage = [
      ['processingEnabledSwitch', 'presetCombo', 'presetChooser'],
      ['rateCombo', 'rateEnforcementCombo'],
      ['dspBackendCombo'],
      [
        'languageCombo',
        'restoreDefaultsButton',
        'pipeTuneVersionLink',
        'effetuneVersionLink',
      ],
    ] as const;

    for (const [width, height] of [
      [1080, 680],
      [900, 560],
    ] as const) {
      const resized = await window.resizeTo(width, height);
      expect(resized.width).toBe(width);
      expect(resized.height).toBe(height);
      expect((await statusPane.capture()).clipped).toBe(false);
      const settingsCapture = await settingsPane.capture();
      expect(settingsCapture.clipped).toBe(false);
      await expectInsideWindow(statusPane, window);
      await expectInsideWindow(settingsPane, window);
      expect((await statusPane.capture()).bounds.width).toBeGreaterThanOrEqual(
        340
      );

      for (let page = 0; page < controlsByPage.length; page += 1) {
        const pageButton = await switcher.childAt(page);
        expect(pageButton).toBeDefined();
        await expectInsideWindow(pageButton as GtkWidgetElement, window);
        await selectSettingsPage(page);
        expect(
          await (await getElement('status-live-processing', 'label')).text()
        ).toBe('Preset');
        for (const controlId of controlsByPage[page] ?? []) {
          await expectInsideBounds(
            await getWidget(controlId),
            settingsCapture.bounds
          );
        }
      }
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
  });

  it('uses expanded status-pane space for long values', async () => {
    session = await launchPipeTuneGtk();
    await waitForConnected();
    const window = await getElement('mainWindow', 'window');
    await window.resizeTo(1200, 740);
    const savedPreset = await getElement('status-saved-preset', 'label');
    expect((await savedPreset.info()).states).toContain('showing');
    expect(await savedPreset.text()).toBe('e2e.effetune_preset');
    const initialWidth = (await savedPreset.capture()).bounds.width;

    await resizeStatusPaneTo(680);

    const expandedWidth = (await savedPreset.capture()).bounds.width;
    expect(expandedWidth).toBeGreaterThanOrEqual(480);
    expect(expandedWidth).toBeGreaterThan(initialWidth + 200);
  });

  it('renders DSP load below the connection status from the label left edge', async () => {
    session = await launchPipeTuneGtk();
    await waitForConnected();
    await waitForLabel('connectionSummaryLabel', 'Connected and monitoring');
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
    await expectLoadMeterBelowConnectionStatus(meter);
    const narrowMeter = await waitForResult(
      async () => measureLoadMeterRaster(window, statusPane),
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
        await expectLoadMeterBelowConnectionStatus(meter);
        const expandedMeter = await measureLoadMeterRaster(window, statusPane);
        expect(expandedMeter.width).toBeGreaterThan(narrowMeter.width);
        expect(expandedMeter.width).toBeGreaterThanOrEqual(278);
        expect(expandedMeter.width).toBeLessThanOrEqual(282);
        expect(expandedMeter.rightInset).toBeGreaterThan(
          narrowMeter.rightInset + 250
        );
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

    await changeRateTo96Khz();
    await selectSettingsPage(2);
    await selectComboItem('dspBackendCombo', 0);
    await waitForCommands(['set-dsp-backend']);
    await waitForLabel('status-dsp-backend', 'Scalar');
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
      rateMode: 'fixed',
      fixedRate: 96000,
      rateEnforcement: 'force',
      dspBackend: 'scalar',
      dspSimdVariant: 'auto',
    });
    expect(await session.app.getWindowCount()).toBeGreaterThan(0);
    const requests = await session.readRequests();
    expect(requests.map((request) => request.command)).toEqual(
      expect.arrayContaining(['set-rate', 'set-dsp-backend', 'bypass'])
    );
  });

  it('refreshes the active snapshot when an EffeTune saved preset changes', async () => {
    session = await launchPipeTuneGtk();
    await waitForConnected();
    await session.replaceEffeTuneSavedPresets(
      JSON.stringify({
        'Live saved preset': {
          plugins: [{ nm: 'Volume', en: true, vl: 6, ch: 'A' }],
        },
      })
    );
    const savedIndex = await findComboItem(
      'presetCombo',
      'Saved in EffeTune · Live saved preset'
    );
    await session.clearRequests();
    await selectComboItem('presetCombo', savedIndex);
    const requests = await waitForCommands(['load']);
    const load = requests.find((request) => request.command === 'load');
    expect(load).toBeDefined();
    const snapshotPath = load?.preset;
    expect(typeof snapshotPath).toBe('string');
    if (typeof snapshotPath !== 'string') {
      throw new Error('Saved preset load path is unavailable.');
    }
    expect(
      (
        JSON.parse(await readFile(snapshotPath, 'utf8')) as {
          plugins: Array<{ vl: number }>;
        }
      ).plugins[0]?.vl
    ).toBe(6);

    await session.replaceEffeTuneSavedPresets(
      JSON.stringify({
        'Live saved preset': {
          plugins: [{ nm: 'Volume', en: true, vl: -6, ch: 'A' }],
        },
      })
    );
    await toPass(
      async () => {
        const snapshot = JSON.parse(await readFile(snapshotPath, 'utf8')) as {
          plugins: Array<{ vl: number }>;
        };
        expect(snapshot.plugins[0]?.vl).toBe(-6);
      },
      {
        timeoutMs: 10_000,
        message: 'The active saved-preset snapshot was not refreshed.',
      }
    );
  });

  it('finishes a processing switch drag across a status refresh', async () => {
    session = await launchPipeTuneGtk();
    await waitForConnected();
    await session.clearRequests();
    const window = await getElement('mainWindow', 'window');
    const processing = await getElement('processingEnabledSwitch', 'switch');
    const bounds = (await processing.capture()).bounds;
    await window.activate();
    const centerY = bounds.y + Math.floor(bounds.height / 2);
    const activeX = bounds.x + Math.floor((bounds.width * 3) / 4);
    const inactiveX = bounds.x + Math.floor(bounds.width / 4);
    await session.app.input.moveMouseTo(activeX, centerY);
    await session.app.input.setMouseButton('left', true);
    try {
      await session.app.input.moveMouseTo(
        bounds.x + Math.floor(bounds.width / 2),
        centerY
      );
      await session.publishStatus();
      await waitForLabel('status-errors-configuration', 'E2E manual status 1');
      await session.app.input.moveMouseTo(inactiveX, centerY);
    } finally {
      await session.app.input.setMouseButton('left', false);
    }

    await toPass(
      async () => {
        expect(await processing.isChecked()).toBe(false);
      },
      {
        timeoutMs: 10_000,
        message: 'Status refresh interrupted the processing switch action.',
      }
    );
    await waitForCommands(['bypass']);
  });

  it('finishes a native backend selection across a status refresh', async () => {
    session = await launchPipeTuneGtk();
    await waitForConnected();
    await selectSettingsPage(2);
    await session.clearRequests();
    const backend = await getElement('dspBackendCombo', 'comboBox');
    await backend.click();
    const scalar = await backend.childAt(0);
    expect(scalar).toBeDefined();

    await session.publishStatus();
    await waitForLabel('status-errors-configuration', 'E2E manual status 1');
    await scalar?.click();

    await expectComboItemSelected('dspBackendCombo', 0);
    await waitForCommands(['set-dsp-backend']);
  });

  it('keeps confirmed SIMD and sample-rate edits applicable after older status events', async () => {
    session = await launchPipeTuneGtk({
      rejectedCommand: undefined,
      staleStatusAfterChange: true,
    });
    await waitForConnected();
    await session.clearRequests();

    await selectSettingsPage(2);
    await selectComboItem('dspBackendCombo', 4);
    await waitForCommands(['set-dsp-backend']);
    await waitForLabel('status-errors-configuration', 'E2E stale status');
    await waitForLabel('status-errors-configuration', 'None');

    const apply = await getElement('applyButton', 'button');
    await toPass(
      async () => {
        expect((await apply.info()).states).toContain('sensitive');
        expect(
          (await (await getElement('dspBackendCombo', 'comboBox')).info())
            .states
        ).toContain('sensitive');
      },
      {
        timeoutMs: 10_000,
        message: 'An older status event locked confirmed SIMD settings.',
      }
    );

    await selectSettingsPage(1);
    await selectComboItem('rateCombo', 3);
    await waitForCommands(['set-rate']);
    await waitForLabel('status-errors-configuration', 'E2E stale status');
    await waitForLabel('status-errors-configuration', 'None');
    await waitForLabel('status-rates-fixed', '96 kHz');
    await toPass(
      async () => {
        expect((await apply.info()).states).toContain('sensitive');
      },
      {
        timeoutMs: 10_000,
        message: 'Apply did not remain available after the sample-rate edit.',
      }
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
    await selectSettingsPage(3);
    const restore = await getElement('restoreDefaultsButton', 'button');
    await restore.click();
    await selectSettingsPage(0);
    expect(
      await (await getElement('processingEnabledSwitch', 'switch')).isChecked()
    ).toBe(false);
    await selectSettingsPage(1);
    await expectComboItemSelected('rateCombo', 0);
    await expectComboItemSelected('rateEnforcementCombo', 0);
    await selectSettingsPage(2);
    await expectComboItemSelected('dspBackendCombo', 0);
    const requests = await waitForCommands([
      'set-rate',
      'set-dsp-backend',
      'bypass',
    ]);
    expect(requests.map((request) => request.command)).toEqual([
      'set-rate',
      'set-dsp-backend',
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
      rateMode: 'automatic',
      fixedRate: 0,
      rateEnforcement: 'suggest',
      dspBackend: 'scalar',
      dspSimdVariant: 'auto',
    });
  });

  it('stages the UI language until Apply and uses it after restart', async () => {
    session = await launchPipeTuneGtk();
    await waitForConnected();
    const initial = await session.inspectConfig();
    await selectSettingsPage(3);
    await selectComboItem('languageCombo', 6);
    const apply = await getElement('applyButton', 'button');
    await toPass(
      async () => {
        expect((await apply.info()).states).toContain('sensitive');
      },
      {
        timeoutMs: 10_000,
        message: 'Apply did not become available after changing language.',
      }
    );
    await expect(
      readFile(session.languageConfigPath, 'utf8')
    ).rejects.toThrow();
    await waitForLabel(
      'languageRestartNotice',
      'Restart PipeTune GTK to use the selected language.'
    );
    expect(await session.inspectConfig()).toEqual(initial);

    await apply.click();
    await toPass(
      async () => {
        expect(await readFile(session?.languageConfigPath ?? '', 'utf8')).toBe(
          '[ui]\nlanguage=ja\n'
        );
      },
      {
        timeoutMs: 10_000,
        message: 'Japanese language preference was not saved by Apply.',
      }
    );

    await toPass(
      async () => {
        expect(await session?.app.getWindowCount()).toBe(2);
      },
      {
        timeoutMs: 10_000,
        message: 'The UI language restart dialog did not appear.',
      }
    );
    const restartDialog = await getElement(
      'ui_language_restart_dialog',
      'infoBar'
    );
    expect((await restartDialog.info()).states).toContain('modal');
    expect(
      (
        await (
          await getElement('ui_language_restart_now_button', 'button')
        ).info()
      ).name
    ).toBe('Restart now');
    await (
      await getElement('ui_language_restart_later_button', 'button')
    ).click();
    await toPass(
      async () => {
        expect(await session?.app.getWindowCount()).toBe(1);
      },
      {
        timeoutMs: 10_000,
        message: 'The restart dialog did not close after choosing Later.',
      }
    );
    await waitForLabel('statusHeadingLabel', 'PipeTune Status');

    await session.restartApplication();
    await waitForLabel('statusHeadingLabel', 'PipeTune の状態');
    await waitForLabel('status-system-connection', '接続済み');
    await selectSettingsPage(3);
    const combo = await getElement('languageCombo', 'comboBox');
    expect(await combo.isChildSelected(6)).toBe(true);
    expect(await session.inspectConfig()).toEqual(initial);
  });

  it('restarts immediately with the saved UI language', async () => {
    session = await launchPipeTuneGtk();
    await waitForConnected();
    await selectSettingsPage(3);
    await selectComboItem('languageCombo', 6);
    await (await getElement('applyButton', 'button')).click();
    await (
      await getElement('ui_language_restart_now_button', 'button')
    ).click();

    await waitForLabel('statusHeadingLabel', 'PipeTune の状態');
    await waitForLabel('status-system-connection', '接続済み');
    await selectSettingsPage(3);
    expect(
      await (await getElement('languageCombo', 'comboBox')).isChildSelected(6)
    ).toBe(true);
  });

  it('discards a staged UI language when Cancel closes the dialog', async () => {
    session = await launchPipeTuneGtk();
    await waitForConnected();
    await selectSettingsPage(3);
    await selectComboItem('languageCombo', 6);
    await (await getElement('cancelButton', 'button')).click();
    await waitForResult(
      async () => {
        const count = await session?.app.getWindowCount();
        if (count !== 0) {
          throw new Error(`dialog still has ${String(count)} windows`);
        }
        return true;
      },
      { timeoutMs: 10_000, message: 'Dialog did not close after Cancel.' }
    );
    await expect(
      readFile(session.languageConfigPath, 'utf8')
    ).rejects.toThrow();

    await session.restartApplication();
    await waitForConnected();
    await selectSettingsPage(3);
    const combo = await getElement('languageCombo', 'comboBox');
    expect(await combo.isChildSelected(0)).toBe(true);
  });

  it('keeps the staged language applicable after a save failure', async () => {
    session = await launchPipeTuneGtk();
    await waitForConnected();
    await session.blockLanguagePreferenceSave();
    await selectSettingsPage(3);
    await selectComboItem('languageCombo', 6);
    const combo = await getElement('languageCombo', 'comboBox');
    const apply = await getElement('applyButton', 'button');
    await toPass(
      async () => {
        expect((await apply.info()).states).toContain('sensitive');
      },
      {
        timeoutMs: 10_000,
        message: 'Staged language did not enable Apply.',
      }
    );
    await apply.click();
    expect(await combo.isChildSelected(6)).toBe(true);
    expect((await apply.info()).states).toContain('sensitive');
    expect(
      (await (await getElement('languageRestartNotice', 'label')).info()).states
    ).toContain('showing');
    await toPass(
      async () => {
        expect(
          await (
            await getElement('logToggleButton', 'toggleButton')
          ).isChecked()
        ).toBe(true);
      },
      {
        timeoutMs: 10_000,
        message: 'Language preference failure was not logged.',
      }
    );
  });

  it('retains, filters, clears, and dismisses rejected-action logs', async () => {
    session = await launchPipeTuneGtk({ rejectedCommand: 'setRate' });
    await waitForConnected();
    await session.clearRequests();
    await selectSettingsPage(1);
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

  it('keeps Restore defaults usable while disconnected', async () => {
    session = await launchPipeTuneGtk();
    await waitForConnected();
    await selectSettingsPage(3);
    await session.clearRequests();

    await session.disconnectDaemon();
    await waitForLabel('status-system-connection', 'Disconnected');
    const restore = await getElement('restoreDefaultsButton', 'button');
    expect((await restore.info()).states).toContain('sensitive');
    await restore.click();

    await session.reconnectDaemon();
    await waitForLabel('status-system-connection', 'Connected');
    const requests = await waitForCommands([
      'subscribe',
      'set-rate',
      'set-dsp-backend',
      'bypass',
    ]);
    expect(requests.map((request) => request.command)).toEqual(
      expect.arrayContaining([
        'subscribe',
        'set-rate',
        'set-dsp-backend',
        'bypass',
      ])
    );
    await waitForLabel('status-live-processing', 'Bypass');
    await toPass(
      async () => {
        expect(
          (await (await getElement('applyButton', 'button')).info()).states
        ).toContain('sensitive');
      },
      {
        timeoutMs: 10_000,
        message: 'Restored defaults did not become applicable after reconnect.',
      }
    );
  });
});
