import { spawnSync } from 'node:child_process';

const [executable] = process.argv.slice(2);
if (executable === undefined) {
  throw new Error('PipeTune GTK executable path is required');
}

const run = (args) =>
  spawnSync(executable, args, {
    encoding: 'utf8',
    env: {
      ...process.env,
      LC_ALL: 'C.UTF-8',
      LANGUAGE: 'ja',
    },
  });

const help = run(['--help']);
if (
  help.status !== 0 ||
  !help.stdout.includes('Show this help text') ||
  help.stdout.includes('ヘルプ')
) {
  throw new Error(`help output was localized: ${help.stdout}${help.stderr}`);
}

const version = run(['--version']);
if (
  version.status !== 0 ||
  !/^PipeTune GTK .*, EffeTune DSP .*\n$/u.test(version.stdout) ||
  version.stderr !== ''
) {
  throw new Error(
    `version output differs: ${version.stdout}${version.stderr}`
  );
}

const invalid = run(['--not-a-pipetune-option']);
if (
  invalid.status !== 2 ||
  !invalid.stderr.includes(
    'unknown PipeTune GTK option: --not-a-pipetune-option'
  ) ||
  !invalid.stderr.includes('Usage: pipetune-gtk')
) {
  throw new Error(
    `syntax error output was localized: ${invalid.stdout}${invalid.stderr}`
  );
}
