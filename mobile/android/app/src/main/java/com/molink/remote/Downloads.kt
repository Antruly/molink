package com.molink.remote

import android.content.ContentValues
import android.content.Context
import android.media.MediaScannerConnection
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.provider.MediaStore
import android.util.Base64
import java.io.File
import java.io.FileOutputStream
import java.io.OutputStream

/**
 * Writes downloads into the public Download directory.
 *
 * On Android 10+ this goes through MediaStore, which needs no permission. On
 * older releases it writes the file directly; the caller has already obtained
 * WRITE_EXTERNAL_STORAGE in that case.
 */
object Downloads {

    private val ILLEGAL = Regex("[\\\\/:*?\"<>|\\u0000-\\u001f\\u007f]")

    fun sanitizeName(raw: String?): String {
        val base = (raw ?: "")
            .substringAfterLast('/')
            .substringAfterLast('\\')
            .replace(ILLEGAL, "_")
            .trim()
        if (base.isEmpty() || base == "." || base == "..") return "download"
        return base.take(180)
    }

    fun open(context: Context, rawName: String, mime: String): Sink? {
        val name = sanitizeName(rawName)
        val type = mime.ifBlank { "application/octet-stream" }
        return try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                openMediaStore(context, name, type)
            } else {
                openLegacy(context, name, type)
            }
        } catch (t: Throwable) {
            null
        }
    }

    private fun openMediaStore(context: Context, name: String, mime: String): Sink? {
        val resolver = context.contentResolver
        val values = ContentValues().apply {
            put(MediaStore.MediaColumns.DISPLAY_NAME, name)
            put(MediaStore.MediaColumns.MIME_TYPE, mime)
            put(MediaStore.MediaColumns.RELATIVE_PATH, Environment.DIRECTORY_DOWNLOADS)
            put(MediaStore.MediaColumns.IS_PENDING, 1)
        }
        val uri = resolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values)
            ?: return null
        val stream = resolver.openOutputStream(uri)
        if (stream == null) {
            resolver.delete(uri, null, null)
            return null
        }
        return Sink(context, uri, null, stream, mime)
    }

    @Suppress("DEPRECATION")
    private fun openLegacy(context: Context, name: String, mime: String): Sink? {
        val dir = Environment.getExternalStoragePublicDirectory(
            Environment.DIRECTORY_DOWNLOADS,
        )
        if (!dir.exists() && !dir.mkdirs()) return null
        val file = uniqueFile(dir, name)
        return Sink(context, null, file, FileOutputStream(file), mime)
    }

    private fun uniqueFile(dir: File, name: String): File {
        var candidate = File(dir, name)
        if (!candidate.exists()) return candidate
        val dot = name.lastIndexOf('.')
        val base = if (dot > 0) name.substring(0, dot) else name
        val ext = if (dot > 0) name.substring(dot) else ""
        var index = 1
        while (candidate.exists()) {
            candidate = File(dir, "$base ($index)$ext")
            index += 1
        }
        return candidate
    }

    /** Saves a `data:` URL (WebView hands these to DownloadListener too). */
    fun writeDataUri(
        context: Context,
        dataUri: String,
        fallbackName: String,
        fallbackMime: String,
    ): Boolean {
        if (!dataUri.startsWith("data:", ignoreCase = true)) return false
        val comma = dataUri.indexOf(',')
        if (comma < 0) return false
        val meta = dataUri.substring(5, comma)
        val body = dataUri.substring(comma + 1)
        val parts = meta.split(';')
        val mime = parts.firstOrNull()?.takeIf { it.contains('/') }
            ?: fallbackMime.ifBlank { "application/octet-stream" }
        val isBase64 = parts.any { it.equals("base64", ignoreCase = true) }
        val bytes = try {
            if (isBase64) {
                Base64.decode(body, Base64.DEFAULT)
            } else {
                Uri.decode(body).toByteArray(Charsets.UTF_8)
            }
        } catch (t: Throwable) {
            return false
        }
        val sink = open(context, fallbackName, mime) ?: return false
        val written = sink.write(bytes)
        if (!written) {
            sink.abort()
            return false
        }
        return sink.finish()
    }

    /** One open destination; writing and closing happen on the JS bridge thread. */
    class Sink internal constructor(
        private val context: Context,
        private val uri: Uri?,
        private val file: File?,
        private val stream: OutputStream,
        private val mime: String,
    ) {
        private var closed = false

        @Synchronized
        fun write(bytes: ByteArray): Boolean {
            if (closed || bytes.isEmpty()) return !closed
            return try {
                stream.write(bytes)
                true
            } catch (t: Throwable) {
                false
            }
        }

        @Synchronized
        fun finish(): Boolean {
            if (closed) return false
            closed = true
            val ok = try {
                stream.flush()
                stream.close()
                true
            } catch (t: Throwable) {
                false
            }
            if (!ok) {
                abortLocked()
                return false
            }
            if (uri != null) {
                try {
                    val values = ContentValues().apply {
                        put(MediaStore.MediaColumns.IS_PENDING, 0)
                    }
                    context.contentResolver.update(uri, values, null, null)
                } catch (t: Throwable) {
                    // The bytes are already on disk; a failed publish leaves
                    // the item pending but does not corrupt it.
                }
            }
            if (file != null) {
                try {
                    MediaScannerConnection.scanFile(
                        context,
                        arrayOf(file.absolutePath),
                        arrayOf(mime),
                        null,
                    )
                } catch (t: Throwable) {
                    // MediaScanner is best-effort.
                }
            }
            return true
        }

        @Synchronized
        fun abort() {
            if (closed) return
            closed = true
            abortLocked()
        }

        private fun abortLocked() {
            try {
                stream.close()
            } catch (t: Throwable) {
                // Already closed.
            }
            if (uri != null) {
                try {
                    context.contentResolver.delete(uri, null, null)
                } catch (t: Throwable) {
                    // Nothing more we can do.
                }
            }
            if (file != null) {
                //noinspection ResultOfMethodCallIgnored
                file.delete()
            }
        }
    }
}
