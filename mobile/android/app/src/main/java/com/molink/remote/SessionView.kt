package com.molink.remote

import android.annotation.SuppressLint
import android.content.Context
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.Color
import android.net.Uri
import android.util.AttributeSet
import android.util.Base64
import android.util.Log
import android.view.MotionEvent
import android.view.View
import android.view.inputmethod.InputMethodManager
import android.webkit.CookieManager
import android.webkit.JavascriptInterface
import android.webkit.ValueCallback
import android.webkit.WebChromeClient
import android.webkit.WebResourceError
import android.webkit.WebResourceRequest
import android.webkit.WebResourceResponse
import android.webkit.WebSettings
import android.webkit.WebView
import android.webkit.WebViewClient
import android.widget.FrameLayout
import android.widget.Toast
import org.json.JSONObject
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.atomic.AtomicLong

enum class PageState { LOADING, READY, ERROR }

data class DownloadRequest(
    val url: String,
    val userAgent: String?,
    val contentDisposition: String?,
    val mimeType: String?,
    val contentLength: Long,
)

/**
 * The WebView half of the shell: page configuration, the touch-to-mouse
 * bridge and the native file/download endpoints.
 *
 * Touch gestures are synthesised in JavaScript rather than fed to the WebView
 * as MotionEvents. The page's desktop surface listens for pointer events and
 * treats a touch as a held left button, so forwarding raw touches would drag
 * with a button down instead of moving the pointer. Injecting pointer events
 * lets the shell keep touchpad semantics (single finger = move, long press =
 * right click, two fingers = wheel, pinch = Ctrl+wheel) while every touch
 * outside the picture still goes to the normal Web UI untouched.
 */
@SuppressLint("SetJavaScriptEnabled")
class SessionView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
) : FrameLayout(context, attrs) {

    val webView: WebView = WebView(context)

    /** Set by MainActivity: opens ACTION_OPEN_DOCUMENT for `<input type=file>`. */
    var onFileChooser: ((WebChromeClient.FileChooserParams, ValueCallback<Array<Uri>>) -> Boolean)? = null

    /** Set by MainActivity: DownloadManager / blob bridge. */
    var onDownload: ((DownloadRequest) -> Unit)? = null

    var onPageChanged: ((PageState) -> Unit)? = null

    private val mainHandler = android.os.Handler(android.os.Looper.getMainLooper())
    private var allowedHost = ""
    private var allowedPort = 0

    /** True while the WebView is on the host page the bridge is allowed to serve. */
    @Volatile
    private var trustedPage = false

    private val downloadSinks = ConcurrentHashMap<String, Downloads.Sink>()
    private val nextDownloadId = AtomicLong(1)

    init {
        setBackgroundColor(Color.BLACK)
        webView.setBackgroundColor(Color.BLACK)
        addView(webView, LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT))
        configure(webView)
        webView.addJavascriptInterface(DownloadBridge(), "MoLinkDownload")
    }

    @SuppressLint("SetJavaScriptEnabled")
    private fun configure(view: WebView) {
        val settings = view.settings
        settings.javaScriptEnabled = true
        settings.domStorageEnabled = true
        settings.databaseEnabled = true
        settings.mediaPlaybackRequiresUserGesture = false
        settings.mixedContentMode = WebSettings.MIXED_CONTENT_COMPATIBILITY_MODE
        settings.setSupportZoom(false)
        settings.builtInZoomControls = false
        settings.displayZoomControls = false
        settings.loadWithOverviewMode = true
        settings.useWideViewPort = true
        settings.cacheMode = WebSettings.LOAD_DEFAULT
        settings.allowFileAccess = false
        // content:// access stays on: the file chooser hands upload URIs to
        // the renderer through it.
        settings.allowContentAccess = true
        settings.javaScriptCanOpenWindowsAutomatically = false
        settings.setSupportMultipleWindows(false)
        settings.userAgentString = settings.userAgentString + " moLinkAndroid/0.1"

        view.isHorizontalScrollBarEnabled = false
        view.isVerticalScrollBarEnabled = false
        view.overScrollMode = View.OVER_SCROLL_NEVER

        CookieManager.getInstance().setAcceptCookie(true)
        CookieManager.getInstance().setAcceptThirdPartyCookies(view, true)

        // The page's key forwarding requires document.hasFocus(); a touch that
        // lands after the top bar or the IME had focus must return focus to
        // the WebView, otherwise the pinch's Ctrl key would be dropped.
        view.setOnTouchListener { _, event ->
            if (event.actionMasked == MotionEvent.ACTION_DOWN) {
                view.requestFocus()
            }
            false
        }

        view.webViewClient = ShellClient()
        view.webChromeClient = ShellChromeClient()
        view.setDownloadListener { url, userAgent, contentDisposition, mimeType, contentLength ->
            if (!url.isNullOrEmpty()) {
                onDownload?.invoke(
                    DownloadRequest(url, userAgent, contentDisposition, mimeType, contentLength),
                )
            }
        }
    }

    fun load(target: HostTarget, clientToken: String?) {
        allowedHost = target.host
        allowedPort = target.port
        webView.loadUrl(target.url(clientToken))
    }

    fun reload() {
        webView.reload()
    }

    fun toggleKeyboard() {
        val manager = context.getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
        if (manager.isAcceptingText) {
            manager.hideSoftInputFromWindow(webView.windowToken, 0)
            webView.clearFocus()
            return
        }
        webView.requestFocus()
        webView.evaluateJavascript(
            "window.__molinkFocusKeyboard && window.__molinkFocusKeyboard()",
        ) {
            mainHandler.postDelayed({
                manager.showSoftInput(webView, InputMethodManager.SHOW_IMPLICIT)
            }, 80)
        }
    }

    /**
     * Runs in the page so a `blob:` URL can be read (it is renderer-local and
     * DownloadManager cannot fetch it). The page streams 192 KiB base64 chunks
     * to [DownloadBridge], which writes them into Download/.
     */
    fun saveBlobUrl(url: String, fallbackName: String, mime: String) {
        val script = "window.__molinkSaveBlob && window.__molinkSaveBlob(" +
            JSONObject.quote(url) + "," +
            JSONObject.quote(fallbackName) + "," +
            JSONObject.quote(mime) + ")"
        webView.evaluateJavascript(script, null)
    }

    /** Fills the host's `#auth-code` field and submits its form once. */
    fun fillLogin(code: String, onResult: (Boolean) -> Unit) {
        val script = "(function(){try{return !!(window.__molinkFillLogin && " +
            "window.__molinkFillLogin(" + JSONObject.quote(code) + "));}catch(e){return false;}})()"
        webView.evaluateJavascript(script) { value ->
            onResult(value == "true")
        }
    }

    fun destroy() {
        mainHandler.removeCallbacksAndMessages(null)
        downloadSinks.values.forEach { it.abort() }
        downloadSinks.clear()
        webView.stopLoading()
        webView.loadUrl("about:blank")
        webView.clearHistory()
        webView.removeAllViews()
        webView.destroy()
    }

    private fun injectBridge() {
        webView.evaluateJavascript(BRIDGE_JS, null)
    }

    private fun updateTrusted(url: String?) {
        if (url.isNullOrEmpty()) {
            trustedPage = false
            return
        }
        val uri = Uri.parse(url)
        trustedPage = (uri.scheme == "http" || uri.scheme == "https") &&
            uri.host == allowedHost && uri.port == allowedPort
    }

    private fun openExternal(uri: Uri) {
        try {
            val intent = Intent(Intent.ACTION_VIEW, uri)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            context.startActivity(intent)
        } catch (_: Exception) {
            // No handler for this scheme.
        }
    }

    private inner class ShellClient : WebViewClient() {

        override fun onPageStarted(view: WebView?, url: String?, favicon: Bitmap?) {
            updateTrusted(url)
            onPageChanged?.invoke(PageState.LOADING)
            // Inject early so the capture listeners exist before the SPA
            // starts; onPageFinished injects again as a fallback.
            injectBridge()
        }

        override fun onPageFinished(view: WebView?, url: String?) {
            updateTrusted(url)
            injectBridge()
            onPageChanged?.invoke(PageState.READY)
        }

        override fun shouldOverrideUrlLoading(
            view: WebView?,
            request: WebResourceRequest?,
        ): Boolean {
            val uri = request?.url ?: return false
            if (uri.scheme == "http" || uri.scheme == "https") {
                if (uri.host == allowedHost && uri.port == allowedPort) return false
                openExternal(uri)
                return true
            }
            if (uri.scheme == "intent") {
                try {
                    context.startActivity(
                        Intent.parseUri(uri.toString(), Intent.URI_INTENT_SCHEME),
                    )
                } catch (_: Exception) {
                    // Nothing handles the intent.
                }
                return true
            }
            openExternal(uri)
            return true
        }

        override fun onReceivedError(
            view: WebView?,
            request: WebResourceRequest?,
            error: WebResourceError?,
        ) {
            if (request?.isForMainFrame != true) return
            onPageChanged?.invoke(PageState.ERROR)
            val detail = error?.description?.toString().orEmpty()
            mainHandler.post {
                Toast.makeText(
                    context,
                    context.getString(R.string.web_error, detail),
                    Toast.LENGTH_LONG,
                ).show()
            }
        }

        override fun onReceivedHttpError(
            view: WebView?,
            request: WebResourceRequest?,
            errorResponse: WebResourceResponse?,
        ) {
            if (request?.isForMainFrame != true) return
            onPageChanged?.invoke(PageState.ERROR)
            val status = errorResponse?.statusCode ?: 0
            mainHandler.post {
                Toast.makeText(
                    context,
                    context.getString(R.string.web_http_error, status),
                    Toast.LENGTH_LONG,
                ).show()
            }
        }
    }

    private inner class ShellChromeClient : WebChromeClient() {

        override fun onShowFileChooser(
            webView: WebView?,
            filePathCallback: ValueCallback<Array<Uri>>?,
            fileChooserParams: FileChooserParams?,
        ): Boolean {
            val callback = filePathCallback ?: return false
            val params = fileChooserParams ?: return false
            val handler = onFileChooser
            if (handler == null) {
                callback.onReceiveValue(null)
                return true
            }
            return handler(params, callback)
        }
    }

    private inner class DownloadBridge {

        @JavascriptInterface
        fun begin(name: String?, mime: String?): String? {
            if (!trustedPage) return null
            val type = mime?.takeIf { it.contains('/') } ?: "application/octet-stream"
            val sink = Downloads.open(context, Downloads.sanitizeName(name), type) ?: return null
            val id = nextDownloadId.incrementAndGet().toString()
            downloadSinks[id] = sink
            return id
        }

        @JavascriptInterface
        fun write(id: String?, base64: String?): Boolean {
            val sink = id?.let { downloadSinks[it] } ?: return false
            val bytes = try {
                Base64.decode(base64 ?: "", Base64.DEFAULT)
            } catch (t: Throwable) {
                return false
            }
            return sink.write(bytes)
        }

        @JavascriptInterface
        fun finish(id: String?) {
            val sink = id?.let { downloadSinks.remove(it) } ?: return
            val ok = sink.finish()
            mainHandler.post {
                Toast.makeText(
                    context,
                    if (ok) R.string.download_saved else R.string.download_failed,
                    Toast.LENGTH_SHORT,
                ).show()
            }
        }

        @JavascriptInterface
        fun abort(id: String?) {
            id?.let { downloadSinks.remove(it) }?.abort()
        }

        @JavascriptInterface
        fun failed(reason: String?) {
            Log.w(TAG, "blob download failed: $reason")
            mainHandler.post {
                Toast.makeText(context, R.string.download_failed, Toast.LENGTH_SHORT).show()
            }
        }
    }

    companion object {
        private const val TAG = "SessionView"

        /**
         * Injected into every document.
         *
         * Everything is one IIFE with an idempotence guard because it is
         * evaluated from both onPageStarted and onPageFinished, and a reload
         * starts a fresh page context.
         */
        private const val BRIDGE_JS = """
(function () {
  if (window.__molinkTouchpad) return;
  var S = { installed: true };
  window.__molinkTouchpad = S;

  // React calls setPointerCapture on every pointerdown. A synthesised event
  // has no active pointer id, so Chromium throws and the rest of the handler
  // (notably the remote button press) would be skipped. Swallow only that.
  try {
    var setCapture = Element.prototype.setPointerCapture;
    Element.prototype.setPointerCapture = function (id) {
      try { return setCapture.call(this, id); } catch (e) { return undefined; }
    };
    var releaseCapture = Element.prototype.releasePointerCapture;
    Element.prototype.releasePointerCapture = function (id) {
      try { return releaseCapture.call(this, id); } catch (e) { return undefined; }
    };
  } catch (e) { /* WebView without the pointer API */ }

  function isSurface(el) {
    var node = el;
    while (node && node !== document.documentElement) {
      if (node.classList &&
          node.classList.contains('touch-none') &&
          node.classList.contains('select-none') &&
          node.classList.contains('outline-none')) {
        return node;
      }
      node = node.parentElement;
    }
    return null;
  }

  function pointer(type, x, y, buttons, button) {
    var event = new PointerEvent(type, {
      bubbles: true,
      cancelable: true,
      composed: true,
      view: window,
      pointerId: 1,
      pointerType: 'touch',
      isPrimary: true,
      clientX: x,
      clientY: y,
      screenX: x,
      screenY: y,
      button: button === undefined ? 0 : button,
      buttons: buttons,
      width: 1,
      height: 1,
      pressure: buttons > 0 ? 0.5 : 0
    });
    event.__molinkSynthetic = true;
    return event;
  }

  function wheel(x, y, dx, dy) {
    if (!surface) return;
    var event = new WheelEvent('wheel', {
      bubbles: true,
      cancelable: true,
      composed: true,
      clientX: x,
      clientY: y,
      screenX: x,
      screenY: y,
      deltaX: dx,
      deltaY: dy,
      deltaMode: 0
    });
    event.__molinkSynthetic = true;
    surface.dispatchEvent(event);
  }

  function sendPointer(type, x, y, buttons, button) {
    if (!surface || !surface.isConnected) return;
    surface.dispatchEvent(pointer(type, x, y, buttons, button));
  }

  function click(x, y, button) {
    sendPointer('pointermove', x, y, 0, 0);
    sendPointer('pointerdown', x, y, button === 2 ? 2 : 1, button);
    sendPointer('pointerup', x, y, 0, button);
  }

  var pointers = new Map();
  var surface = null;
  var startX = 0;
  var startY = 0;
  var startedAt = 0;
  var moved = false;
  var suppressTap = false;
  var longPress = null;
  var mode = 'single';
  var scrollX = 0;
  var scrollY = 0;
  var lastX = 0;
  var lastY = 0;
  var pinchStart = 0;
  var pinchNotches = 0;
  var ctrlHeld = false;

  function controlKey(type) {
    var event = new KeyboardEvent(type, {
      key: 'Control',
      code: 'ControlLeft',
      location: 1,
      bubbles: true,
      cancelable: true,
      composed: true,
      ctrlKey: type === 'keydown'
    });
    event.__molinkSynthetic = true;
    return event;
  }

  function ctrlDown() {
    if (ctrlHeld) return;
    ctrlHeld = true;
    window.dispatchEvent(controlKey('keydown'));
  }

  function ctrlUp() {
    if (!ctrlHeld) return;
    ctrlHeld = false;
    window.dispatchEvent(controlKey('keyup'));
  }

  function clearLongPress() {
    if (longPress !== null) {
      clearTimeout(longPress);
      longPress = null;
    }
  }

  function centroid() {
    var x = 0;
    var y = 0;
    var n = 0;
    pointers.forEach(function (p) {
      x += p.x;
      y += p.y;
      n += 1;
    });
    return { x: n ? x / n : 0, y: n ? y / n : 0 };
  }

  function span() {
    var first = null;
    var second = null;
    pointers.forEach(function (p) {
      if (first === null) first = p;
      else if (second === null) second = p;
    });
    if (first === null || second === null) return 0;
    var dx = first.x - second.x;
    var dy = first.y - second.y;
    return Math.sqrt(dx * dx + dy * dy);
  }

  function longPressNow() {
    longPress = null;
    if (pointers.size !== 1 || moved) return;
    suppressTap = true;
    var point = pointers.values().next().value;
    click(point.x, point.y, 2);
    if (navigator.vibrate) navigator.vibrate(12);
  }

  function down(event) {
    if (event.__molinkSynthetic || event.pointerType !== 'touch') return;
    var found = isSurface(event.target);
    if (!found) return;
    event.preventDefault();
    event.stopPropagation();
    if (event.stopImmediatePropagation) event.stopImmediatePropagation();

    if (pointers.size === 0) {
      surface = found;
      startX = event.clientX;
      startY = event.clientY;
      startedAt = Date.now();
      moved = false;
      suppressTap = false;
      mode = 'single';
      scrollX = 0;
      scrollY = 0;
      lastX = event.clientX;
      lastY = event.clientY;
      pinchStart = 0;
      pinchNotches = 0;
      clearLongPress();
      longPress = setTimeout(longPressNow, 500);
    } else {
      clearLongPress();
      suppressTap = true;
      if (mode === 'single') {
        mode = 'scroll';
        var center = centroid();
        lastX = center.x;
        lastY = center.y;
        pinchStart = 0;
        pinchNotches = 0;
      }
    }

    pointers.set(event.pointerId, { x: event.clientX, y: event.clientY });
    if (pointers.size === 2) {
      pinchStart = span();
      var twoFingerCenter = centroid();
      lastX = twoFingerCenter.x;
      lastY = twoFingerCenter.y;
    }
  }

  function move(event) {
    if (event.__molinkSynthetic || event.pointerType !== 'touch') return;
    var point = pointers.get(event.pointerId);
    if (!point || !surface) return;
    event.preventDefault();
    event.stopPropagation();
    if (event.stopImmediatePropagation) event.stopImmediatePropagation();
    point.x = event.clientX;
    point.y = event.clientY;

    if (pointers.size === 1) {
      if (!moved) {
        var dx = event.clientX - startX;
        var dy = event.clientY - startY;
        if (dx * dx + dy * dy > 64) {
          moved = true;
          clearLongPress();
        }
      }
      // A one-finger drag is pointer movement, not a held mouse button.
      if (moved) sendPointer('pointermove', event.clientX, event.clientY, 0, 0);
      return;
    }

    clearLongPress();
    suppressTap = true;
    var center = centroid();

    if (mode !== 'pinch') {
      if (pinchStart <= 0) pinchStart = span();
      if (pinchStart > 0 && Math.abs(span() / pinchStart - 1) > 0.12) {
        mode = 'pinch';
        ctrlDown();
        // The host zooms at the cursor; put it under the pinch centre.
        sendPointer('pointermove', center.x, center.y, 0, 0);
        pinchNotches = 0;
      }
    }

    if (mode === 'pinch') {
      var ratio = pinchStart > 0 ? span() / pinchStart : 1;
      var notches = Math.trunc((ratio - 1) * 10);
      if (notches !== pinchNotches) {
        var delta = notches - pinchNotches;
        pinchNotches = notches;
        wheel(center.x, center.y, 0, -delta * 100);
      }
      return;
    }

    scrollX += center.x - lastX;
    scrollY += center.y - lastY;
    lastX = center.x;
    lastY = center.y;
    var step = 20;
    var nx = Math.trunc(scrollX / step);
    var ny = Math.trunc(scrollY / step);
    if (nx !== 0 || ny !== 0) {
      scrollX -= nx * step;
      scrollY -= ny * step;
      // Fingers moving down scroll the remote view up (touchpad direction).
      // Send a real 0 rather than -0 for the axis that did not move.
      wheel(
        center.x,
        center.y,
        nx === 0 ? 0 : -nx * 100,
        ny === 0 ? 0 : -ny * 100
      );
    }
  }

  function finish(event, cancelled) {
    if (event.__molinkSynthetic || event.pointerType !== 'touch') return;
    var point = pointers.get(event.pointerId);
    if (!point || !surface) {
      pointers.delete(event.pointerId);
      return;
    }
    event.preventDefault();
    event.stopPropagation();
    if (event.stopImmediatePropagation) event.stopImmediatePropagation();
    pointers.delete(event.pointerId);
    clearLongPress();

    if (mode === 'pinch') ctrlUp();

    if (pointers.size === 0) {
      var quick = Date.now() - startedAt < 500;
      if (!cancelled && mode !== 'pinch' && !suppressTap && !moved && quick) {
        click(event.clientX, event.clientY, 0);
      }
      surface = null;
      moved = false;
      suppressTap = false;
      mode = 'single';
      scrollX = 0;
      scrollY = 0;
      lastX = 0;
      lastY = 0;
      pinchStart = 0;
      pinchNotches = 0;
    } else {
      // One finger of a two-finger gesture lifted: never make that a tap.
      suppressTap = true;
      moved = true;
      mode = 'single';
      pinchStart = 0;
      pinchNotches = 0;
      var center = centroid();
      lastX = center.x;
      lastY = center.y;
    }
  }

  function up(event) { finish(event, false); }
  function cancel(event) { finish(event, true); }

  // Capture phase: the shell owns touches over the remote picture before the
  // page's React handlers see them. Everything outside the picture is left
  // alone so toolbars, dialogs and the login form keep normal touch input.
  window.addEventListener('pointerdown', down, true);
  window.addEventListener('pointermove', move, true);
  window.addEventListener('pointerup', up, true);
  window.addEventListener('pointercancel', cancel, true);

  // The picture already has touch-action: none; preventing the touch sequence
  // also suppresses the native long-press menu and text selection.
  window.addEventListener('touchstart', function (event) {
    if (isSurface(event.target)) event.preventDefault();
  }, { capture: true, passive: false });
  window.addEventListener('touchmove', function (event) {
    if (isSurface(event.target)) event.preventDefault();
  }, { capture: true, passive: false });
  window.addEventListener('contextmenu', function (event) {
    if (isSurface(event.target)) event.preventDefault();
  }, true);

  // Blob URLs are renderer-local; DownloadManager cannot fetch them. Remember
  // the anchor's suggested file name, then stream the blob to the native
  // bridge in chunks from __molinkSaveBlob below.
  var blobNames = new Map();
  window.addEventListener('click', function (event) {
    var node = event.target;
    while (node && node !== document) {
      if (node.tagName === 'A' && node.hasAttribute &&
          node.hasAttribute('download')) {
        var href = node.href || '';
        if (href.indexOf('blob:') === 0) {
          blobNames.set(href, node.getAttribute('download') || '');
        }
        break;
      }
      node = node.parentElement;
    }
  }, true);

  window.__molinkSaveBlob = function (url, fallbackName, mime) {
    var name = blobNames.get(url) || fallbackName || 'download';
    blobNames.delete(url);
    fetch(url).then(function (response) {
      return response.blob();
    }).then(function (blob) {
      var reader = new FileReader();
      reader.onload = function () {
        var bytes = new Uint8Array(reader.result);
        var id = window.MoLinkDownload
          ? window.MoLinkDownload.begin(name, mime || blob.type ||
              'application/octet-stream')
          : null;
        if (!id) {
          if (window.MoLinkDownload) window.MoLinkDownload.failed('begin');
          return;
        }
        var step = 192 * 1024;
        function next(offset) {
          if (offset >= bytes.length) {
            window.MoLinkDownload.finish(id);
            return;
          }
          var end = Math.min(offset + step, bytes.length);
          var binary = '';
          for (var i = offset; i < end; i++) {
            binary += String.fromCharCode(bytes[i]);
          }
          if (!window.MoLinkDownload.write(id, btoa(binary))) {
            window.MoLinkDownload.abort(id);
            return;
          }
          setTimeout(function () { next(end); }, 0);
        }
        next(0);
      };
      reader.readAsArrayBuffer(blob);
    }).catch(function () {
      if (window.MoLinkDownload) window.MoLinkDownload.failed('fetch');
    });
    return true;
  };

  window.__molinkFocusKeyboard = function () {
    var input = document.querySelector('input[data-remote-ime="true"]');
    if (!input) return false;
    input.focus();
    return true;
  };

  window.__molinkFillLogin = function (code) {
    var input = document.getElementById('auth-code');
    if (!input) return false;
    var descriptor = Object.getOwnPropertyDescriptor(
      HTMLInputElement.prototype,
      'value'
    );
    if (descriptor && descriptor.set) descriptor.set.call(input, code);
    else input.value = code;
    input.dispatchEvent(new Event('input', { bubbles: true }));
    input.dispatchEvent(new Event('change', { bubbles: true }));
    var form = input.form;
    if (form) {
      if (form.requestSubmit) {
        form.requestSubmit();
      } else {
        form.dispatchEvent(
          new Event('submit', { bubbles: true, cancelable: true })
        );
      }
    }
    return true;
  };
})();
"""
    }
}
