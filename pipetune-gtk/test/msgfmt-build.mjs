import { mkdtempSync, rmSync, statSync } from 'node:fs';
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

const [cmake, projectDirectory, msgfmt] = process.argv.slice(2);

if (!cmake || !projectDirectory || !msgfmt) {
  fail('msgfmt build test arguments are incomplete');
} else {
  const temporaryDirectory = mkdtempSync(
    join(tmpdir(), 'pipetune gtk msgfmt build-')
  );
  const buildDirectory = join(temporaryDirectory, 'build output');
  try {
    const configured = spawnSync(
      cmake,
      [
        '-S',
        projectDirectory,
        '-B',
        buildDirectory,
        '-DBUILD_TESTING=OFF',
        `-DPIPETUNE_GTK_MSGFMT_EXECUTABLE=${msgfmt}`,
      ],
      { encoding: 'utf8' }
    );
    if (configured.status !== 0) {
      fail('msgfmt test build configuration failed', configured);
    } else {
      const built = spawnSync(
        cmake,
        ['--build', buildDirectory, '--target', 'pipetune_gtk_translations'],
        { encoding: 'utf8' }
      );
      if (built.status !== 0) {
        fail('msgfmt translation target failed', built);
      } else {
        const catalogLocales = [
          'ar',
          'es',
          'fr',
          'hi',
          'ja',
          'ko',
          'pt',
          'ru',
          'zh',
        ];
        for (const locale of catalogLocales) {
          const catalog = join(
            buildDirectory,
            'pipetune-gtk-build',
            'locale',
            locale,
            'LC_MESSAGES',
            'pipetune-gtk.mo'
          );
          const catalogStatus = statSync(catalog, { throwIfNoEntry: false });
          if (catalogStatus === undefined || catalogStatus.size === 0) {
            fail(`msgfmt translation target did not produce ${locale}`);
          }
        }
      }
    }
  } finally {
    rmSync(temporaryDirectory, { recursive: true, force: true });
  }
}
