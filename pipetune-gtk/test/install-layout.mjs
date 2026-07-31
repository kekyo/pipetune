import {
  constants,
  accessSync,
  mkdtempSync,
  rmSync,
  statSync,
} from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { spawnSync } from 'node:child_process';

const fail = (message, command) => {
  process.stderr.write(`${message}\n`);
  if (command?.stdout) {
    process.stderr.write(command.stdout);
  }
  if (command?.stderr) {
    process.stderr.write(command.stderr);
  }
  process.exitCode = 1;
};

const [
  cmake,
  buildDirectory,
  desktopFileValidate,
  pixbufThumbnailer,
  installPrefix,
  binaryDirectory,
  applicationDirectory,
  autostartDirectory,
  iconDirectory,
  presetDirectory,
  localeDirectory,
] = process.argv.slice(2);

if (
  !cmake ||
  !buildDirectory ||
  !desktopFileValidate ||
  !pixbufThumbnailer ||
  !installPrefix ||
  !binaryDirectory ||
  !applicationDirectory ||
  !autostartDirectory ||
  !iconDirectory ||
  !presetDirectory ||
  !localeDirectory
) {
  fail('GTK install layout test arguments are incomplete');
} else {
  const stagingDirectory = mkdtempSync(
    join(tmpdir(), 'pipetune-gtk-install-test-')
  );
  const installPath = (directory, fileName) => {
    const destination = directory.startsWith('/')
      ? directory
      : join(installPrefix, directory);
    return join(stagingDirectory, destination.replace(/^\/+/u, ''), fileName);
  };
  try {
    const installed = spawnSync(cmake, ['--install', buildDirectory], {
      encoding: 'utf8',
      env: { ...process.env, DESTDIR: stagingDirectory },
    });
    if (installed.status !== 0) {
      fail('staged PipeTune GTK installation failed', installed);
    } else {
      const executable = installPath(binaryDirectory, 'pipetune-gtk');
      const application = installPath(
        applicationDirectory,
        'net.kekyo.pipetune-gtk.desktop'
      );
      const autostart = installPath(
        autostartDirectory,
        'net.kekyo.pipetune-gtk.desktop'
      );
      const icon = installPath(iconDirectory, 'pipetune.svg');
      const presetManifest = installPath(presetDirectory, 'presets.txt');
      const standardPreset = installPath(
        presetDirectory,
        'processor/bbe.effetune_preset'
      );
      const japaneseCatalog = installPath(
        join(localeDirectory, 'ja', 'LC_MESSAGES'),
        'pipetune-gtk.mo'
      );
      try {
        accessSync(executable, constants.X_OK);
        accessSync(application, constants.R_OK);
        accessSync(autostart, constants.R_OK);
        accessSync(icon, constants.R_OK);
        accessSync(presetManifest, constants.R_OK);
        accessSync(standardPreset, constants.R_OK);
        accessSync(japaneseCatalog, constants.R_OK);
        if (statSync(japaneseCatalog).size === 0) {
          throw new Error('Japanese message catalog is empty');
        }
      } catch (error) {
        fail(`installed PipeTune GTK layout is incomplete: ${error.message}`);
      }

      if (process.exitCode !== 1) {
        const version = spawnSync(executable, ['--version'], {
          encoding: 'utf8',
        });
        if (
          version.status !== 0 ||
          !version.stdout.startsWith('PipeTune GTK ')
        ) {
          fail('installed PipeTune GTK executable is not runnable', version);
        }

        for (const desktopFile of [application, autostart]) {
          const validated = spawnSync(
            desktopFileValidate,
            [desktopFile],
            { encoding: 'utf8' }
          );
          if (validated.status !== 0) {
            fail('installed PipeTune GTK desktop entry is invalid', validated);
          }
        }

        const thumbnail = join(stagingDirectory, 'pipetune-thumbnail.png');
        const rendered = spawnSync(
          pixbufThumbnailer,
          ['--size', '64', icon, thumbnail],
          { encoding: 'utf8' }
        );
        if (
          rendered.status !== 0 ||
          statSync(thumbnail, { throwIfNoEntry: false })?.size === 0
        ) {
          fail('installed PipeTune GTK icon cannot be rendered', rendered);
        }
      }
    }
  } finally {
    rmSync(stagingDirectory, { recursive: true, force: true });
  }
}
