// The File System Access API's picker entry points are not part of TypeScript's
// lib.dom yet (the handle and writable types are). Declare only what this app
// calls, optionally, so browsers without the API remain a normal code path.
interface SaveFilePickerOptions {
  suggestedName?: string
}

interface DirectoryPickerOptions {
  mode?: 'read' | 'readwrite'
}

interface Window {
  showSaveFilePicker?: (
    options?: SaveFilePickerOptions,
  ) => Promise<FileSystemFileHandle>
  showDirectoryPicker?: (
    options?: DirectoryPickerOptions,
  ) => Promise<FileSystemDirectoryHandle>
}
