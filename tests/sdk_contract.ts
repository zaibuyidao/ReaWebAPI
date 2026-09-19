/// <reference path="../runtime/reaper.d.ts" />
export {};
async function contract(track: MediaTrackHandle, take: MediaItem_TakeHandle) {
  const name: [boolean, string] = await reaper.GetTrackName(track);
  const midi: [boolean, Uint8Array] = await reaper.MIDI_GetAllEvts(take);
  const project: [ReaProjectHandle | null, string] = await reaper.EnumProjects(-1);
  await reaper.MIDI_SetNote(take, 0, null, undefined, null, null, null, 60);
  await reaper.ReaWeb_Batch([{ method: 'CountTracks', args: [0] }]);
  await reaper.ReaWeb_Batch([
    { method: 'GetTrack', args: [0, 0] },
    { method: 'SetMediaTrackInfo_Value', args: [{ $ref: 0 }, 'D_VOL', 0.5] }
  ]);
  const text: string = await reaper.ReaWeb_ReadFile('data.json');
  const bytes: Uint8Array = await reaper.ReaWeb_ReadFile('data.bin', {encoding:'binary'});
  await reaper.ReaWeb_WriteFile('copy.bin', bytes, {encoding:'binary'});
  const value: number = await reaper.ReaWeb_WithUndo('edit', async () => 42);
  const runtime: ReaWebRuntimeInfo = (await reaper.ReaWeb_GetCapabilities()).webRuntime;
  const contractVersion: 1 = runtime.contract;
  // @ts-expect-error Destructive project lifetime calls are excluded from batching.
  await reaper.ReaWeb_Batch([{method:'Main_OnCommand', args:[40004, 0]}]);
  // @ts-expect-error Binary writes must explicitly select their encoding.
  await reaper.ReaWeb_WriteFile('copy.bin', bytes);
  // @ts-expect-error Track and take handles are different native types.
  await reaper.GetTrackName(take);
  // @ts-expect-error A batch must explicitly specify its project argument.
  await reaper.ReaWeb_Batch([{ method: 'CountTracks', args: [] }]);
  // @ts-expect-error The standard API also requires a project argument.
  await reaper.CountTracks();
  // @ts-expect-error Binary results are not strings.
  const wrong: [boolean, string] = await reaper.MIDI_GetAllEvts(take);
}
