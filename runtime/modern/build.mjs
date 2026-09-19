import { build } from 'vite';
import { readFile, writeFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
const root = fileURLToPath(new URL('.', import.meta.url));
await build({
  root,
  build: {
    target: 'es2022',
    lib: { entry: fileURLToPath(new URL('src/main.ts', import.meta.url)), name: 'ReaWebStarter', formats: ['iife'], fileName: () => 'app.js', cssFileName: 'style' },
    cssCodeSplit: false
  }
});
const source = await readFile(new URL('index.html', import.meta.url), 'utf8');
await writeFile(new URL('dist/index.html', import.meta.url), source
  .replace('<script type="module" src="/src/main.ts"></script>', '<script defer src="./app.js"></script>')
  .replace('</head>', '<link rel="stylesheet" href="./style.css"></head>'));
