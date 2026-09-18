interface MediaTrackHandle {
  readonly type: 'MediaTrack';
  readonly id: string;
}
interface ReaWebCapabilities {
  version: string;
  methods: string[];
  projectScope: 'current';
}
interface ReaWebAPI {
  CountTracks(project?: 0 | null): Promise<number>;
  CountSelectedTracks(project?: 0 | null): Promise<number>;
  GetTrack(project: 0 | null, index: number): Promise<MediaTrackHandle | null>;
  GetSelectedTrack(project?: 0 | null, index?: number): Promise<MediaTrackHandle | null>;
  GetTrackName(track: MediaTrackHandle): Promise<string>;
  GetMediaTrackInfo_Value(track: MediaTrackHandle, key: 'D_VOL' | 'D_PAN' | 'B_MUTE' | 'I_SOLO'): Promise<number>;
  SetMediaTrackInfo_Value(track: MediaTrackHandle, key: 'D_VOL' | 'D_PAN' | 'B_MUTE' | 'I_SOLO', value: number): Promise<boolean>;
  GetAppVersion(): Promise<string>;
  ReaWebOpen(path: string): Promise<number>;
  ReaWeb_Close(): Promise<boolean>;
  ReaWeb_DevTools(): Promise<boolean>;
  /** Move this window into or out of REAPER's Docker. Returns the resulting docked state. */
  ReaWeb_SetDocked(docked: boolean): Promise<boolean>;
  ReaWeb_IsDocked(): Promise<boolean>;
  ReaWeb_GetCapabilities(): Promise<ReaWebCapabilities>;
}
declare const reaper: Readonly<ReaWebAPI>;
