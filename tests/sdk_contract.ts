/// <reference path="../runtime/reaper.d.ts" />

async function hostMessages() {
  const accepted: boolean = await reaper.host.send({type: 'setVolume', value: 0.5, nested: [null, true]});
  await reaper.host.send('text');
  await reaper.events.on('message', text => { const original: string = text; });
  // @ts-expect-error Host messages require JSON values.
  await reaper.host.send({callback: () => {}});
  // @ts-expect-error Message events carry strings.
  await reaper.events.on('message', (data: number) => {});
}
export {};

// Public Runtime names must match the reviewed inventory; no flat aliases may leak through.
type RuntimeAssert<T extends true> = T;
type RuntimeEqual<A, B> = [A] extends [B] ? ([B] extends [A] ? true : false) : false;
type NoFlatRuntime = RuntimeAssert<RuntimeEqual<Extract<keyof ReaWebAPI, `ReaWeb${string}` | 'ready'>, never>>;

type windowSurface = RuntimeAssert<RuntimeEqual<keyof ReaWebAPI['window'], 'open' | 'openDev' | 'getSize' | 'setSize' | 'getPosition' | 'setPosition' | 'show' | 'hide' | 'getState' | 'setTitle' | 'setIcon' | 'setIconVisible' | 'focus' | 'setDocked' | 'isDocked' | 'setKeyboardCapture' | 'close' | 'reload'>>;
type themeSurface = RuntimeAssert<RuntimeEqual<keyof ReaWebAPI['theme'], 'getColors' | 'apply'>>;
type dialogSurface = RuntimeAssert<RuntimeEqual<keyof ReaWebAPI['dialog'], 'openFile' | 'saveFile' | 'selectFolder'>>;
type eventsSurface = RuntimeAssert<RuntimeEqual<keyof ReaWebAPI['events'], 'on' | 'off'>>;
type lifecycleSurface = RuntimeAssert<RuntimeEqual<keyof ReaWebAPI['lifecycle'], 'ready' | 'on'>>;
type lifecycleEvents = RuntimeAssert<RuntimeEqual<Parameters<ReaWebAPI['lifecycle']['on']>[0], 'before-close' | 'before-reload' | 'cleanup'>>;
type debugSurface = RuntimeAssert<RuntimeEqual<keyof ReaWebAPI['debug'], 'log' | 'warn' | 'error' | 'inspect' | 'getLogs' | 'getDiagnostics' | 'openDevTools' | 'setBufferSize'>>;
type fsSurface = RuntimeAssert<RuntimeEqual<keyof ReaWebAPI['fs'], 'readText' | 'writeText' | 'readBinary' | 'writeBinary' | 'readFile' | 'writeFile' | 'stat' | 'readDirectory' | 'makeDirectory'>>;
type audioSurface = RuntimeAssert<RuntimeEqual<keyof ReaWebAPI['audio'], 'getFileInfo' | 'getWaveform' | 'getTrackMeter' | 'setTrackValueLatest'>>;
type clipboardSurface = RuntimeAssert<RuntimeEqual<keyof ReaWebAPI['clipboard'], 'readText' | 'writeText'>>;
type dragDropSurface = RuntimeAssert<RuntimeEqual<keyof ReaWebAPI['dragDrop'], 'startFiles' | 'startText'>>;
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
  const icon: boolean = await reaper.window.setIcon('logo.svg');
  const iconVisibility: boolean = await reaper.window.setIconVisible(false);
  const iconVisible: boolean = (await reaper.window.getState()).iconVisible;
  // @ts-expect-error Icon visibility requires a boolean.
  await reaper.window.setIconVisible('false');
  // @ts-expect-error Window icons require a local path string.
  await reaper.window.setIcon(new Uint8Array());
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
  const dispose: ReaWebDispose = await reaper.events.on('native-drop', callback);
  await reaper.events.off('native-drop', callback);
  await dispose();
  // @ts-expect-error Removed duplicate logging API.
  reaper.debug.info('hello');
  // @ts-expect-error Namespace remains camelCase, without a lowercase alias.
  reaper.dragdrop.startText('hello');
}

type builderSurface = RuntimeAssert<RuntimeEqual<keyof ReaWebBatchBuilder, ReaWebBatchMethod>>;
async function batchBuilderContract(track: MediaTrackHandle, take: MediaItem_TakeHandle) {
  const data = await reaper.transaction.batch(b => {
    const track = b.GetTrack(0, 0);
    const [, name] = b.GetTrackName(track);
    const volume = b.GetMediaTrackInfo_Value(track, 'D_VOL');
    b.SetMediaTrackInfo_Value(track, 'D_VOL', volume);
    b.GetSetMediaTrackInfo_String(track, 'P_NAME', name, true);
    const [, bytes] = b.MIDI_GetAllEvts(take);
    b.MIDI_SetAllEvts(take, bytes);
    b.MIDI_SetNote(take, 0, null, undefined, null, null, null, 60);
    return { name, volume, track, bytes, nested: [volume, name] as const };
  }, { undoLabel: 'Edit' });
  const name: string = data.name;
  const volume: number = data.volume;
  const handle: MediaTrackHandle | null = data.track;
  const bytes: Uint8Array = data.bytes;
  const nested: readonly [number, string] = data.nested;
  const tuple: [boolean, string] = await reaper.transaction.batch(b => b.GetTrackName(track));
  const transport: { position: number; state: number; beats: number; display: string; valid: boolean } =
    await reaper.transaction.batch(b => {
      const position = b.GetPlayPosition();
      const [beats] = b.TimeMap2_timeToBeats(0, position);
      return { position, state: b.GetPlayState(), beats,
        display: b.format_timestr_pos(position, '', 0), valid: b.ValidatePtr2(0, track, 'MediaTrack*') };
    });
  await reaper.transaction.batch([
    { method: 'GetPlayPositionEx', args: [0] },
    { method: 'GetPlayStateEx', args: [0] },
    { method: 'TimeMap2_timeToBeats', args: [0, { $ref: 0 }] }
  ]);
  const fxName: [boolean, string] = await reaper.transaction.batch(b => b.TrackFX_GetParamName(track, 0, 0));
  const voidResult: null = await reaper.transaction.batch(b => b.UpdateArrange());
  const raw: unknown[] = await reaper.transaction.batch(b => { b.UpdateArrange(); });
  // @ts-expect-error Builder callbacks cannot return Promises.
  await reaper.transaction.batch(async b => b.CountTracks(0));
  await reaper.transaction.batch(b => {
    // @ts-expect-error Action dispatch is outside the reviewed batch set.
    b.Main_OnCommand(40004, 0);
    // @ts-expect-error File rescanning is outside the reviewed batch set.
    b.EnumInstalledFX(-1);
    // @ts-expect-error Audio sample arrays remain individual calls.
    b.GetMediaItemTake_Peaks(take, 100, 0, 2, 10, 0, new Float64Array(40));
    // @ts-expect-error Project variants require an explicit project.
    b.GetPlayStateEx();
    // @ts-expect-error Runtime services are not Mirror builder methods.
    b.window.close();
    // @ts-expect-error Mirror argument requirements are preserved.
    b.CountTracks();
    // @ts-expect-error Deferred track and take handles remain distinct.
    b.GetTrackName(b.GetActiveTake(b.GetMediaItem(0, 0)));
    // @ts-expect-error A string reference cannot supply a numeric argument.
    b.SetMediaTrackInfo_Value(track, 'D_VOL', b.GetTrackName(track)[1]);
    // @ts-expect-error Deferred numbers are not usable before execution.
    const count: number = b.CountTracks(0);
    // @ts-expect-error Ordinary Mirror calls do not accept builder references.
    reaper.GetTrackName(b.GetTrack(0, 0));
  });
}
