/** ReaWebAPI Runtime contract 2. Standard REAPER APIs use their original names. */
type ReaWebDispose = () => Promise<void>;
interface ReaWebStreamInfo {
  name: string; kind: 'frame' | 'audio' | 'spectrum' | 'meter' | 'waveform' | 'binary' | 'midi';
  format: 'rgba8' | 'bgra8' | 'float32' | 'bytes'; capacity: number; maxBytes: number;
  width: number; height: number; stride: number; channels: number; sampleRate: number;
  blockFrames: number; fftSize: number; binHz: number; updateRate: number; source: string;
  policy: 'latest' | 'drop-new';
  layoutVersion?: 2;
  jsfx?: Readonly<{ sessionId: string; id: string; source: string; [key: string]: unknown }>;
}
interface ReaWebStreamPacket {
  sequence: bigint; frameId: bigint; timestamp: number; producerDropped: bigint;
  data: Uint8Array | Float32Array; bytes: Uint8Array;
  timing?: Readonly<{ clockEpoch: bigint; firstSample: bigint; sourceDropped: bigint;
    position: number; playState: number; discontinuity: boolean }>;
}
interface ReaWebStream {
  readonly info: Readonly<ReaWebStreamInfo>;
  readonly closed: boolean;
  readonly error: (Error & { code: string }) | null;
  readonly dropped: number;
  /** Reads the consumer's cached data without a native call. */
  latest(): ReaWebStreamPacket | null;
  /** Audio/MIDI dequeue one block. Other types return latest(). Empty queues return null. */
  read(): ReaWebStreamPacket | null;
  on(event: 'data', callback: (packet: ReaWebStreamPacket) => void): () => void;
  on(event: 'close' | 'error', callback: (error: Error & { code: string }) => void): () => void;
  /** Detaches this consumer. The native producer and other consumers remain active. */
  close(): Promise<void>;
}
interface ReaWebFileChange {
  id: number; type: 'ready' | 'created' | 'changed' | 'renamed' | 'deleted' | 'overflow' | 'error';
  path?: string; oldPath?: string; directory?: boolean; code?: string; message?: string;
}
interface ReaWebDevices {
  audio: Record<'MODE' | 'IDENT_IN' | 'IDENT_OUT' | 'BSIZE' | 'SRATE' | 'BPS', string | null> & {
    inputs: { id: number; name: string }[]; outputs: { id: number; name: string }[];
  };
  midiInputs: { id: number; name: string; present: boolean }[];
  midiOutputs: { id: number; name: string; present: boolean }[];
}
interface ReaWebDisplay {
  id: string; bounds: { x: number; y: number; width: number; height: number };
  workArea: ReaWebDisplay['bounds']; scaleFactor: number; dpi: number; primary: boolean; units: 'native';
}
type ReaWebHostMessage = string | number | boolean | null | ReaWebHostMessage[] | { [key: string]: ReaWebHostMessage };
interface ReaWebHostService {
  invoke<T = ReaWebHostMessage>(method: string, payload?: ReaWebHostMessage): Promise<T>;
  /** Fire-and-forget. Dispatch failures are reported to the Runtime error log. */
  send(method: string, payload?: ReaWebHostMessage): void;
  /** No replay. The reserved unloaded event carries EXTENSION_UNLOADED. */
  on<T = ReaWebHostMessage>(event: string, callback: (data: T) => void): Promise<ReaWebDispose>;
  off<T = ReaWebHostMessage>(event: string, callback: (data: T) => void): Promise<void>;
}
interface ReaWebNativeState { projectEpoch: number; revision: number; available: boolean; }
interface ReaWebNativeInvalidation extends ReaWebNativeState { count: number; invalidated: true; }
interface ReaWebRegion { id: number; start: number; end: number; name: string; color: number; }
type ReaWebPlatform = 'windows' | 'macos' | 'linux';
/** Architecture of the running extension process (including under emulation). */
type ReaWebArchitecture = 'x64' | 'arm64' | 'x86' | 'arm' | 'unknown';
interface ReaWebDropPayload {
  /** Absolute native paths; incoming directories are allowed. No file contents are read. */
  files: string[];
  text: string;
  /** Drop position in viewport CSS pixels. */
  x: number; y: number;
}
interface ReaWebBounds { x: number; y: number; width: number; height: number; mode: 'floating' | 'docked'; units: 'native'; }
interface ReaWebTheme {
  available: boolean;
  colors: { background: string; text: string; highlight: string; panel: string; border: string };
  cssVariables: Record<'--reaper-background' | '--reaper-text' | '--reaper-highlight' | '--reaper-panel' | '--reaper-border', string>;
}
interface ReaWebDialogOptions {
  title?: string;
  initialPath?: string;
  filters?: { name: string; extensions: string[] }[];
}
interface ReaWebAudioFileInfo {
  path: string; sampleRate: number; channels: number;
  /** Null when the decoder does not expose a meaningful source bit depth. */
  bitDepth: number | null;
  /** Complete source length, in seconds. */
  duration: number;
  format: string;
}
interface ReaWebWaveformOptions {
  /** 1..8192 peak frames per channel; default 1024. */
  points?: number;
  /** Seconds from the beginning of the file. */
  start?: number;
  /** Seconds to cover; defaults to the remainder of the file. */
  duration?: number;
}
interface ReaWebWaveform extends ReaWebAudioFileInfo {
  start: number; rangeDuration: number; points: number; requestedPoints: number; peaksPerSecond: number;
  /** Per-channel linear-amplitude minimum and maximum envelopes. */
  data: { min: number[]; max: number[] }[];
}
interface ReaWebTrackMeter {
  channels: number; peak: number[];
  /** dBFS; null represents silence. These are instantaneous host peak readings, not RMS/LUFS. */
  peakDb: (number | null)[];
  unit: 'linear-amplitude';
}
interface ReaWebLogEntry {
  time: number; windowId: number; appId: string; documentGeneration: number;
  level: 'debug' | 'info' | 'warn' | 'error'; source: 'javascript' | 'native' | 'runtime'; message: string;
  code?: string; method?: string; requestId?: number | null;
}
interface ReaWebCleanupEvent { reason: 'close' | 'reload' | 'unload'; timeoutMs: number; }
type ReaWebRuntimeNamespace = 'window' | 'theme' | 'dialog' | 'events' | 'lifecycle' | 'debug' | 'fs' | 'audio'
  | 'clipboard' | 'dragDrop' | 'app' | 'system' | 'transaction' | 'host' | 'stream';
interface ReaWebRuntimeCapabilities {
  contract: 2; namespaces: ReaWebRuntimeNamespace[]; cleanupTimeoutMs: number;
  /** JSFX-specific service protocol, when the optional module is registered. */
  jsfx?: { protocolVersion: 1 };
  fs?: { mediaURLs: boolean; maxOpenMedia: number; maxMediaBytes: number };
  window?: { hiddenStart: boolean; frameless: boolean; resizable: boolean; dragRegions?: boolean; windowSystem?: 'win32' | 'appkit' | 'x11';
    setFrameless?: boolean;
    cornerRadius?: { supported: boolean; framed: boolean; frameless: boolean; max: number };
    /** The creation option is accepted. Check transparency.supported for desktop Alpha. */
    transparent: boolean;
    transparency: { supported: boolean; framed: boolean; frameless: boolean; floatingOnly: boolean; backend: string; reason: string } };
  /** Reserved Runtime namespace identifiers. */
  reservedNamespaces: ReaWebRuntimeNamespace[];
  host: { maxMessageBytes: number; maxPendingMessages: number; maxQueuedBytes: number;
    serviceABI: 1; serviceTimeoutMs: 30000; builtinServices: string[] };
  dragDrop: { maxFiles: number; maxTextBytes: number; effect: 'copy' };
  audio: { maxChannels: number; maxWaveformPoints: number; maxPendingJobs: number };
  stream: { abi: 1; transport: 'websocket-binary'; maxStreams: number; maxConsumers: number; maxBytes: number; kinds: ReaWebStreamInfo['kind'][] };
}
interface ReaWebCapabilities { runtime: ReaWebRuntimeCapabilities; }
interface ReaWebWindowOpenOptions { hidden?: boolean; frameless?: boolean; resizable?: boolean; transparent?: boolean; }
interface ReaWebDevToolsState {
  mode: 'embedded' | 'floating';
  /** Embedded panel share of available width: 0.2–0.8, default 0.4. */
  widthRatio: number;
  embeddedSupported: boolean;
  /** Visibility of the inspector managed by this backend. */
  visible?: boolean;
  pending?: boolean;
  /** Whether this backend can open/hide the native inspector programmatically. */
  nativeToggleSupported?: boolean;
  fallbackReason?: string;
  lastError?: string;
}
interface ReaWebDiagnostics {
  system: { processCpuSeconds: number; logicalProcessors: number };
  streams: { allocatedBytes: number; consumers: number; streams: { name: string; published: number; dropped: number }[]; transport?: { connections: number; handshakeFailures: number; expiredTickets: number; lastHandshakeError: string } };
  audioCapture: { callbacks: number; users: number; sampleRate: number; published: number; dropped: number; unavailable: number; blockFrames: number; channels: number; bufferAvailable: boolean }[];
  lifecycleAction: string; audioJobs: number; recentLogs: ReaWebLogEntry[]; devtools: ReaWebDevToolsState;
  /** Frame channel. `pushed` keeps rising while the page is idle; `luaFrameAgeMs` says how stale Lua's content is. */
  frames?: { pushed: number; intervalMs: number; luaWrites: number; luaFrameAgeMs: number | null }; }
interface ReaWebEvents {
  /** Pushed by the extension on a fixed cadence, independent of how often Lua runs.
   * `lua` is the JSON text Lua last wrote with ReaWeb_SetFrame, unchanged. */
  frame: { sequence: number; playing?: number; position?: number; tempo?: number; cursor?: number;
    lua?: string; luaAgeMs?: number };
  devicesChanged: ReaWebDevices;
  'file-change': ReaWebFileChange;
  'native-timer': { id: number };
  trackSelectionChanged: ReaWebNativeInvalidation;
  trackStateChanged: ReaWebNativeInvalidation;
  markersChanged: ReaWebNativeInvalidation;
  regionsChanged: ReaWebNativeInvalidation;
  transportChanged: ReaWebNativeState & { state?: number; playing?: boolean; paused?: boolean; recording?: boolean;
    position?: number; cursor?: number; rate?: number; loop?: boolean };
  projectChanged: ReaWebNativeState & { activeProject?: string; projects?: { id: string; path: string; dirty: boolean }[];
    changeCount?: number; generation?: number };
  currentRegionChanged: ReaWebNativeState & { region?: ReaWebRegion | null };
  loopPointsChanged: ReaWebNativeState & { start?: number; end?: number };
  timeSelectionChanged: ReaWebEvents['loopPointsChanged'];
  /** Original Lua text, FIFO within this document. No snapshot or replay. */
  message: string;
  /** Discrete native drops; no initial snapshot or replay. */
  'native-drop': ReaWebDropPayload;
  'track-added': { projectEpoch: number; revision: number; guids: string[] };
  'track-deleted': { projectEpoch: number; revision: number; guids: string[] };
  'track-selected': ReaWebEvents['selectionchange'];
  /** Project-wide invalidation, not a per-object edit log. */
  'item-changed': { projectEpoch: number; changeCount: number; scope: 'project'; invalidated: true };
  'take-changed': ReaWebEvents['item-changed'];
  'playback-state-changed': { projectEpoch: number; available: boolean; state?: number };
  'tempo-changed': { projectEpoch: number; available: boolean; tempo?: number };
  'marker-changed': { projectEpoch: number; revision: number; invalidated: true };
  'fx-changed': ReaWebEvents['marker-changed'];
  'project-loaded': { projectEpoch: number };
  /** Observed current-project save; excludes Undo serialization and autosave backups. */
  'project-saved': { projectEpoch: number; path: string; observation: 'file-updated-and-project-clean' };
  'theme-changed': ReaWebTheme;
}
interface ReaWebAPI {
  readonly stream: {
    open(name: string): Promise<ReaWebStream>;
    getDiagnostics(): Promise<{ allocatedBytes: number; consumers: number; streams: { name: string; published: number; dropped: number }[]; transport?: { connections: number; handshakeFailures: number; expiredTickets: number; lastHandshakeError: string } }>;
  };
  readonly host: {
    service(name: string): ReaWebHostService;
    /** Queue text for Lua ReaWeb_Receive. Other JSON values are serialized. Resolves true on acceptance.
     * Limit 1 MiB UTF-8, no raw NUL. Invalid values and queue overflow reject. */
    send(message: ReaWebHostMessage): Promise<boolean>;
  };
  readonly fs: {
    /** Document-scoped read-only media URL. Release when unused. Reload/close revokes it. */
    openMedia(path: string): Promise<Readonly<{ url: string; size: number; type: string; close(): Promise<boolean> }>>;
    /** Resolves after the native baseline scan. Changes are coalesced at 250 ms. */
    watch(path: string, callback: (event: ReaWebFileChange) => void | Promise<void>, options?: { recursive?: boolean }): Promise<ReaWebDispose>;
    /** Encoding-specific helpers use the same worker and 16 MiB limit as readFile/writeFile. */
    readText(path: string): Promise<string>;
    writeText(path: string, text: string, options?: { overwrite?: boolean }): Promise<{ path: string; bytes: number }>;
    readBinary(path: string): Promise<Uint8Array>;
    writeBinary(path: string, bytes: Uint8Array, options?: { overwrite?: boolean }): Promise<{ path: string; bytes: number }>;
    /** Files run on the worker, relative to the entry directory. Absolute local paths are allowed. */
    readFile(path: string, options?: { encoding?: 'utf8' }): Promise<string>;
    readFile(path: string, options: { encoding: 'binary' }): Promise<Uint8Array>;
    /** Atomic replacement; overwrite defaults to false. Parent directory must exist. Limit 16 MiB. */
    writeFile(path: string, data: string, options?: { encoding?: 'utf8'; overwrite?: boolean }): Promise<{ path: string; bytes: number }>;
    writeFile(path: string, data: Uint8Array, options: { encoding: 'binary'; overwrite?: boolean }): Promise<{ path: string; bytes: number }>;
    stat(path: string): Promise<ReaWebFileInfo>;
    readDirectory(path: string): Promise<ReaWebDirectoryEntry[]>;
    makeDirectory(path: string, options?: { recursive?: boolean }): Promise<boolean>;
    /** Worker operation. Both paths are entry-relative or absolute filenames. Parent must exist.
     * No overwrite by default. Regular files only, no final symlinks. Cross-volume moves copy then delete.
     * FILE_MOVE_PARTIAL means the complete destination exists but removing the source failed. */
    moveFile(source: string, destination: string, options?: { overwrite?: boolean }): Promise<{ source: string; path: string; bytes: number }>;
  };
  readonly clipboard: {
    /** Custom ReaWebAPI MIME format. Limit 16 MiB. */
    readBinary(format: string): Promise<Uint8Array>;
    writeBinary(format: string, bytes: Uint8Array): Promise<boolean>;
    readText(): Promise<string>;
    writeText(text: string): Promise<boolean>;
  };
  readonly system: {
    getDevices(): Promise<ReaWebDevices>;
    getDisplays(): Promise<ReaWebDisplay[]>;
    openMIDIInput(device?: number): Promise<ReaWebStream>;
    /** Milliseconds. Repeats skip missed periods. This is not a frame clock. */
    schedule(callback: (event: { id: number }) => void | Promise<void>, options?: { delay?: number; interval?: number }): Promise<ReaWebDispose>;
    getCapabilities(): Promise<ReaWebCapabilities>;
    getPlatform(): Promise<ReaWebPlatform>;
    getArchitecture(): Promise<ReaWebArchitecture>;
    /** Selects an existing local path in the OS file manager. Linux may open its parent instead. */
    revealInFileManager(path: string): Promise<boolean>;
    /** Opens http/https/mailto using the operating system; never navigates the privileged page. */
    openExternal(url: string): Promise<boolean>;
  };
  readonly dragDrop: {
    /** Native copy drag of 1..256 existing regular files. Hold the left mouse button.
     * Relative paths resolve from the entry directory. False means cancelled/rejected. */
    startFiles(paths: string[]): Promise<boolean>;
    /** Native copy drag of nonempty text (at most 16 MiB UTF-8). Requires the same mouse gesture. */
    startText(text: string): Promise<boolean>;
  };
  readonly app: {
    /** Manifest ID or normalized Lua launcher filename for production Apps, URL-derived identity in development. */
    getId(): Promise<string>;
    /** app.json name, otherwise the entry directory's name. */
    getName(): Promise<string>;
    /** app.json version, or null when absent. */
    getVersion(): Promise<string | null>;
    getRootPath(): Promise<string>;
    /** App-specific writable directory under REAPER's resource directory. */
    getDataPath(): Promise<string>;
  };
  readonly events: {
    on<K extends keyof ReaWebEvents>(name: K, callback: (state: ReaWebEvents[K]) => void | Promise<void>): Promise<ReaWebDispose>;
    /** Removes all registrations of this callback for this name; absent callbacks are a no-op. */
    off<K extends keyof ReaWebEvents>(name: K, callback: (state: ReaWebEvents[K]) => void | Promise<void>): Promise<void>;
  };
  readonly window: {
    /** Opens local HTML; relative paths resolve from the calling page's directory. */
    open(path: string, options?: ReaWebWindowOpenOptions): Promise<number>;
    setDocked(docked: boolean): Promise<boolean>;
    isDocked(): Promise<boolean>;
    /** Default true. False allows REAPER's normal global shortcut policy for this window. */
    setKeyboardCapture(capture: boolean): Promise<boolean>;
    /** Route unmodified Space outside editable fields to REAPER for this App or JSFX page.
     * Overrides JSON until navigation/reload, without writing configuration or changing keyboard capture. */
    setDawKeyboard(enabled: boolean): Promise<boolean>;
    /** Explicitly trust a loopback HTTP server; regular window.open stays local-file only. */
    openDev(url: string): Promise<number>;

    getSize(): Promise<Pick<ReaWebBounds, 'width' | 'height' | 'mode' | 'units'>>;
    /** Floating windows only. Values are outer dimensions in native desktop units, not CSS pixels. */
    setSize(width: number, height: number): Promise<ReaWebBounds>;
    getPosition(): Promise<Pick<ReaWebBounds, 'x' | 'y' | 'mode' | 'units'>>;
    setPosition(x: number, y: number): Promise<ReaWebBounds>;
    show(): Promise<ReaWebWindowState>; hide(): Promise<ReaWebWindowState>;
    getState(): Promise<ReaWebWindowState>;
    /** Changes the current window without navigation. Docker retains the preference until undocking. */
    setFrameless(frameless: boolean): Promise<ReaWebWindowState>;
    /** Finite radius 0..64 in logical desktop units. Zero removes custom clipping.
     * Suspended while docked/maximized. macOS/Linux use system corners when framed. */
    setCornerRadius(radius: number): Promise<ReaWebWindowState>;
    /** Override the HTML title for this window, including reloads. Requires 1–256 UTF-8 bytes without NUL. */
    setTitle(title: string): Promise<boolean>;
    /** Overrides automatic HTML favicon synchronization for this window session, including reloads. PNG, ICO or SVG from an App-root-relative or absolute local path. */
    setIcon(path: string): Promise<boolean>;
    /** Shows/hides the title-bar icon and its space, retaining the icon. Defaults to true and survives reloads. Linux decorations depend on the window manager. Resolves to true on success. */
    setIconVisible(visible: boolean): Promise<boolean>;
    focus(): Promise<boolean>;
    close(): Promise<boolean>; reload(): Promise<boolean>;
  };
  readonly dialog: {
    /** Null is cancellation, not an error. */
    openFile(options?: ReaWebDialogOptions): Promise<string | null>;
    /** Chooses a destination; does not write the file. */
    saveFile(options?: ReaWebDialogOptions): Promise<string | null>;
    selectFolder(options?: ReaWebDialogOptions): Promise<string | null>;
  };
  readonly theme: {
    getColors(): Promise<ReaWebTheme>;
    /** Applies variables and follows changes; disposal restores previous inline values. */
    apply(element?: HTMLElement): Promise<ReaWebDispose>;
  };
  readonly debug: {
    /** Capacity for fixed native output buffers in this document (4096..16777216 bytes). Default 65536.
     * NeedBig buffers grow through REAPER's allocator automatically. */
    setBufferSize(bytes: number): Promise<number>;

    log(...values: unknown[]): Promise<boolean>;
    warn(...values: unknown[]): Promise<boolean>; error(...values: unknown[]): Promise<boolean>;
    inspect(value: unknown): Promise<boolean>; getLogs(): Promise<ReaWebLogEntry[]>;
    getDiagnostics(): Promise<ReaWebDiagnostics>; openDevTools(): Promise<boolean>;
  };
  readonly lifecycle: {
    /** Resolves after the native protocol handshake. API calls wait for this automatically. */
    readonly ready: Promise<Readonly<ReaWebCapabilities & { windowId: number; projectEpoch: number }>>;
    /** Callbacks run before document destruction and may return a Promise.
     * Native cleanup still runs after a 2000 ms deadline. */
    on(name: 'before-close' | 'before-reload' | 'cleanup', callback: (event: ReaWebCleanupEvent) => void | Promise<void>): Promise<ReaWebDispose>;
  };
  readonly transaction: {
    /** 1–128 synchronous calls on the current project. Literal arguments are validated up front;
     * references are resolved and validated before their call. See capabilities.batchMethods.
     * BATCH_FAILED includes completed/results/cause; completed writes are not rolled back.
     */
    batch(calls: ReaWebBatchCall[], options?: { undoLabel?: string }): Promise<unknown[]>;
    /** Collect Mirror calls synchronously. Tuple results support indexing and destructuring.
     * Return a reference or nested plain object/array to select results; no return yields the ordered result array.
     * References are not Promises or values for branching/arithmetic. Native void results remain null.
     */
    batch<T>(callback: (builder: ReaWebBatchBuilder) => T & (T extends PromiseLike<unknown> ? never : unknown),
      options?: { undoLabel?: string }): Promise<ReaWebBatchResult<T>>;
    /** One managed gesture at a time. Ends on close, reload, project change or after 30 seconds.
     * Does not hold PreventUIRefresh across browser events. Uses the reviewed batch API set. End before batching. */
    beginUndo(label: string): Promise<string>;
    endUndo(token: string): Promise<boolean>;
    withUndo<T>(label: string, callback: () => T | Promise<T>): Promise<T>;

  };
  readonly audio: {
    /** Built-in meter Float32 prefix (7*C+14): samplePeak[C], truePeak[C], channelRms[C], sampleClipCount[C],
     * truePeakClipCount[C], channelMaxSamplePeak[C], channelMaxTruePeak[C],
     * rmsMomentary, rmsIntegrated, maxRmsMomentary, lufsMomentary, lufsShortTerm, lufsIntegrated,
     * loudnessRange, maxLufsMomentary, maxLufsShortTerm, maxSamplePeak, maxTruePeak, processedSeconds,
     * loudnessRangeLow, loudnessRangeHigh. Global peak maxima reduce the channel maxima.
     * A 6*C Float32 suffix preserves uint64 counters as 24/24/16-bit limbs.
     * Use decodeMeter for exact bigint counts. Prefix counter slots are approximate compatibility values.
     * True Peak, RMS-I, LUFS-M/S/I and LRA follow Cockos loudness_meter.
     * Peaks/channel RMS are linear amplitude. Global RMS is dBFS, loudness is LUFS, LRA is LU.
     * Clip counts and maxima accumulate until reset. Channel RMS covers the publication interval.
     * All meter history shares one reset lifecycle. processedSeconds counts processed PCM frames / sample rate. */
    openStream(kind: 'audio' | 'spectrum' | 'meter' | 'waveform', options?: {
      source?: 'master' | 'input' | 'selected-track' | `track:${string}`; fftSize?: number; updateRate?: number;
      /** Sum routed pre-FX track source PCM. Track sources only, defaults to false. */
      aggregate?: boolean;
      /** Meter only. Cockos mono calibration: subtract 3 dB from global RMS/LUFS, without downmixing PCM. */
      forceMono?: boolean;
      /** Meter only. Defaults to playback-only. Controls RMS-I, LUFS-I and LRA history only. */
      integratedMode?: 'continuous' | 'playback-only';
      /** Meter only, defaults to true. Hardware sources still analyze while stopped. */
      resetOnPlaybackStart?: boolean;
    }): Promise<ReaWebStream>;
    /** Reset a built-in meter opened by this window. Resolves when the worker reset is queued. */
    resetMeter(name: string): Promise<boolean>;
    /** Decode the built-in Meter's 13*C+14 Float32 payload without modifying it. */
    decodeMeter(data: Float32Array, channels: number): {
      samplePeak: Float32Array; truePeak: Float32Array; channelRms: Float32Array;
      sampleClipCount: bigint[]; truePeakClipCount: bigint[];
      channelMaxSamplePeak: Float32Array; channelMaxTruePeak: Float32Array;
      rmsMomentary: number; rmsIntegrated: number; maxRmsMomentary: number;
      lufsMomentary: number; lufsShortTerm: number; lufsIntegrated: number;
      loudnessRange: number; loudnessRangeLow: number; loudnessRangeHigh: number;
      maxLufsMomentary: number; maxLufsShortTerm: number;
      maxSamplePeak: number; maxTruePeak: number; processedSeconds: number;
    };
    /** Explicit coalescing: one in-flight write and the latest waiting value per track/key.
     * Superseded values settle without being sent. Does not create an Undo gesture.
     */
    setTrackValueLatest(track: MediaTrackHandle, key: ReaWebContinuousTrackKey, value: number): Promise<{ applied: boolean; superseded: boolean }>;

    getFileInfo(path: string): Promise<ReaWebAudioFileInfo>;
    getWaveform(path: string, options?: ReaWebWaveformOptions): Promise<ReaWebWaveform>;
    getTrackMeter(track: MediaTrackHandle): Promise<ReaWebTrackMeter>;
  };
}
