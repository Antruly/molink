package com.molink.remote

import android.Manifest
import android.app.DownloadManager
import android.content.ActivityNotFoundException
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Environment
import android.os.Handler
import android.os.Looper
import android.view.WindowManager
import android.webkit.CookieManager
import android.webkit.URLUtil
import android.webkit.ValueCallback
import android.webkit.WebChromeClient
import android.widget.Button
import android.widget.FrameLayout
import android.widget.TextView
import android.widget.Toast
import androidx.activity.addCallback
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat

class MainActivity : AppCompatActivity() {

    private lateinit var sessionView: SessionView
    private lateinit var hostLabel: TextView
    private lateinit var statusLabel: TextView

    private var target: HostTarget? = null
    private var loginCode: String = ""

    private var pendingChooser: ValueCallback<Array<Uri>>? = null
    private var pendingDownload: DownloadRequest? = null
    private var autoLoginDone = false
    private var autoLoginRunning = false
    private val autoLoginHandler = Handler(Looper.getMainLooper())

    private val fileChooserLauncher =
        registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
            val callback = pendingChooser
            pendingChooser = null
            if (callback == null) return@registerForActivityResult
            val data = result.data
            if (result.resultCode != RESULT_OK || data == null) {
                callback.onReceiveValue(null)
                return@registerForActivityResult
            }
            val uris = ArrayList<Uri>()
            val clip = data.clipData
            if (clip != null) {
                for (index in 0 until clip.itemCount) {
                    clip.getItemAt(index)?.uri?.let { uris.add(it) }
                }
            } else {
                data.data?.let { uris.add(it) }
            }
            if (uris.isEmpty()) {
                callback.onReceiveValue(null)
                return@registerForActivityResult
            }
            uris.forEach { uri ->
                try {
                    contentResolver.takePersistableUriPermission(
                        uri,
                        Intent.FLAG_GRANT_READ_URI_PERMISSION,
                    )
                } catch (_: SecurityException) {
                    // Some providers do not offer persistable grants.
                }
            }
            callback.onReceiveValue(uris.toTypedArray())
        }

    private val writePermissionLauncher =
        registerForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
            val request = pendingDownload
            pendingDownload = null
            if (granted && request != null) {
                performDownload(request)
            } else {
                toast(R.string.storage_permission_denied)
            }
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        val host = intent.getStringExtra(EXTRA_HOST).orEmpty()
        val port = intent.getIntExtra(EXTRA_PORT, HostTarget.DEFAULT_PORT)
        if (host.isBlank()) {
            finish()
            return
        }
        val destination = HostTarget(host, port)
        target = destination
        loginCode = intent.getStringExtra(EXTRA_CODE).orEmpty().trim()
        val clientToken = intent.getStringExtra(EXTRA_CLIENT_TOKEN)
            ?.trim()
            ?.takeIf { it.isNotEmpty() }

        hostLabel = findViewById(R.id.text_host)
        statusLabel = findViewById(R.id.text_status)
        hostLabel.text = destination.authority
        statusLabel.setText(R.string.status_loading)

        val container = findViewById<FrameLayout>(R.id.session_container)
        sessionView = SessionView(this).also { view ->
            container.addView(
                view,
                FrameLayout.LayoutParams(
                    FrameLayout.LayoutParams.MATCH_PARENT,
                    FrameLayout.LayoutParams.MATCH_PARENT,
                ),
            )
            view.onFileChooser = ::showFileChooser
            view.onDownload = ::onDownload
            view.onPageChanged = ::onPageChanged
        }
        sessionView.load(destination, clientToken)

        findViewById<Button>(R.id.button_keyboard).setOnClickListener {
            sessionView.toggleKeyboard()
        }
        findViewById<Button>(R.id.button_reload).setOnClickListener {
            autoLoginDone = false
            sessionView.reload()
        }
        findViewById<Button>(R.id.button_host).setOnClickListener {
            finish()
        }

        onBackPressedDispatcher.addCallback(this) {
            if (sessionView.webView.canGoBack()) {
                sessionView.webView.goBack()
            } else {
                finish()
            }
        }
    }

    override fun onResume() {
        super.onResume()
        if (::sessionView.isInitialized) sessionView.webView.onResume()
    }

    override fun onPause() {
        if (::sessionView.isInitialized) {
            sessionView.webView.onPause()
            CookieManager.getInstance().flush()
        }
        super.onPause()
    }

    override fun onDestroy() {
        autoLoginHandler.removeCallbacksAndMessages(null)
        if (::sessionView.isInitialized) sessionView.destroy()
        super.onDestroy()
    }

    private fun onPageChanged(state: PageState) {
        statusLabel.setText(
            when (state) {
                PageState.LOADING -> R.string.status_loading
                PageState.READY -> R.string.status_ready
                PageState.ERROR -> R.string.status_error
            },
        )
        if (state == PageState.READY) tryAutoLogin()
    }

    /**
     * The host's remote login screen is the AuthGate with `#auth-code`. When
     * the setup screen collected a code, fill and submit that form instead of
     * making the user type it twice. Stops after a bounded number of retries:
     * a wrong code must not turn into a login-lockout loop.
     */
    private fun tryAutoLogin() {
        if (loginCode.isEmpty() || autoLoginDone || autoLoginRunning) return
        autoLoginRunning = true
        var attempts = 0
        val task = object : Runnable {
            override fun run() {
                if (autoLoginDone || attempts >= 20 || isFinishing || isDestroyed) {
                    autoLoginRunning = false
                    return
                }
                attempts += 1
                sessionView.fillLogin(loginCode) { accepted ->
                    if (accepted) {
                        autoLoginDone = true
                        autoLoginRunning = false
                    } else {
                        autoLoginHandler.postDelayed(this, 700)
                    }
                }
            }
        }
        autoLoginHandler.postDelayed(task, 400)
    }

    private fun showFileChooser(
        params: WebChromeClient.FileChooserParams,
        callback: ValueCallback<Array<Uri>>,
    ): Boolean {
        pendingChooser?.onReceiveValue(null)
        pendingChooser = callback

        val accept = params.acceptTypes
            .map { it.trim() }
            .filter { it.isNotEmpty() && it.contains('/') && it != "*/*" }
            .distinct()
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"
            if (accept.isNotEmpty()) {
                putExtra(Intent.EXTRA_MIME_TYPES, accept.toTypedArray())
            }
            putExtra(
                Intent.EXTRA_ALLOW_MULTIPLE,
                params.mode == WebChromeClient.FileChooserParams.MODE_OPEN_MULTIPLE,
            )
        }
        return try {
            fileChooserLauncher.launch(intent)
            true
        } catch (_: ActivityNotFoundException) {
            pendingChooser = null
            callback.onReceiveValue(null)
            toast(R.string.file_picker_unavailable)
            false
        }
    }

    private fun onDownload(request: DownloadRequest) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q &&
            ContextCompat.checkSelfPermission(
                this,
                Manifest.permission.WRITE_EXTERNAL_STORAGE,
            ) != PackageManager.PERMISSION_GRANTED
        ) {
            pendingDownload = request
            writePermissionLauncher.launch(Manifest.permission.WRITE_EXTERNAL_STORAGE)
            return
        }
        performDownload(request)
    }

    private fun performDownload(request: DownloadRequest) {
        val guessed = URLUtil.guessFileName(
            request.url,
            request.contentDisposition,
            request.mimeType,
        )
        val name = Downloads.sanitizeName(guessed)
        val mime = request.mimeType?.takeIf { it.isNotBlank() }
            ?: "application/octet-stream"

        when {
            request.url.startsWith("blob:", ignoreCase = true) -> {
                // The blob lives in the renderer; the page streams it to the
                // JavascriptInterface instead of DownloadManager.
                sessionView.saveBlobUrl(request.url, name, mime)
                toast(R.string.download_started)
            }
            request.url.startsWith("data:", ignoreCase = true) -> {
                val ok = Downloads.writeDataUri(this, request.url, name, mime)
                toast(if (ok) R.string.download_saved else R.string.download_failed)
            }
            request.url.startsWith("http://", ignoreCase = true) ||
                request.url.startsWith("https://", ignoreCase = true) -> {
                enqueueDownload(request, name, mime)
            }
            else -> toast(R.string.download_unsupported)
        }
    }

    private fun enqueueDownload(request: DownloadRequest, name: String, mime: String) {
        val manager = getSystemService(Context.DOWNLOAD_SERVICE) as DownloadManager
        val download = DownloadManager.Request(Uri.parse(request.url)).apply {
            setTitle(name)
            setDescription(getString(R.string.download_description))
            setMimeType(mime)
            setNotificationVisibility(
                DownloadManager.Request.VISIBILITY_VISIBLE_NOTIFY_COMPLETED,
            )
            setDestinationInExternalPublicDir(Environment.DIRECTORY_DOWNLOADS, name)
            val cookie = CookieManager.getInstance().getCookie(request.url)
            if (!cookie.isNullOrEmpty()) addRequestHeader("Cookie", cookie)
            if (!request.userAgent.isNullOrEmpty()) {
                addRequestHeader("User-Agent", request.userAgent)
            }
        }
        try {
            manager.enqueue(download)
            toast(R.string.download_started)
        } catch (_: Exception) {
            toast(R.string.download_failed)
        }
    }

    private fun toast(resId: Int) {
        Toast.makeText(this, resId, Toast.LENGTH_SHORT).show()
    }

    companion object {
        const val EXTRA_HOST = "host"
        const val EXTRA_PORT = "port"
        const val EXTRA_CODE = "code"
        const val EXTRA_CLIENT_TOKEN = "client_token"
    }
}
