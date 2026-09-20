/** ReaWebAPI Runtime contract 2. Standard REAPER APIs use their original names. */
type ReaWebDispose = () => Promise<void>;
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
  | 'clipboard' | 'dragDrop' | 'app' | 'system' | 'transaction';
interface ReaWebRuntimeCapabilities {
  contract: 2; namespaces: ReaWebRuntimeNamespace[]; cleanupTimeoutMs: number;
  /** Reserved Runtime namespace identifiers. */
  reservedNamespaces: ReaWebRuntimeNamespace[];
  dragDrop: { maxFiles: number; maxTextBytes: number; effect: 'copy' };
  audio: { maxChannels: number; maxWaveformPoints: number; maxPendingJobs: number };
}
interface ReaWebCapabilities { runtime: ReaWebRuntimeCapabilities; }
interface ReaWebDiagnostics { lifecycleAction: string; audioJobs: number; recentLogs: ReaWebLogEntry[]; }
interface ReaWebEvents {
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
  readonly fs: {
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
  };
  readonly clipboard: {
    readText(): Promise<string>;
    writeText(text: string): Promise<boolean>;
  };
  readonly system: {
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
    /** Stable storage identity for the canonical local entry directory or development URL. */
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
    open(path: string): Promise<number>;
    setDocked(docked: boolean): Promise<boolean>;
    isDocked(): Promise<boolean>;
    /** Default true. False allows REAPER's normal global shortcut policy for this window. */
    setKeyboardCapture(capture: boolean): Promise<boolean>;
    /** Explicitly trust a loopback HTTP server; regular window.open stays local-file only. */
    openDev(url: string): Promise<number>;

    getSize(): Promise<Pick<ReaWebBounds, 'width' | 'height' | 'mode' | 'units'>>;
    /** Floating windows only. Values are outer dimensions in native desktop units, not CSS pixels. */
    setSize(width: number, height: number): Promise<ReaWebBounds>;
    getPosition(): Promise<Pick<ReaWebBounds, 'x' | 'y' | 'mode' | 'units'>>;
    setPosition(x: number, y: number): Promise<ReaWebBounds>;
    show(): Promise<ReaWebWindowState>; hide(): Promise<ReaWebWindowState>;
    getState(): Promise<ReaWebWindowState>; setTitle(title: string): Promise<boolean>;
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
    /** One managed gesture at a time. Ends on close, reload, project change or after 30 seconds.
     * Does not hold PreventUIRefresh across browser events. Uses the reviewed batch API set. End before batching. */
    beginUndo(label: string): Promise<string>;
    endUndo(token: string): Promise<boolean>;
    withUndo<T>(label: string, callback: () => T | Promise<T>): Promise<T>;

  };
  readonly audio: {
    /** Explicit coalescing: one in-flight write and the latest waiting value per track/key.
     * Superseded values settle without being sent. Does not create an Undo gesture.
     */
    setTrackValueLatest(track: MediaTrackHandle, key: ReaWebContinuousTrackKey, value: number): Promise<{ applied: boolean; superseded: boolean }>;

    getFileInfo(path: string): Promise<ReaWebAudioFileInfo>;
    getWaveform(path: string, options?: ReaWebWaveformOptions): Promise<ReaWebWaveform>;
    getTrackMeter(track: MediaTrackHandle): Promise<ReaWebTrackMeter>;
  };
}
