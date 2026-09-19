/// <reference path="../runtime/reaper.d.ts" />
export {};

// Public Runtime names must match the reviewed inventory; no flat aliases may leak through.
type RuntimeAssert<T extends true> = T;
type RuntimeEqual<A, B> = [A] extends [B] ? ([B] extends [A] ? true : false) : false;
type NoFlatRuntime = RuntimeAssert<RuntimeEqual<Extract<keyof ReaWebAPI, `ReaWeb${string}` | 'ready'>, never>>;

type windowSurface = RuntimeAssert<RuntimeEqual<keyof ReaWebAPI['window'], 'open' | 'openDev' | 'getSize' | 'setSize' | 'getPosition' | 'setPosition' | 'show' | 'hide' | 'getState' | 'setTitle' | 'focus' | 'dock' | 'undock' | 'setDocked' | 'isDocked' | 'setKeyboardCapture' | 'close' | 'reload'>>;
type themeSurface = RuntimeAssert<RuntimeEqual<keyof ReaWebAPI['theme'], 'getColors' | 'onChange' | 'apply'>>;
type dialogSurface = RuntimeAssert<RuntimeEqual<keyof ReaWebAPI['dialog'], 'openFile' | 'saveFile' | 'selectFolder'>>;
type eventsSurface = RuntimeAssert<RuntimeEqual<keyof ReaWebAPI['events'], 'on' | 'off'>>;
type lifecycleSurface = RuntimeAssert<RuntimeEqual<keyof ReaWebAPI['lifecycle'], 'ready' | 'on'>>;
type debugSurface = RuntimeAssert<RuntimeEqual<keyof ReaWebAPI['debug'], 'log' | 'warn' | 'error' | 'inspect' | 'getLogs' | 'getDiagnostics' | 'openDevTools' | 'setBufferSize'>>;
type fsSurface = RuntimeAssert<RuntimeEqual<keyof ReaWebAPI['fs'], 'readText' | 'writeText' | 'readBinary' | 'writeBinary' | 'readFile' | 'writeFile' | 'stat' | 'readDirectory' | 'makeDirectory'>>;
type audioSurface = RuntimeAssert<RuntimeEqual<keyof ReaWebAPI['audio'], 'getFileInfo' | 'getWaveform' | 'getTrackMeter' | 'setTrackValueLatest'>>;
type clipboardSurface = RuntimeAssert<RuntimeEqual<keyof ReaWebAPI['clipboard'], 'readText' | 'writeText'>>;
type dragDropSurface = RuntimeAssert<RuntimeEqual<keyof ReaWebAPI['dragDrop'], 'startFiles' | 'startText' | 'onDrop'>>;
type appSurface = RuntimeAssert<RuntimeEqual<keyof ReaWebAPI['app'], 'getId' | 'getName' | 'getVersion' | 'getRootPath' | 'getDataPath'>>;
type systemSurface = RuntimeAssert<RuntimeEqual<keyof ReaWebAPI['system'], 'getPlatform' | 'getArchitecture' | 'revealInFileManager' | 'openExternal' | 'getCapabilities'>>;
type transactionSurface = RuntimeAssert<RuntimeEqual<keyof ReaWebAPI['transaction'], 'batch' | 'beginUndo' | 'endUndo' | 'withUndo'>>;

async function contract(track: MediaTrackHandle, take: MediaItem_TakeHandle) {
  const name: [boolean, string] = await reaper.GetTrackName(track);
  const midi: [boolean, Uint8Array] = await reaper.MIDI_GetAllEvts(take);
  const project: [ReaProjectHandle | null, string] = await reaper.EnumProjects(-1);
  await reaper.MIDI_SetNote(take, 0, null, undefined, null, null, null, 60);
  await reaper.transaction.batch([{ method: 'CountTracks', args: [0] }]);
  await reaper.transaction.batch([
    { method: 'GetTrack', args: [0, 0] },
    { method: 'SetMediaTrackInfo_Value', args: [{ $ref: 0 }, 'D_VOL', 0.5] }
  ]);
  const text: string = await reaper.fs.readFile('data.json');
  const bytes: Uint8Array = await reaper.fs.readFile('data.bin', {encoding:'binary'});
  await reaper.fs.writeFile('copy.bin', bytes, {encoding:'binary'});
  const value: number = await reaper.transaction.withUndo('edit', async () => 42);
  const runtime: ReaWebRuntimeInfo = (await reaper.system.getCapabilities()).webRuntime;
  const contractVersion: 1 = runtime.contract;
  // @ts-expect-error Destructive project lifetime calls are excluded from batching.
  await reaper.transaction.batch([{method:'Main_OnCommand', args:[40004, 0]}]);
  // @ts-expect-error Binary writes must explicitly select their encoding.
  await reaper.fs.writeFile('copy.bin', bytes);
  // @ts-expect-error Track and take handles are different native types.
  await reaper.GetTrackName(take);
  // @ts-expect-error A batch must explicitly specify its project argument.
  await reaper.transaction.batch([{ method: 'CountTracks', args: [] }]);
  // @ts-expect-error The standard API also requires a project argument.
  await reaper.CountTracks();
  // @ts-expect-error Binary results are not strings.
  const wrong: [boolean, string] = await reaper.MIDI_GetAllEvts(take);
}

async function runtimeContract(track: MediaTrackHandle, take: MediaItem_TakeHandle) {
  const windowId: number = await reaper.window.open('other/index.html');
  // @ts-expect-error The old entry name was removed; use window.open.
  reaper.ReaWebOpen('legacy/index.html');
  // @ts-expect-error Flat Runtime compatibility aliases are intentionally absent.
  reaper.ReaWeb_Open('legacy/index.html');
  // @ts-expect-error Runtime readiness belongs to lifecycle.
  await reaper.ready;
  const ready: Readonly<ReaWebCapabilities> = await reaper.lifecycle.ready;
  const reserved: ReaWebRuntimeNamespace[] = ready.runtime.reservedNamespaces;
  // @ts-expect-error Transactions have moved out of lifecycle; no compatibility alias.
  reaper.lifecycle.batch([]);
  // @ts-expect-error Clipboard I/O is separate from filesystem I/O.
  reaper.fs.readClipboardText();
  // @ts-expect-error External links belong to system.
  reaper.window.openExternal('https://example.com');
  // @ts-expect-error Capability discovery belongs to system.
  reaper.debug.getCapabilities();
  // @ts-expect-error There is no generic App getter; use the five named methods.
  reaper.app.getInfo();
  // @ts-expect-error Choose startFiles or startText explicitly.
  reaper.dragDrop.start();
  // @ts-expect-error The agreed namespace is transaction, not edit.
  reaper.edit.batch([]);
  const text: string = await reaper.fs.readText('settings.json');
  const bytes: Uint8Array = await reaper.fs.readBinary('settings.bin');
  await reaper.fs.writeText('settings.json', text, {overwrite:true});
  await reaper.fs.writeBinary('settings.bin', bytes, {overwrite:true});
  // @ts-expect-error Text helpers require text, not binary buffers.
  await reaper.fs.writeText('settings.json', bytes);
  // @ts-expect-error Binary helpers require byte buffers, not text.
  await reaper.fs.writeBinary('settings.bin', text);
  const bounds: ReaWebBounds = await reaper.window.setSize(800, 600);
  const selected: string | null = await reaper.dialog.openFile({filters:[{name:'Audio', extensions:['wav','flac']}]});
  const cleanup: ReaWebDispose = await reaper.lifecycle.on('cleanup', async () => { await reaper.fs.writeFile('state.json','{}',{overwrite:true}); });
  const stop = await reaper.events.on('track-added', event => { const ids: string[] = event.guids; });
  const waveform: ReaWebWaveform = await reaper.audio.getWaveform('sample.wav', {points:1024});
  const meter: ReaWebTrackMeter = await reaper.audio.getTrackMeter(track);
  const theme: ReaWebTheme = await reaper.theme.getColors();
  const logs: ReaWebLogEntry[] = await reaper.debug.getLogs();
  // @ts-expect-error Take handles cannot be used as track meters.
  await reaper.audio.getTrackMeter(take);
  // @ts-expect-error Event payloads depend on their event name.
  await reaper.events.on('tempo-changed', event => { event.guids; });
  // @ts-expect-error Mirror methods still return Promise values.
  const wrong: MediaTrackHandle | null = reaper.GetTrack(0, 0);
  await cleanup(); await stop();
}

async function appContract() {
  const id: string = await reaper.app.getId();
  const name: string = await reaper.app.getName();
  const version: string | null = await reaper.app.getVersion();
  const root: string = await reaper.app.getRootPath();
  const data: string = await reaper.app.getDataPath();
  const platform: ReaWebPlatform = await reaper.system.getPlatform();
  const arch: ReaWebArchitecture = await reaper.system.getArchitecture();
  const revealed: boolean = await reaper.system.revealInFileManager(data);
  const copied: boolean = await reaper.dragDrop.startFiles([root + '/sample.wav']);
  const text: boolean = await reaper.dragDrop.startText('hello');
  const callback = (drop: ReaWebDropPayload) => { const path: string | undefined = drop.files[0]; };
  const dispose: ReaWebDispose = await reaper.dragDrop.onDrop(callback);
  await reaper.events.off('native-drop', callback);
  await dispose();
  // @ts-expect-error Removed duplicate logging API.
  reaper.debug.info('hello');
  // @ts-expect-error Namespace remains camelCase, without a lowercase alias.
  reaper.dragdrop.startText('hello');
}
